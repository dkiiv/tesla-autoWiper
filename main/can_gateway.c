/**
 * @file can_gateway.c
 * @brief Bidirectional CAN MITM gateway — see can_gateway.h for the task layout.
 */

#include "can_gateway.h"
#include "config.h"
#include "wiper_logic.h"

#include "driver/twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "CAN_GW";

/*
 * How long a forwarding task waits for a frame before looping to feed the
 * watchdog.  Must be well under CONFIG_ESP_TASK_WDT_TIMEOUT_S.
 */
#define FWD_RX_WAIT_MS     1000
/* How long we wait for space in the destination TX queue before dropping. */
#define FWD_TX_WAIT_MS     2

/* ─── State ─────────────────────────────────────────────────────────────── */

typedef struct {
    const char     *name;         /* "car→act" / "act→car" for logs     */
    twai_handle_t   src;
    twai_handle_t   dst;
    bool            apply_wiper;  /* only the car→actuator direction     */
    can_dir_stats_t stats;
} fwd_dir_t;

static twai_handle_t s_can_car = NULL;   /* TWAI0 */
static twai_handle_t s_can_act = NULL;   /* TWAI1 */

static fwd_dir_t s_dir[2];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

#define STAT_INC(dir, field)  do { portENTER_CRITICAL(&s_mux); (dir)->stats.field++; portEXIT_CRITICAL(&s_mux); } while (0)

/* ─── Debug helper ──────────────────────────────────────────────────────── */

#if DEBUG_CAN_GATEWAY
static void log_frame(const char *label, const twai_message_t *msg)
{
    char hex[3 * 8 + 1] = {0};
    for (int i = 0; i < msg->data_length_code && i < 8; i++) {
        snprintf(hex + i * 3, 4, "%02X ", msg->data[i]);
    }
    ESP_LOGI(TAG, "%-8s ID=0x%03lX DLC=%d [%s]", label,
             (unsigned long)msg->identifier, msg->data_length_code, hex);
}
#else
#define log_frame(label, msg)   ((void)0)
#endif

/* ─── Init ──────────────────────────────────────────────────────────────── */

static esp_err_t init_twai_controller(int controller_id, gpio_num_t tx_pin,
                                      gpio_num_t rx_pin, twai_handle_t *handle_out)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(tx_pin, rx_pin, TWAI_MODE_NORMAL);
    g.controller_id  = controller_id;
    g.rx_queue_len   = CAN_RX_QUEUE_LEN;
    g.tx_queue_len   = CAN_TX_QUEUE_LEN;
    g.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED |
                       TWAI_ALERT_ERR_PASS | TWAI_ALERT_ERR_ACTIVE |
                       TWAI_ALERT_RX_QUEUE_FULL | TWAI_ALERT_RX_FIFO_OVERRUN;

    twai_timing_config_t t = CAN_BAUD_RATE;
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t ret = twai_driver_install_v2(&g, &t, &f, handle_out);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TWAI%d install failed: %s", controller_id, esp_err_to_name(ret));
        return ret;
    }
    ret = twai_start_v2(*handle_out);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TWAI%d start failed: %s", controller_id, esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "TWAI%d ready  TX=GPIO%d  RX=GPIO%d", controller_id, tx_pin, rx_pin);
    return ESP_OK;
}

esp_err_t can_gateway_init(void)
{
    esp_err_t ret = init_twai_controller(0, CAN_CAR_TX_PIN, CAN_CAR_RX_PIN, &s_can_car);
    if (ret != ESP_OK) return ret;
    ret = init_twai_controller(1, CAN_WIPER_TX_PIN, CAN_WIPER_RX_PIN, &s_can_act);
    if (ret != ESP_OK) return ret;

    s_dir[0] = (fwd_dir_t){ .name = "car→act", .src = s_can_car, .dst = s_can_act, .apply_wiper = true  };
    s_dir[1] = (fwd_dir_t){ .name = "act→car", .src = s_can_act, .dst = s_can_car, .apply_wiper = false };
    return ESP_OK;
}

/* ─── Forwarding task (one instance per direction) ─────────────────────── */

static void can_fwd_task(void *arg)
{
    fwd_dir_t *dir = (fwd_dir_t *)arg;
    twai_message_t msg;

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    ESP_LOGI(TAG, "Forwarding task %s started", dir->name);

    while (1) {
        esp_task_wdt_reset();

        if (twai_receive_v2(dir->src, &msg, pdMS_TO_TICKS(FWD_RX_WAIT_MS)) != ESP_OK) {
            continue;   /* idle bus (car asleep / bench) — just feed the watchdog */
        }
        STAT_INC(dir, rx);

        if (dir->apply_wiper) {
            wiper_logic_process_can_frame(&msg);
        }
        log_frame(dir->name, &msg);

        if (twai_transmit_v2(dir->dst, &msg, pdMS_TO_TICKS(FWD_TX_WAIT_MS)) != ESP_OK) {
            /* Counted, not logged: logging here would make a congested bus worse. */
            STAT_INC(dir, tx_drop);
        }
    }
}

/* ─── Alert / bus-off recovery task ────────────────────────────────────── */

/*
 * Bus-off is the CAN controller's self-defence after too many errors: it
 * stops participating entirely.  For a MITM that means one direction is
 * dead until we recover, so this task restarts controllers automatically.
 */
static void handle_alerts(fwd_dir_t *dir, int ctrl)
{
    uint32_t alerts = 0;
    if (twai_read_alerts_v2(dir->src, &alerts, pdMS_TO_TICKS(50)) != ESP_OK || alerts == 0) {
        return;
    }
    if (alerts & TWAI_ALERT_ERR_PASS)        ESP_LOGW(TAG, "TWAI%d error-passive", ctrl);
    if (alerts & TWAI_ALERT_ERR_ACTIVE)      ESP_LOGI(TAG, "TWAI%d error-active again", ctrl);
    if (alerts & TWAI_ALERT_RX_QUEUE_FULL)   ESP_LOGW(TAG, "TWAI%d RX queue full - frames lost", ctrl);
    if (alerts & TWAI_ALERT_RX_FIFO_OVERRUN) ESP_LOGW(TAG, "TWAI%d RX FIFO overrun", ctrl);

    if (alerts & TWAI_ALERT_BUS_OFF) {
        STAT_INC(dir, bus_off);
        ESP_LOGE(TAG, "TWAI%d BUS-OFF - initiating recovery", ctrl);
        twai_initiate_recovery_v2(dir->src);
    }
    if (alerts & TWAI_ALERT_BUS_RECOVERED) {
        ESP_LOGW(TAG, "TWAI%d recovered - restarting", ctrl);
        twai_start_v2(dir->src);
    }
}

static void can_alert_task(void *arg)
{
    while (1) {
        handle_alerts(&s_dir[0], 0);
        handle_alerts(&s_dir[1], 1);
    }
}

/* ─── Public ────────────────────────────────────────────────────────────── */

esp_err_t can_gateway_start(void)
{
    if (xTaskCreate(can_fwd_task, "can_car2act", TASK_STACK_CAN_FWD, &s_dir[0],
                    TASK_PRIORITY_CAN_FWD, NULL) != pdPASS ||
        xTaskCreate(can_fwd_task, "can_act2car", TASK_STACK_CAN_FWD, &s_dir[1],
                    TASK_PRIORITY_CAN_FWD, NULL) != pdPASS ||
        xTaskCreate(can_alert_task, "can_alert", TASK_STACK_CAN_ALERT, NULL,
                    TASK_PRIORITY_CAN_ALERT, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void can_gateway_get_stats(can_gateway_stats_t *out)
{
    if (out == NULL) return;

    /* Driver-side loss counters come from status info, not our own tallies. */
    twai_status_info_t si0 = {0}, si1 = {0};
    twai_get_status_info_v2(s_can_car, &si0);
    twai_get_status_info_v2(s_can_act, &si1);

    portENTER_CRITICAL(&s_mux);
    out->car_to_act = s_dir[0].stats;
    out->act_to_car = s_dir[1].stats;
    portEXIT_CRITICAL(&s_mux);

    out->car_to_act.rx_missed  = si0.rx_missed_count;
    out->car_to_act.rx_overrun = si0.rx_overrun_count;
    out->act_to_car.rx_missed  = si1.rx_missed_count;
    out->act_to_car.rx_overrun = si1.rx_overrun_count;
}
