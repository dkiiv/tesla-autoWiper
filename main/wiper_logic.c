/**
 * @file wiper_logic.c
 * @brief Glue: sensor reading + policy decision → in-place 0x3E9 rewrite.
 *
 * Logging
 * ───────
 * wiper_logic_process_can_frame() runs inside the highest-priority CAN
 * forwarding task, and console output blocks (a ~80-char line is several ms
 * at 115200 baud).  So nothing here logs from that path: it posts a small
 * event to a queue with zero wait, and wiper_logic_log_task (low priority)
 * prints it.  If the queue is full the event is dropped and counted.
 */

#include "wiper_logic.h"
#include "wiper_policy.h"
#include "protocol.h"
#include "lin_sensor.h"
#include "config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "WIPER";

/* Compile-time guards: config.h and protocol.h must agree. */
_Static_assert(WIPER_CAN_MSG_ID == TESLA_BODYCTRL_CAN_ID, "WIPER_CAN_MSG_ID must be 0x3E9");
_Static_assert(WIPER_CAN_MSG_LEN == TESLA_BODYCTRL_LEN,   "DAS_bodyControls is 8 bytes");

static wiper_policy_t        s_policy;
static wiper_logic_stats_t   s_stats;
static portMUX_TYPE          s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t               s_last_logged_speed = 0xFF;
static QueueHandle_t         s_log_q = NULL;

#define LOG_QUEUE_LEN 16

typedef struct {
    uint8_t                 out;
    uint8_t                 oem;
    uint8_t                 rain;
    uint32_t                age_ms;
    wiper_decision_reason_t reason;
    bool                    transition;   /* false = DEBUG_WIPER_POLICY line */
} wiper_log_event_t;

esp_err_t wiper_logic_init(void)
{
    const wiper_policy_config_t cfg = {
        .rain_to_speed = WIPER_RAIN_TO_SPEED_MAP,
        .fall_hold_ms  = WIPER_FALL_HOLD_MS,
        .rise_hold_ms  = WIPER_RISE_HOLD_MS,
        .warmup_ms     = WIPER_WARMUP_MS,
        .stale_ms      = LIN_STALE_MS,
        .status_fault_mask = LIN_SENSOR_FAULT_MASK,
        .raise_only    = !WIPER_ALLOW_OVERRIDE_DOWN,
    };
    wiper_policy_init(&s_policy, &cfg);

    s_log_q = xQueueCreate(LOG_QUEUE_LEN, sizeof(wiper_log_event_t));
    if (s_log_q == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Wiper policy ready  rise_hold=%dms fall_hold=%dms warmup=%dms stale=%dms  "
             "mode=%s  fault_mask=0x%02X",
             WIPER_RISE_HOLD_MS, WIPER_FALL_HOLD_MS, WIPER_WARMUP_MS, LIN_STALE_MS,
             WIPER_ALLOW_OVERRIDE_DOWN ? "authoritative" : "raise-only",
             LIN_SENSOR_FAULT_MASK);
    return ESP_OK;
}

static void post_log(const wiper_log_event_t *ev)
{
    if (xQueueSend(s_log_q, ev, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_mux);
        s_stats.log_dropped++;
        portEXIT_CRITICAL(&s_mux);
    }
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
                                           sensor.status, sensor.rain_intensity, oem);
    if (d.modify) {
        tesla_wiper_speed_set(msg->data, d.speed);
    }
    uint8_t out = tesla_wiper_speed_get(msg->data);

    portENTER_CRITICAL(&s_mux);
    s_stats.frames_seen++;
    if (d.modify) s_stats.frames_modified++;
    if (d.reason == WIPER_DECISION_PASSTHROUGH_SENSOR_FAULT) s_stats.sensor_fault++;
    s_stats.last_oem_speed = oem;
    s_stats.last_out_speed = out;
    s_stats.last_reason    = d.reason;
    portEXIT_CRITICAL(&s_mux);

    wiper_log_event_t ev = {
        .out = out, .oem = oem, .rain = sensor.rain_intensity,
        .age_ms = age, .reason = d.reason, .transition = true,
    };
    /* Transitions only — 0x3E9 arrives many times per second. */
    if (out != s_last_logged_speed) {
        s_last_logged_speed = out;
        post_log(&ev);
    }
#if DEBUG_WIPER_POLICY
    ev.transition = false;
    post_log(&ev);
#endif
}

void wiper_logic_log_task(void *arg)
{
    wiper_log_event_t ev;
    while (1) {
        if (xQueueReceive(s_log_q, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (ev.transition) {
            ESP_LOGI(TAG, "Wiper speed → %d  (oem=%d rain=%d %s)",
                     ev.out, ev.oem, ev.rain, wiper_decision_reason_str(ev.reason));
        } else {
            ESP_LOGI(TAG, "oem=%d rain=%d age=%lu → out=%d %s", ev.oem, ev.rain,
                     (unsigned long)ev.age_ms, ev.out, wiper_decision_reason_str(ev.reason));
        }
    }
}

void wiper_logic_get_stats(wiper_logic_stats_t *out)
{
    if (out == NULL) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_stats;
    portEXIT_CRITICAL(&s_mux);
}
