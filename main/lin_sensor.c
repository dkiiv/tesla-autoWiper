/**
 * @file lin_sensor.c
 * @brief LIN bus master driver for the VAG rain/light sensor.
 *
 * Break-field workaround
 * ─────────────────────
 * The ESP32 UART treats a received string of 10+ zero-bits as a UART error
 * and also cannot *transmit* an intentional break field at normal baud.
 * The fix (from mestrode/Lin-Interface-Library) is to temporarily halve the
 * baud rate, send one 0x00 byte, then restore the original baud rate.
 *
 *   Normal baud  = 19200 bps  →  1 bit = 52 µs
 *   Half baud    =  9600 bps  →  1 bit = 104 µs
 *
 * Sending 0x00 at 9600 bps:  START + 8 data-zeros + STOP  = 10 bits @ 9600
 *   → as seen on the bus at 19200 bps this looks like ~20 dominant bits,
 *     which comfortably exceeds the LIN 1.x/2.x break requirement of ≥13 bits.
 *
 * Additionally, since UART RX sees its own TX echo (single-wire LIN bus),
 * we flush the RX FIFO after sending the header before reading the response.
 */

#include "lin_sensor.h"
#include "config.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "string.h"

static const char *TAG = "LIN";

/* ─── Internal state ────────────────────────────────────────────────────── */

static lin_rain_sensor_data_t s_latest_data = { 0 };
static SemaphoreHandle_t      s_data_mutex  = NULL;

/* ─── LIN protocol helpers ──────────────────────────────────────────────── */

/**
 * @brief Calculate the protected identifier (PID) from a 6-bit frame ID.
 *
 * LIN 2.x PID parity:
 *   P0 = ID0 ^ ID1 ^ ID2 ^ ID4
 *   P1 = ~(ID1 ^ ID3 ^ ID4 ^ ID5)
 */
static uint8_t lin_calc_pid(uint8_t frame_id)
{
    uint8_t id = frame_id & 0x3F;
    uint8_t p0 = ((id >> 0) ^ (id >> 1) ^ (id >> 2) ^ (id >> 4)) & 0x01;
    uint8_t p1 = (~((id >> 1) ^ (id >> 3) ^ (id >> 4) ^ (id >> 5))) & 0x01;
    return id | (p0 << 6) | (p1 << 7);
}

/**
 * @brief Calculate LIN 2.x enhanced checksum (PID included in sum).
 *
 * Sum all bytes (PID + data), carry any overflow back, then invert.
 */
static uint8_t lin_calc_checksum_enhanced(uint8_t pid, const uint8_t *data, uint8_t len)
{
    uint16_t sum = pid;
    for (int i = 0; i < len; i++) {
        sum += data[i];
        if (sum > 0xFF) {
            sum -= 0xFF;   /* carry wrap-around */
        }
    }
    return (uint8_t)(~sum);
}

/* ─── UART / break-field helpers ────────────────────────────────────────── */

/**
 * @brief Send a LIN break field using the baud-rate halving workaround.
 *
 * Drops to LIN_BAUD/2, transmits 0x00 (= 10 dominant bits at half speed =
 * ~20 bits at nominal), waits for TX FIFO to drain, then restores baud.
 */
static void lin_send_break(void)
{
    uart_set_baudrate(LIN_UART_NUM, LIN_BAUD / 2);

    const uint8_t break_byte = 0x00;
    uart_write_bytes(LIN_UART_NUM, &break_byte, 1);
    uart_wait_tx_done(LIN_UART_NUM, pdMS_TO_TICKS(10));

    uart_set_baudrate(LIN_UART_NUM, LIN_BAUD);
}

/**
 * @brief Send LIN sync byte (always 0x55) and the protected ID.
 */
static void lin_send_header(uint8_t pid)
{
    const uint8_t header[2] = { 0x55, pid };
    uart_write_bytes(LIN_UART_NUM, header, sizeof(header));
    uart_wait_tx_done(LIN_UART_NUM, pdMS_TO_TICKS(5));

    /*
     * On a single-wire LIN bus the transceiver echoes TX back to RX.
     * Flush whatever we just sent so we only read the slave's response.
     */
    uart_flush_input(LIN_UART_NUM);
}

/* ─── Public API ────────────────────────────────────────────────────────── */

esp_err_t lin_sensor_init(void)
{
    uart_config_t uart_cfg = {
        .baud_rate  = LIN_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };

    ESP_ERROR_CHECK(uart_param_config(LIN_UART_NUM, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(LIN_UART_NUM,
                                 LIN_TX_PIN, LIN_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(LIN_UART_NUM,
                                        256,  /* RX buffer */
                                        256,  /* TX buffer */
                                        0, NULL, 0));

    s_data_mutex = xSemaphoreCreateMutex();
    if (s_data_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create data mutex");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LIN UART initialised  baud=%d  TX=GPIO%d  RX=GPIO%d",
             LIN_BAUD, LIN_TX_PIN, LIN_RX_PIN);
    return ESP_OK;
}

esp_err_t lin_sensor_poll(lin_rain_sensor_data_t *out)
{
    /* Number of bytes to read: data payload + 1 checksum byte */
    const int read_len = LIN_RAIN_SENSOR_DATA_LEN + 1;

    uint8_t  pid = lin_calc_pid(LIN_RAIN_SENSOR_FRAME_ID);
    uint8_t  buf[read_len];

    /* ── 1. Send break + header ──────────────────────────────────────── */
    lin_send_break();
    lin_send_header(pid);

    /* ── 2. Read slave response ──────────────────────────────────────── */
    /*
     * Timeout is sized to cover the full response slot:
     *   (data_len + 1 checksum) bytes × 10 bits/byte ÷ baud + margin
     */
    const int timeout_ms = ((read_len * 10 * 1000) / LIN_BAUD) + 10;
    int received = uart_read_bytes(LIN_UART_NUM, buf, read_len,
                                   pdMS_TO_TICKS(timeout_ms));

    if (received < read_len) {
        ESP_LOGW(TAG, "Timeout – got %d of %d bytes", received, read_len);
        if (out) out->valid = false;
        return ESP_ERR_TIMEOUT;
    }

    /* ── 3. Verify enhanced checksum ─────────────────────────────────── */
    uint8_t expected_cs = lin_calc_checksum_enhanced(pid, buf, LIN_RAIN_SENSOR_DATA_LEN);
    uint8_t received_cs = buf[LIN_RAIN_SENSOR_DATA_LEN];

    if (received_cs != expected_cs) {
        ESP_LOGW(TAG, "Checksum mismatch  got=0x%02X  expected=0x%02X",
                 received_cs, expected_cs);
        if (out) out->valid = false;
        return ESP_ERR_INVALID_CRC;
    }

    /* ── 4. Parse payload ────────────────────────────────────────────── */
    if (out) {
        memcpy(out->raw, buf, LIN_RAIN_SENSOR_DATA_LEN);

        /*
         * VAG sensor byte map (verify against your specific part number):
         *   Byte 0 – Rain intensity  bits [3:0]   (upper nibble = other flags)
         *   Byte 1 – Light/brightness
         *   Byte 2 – Sensor status
         */
        out->rain_intensity = buf[0] & 0x0F;
        out->light_intensity = buf[1];
        out->status          = buf[2];
        out->valid           = true;

#if DEBUG_LIN_SENSOR
        ESP_LOGI(TAG, "Rain=%-2d  Light=%-3d  Status=0x%02X  raw: "
                 "%02X %02X %02X %02X %02X %02X %02X",
                 out->rain_intensity, out->light_intensity, out->status,
                 buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);
#endif
    }

    return ESP_OK;
}

void lin_sensor_task(void *arg)
{
    ESP_LOGI(TAG, "LIN sensor task started  poll_interval=%d ms",
             LIN_POLL_INTERVAL_MS);

    lin_rain_sensor_data_t local = { 0 };

    while (1) {
        esp_err_t ret = lin_sensor_poll(&local);

        if (ret == ESP_OK) {
            /* Push valid data to shared state */
            if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                s_latest_data = local;
                xSemaphoreGive(s_data_mutex);
            }
        }
        /* On error, s_latest_data.valid stays false until next success */

        vTaskDelay(pdMS_TO_TICKS(LIN_POLL_INTERVAL_MS));
    }
}

void lin_sensor_get_latest(lin_rain_sensor_data_t *out)
{
    if (out == NULL) return;

    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        *out = s_latest_data;
        xSemaphoreGive(s_data_mutex);
    } else {
        out->valid = false;
    }
}
