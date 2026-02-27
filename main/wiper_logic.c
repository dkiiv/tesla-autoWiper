/**
 * @file wiper_logic.c
 * @brief Rain-sensor-driven wiper control – implementation.
 *
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │                    *** YOUR WORK GOES HERE ***                      │
 * │                                                                     │
 * │  Both the LIN rain sensor and the car's wiper CAN message use a    │
 * │  0–15 scale:                                                        │
 * │    Rain intensity  0 = dry       …  15 = heavy rain                │
 * │    Wiper speed     0 = off       …  15 = maximum speed             │
 * │                                                                     │
 * │  Steps to complete this module:                                     │
 * │                                                                     │
 * │  1. Sniff the wiper CAN frame while operating the stalk.            │
 * │     Identify which byte(s) carry the 0–15 speed value.             │
 * │                                                                     │
 * │  2. Fill in encode_wiper_speed() below with the correct byte        │
 * │     offset(s).                                                      │
 * │                                                                     │
 * │  3. Implement rain_intensity_to_wiper_speed() with whatever         │
 * │     mapping logic suits you (direct, thresholded, hysteresis…).    │
 * └─────────────────────────────────────────────────────────────────────┘
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

/* ─── Rain intensity → wiper speed ─────────────────────────────────────── */

/**
 * @brief  Map a 0–15 rain intensity to a 0–15 wiper speed.
 *
 * *** IMPLEMENT YOUR MAPPING LOGIC HERE ***
 *
 * The simplest possible starting point is a direct pass-through:
 *
 *   return rain_intensity;
 *
 * From there you can add thresholds, hysteresis, or any other logic.
 * The wiper_logic_task() below is available for time-based effects.
 */
static uint8_t rain_intensity_to_wiper_speed(uint8_t rain_intensity)
{
    /* TODO: replace with your mapping */
    return rain_intensity;
}

/* ─── CAN frame encoding ────────────────────────────────────────────────── */

/**
 * @brief  Write a 0–15 wiper speed into the correct byte(s) of a CAN frame.
 *
 * *** FILL IN THE BYTE OFFSET(S) FOR YOUR CAR ***
 *
 * Example – if the speed lives in the lower nibble of byte 0:
 *
 *   data[0] = (data[0] & 0xF0) | (speed & 0x0F);
 *
 * @param[in,out] data   Pointer to msg->data[0].
 * @param[in]     speed  Target wiper speed, 0–15.
 */
static void encode_wiper_speed(uint8_t *data, uint8_t speed)
{
    /* TODO: write speed into the correct byte position */
    (void)data;
    (void)speed;
    ESP_LOGW(TAG, "encode_wiper_speed() not yet implemented");
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
    /* Only act on the wiper control frame; pass everything else through */
    if (msg->identifier != WIPER_CAN_MSG_ID) {
        return;
    }

    /* ── Read latest LIN sensor data ────────────────────────────────── */
    lin_rain_sensor_data_t sensor;
    lin_sensor_get_latest(&sensor);

    if (!sensor.valid) {
        /* No valid sensor reading – let the original frame through so the
           car's own wiper stalk continues to work normally. */
        ESP_LOGD(TAG, "No valid LIN data – forwarding original wiper frame");
        return;
    }

    /* ── Compute desired wiper speed ────────────────────────────────── */
    uint8_t desired_speed = rain_intensity_to_wiper_speed(sensor.rain_intensity);

    if (desired_speed != s_last_wiper_speed) {
        ESP_LOGI(TAG, "Wiper speed %d → %d  (rain_intensity=%d)",
                 s_last_wiper_speed, desired_speed, sensor.rain_intensity);
        s_last_wiper_speed = desired_speed;
    }

    /* ── Overwrite speed byte(s) in the CAN frame ───────────────────── */
    encode_wiper_speed(msg->data, desired_speed);
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

