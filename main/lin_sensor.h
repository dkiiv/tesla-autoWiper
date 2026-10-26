/**
 * @file lin_sensor.h
 * @brief LIN bus master driver for the VAG rain/light sensor.
 *
 * This module is the LIN master.  It periodically sends a header (break +
 * sync + PID) and reads back the sensor's response payload.
 *
 * The ESP32 UART peripheral cannot natively generate a LIN break field
 * (13+ dominant bits).  The workaround used here - taken from the mestrode
 * Lin-Interface-Library - is to halve the baud rate before sending a single
 * 0x00 byte.  At half speed, that one byte occupies 20 bit-periods at the
 * nominal rate, which satisfies the ≥13-bit break requirement.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ─── Sensor data structure ─────────────────────────────────────────────── */

/**
 * Parsed output from one poll of the rain/light sensor.
 * Raw byte layout (VAG Bosch/Continental sensor, LIN 2.x) — from community
 * reverse-engineering, UNVERIFIED against our sensor:
 *
 *   Byte 0  - Rain intensity   (lower nibble, 0 = dry … 15 = heavy rain)
 *   Byte 1  - Light intensity  (0 = bright … 255 = very dark)
 *   Byte 2  - Sensor status flags
 *   Byte 3-6 - Solar / IR / additional light channels
 *   (Byte 7)  - LIN enhanced checksum  [consumed internally, not stored here]
 *
 * Use DEBUG_LIN_SENSOR=1 in config.h to log raw bytes on the bench.
 */
typedef struct {
    uint8_t  rain_intensity;    /**< 0-15, where 0 = no rain               */
    uint8_t  light_intensity;   /**< 0-255, ambient brightness             */
    uint8_t  status;            /**< sensor status / fault flags           */
    uint8_t  raw[7];            /**< full raw payload for debugging        */
    bool     valid;             /**< true once at least one poll succeeded */
    uint32_t timestamp_ms;      /**< when this reading was taken (see lin_sensor_now_ms) */
} lin_rain_sensor_data_t;

/** Poll statistics for the health log. */
typedef struct {
    uint32_t ok;
    uint32_t timeout;
    uint32_t bad_checksum;
    uint32_t bad_echo;
} lin_sensor_stats_t;

/* ─── Public API ────────────────────────────────────────────────────────── */

/** @brief  Initialise the UART used as LIN master. Non-fatal on failure. */
esp_err_t lin_sensor_init(void);

/**
 * @brief  Send a LIN header and read the sensor's response (blocks ~10 ms).
 * @return ESP_OK, ESP_ERR_TIMEOUT, ESP_ERR_INVALID_CRC, or
 *         ESP_ERR_INVALID_RESPONSE (echoed header didn't match what we sent).
 */
esp_err_t lin_sensor_poll(lin_rain_sensor_data_t *out);

/** @brief  FreeRTOS task: polls at LIN_POLL_INTERVAL_MS. Spawn with xTaskCreate. */
void lin_sensor_task(void *arg);

/**
 * @brief  Thread-safe copy of the latest GOOD reading.
 *
 * The reading is kept even after the sensor goes silent — callers must
 * check its age (lin_sensor_now_ms() - timestamp_ms) against LIN_STALE_MS.
 */
void lin_sensor_get_latest(lin_rain_sensor_data_t *out);

/** @brief  Copy of the poll counters. */
void lin_sensor_get_stats(lin_sensor_stats_t *out);

/** @brief  Monotonic milliseconds since boot (same clock as timestamp_ms). */
uint32_t lin_sensor_now_ms(void);
