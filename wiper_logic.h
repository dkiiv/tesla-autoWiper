/**
 * @file wiper_logic.h
 * @brief Rain-sensor-driven wiper control logic.
 *
 * Flow
 * ────
 *  1. lin_sensor_task()  populates a shared rain_intensity value (0–15).
 *  2. can_gateway_can0_rx_task() calls wiper_logic_process_can_frame()
 *     for every frame received from the car.
 *  3. wiper_logic_process_can_frame() reads the latest rain intensity and,
 *     if the incoming frame matches WIPER_CAN_MSG_ID, writes a new wiper
 *     speed value (0–15) into the relevant byte(s) of the frame.
 *  4. The modified frame is forwarded to the wiper actuator.
 *
 * Wiper speed scale
 * ─────────────────
 *  The car's wiper CAN message carries a 0–15 speed value:
 *    0  = off
 *    15 = maximum speed
 *
 *  The LIN rain sensor also returns a 0–15 rain intensity value:
 *    0  = dry
 *    15 = heavy rain
 *
 *  Your job in wiper_logic.c is to decide how to map one to the other.
 */

#pragma once

#include "esp_err.h"
#include "driver/twai.h"
#include <stdint.h>

/* ─── Public API ────────────────────────────────────────────────────────── */

/**
 * @brief  Initialise wiper logic state.
 */
esp_err_t wiper_logic_init(void);

/**
 * @brief  Called by the CAN gateway for every frame received from the car.
 *
 * If msg->identifier == WIPER_CAN_MSG_ID the function reads the current
 * rain intensity and may overwrite bytes inside msg->data[] before the
 * frame is forwarded.  All other frames are left untouched.
 *
 * @param[in,out] msg  CAN frame to inspect / modify in-place.
 */
void wiper_logic_process_can_frame(twai_message_t *msg);

/**
 * @brief  Optional background FreeRTOS task for time-based logic
 *         (hysteresis, fade-out timers, etc.).
 */
void wiper_logic_task(void *arg);
