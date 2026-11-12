/**
 * @file can_gateway.c
 * @brief Bidirectional CAN MITM gateway — see can_gateway.h for the task layout.
 */

#include "can_gateway.h"
#include "config.h"
#include "protocol.h"
#include "wiper_logic.h"
#include "lin_sensor.h"

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
/*
 * How long the alert task waits on each controller's alerts.  One loop is
 * 2 × this (~100 ms) between watchdog feeds — far under the 3 s timeout.
 */
#define ALERT_WAIT_MS      50

/* ─── State ─────────────────────────────────────────────────────────────── */

typedef struct {
    const char     *name;         /* "car→act" / "act→car" for logs     */
    twai_handle_t   src;
    twai_handle_t   dst;
    bool            apply_wiper;  /* only the car→actuator direction     */
    int             ctrl;         /* source controller number (0 / 1)    */
    can_dir_stats_t stats;
    can_sniff_stats_t sniff;      /* sniffer build only                  */
} fwd_dir_t;

static twai_handle_t s_can_car = NULL;   /* TWAI0 */
static twai_handle_t s_can_act = NULL;   /* TWAI1 */

static fwd_dir_t s_dir[2];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

#define STAT_INC(dir, field)  do { portENTER_CRITICAL(&s_mux); (dir)->stats.field++; portEXIT_CRITICAL(&s_mux); } while (0)

/* ─── Debug helper ──────────────────────────────────────────────────────── */

#if DEBUG_CAN_GATEWAY || CAN_LISTEN_ONLY
static void format_frame(char *hex, size_t n, const twai_message_t *msg)
{
    hex[0] = '\0';
    for (int i = 0; i < msg->data_length_code && i < 8 && (size_t)(i * 3 + 4) <= n; i++) {
        snprintf(hex + i * 3, 4, "%02X ", msg->data[i]);
    }
}
#endif

#if DEBUG_CAN_GATEWAY
static void log_frame(const char *label, const twai_message_t *msg)
{
    char hex[3 * 8 + 1];
    format_frame(hex, sizeof(hex), msg);
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
    const twai_mode_t mode = CAN_LISTEN_ONLY ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL;
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(tx_pin, rx_pin, mode);
    g.controller_id  = controller_id;
    g.rx_queue_len   = CAN_RX_QUEUE_LEN;
    g.tx_queue_len   = CAN_LISTEN_ONLY ? 0 : CAN_TX_QUEUE_LEN;
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
    ESP_LOGI(TAG, "TWAI%d ready  %s  TX=GPIO%d  RX=GPIO%d", controller_id,
             CAN_LISTEN_ONLY ? "LISTEN-ONLY" : "normal", tx_pin, rx_pin);
    return ESP_OK;
}

esp_err_t can_gateway_init(void)
{
    ESP_LOGI(TAG, "CAN bitrate %d kbit/s (both controllers)", CAN_BITRATE_KBPS);
    esp_err_t ret = init_twai_controller(0, CAN_CAR_TX_PIN, CAN_CAR_RX_PIN, &s_can_car);
    if (ret != ESP_OK) return ret;
    ret = init_twai_controller(1, CAN_WIPER_TX_PIN, CAN_WIPER_RX_PIN, &s_can_act);
    if (ret != ESP_OK) return ret;

    s_dir[0] = (fwd_dir_t){ .name = "car→act", .src = s_can_car, .dst = s_can_act,
                            .apply_wiper = true,  .ctrl = 0 };
    s_dir[1] = (fwd_dir_t){ .name = "act→car", .src = s_can_act, .dst = s_can_car,
                            .apply_wiper = false, .ctrl = 1 };
    return ESP_OK;
}

/* ─── Forwarding task (one instance per direction) ─────────────────────── */

#if !CAN_LISTEN_ONLY
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
#endif

/* ─── Sniffer task (listen-only build, one per controller) ─────────────── */

#if CAN_LISTEN_ONLY
/*
 * Nothing is transmitted or forwarded, so logging from here can't hurt the
 * vehicle; at worst our own RX queue overflows, which shows up as `miss`.
 */
static void can_sniff_task(void *arg)
{
    fwd_dir_t *dir = (fwd_dir_t *)arg;
    twai_message_t msg;
    bool     have_prev  = false;
    uint8_t  prev_ctr   = 0;
    uint8_t  prev_speed = 0xFF;
    uint32_t prev_ms    = 0;

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    ESP_LOGI(TAG, "Sniffer on TWAI%d started (listen-only, nothing forwarded)", dir->ctrl);

    while (1) {
        esp_task_wdt_reset();
        if (twai_receive_v2(dir->src, &msg, pdMS_TO_TICKS(FWD_RX_WAIT_MS)) != ESP_OK) {
            continue;
        }
        STAT_INC(dir, rx);
        log_frame(dir->name, &msg);

        if (msg.identifier != WIPER_CAN_MSG_ID || msg.extd || msg.rtr) {
            continue;
        }

        uint32_t now = lin_sensor_now_ms();
        portENTER_CRITICAL(&s_mux);
        can_sniff_stats_t *s = &dir->sniff;
        s->bodyctrl_seen++;
        if (msg.data_length_code != WIPER_CAN_MSG_LEN) {
            s->bad_dlc++;
            portEXIT_CRITICAL(&s_mux);
            continue;
        }
        bool    cs_ok = tesla_bodyctrl_checksum_ok(msg.data);
        uint8_t ctr   = tesla_bodyctrl_counter_get(msg.data);
        uint8_t speed = tesla_wiper_speed_get(msg.data);
        if (!cs_ok) s->bad_checksum++;
        if (have_prev) {
            if (ctr != (uint8_t)((prev_ctr + 1) & 0x0F)) s->counter_skips++;
            uint32_t dt = now - prev_ms;
            if (s->min_interval_ms == 0 || dt < s->min_interval_ms) s->min_interval_ms = dt;
            if (dt > s->max_interval_ms) s->max_interval_ms = dt;
        }
        s->last_speed = speed;
        portEXIT_CRITICAL(&s_mux);

        have_prev = true;
        prev_ctr  = ctr;
        prev_ms   = now;

        if (speed != prev_speed || !cs_ok) {
            char hex[3 * 8 + 1];
            format_frame(hex, sizeof(hex), &msg);
            ESP_LOGI(TAG, "TWAI%d 0x3E9 [%s] wiperSpeed=%d counter=%d checksum=%s",
                     dir->ctrl, hex, speed, ctr, cs_ok ? "OK" : "BAD");
            prev_speed = speed;
        }
    }
}
#endif

/* ─── Alert / bus-off recovery task ────────────────────────────────────── */

/*
 * Bus-off is the CAN controller's self-defence after too many errors: it
 * stops participating entirely.  For a MITM that means one direction is
 * dead until we recover, so this task restarts controllers automatically.
 */
static void handle_alerts(fwd_dir_t *dir, int ctrl)
{
    uint32_t alerts = 0;
    if (twai_read_alerts_v2(dir->src, &alerts, pdMS_TO_TICKS(ALERT_WAIT_MS)) != ESP_OK || alerts == 0) {
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

/*
 * Watchdog-registered like the forwarding tasks: if this task hangs, bus-off
 * recovery silently stops and one direction stays dead, so a hang must
 * reboot the chip instead.
 */
static void can_alert_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    while (1) {
        esp_task_wdt_reset();
        handle_alerts(&s_dir[0], 0);
        handle_alerts(&s_dir[1], 1);
    }
}

/* ─── Public ────────────────────────────────────────────────────────────── */

esp_err_t can_gateway_start(void)
{
#if CAN_LISTEN_ONLY
    TaskFunction_t rx_task = can_sniff_task;
    const char *names[2]   = { "can_sniff0", "can_sniff1" };
#else
    TaskFunction_t rx_task = can_fwd_task;
    const char *names[2]   = { "can_car2act", "can_act2car" };
#endif
    if (xTaskCreate(rx_task, names[0], TASK_STACK_CAN_FWD, &s_dir[0],
                    TASK_PRIORITY_CAN_FWD, NULL) != pdPASS ||
        xTaskCreate(rx_task, names[1], TASK_STACK_CAN_FWD, &s_dir[1],
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
    out->sniff[0]   = s_dir[0].sniff;
    out->sniff[1]   = s_dir[1].sniff;
    portEXIT_CRITICAL(&s_mux);

    out->car_to_act.rx_missed  = si0.rx_missed_count;
    out->car_to_act.rx_overrun = si0.rx_overrun_count;
    out->act_to_car.rx_missed  = si1.rx_missed_count;
    out->act_to_car.rx_overrun = si1.rx_overrun_count;
}
