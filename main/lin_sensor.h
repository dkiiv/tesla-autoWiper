/**
 * @file lin_sensor.h
 * @brief LIN bus master driver for the VAG rain/light sensor.
 *
 * This module is the LIN master.  It periodically sends a header (break +
 * sync + PID) and reads back the sensor's response payload.
 *
 * The ESP32 UART peripheral cannot natively generate a LIN break field
 * (13+ dominant bits) because the idle-to-break transition hits an internal
 * error detector.  The workaround used here – taken from the mestrode
 * Lin-Interface-Library – is to halve the baud rate before sending a single
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
 * Raw byte layout (VAG Bosch/Continental sensor, LIN 2.x):
 *
 *   Byte 0  – Rain intensity   (4-bit field, 0 = dry … 15 = heavy rain)
 *   Byte 1  – Light intensity  (0 = bright … 255 = very dark)
 *   Byte 2  – Sensor status flags
 *   Byte 3  – Solar sensor / IR
 *   Byte 4-6 – Reserved / additional light channels
 *   (Byte 7)  – LIN enhanced checksum  [consumed internally, not stored here]
 *
 * NOTE: Byte positions may differ on your specific sensor variant.
 *       Use DEBUG_LIN_SENSOR=1 in config.h and log raw bytes first.
 */
typedef struct {
    uint8_t  rain_intensity;    /**< 0–15, where 0 = no rain               */
    uint8_t  light_intensity;   /**< 0–255, ambient brightness             */
    uint8_t  status;            /**< sensor status / fault flags           */
    uint8_t  raw[7];            /**< full raw payload for debugging        */
    bool     valid;             /**< true if last poll succeeded           */
} lin_rain_sensor_data_t;

/* ─── Public API ────────────────────────────────────────────────────────── */

/**
 * @brief  Initialise the UART peripheral used as a LIN master.
 * @return ESP_OK on success.
 */
esp_err_t lin_sensor_init(void);

/**
 * @brief  Send a LIN header and read the sensor's response.
 *
 * Blocks for the duration of the transaction (a few milliseconds).
 *
 * @param[out] out  Populated with sensor readings on success.
 * @return ESP_OK if the response was received and checksum verified.
 *         ESP_ERR_TIMEOUT if no response within the slot window.
 *         ESP_ERR_INVALID_CRC if checksum mismatch.
 */
esp_err_t lin_sensor_poll(lin_rain_sensor_data_t *out);

/**
 * @brief  FreeRTOS task function – polls the sensor at LIN_POLL_INTERVAL_MS
 *         and stores results in the shared sensor_data variable.
 *
 * Spawn this with xTaskCreate(); do not call directly.
 */
void lin_sensor_task(void *arg);

/**
 * @brief  Thread-safe accessor: copy the latest sensor reading.
 * @param[out] out  Destination for the sensor snapshot.
 */
void lin_sensor_get_latest(lin_rain_sensor_data_t *out);
