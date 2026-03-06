/**
 * @file main.c
 * @brief ESP32-P4 rain-sensor-driven wiper controller – entry point.
 *
 * System overview
 * ───────────────
 *
 *   ┌──────────────┐     LIN (19200 baud)      ┌──────────────────────┐
 *   │  Rain/light  │◄────────────────────────► │  ESP32-P4  (master)  │
 *   │   sensor     │     UART1 + transceiver   │                      │
 *   └──────────────┘                           │                      │
 *                                              │  ┌────────────────┐  │
 *   ┌──────────────┐      CAN0 (1 mbps)        │  │  wiper_logic   │  │
 *   │   Car ECU    │◄────────────────────────► │  │  (intercept +  │  │
 *   └──────────────┘                           │  │   modify)      │  │
 *                                              │  └────────────────┘  │
 *   ┌──────────────┐      CAN1 (1 mbps)        │                      │
 *   │   Wiper      │◄────────────────────────► │                      │
 *   │  actuator    │                           └──────────────────────┘
 *   └──────────────┘
 *
 * Task structure
 * ──────────────
 *   lin_sensor_task          – polls sensor at LIN_POLL_INTERVAL_MS
 *   can_gateway_can0_rx_task – RX from car, optionally modifies, enqueues for TX
 *   can_gateway_can1_tx_task – TX to wiper actuator
 *   can_gateway_can1_rx_task – RX from wiper actuator, pass-through to car
 *   can_gateway_can0_tx_task – TX back to car
 *   wiper_logic_task         – optional background / timer logic
 *   health_task              – periodic system status log
 *
 * Fault tolerance
 * ───────────────
 *   LIN sensor is OPTIONAL. If the sensor is absent or unresponsive,
 *   lin_sensor_get_latest() returns valid=false and the CAN gateway
 *   forwards all frames unmodified. The car's wiper stalk continues
 *   to work normally.
 *
 *   CAN passthrough is ALWAYS active, independent of LIN health.
 */

#include "config.h"
#include "lin_sensor.h"
#include "can_gateway.h"
#include "wiper_logic.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "MAIN";

/* ─── Health / watchdog task ─────────────────────────────────────────────── */

/**
 * @brief  Periodically logs system state so you can confirm what's running.
 *
 * Prints every 5 seconds:
 *   - LIN sensor validity and last rain intensity
 *   - A reminder that CAN passthrough is always active
 */
static void health_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        lin_rain_sensor_data_t sensor;
        lin_sensor_get_latest(&sensor);

        if (sensor.valid) {
            ESP_LOGI(TAG, "[HEALTH] LIN ✓  rain=%d  light=%d  status=0x%02X",
                     sensor.rain_intensity,
                     sensor.light_intensity,
                     sensor.status);
        } else {
            ESP_LOGW(TAG, "[HEALTH] LIN ✗  sensor absent or unresponsive – "
                          "CAN passthrough active, wiper stalk works normally");
        }
    }
}

/* ─── app_main ───────────────────────────────────────────────────────────── */

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_VERBOSE);

    ESP_LOGI(TAG, "╔══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  Rain-Sensor Wiper Controller  v0.1  ║");
    ESP_LOGI(TAG, "║  ESP32-P4  –  ESP-IDF                ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════╝");

    /* ── 1. Initialise subsystems ────────────────────────────────────── */

    /*
     * LIN sensor: non-fatal. A missing transceiver or disconnected sensor
     * just means the sensor task will keep logging timeouts and sensor.valid
     * stays false. CAN passthrough is unaffected.
     */
    esp_err_t lin_ret = lin_sensor_init();
    if (lin_ret != ESP_OK) {
        ESP_LOGW(TAG, "LIN sensor init failed (%s) – running in CAN-passthrough-only mode",
                 esp_err_to_name(lin_ret));
    } else {
        ESP_LOGI(TAG, "LIN sensor initialised");
    }

    /*
     * CAN gateway: fatal. Without CAN we can't do anything useful.
     */
    ESP_ERROR_CHECK(can_gateway_init());
    ESP_LOGI(TAG, "CAN gateway initialised");

    ESP_ERROR_CHECK(wiper_logic_init());
    ESP_LOGI(TAG, "Wiper logic initialised");

    ESP_LOGI(TAG, "──────────────────────────────────────");
    ESP_LOGI(TAG, "  CAN passthrough: ALWAYS ACTIVE");
    ESP_LOGI(TAG, "  LIN sensor init:      %s", lin_ret == ESP_OK ? "OK" : "ERROR");
    ESP_LOGI(TAG, "──────────────────────────────────────");

    /* ── 2. Spawn tasks ──────────────────────────────────────────────── */

    /* Core 0 ──────────────────────────────────────────────────────────── */

    xTaskCreatePinnedToCore(
        lin_sensor_task,
        "lin_sensor",
        TASK_STACK_LIN_POLL,
        NULL,
        TASK_PRIORITY_LIN_POLL,
        NULL,
        0
    );

    xTaskCreatePinnedToCore(
        wiper_logic_task,
        "wiper_logic",
        TASK_STACK_WIPER_LOGIC,
        NULL,
        TASK_PRIORITY_WIPER_LOGIC,
        NULL,
        0
    );

    xTaskCreatePinnedToCore(
        health_task,
        "health",
        2048,
        NULL,
        1,   /* lowest priority */
        NULL,
        0
    );

    /* Core 1 ──────────────────────────────────────────────────────────── */

    xTaskCreatePinnedToCore(
        can_gateway_can0_rx_task,
        "can0_rx",
        TASK_STACK_CAN_RX,
        NULL,
        TASK_PRIORITY_CAN_RX,
        NULL,
        1
    );

    xTaskCreatePinnedToCore(
        can_gateway_can1_tx_task,
        "can1_tx",
        TASK_STACK_CAN_TX,
        NULL,
        TASK_PRIORITY_CAN_TX,
        NULL,
        1
    );

    xTaskCreatePinnedToCore(
        can_gateway_can1_rx_task,
        "can1_rx",
        TASK_STACK_CAN_RX,
        NULL,
        TASK_PRIORITY_CAN_RX,
        NULL,
        1
    );

    xTaskCreatePinnedToCore(
        can_gateway_can0_tx_task,
        "can0_tx",
        TASK_STACK_CAN_TX,
        NULL,
        TASK_PRIORITY_CAN_TX,
        NULL,
        1
    );

    ESP_LOGI(TAG, "All tasks started – system running");
}