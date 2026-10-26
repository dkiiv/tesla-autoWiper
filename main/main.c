/**
 * @file main.c
 * @brief Rain-sensor-driven wiper controller — entry point.
 *
 * System overview
 * ───────────────
 *
 *   ┌──────────────┐     LIN (19200 baud)      ┌──────────────────────┐
 *   │  VAG rain/   │◄────────────────────────► │  ESP32-C6 (prod)     │
 *   │ light sensor │     UART1 + transceiver   │  ESP32-P4 (bench)    │
 *   └──────────────┘                           │                      │
 *                                              │  ┌────────────────┐  │
 *   ┌──────────────┐      CAN0 / TWAI0         │  │  wiper_logic + │  │
 *   │ AP computer  │◄────────────────────────► │  │  wiper_policy  │  │
 *   │ (car side)   │                           │  └────────────────┘  │
 *   └──────────────┘                           │                      │
 *   ┌──────────────┐      CAN1 / TWAI1         │                      │
 *   │ Body ctrl /  │◄────────────────────────► │                      │
 *   │ wiper side   │                           └──────────────────────┘
 *   └──────────────┘
 *
 * Tasks
 * ─────
 *   can_car2act / can_act2car  - one forwarding task per direction (can_gateway.c)
 *   can_alert                  - bus-off detection + recovery
 *   lin_sensor                 - polls the rain sensor at LIN_POLL_INTERVAL_MS
 *   health                     - one status line every HEALTH_LOG_INTERVAL_MS
 *
 * Fault tolerance
 * ───────────────
 *   The LIN sensor is OPTIONAL.  If it is absent, unresponsive or its last
 *   reading is older than LIN_STALE_MS, every frame is forwarded unmodified.
 *   CAN passthrough never depends on LIN health.
 */

#include "config.h"
#include "lin_sensor.h"
#include "can_gateway.h"
#include "wiper_logic.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "MAIN";

/* ─── Health task ─────────────────────────────────────────────────────── */

/* One compact line per interval — the first thing to read when debugging. */
static void health_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(HEALTH_LOG_INTERVAL_MS));

        lin_rain_sensor_data_t s;
        lin_sensor_stats_t     ls;
        can_gateway_stats_t    cs;
        wiper_logic_stats_t    ws;
        lin_sensor_get_latest(&s);
        lin_sensor_get_stats(&ls);
        can_gateway_get_stats(&cs);
        wiper_logic_get_stats(&ws);

        uint32_t age = s.valid ? lin_sensor_now_ms() - s.timestamp_ms : 0;
        bool fresh = s.valid && age <= LIN_STALE_MS;

        ESP_LOGI(TAG, "[HEALTH] LIN %s rain=%d light=%d age=%lums ok/to/cs/echo=%lu/%lu/%lu/%lu",
                 fresh ? "OK " : "-- ", s.rain_intensity, s.light_intensity,
                 (unsigned long)age, (unsigned long)ls.ok, (unsigned long)ls.timeout,
                 (unsigned long)ls.bad_checksum, (unsigned long)ls.bad_echo);
        ESP_LOGI(TAG, "[HEALTH] CAN car→act rx=%lu drop=%lu miss=%lu off=%lu | "
                 "act→car rx=%lu drop=%lu miss=%lu off=%lu",
                 (unsigned long)cs.car_to_act.rx, (unsigned long)cs.car_to_act.tx_drop,
                 (unsigned long)(cs.car_to_act.rx_missed + cs.car_to_act.rx_overrun),
                 (unsigned long)cs.car_to_act.bus_off,
                 (unsigned long)cs.act_to_car.rx, (unsigned long)cs.act_to_car.tx_drop,
                 (unsigned long)(cs.act_to_car.rx_missed + cs.act_to_car.rx_overrun),
                 (unsigned long)cs.act_to_car.bus_off);
        ESP_LOGI(TAG, "[HEALTH] WIPER 0x3E9 seen=%lu modified=%lu badDLC=%lu oem=%d out=%d",
                 (unsigned long)ws.frames_seen, (unsigned long)ws.frames_modified,
                 (unsigned long)ws.bad_dlc, ws.last_oem_speed, ws.last_out_speed);
    }
}

/* ─── app_main ───────────────────────────────────────────────────────────── */

void app_main(void)
{
    esp_log_level_set("*", APP_LOG_LEVEL);

    ESP_LOGI(TAG, "Rain-Sensor Wiper Controller  (%s, ESP-IDF %s)",
             CONFIG_IDF_TARGET, esp_get_idf_version());

    /* Order matters: wiper logic must be ready before CAN frames start flowing. */
    ESP_ERROR_CHECK(wiper_logic_init());

    /* LIN: non-fatal. No sensor = passthrough-only mode. */
    esp_err_t lin_ret = lin_sensor_init();
    if (lin_ret != ESP_OK) {
        ESP_LOGW(TAG, "LIN init failed (%s) - CAN passthrough only",
                 esp_err_to_name(lin_ret));
    }

    /* CAN: fatal. Without it we are just a break in the bus — reboot and retry. */
    ESP_ERROR_CHECK(can_gateway_init());
    ESP_ERROR_CHECK(can_gateway_start());

    if (lin_ret == ESP_OK) {
        xTaskCreate(lin_sensor_task, "lin_sensor", TASK_STACK_LIN_POLL, NULL,
                    TASK_PRIORITY_LIN_POLL, NULL);
    }
    xTaskCreate(health_task, "health", TASK_STACK_HEALTH, NULL, TASK_PRIORITY_HEALTH, NULL);

    ESP_LOGI(TAG, "Running  CAN passthrough=ON  LIN=%s", lin_ret == ESP_OK ? "ON" : "OFF");
}
