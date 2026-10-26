/**
 * @file lin_sensor.c
 * @brief LIN bus master driver for the VAG rain/light sensor.
 *
 * Break-field workaround
 * ─────────────────────
 * The ESP32 UART cannot transmit an intentional break field at normal baud.
 * The fix (from mestrode/Lin-Interface-Library) is to temporarily halve the
 * baud rate, send one 0x00 byte, then restore the original baud rate.
 *
 *   Normal baud  = 19200 bps  →  1 bit = 52 µs
 *   Half baud    =  9600 bps  →  1 bit = 104 µs
 *
 * Sending 0x00 at 9600 bps:  START + 8 data-zeros = 9 dominant bits @ 9600
 *   → ~18 dominant bits at 19200, comfortably over the LIN ≥13-bit break.
 *
 * Echo handling
 * ─────────────
 * On a single-wire LIN bus the transceiver echoes everything we transmit
 * back onto RX.  Sequence per poll:
 *
 *   1. flush RX                 (drop leftovers from the previous poll)
 *   2. send break, flush RX     (break echo is garbage at the wrong baud)
 *   3. send sync 0x55 + PID
 *   4. read  [0x55, PID] echo + DATA_LEN bytes + checksum  in ONE read
 *   5. verify the echo matches, then verify the checksum
 *
 * Reading the echo instead of flushing it (the old approach) means a sensor
 * that answers quickly can't have the start of its reply thrown away, and a
 * bus collision shows up as a bad echo instead of silent garbage.
 */

#include "lin_sensor.h"
#include "protocol.h"
#include "config.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "LIN";

#if LIN_EXPECT_ECHO
#define ECHO_LEN 2      /* sync + PID */
#else
#define ECHO_LEN 0
#endif

/* ─── Internal state ────────────────────────────────────────────────────── */

static lin_rain_sensor_data_t s_latest = { 0 };
static lin_sensor_stats_t     s_stats  = { 0 };

/* Held only while copying structs between tasks. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

uint32_t lin_sensor_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ─── UART helpers ──────────────────────────────────────────────────────── */

static void lin_send_break(void)
{
    uart_set_baudrate(LIN_UART_NUM, LIN_BAUD / 2);

    const uint8_t break_byte = 0x00;
    uart_write_bytes(LIN_UART_NUM, &break_byte, 1);
    uart_wait_tx_done(LIN_UART_NUM, pdMS_TO_TICKS(10));

    uart_set_baudrate(LIN_UART_NUM, LIN_BAUD);
    uart_flush_input(LIN_UART_NUM);   /* break echo, read at the wrong baud */
}

static void lin_send_header(uint8_t pid)
{
    const uint8_t header[2] = { 0x55, pid };
    uart_write_bytes(LIN_UART_NUM, header, sizeof(header));
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
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* Non-fatal by design: return the error, main.c keeps CAN running. */
    esp_err_t ret = uart_driver_install(LIN_UART_NUM, 256, 256, 0, NULL, 0);
    if (ret == ESP_OK) ret = uart_param_config(LIN_UART_NUM, &uart_cfg);
    if (ret == ESP_OK) ret = uart_set_pin(LIN_UART_NUM, LIN_TX_PIN, LIN_RX_PIN,
                                          UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "LIN UART%d up  baud=%d  TX=GPIO%d  RX=GPIO%d  echo=%d",
             LIN_UART_NUM, LIN_BAUD, LIN_TX_PIN, LIN_RX_PIN, LIN_EXPECT_ECHO);
    return ESP_OK;
}

esp_err_t lin_sensor_poll(lin_rain_sensor_data_t *out)
{
    const uint8_t pid      = lin_calc_pid(LIN_RAIN_SENSOR_FRAME_ID);
    const int     resp_len = LIN_RAIN_SENSOR_DATA_LEN + 1;      /* data + checksum */
    const int     read_len = ECHO_LEN + resp_len;
    uint8_t       buf[ECHO_LEN + LIN_RAIN_SENSOR_DATA_LEN + 1];

    uart_flush_input(LIN_UART_NUM);
    lin_send_break();
    lin_send_header(pid);

    /*
     * Slot time: (echo + response) bytes × 10 bits ÷ baud, ×1.4 for the
     * LIN-spec response tolerance, + 5 ms margin.  Needs a 1 kHz tick to be
     * meaningful — sdkconfig.defaults sets CONFIG_FREERTOS_HZ=1000.
     */
    const int timeout_ms = (read_len * 10 * 1000 * 14) / (LIN_BAUD * 10) + 5;
    int got = uart_read_bytes(LIN_UART_NUM, buf, read_len, pdMS_TO_TICKS(timeout_ms));

    if (got < read_len) {
        s_stats.timeout++;
        ESP_LOGD(TAG, "Timeout - got %d of %d bytes", got, read_len);
        return ESP_ERR_TIMEOUT;
    }

#if LIN_EXPECT_ECHO
    if (buf[0] != 0x55 || buf[1] != pid) {
        s_stats.bad_echo++;
        ESP_LOGD(TAG, "Bad echo %02X %02X (expected 55 %02X)", buf[0], buf[1], pid);
        return ESP_ERR_INVALID_RESPONSE;
    }
#endif

    const uint8_t *data = &buf[ECHO_LEN];
    uint8_t expected_cs = lin_calc_checksum_enhanced(pid, data, LIN_RAIN_SENSOR_DATA_LEN);
    uint8_t received_cs = data[LIN_RAIN_SENSOR_DATA_LEN];
    if (received_cs != expected_cs) {
        s_stats.bad_checksum++;
        ESP_LOGD(TAG, "Checksum mismatch  got=0x%02X  expected=0x%02X",
                 received_cs, expected_cs);
        return ESP_ERR_INVALID_CRC;
    }

    s_stats.ok++;
    if (out) {
        memcpy(out->raw, data, LIN_RAIN_SENSOR_DATA_LEN);
        out->rain_intensity  = data[0] & 0x0F;
        out->light_intensity = data[1];
        out->status          = data[2];
        out->valid           = true;
        out->timestamp_ms    = lin_sensor_now_ms();

#if DEBUG_LIN_SENSOR
        ESP_LOGI(TAG, "Rain=%-2d  Light=%-3d  Status=0x%02X  raw: "
                 "%02X %02X %02X %02X %02X %02X %02X",
                 out->rain_intensity, out->light_intensity, out->status,
                 data[0], data[1], data[2], data[3], data[4], data[5], data[6]);
#endif
    }
    return ESP_OK;
}

void lin_sensor_task(void *arg)
{
    ESP_LOGI(TAG, "LIN sensor task started  poll_interval=%d ms", LIN_POLL_INTERVAL_MS);

    lin_rain_sensor_data_t local = { 0 };
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        if (lin_sensor_poll(&local) == ESP_OK) {
            portENTER_CRITICAL(&s_mux);
            s_latest = local;
            portEXIT_CRITICAL(&s_mux);
        }
        /* Fixed-rate polling regardless of how long the transaction took. */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(LIN_POLL_INTERVAL_MS));
    }
}

void lin_sensor_get_latest(lin_rain_sensor_data_t *out)
{
    if (out == NULL) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_latest;
    portEXIT_CRITICAL(&s_mux);
}

void lin_sensor_get_stats(lin_sensor_stats_t *out)
{
    if (out == NULL) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_stats;
    portEXIT_CRITICAL(&s_mux);
}
