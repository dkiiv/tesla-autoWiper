/**
 * @file wiper_logic.c
 * @brief Glue: sensor reading + policy decision → in-place 0x3E9 rewrite.
 */

#include "wiper_logic.h"
#include "wiper_policy.h"
#include "protocol.h"
#include "lin_sensor.h"
#include "config.h"

#include "freertos/FreeRTOS.h"
#include "esp_log.h"

static const char *TAG = "WIPER";

/* Compile-time guards: config.h and protocol.h must agree. */
_Static_assert(WIPER_CAN_MSG_ID == TESLA_BODYCTRL_CAN_ID, "WIPER_CAN_MSG_ID must be 0x3E9");
_Static_assert(WIPER_CAN_MSG_LEN == TESLA_BODYCTRL_LEN,   "DAS_bodyControls is 8 bytes");

static wiper_policy_t        s_policy;
static wiper_logic_stats_t   s_stats;
static portMUX_TYPE          s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t               s_last_logged_speed = 0xFF;

esp_err_t wiper_logic_init(void)
{
    const wiper_policy_config_t cfg = {
        .rain_to_speed = WIPER_RAIN_TO_SPEED_MAP,
        .fall_hold_ms  = WIPER_FALL_HOLD_MS,
        .stale_ms      = LIN_STALE_MS,
    };
    wiper_policy_init(&s_policy, &cfg);
    ESP_LOGI(TAG, "Wiper policy ready  fall_hold=%dms  stale=%dms",
             WIPER_FALL_HOLD_MS, LIN_STALE_MS);
    return ESP_OK;
}

void wiper_logic_process_can_frame(twai_message_t *msg)
{
    if (msg->identifier != WIPER_CAN_MSG_ID || msg->extd || msg->rtr) {
        return;
    }
    if (msg->data_length_code != WIPER_CAN_MSG_LEN) {
        portENTER_CRITICAL(&s_mux);
        s_stats.bad_dlc++;
        portEXIT_CRITICAL(&s_mux);
        return;   /* never rewrite a frame we don't fully understand */
    }

    lin_rain_sensor_data_t sensor;
    lin_sensor_get_latest(&sensor);

    uint32_t now = lin_sensor_now_ms();
    uint32_t age = sensor.valid ? (now - sensor.timestamp_ms) : UINT32_MAX;
    uint8_t  oem = tesla_wiper_speed_get(msg->data);

    wiper_decision_t d = wiper_policy_step(&s_policy, now, sensor.valid, age,
                                           sensor.rain_intensity, oem);
    if (d.modify) {
        tesla_wiper_speed_set(msg->data, d.speed);
    }
    uint8_t out = tesla_wiper_speed_get(msg->data);

    portENTER_CRITICAL(&s_mux);
    s_stats.frames_seen++;
    if (d.modify) s_stats.frames_modified++;
    s_stats.last_oem_speed = oem;
    s_stats.last_out_speed = out;
    portEXIT_CRITICAL(&s_mux);

    /* Log transitions only — 0x3E9 arrives many times per second. */
    if (out != s_last_logged_speed) {
        ESP_LOGI(TAG, "Wiper speed → %d  (oem=%d rain=%d %s)",
                 out, oem, sensor.rain_intensity, wiper_decision_reason_str(d.reason));
        s_last_logged_speed = out;
    }
#if DEBUG_WIPER_POLICY
    ESP_LOGI(TAG, "oem=%d rain=%d age=%lu → out=%d %s", oem, sensor.rain_intensity,
             (unsigned long)age, out, wiper_decision_reason_str(d.reason));
#endif
}

void wiper_logic_get_stats(wiper_logic_stats_t *out)
{
    if (out == NULL) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_stats;
    portEXIT_CRITICAL(&s_mux);
}
