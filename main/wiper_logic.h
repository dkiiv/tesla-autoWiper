/**
 * @file wiper_logic.h
 * @brief Glue between the CAN gateway, the LIN sensor and the wiper policy.
 *
 * Flow
 * ────
 *  1. lin_sensor_task keeps a timestamped latest rain reading.
 *  2. The car→actuator forwarding task calls wiper_logic_process_can_frame()
 *     for every 0x3E9 DAS_bodyControls frame.
 *  3. This module reads the sensor, asks wiper_policy what speed to send,
 *     and — only if the policy says so — rewrites DAS_wiperSpeed and the
 *     checksum in place (protocol.h).  Otherwise the frame is untouched.
 *
 * The decision rules live in wiper_policy.h; the bit packing lives in
 * protocol.h.  Both are pure C and host-tested.
 */

#pragma once

#include "esp_err.h"
#include "driver/twai.h"
#include <stdint.h>

typedef struct {
    uint32_t frames_seen;      /**< 0x3E9 frames inspected               */
    uint32_t frames_modified;  /**< ... of which we rewrote              */
    uint32_t bad_dlc;          /**< 0x3E9 with unexpected length (untouched) */
    uint8_t  last_oem_speed;   /**< speed the car asked for, last frame  */
    uint8_t  last_out_speed;   /**< speed we put on the bus, last frame  */
} wiper_logic_stats_t;

/** @brief  Load the policy config from config.h and reset state. */
esp_err_t wiper_logic_init(void);

/**
 * @brief  Inspect / modify one car→actuator frame in place.
 *         Frames that aren't a full-length 0x3E9 are left alone.
 *         Only called from the single car→actuator task (not reentrant).
 */
void wiper_logic_process_can_frame(twai_message_t *msg);

/** @brief  Snapshot of counters for the health log. */
void wiper_logic_get_stats(wiper_logic_stats_t *out);
