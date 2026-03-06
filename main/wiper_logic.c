/**
 * @file wiper_logic.c
 * @brief Rain-sensor-driven wiper control – implementation.
 */

#include "wiper_logic.h"
#include "lin_sensor.h"
#include "config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "WIPER";

/* ─── Internal state ────────────────────────────────────────────────────── */

static uint8_t s_last_wiper_speed = 0;   /* 0–15, last value written to bus */

/* ─── CAN frame encoding ────────────────────────────────────────────────── */

/**
 * @brief  Compute the Tesla CAN checksum for DAS_bodyControls.
 *
 * DAS_bodyControlsChecksum start_bit = 56 → checksum_byte = 56 / 8 = 7
 * CAN ID 0x3E9 → seed = (0xE9) + (0x03) = 0xEC
 */
static uint8_t compute_checksum(const uint8_t *data)
{
    const uint16_t address       = WIPER_CAN_MSG_ID;
    const int      checksum_byte = 7;   /* DAS_bodyControlsChecksum start_bit 56 / 8 */
    const int      msg_len       = 8;

    uint8_t sum = (uint8_t)(address & 0xFF) + (uint8_t)((address >> 8) & 0xFF);
    for (int i = 0; i < msg_len; i++) {
        if (i != checksum_byte) {
            sum += data[i];
        }
    }
    return sum & 0xFF;
}

/**
 * @brief  Write wiper speed, rolling counter, and checksum into the CAN frame.
 *
 * Signal layout from DBC:
 *
 *   DAS_wiperSpeed         : 4|4@1+   → bits [7:4] of byte 0  (upper nibble)
 *   DAS_bodyControlsCounter: 52|4@1+  → bits [7:4] of byte 6  (upper nibble)
 *   DAS_bodyControlsChecksum: 56|8@1+ → byte 7 (full byte)
 *
 * All signals are little-endian (Intel) unsigned.
 *
 * @param[in,out] data   Pointer to msg->data[0].
 * @param[in]     speed  Target wiper speed, 0–15.
 */
static void encode_wiper_speed(uint8_t *data, uint8_t speed)
{
    // DAS_wiperSpeed: start bit 4, length 4 → upper nibble of byte 0
    data[0] = (data[0] & 0x0F) | ((speed & 0x0F) << 4);

    // DAS_bodyControlsChecksum: start bit 56, length 8 → byte 7
    data[7] = compute_checksum(data);

    ESP_LOGD(TAG, "CAN frame encoded: speed=%d checksum=0x%02X",
             speed, data[7]);
}

/* ─── Public API ────────────────────────────────────────────────────────── */

esp_err_t wiper_logic_init(void)
{
    s_last_wiper_speed = 0;
    ESP_LOGI(TAG, "Wiper logic initialised");
    return ESP_OK;
}

void wiper_logic_process_can_frame(twai_message_t *msg)
{
    /* ── Read latest LIN sensor data ────────────────────────────────── */
    lin_rain_sensor_data_t sensor;
    lin_sensor_get_latest(&sensor);

    if (!sensor.valid) {
        // TODO: add other criteria which pass through OEM wiper signal
        ESP_LOGD(TAG, "No valid LIN data, fwd'ing original");
        return;
    }

    if (sensor.rain_intensity != s_last_wiper_speed) {
        ESP_LOGI(TAG, "Wiper speed %d → %d  (rain_intensity=%d)",
                 s_last_wiper_speed, sensor.rain_intensity, sensor.rain_intensity);
        s_last_wiper_speed = sensor.rain_intensity;
    }

    /* ── Overwrite speed and checksum in the CAN frame ────── */
    encode_wiper_speed(msg->data, sensor.rain_intensity);
}

void wiper_logic_task(void *arg)
{
    /*
     * Background task – available for time-based logic such as:
     *   - Hold wipers on for N seconds after rain stops
     *   - Gradually ramp speed down instead of cutting off abruptly
     *   - Inhibit wipers during a car-wash signal on a separate GPIO
     */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}