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
 *   can_gateway_car_rx_task  – RX from car, optionally modifies, enqueues for TX
 *   can_gateway_wiper_tx_task– TX to wiper actuator
 *   wiper_logic_task         – optional background / timer logic
 *
 * Getting started
 * ───────────────
 *  1. Edit config.h:  set pin numbers, CAN ID of your wiper message.
 *  2. Set DEBUG_LIN_SENSOR=1 and observe raw sensor bytes on the serial
 *     monitor.  Verify rain_intensity maps to byte 0 nibble as expected.
 *  3. Sniff CAN traffic while operating the wiper stalk; find WIPER_CAN_MSG_ID.
 *  4. Implement encode_wiper_command() in wiper_logic.c.
 *  5. Tune RAIN_INTENSITY_* thresholds in config.h.
 */

#include "config.h"
#include "lin_sensor.h"
#include "can_gateway.h"
#include "wiper_logic.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "MAIN";

/* ─── app_main ───────────────────────────────────────────────────────────── */

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_VERBOSE);  // set most verbose log level, for debug/POC
    ESP_LOGI(TAG, "╔══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  Rain-Sensor Wiper Controller  v0.1  ║");
    ESP_LOGI(TAG, "║  ESP32-P4  –  ESP-IDF                ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════╝");

    /* ── 1. Initialise subsystems ────────────────────────────────────── */

    ESP_ERROR_CHECK(lin_sensor_init());
    ESP_LOGI(TAG, "LIN sensor initialised");

    ESP_ERROR_CHECK(can_gateway_init());
    ESP_LOGI(TAG, "CAN gateway initialised");

    ESP_ERROR_CHECK(wiper_logic_init());
    ESP_LOGI(TAG, "Wiper logic initialised");

    /* ── 2. Spawn tasks ──────────────────────────────────────────────── */

    /* ── Core 0 ─────────────────────────────────────────────────────── */

    /*
     * LIN sensor task – Core 0
     * Polls the rain sensor at LIN_POLL_INTERVAL_MS and stores the result
     * in a mutex-protected shared variable read by wiper_logic.
     */
    xTaskCreatePinnedToCore(
        lin_sensor_task,
        "lin_sensor",
        TASK_STACK_LIN_POLL,
        NULL,
        TASK_PRIORITY_LIN_POLL,
        NULL,
        0
    );

    /*
     * Wiper logic background task – Core 0
     * Optional time-based logic: hysteresis, ramp-down, inhibit during wash.
     */
    xTaskCreatePinnedToCore(
        wiper_logic_task,
        "wiper_logic",
        TASK_STACK_WIPER_LOGIC,
        NULL,
        TASK_PRIORITY_WIPER_LOGIC,
        NULL,
        0
    );

    /* ── Core 1 ─────────────────────────────────────────────────────── */

    /*
     * CAN0 RX task – Core 1  (car side → wiper side)
     * Receives frames from the car, passes them through wiper_logic, and
     * enqueues them for CAN1 TX.
     */
    xTaskCreatePinnedToCore(
        can_gateway_can0_rx_task,
        "can0_rx",
        TASK_STACK_CAN_RX,
        NULL,
        TASK_PRIORITY_CAN_RX,
        NULL,
        1
    );

    /*
     * CAN1 TX task – Core 1  (car side → wiper side)
     * Drains the to_can1 queue onto the wiper actuator bus.
     */
    xTaskCreatePinnedToCore(
        can_gateway_can1_tx_task,
        "can1_tx",
        TASK_STACK_CAN_TX,
        NULL,
        TASK_PRIORITY_CAN_TX,
        NULL,
        1
    );

    /*
     * CAN1 RX task – Core 1  (wiper side → car side)
     * Receives frames from the wiper actuator and enqueues them for CAN0 TX.
     * No frame modification on this return path.
     */
    xTaskCreatePinnedToCore(
        can_gateway_can1_rx_task,
        "can1_rx",
        TASK_STACK_CAN_RX,
        NULL,
        TASK_PRIORITY_CAN_RX,
        NULL,
        1
    );

    /*
     * CAN0 TX task – Core 1  (wiper side → car side)
     * Drains the to_can0 queue back onto the car bus.
     */
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

    /*
     * app_main is itself a task and will be deleted when it returns.
     * Nothing more to do here; all work happens in the tasks above.
     */
}
