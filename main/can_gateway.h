/**
 * @file can_gateway.h
 * @brief Bidirectional CAN MITM gateway – 4-task symmetric architecture.
 *
 * Both CAN buses forward to each other.  Traffic in the car→wiper direction
 * passes through wiper_logic_process_can_frame() so the wiper control frame
 * can be modified based on live rain sensor data.  Traffic in the wiper→car
 * direction is forwarded untouched.
 *
 *   Car ECU ──[CAN0]──► can0_rx_task ──► (wiper logic) ──► to_can1_queue ──► can1_tx_task ──[CAN1]──► Wiper actuator
 *
 *   Car ECU ◄─[CAN0]── can0_tx_task ◄── to_can0_queue ◄── can1_rx_task ◄──[CAN1]── Wiper actuator
 */

#pragma once

#include "esp_err.h"
#include "driver/twai.h"

/* ─── Public API ────────────────────────────────────────────────────────── */

/**
 * @brief  Initialise both TWAI controllers and the inter-task TX queues.
 * @return ESP_OK on success.
 */
esp_err_t can_gateway_init(void);

/**
 * @brief  [Core 1]  Receive frames from CAN0 (car side).
 *         Passes each frame through wiper_logic_process_can_frame(),
 *         then enqueues for CAN1 TX.
 */
void can_gateway_can0_rx_task(void *arg);

/**
 * @brief  [Core 1]  Drain the to_can1 queue → transmit on CAN1 (wiper side).
 */
void can_gateway_can1_tx_task(void *arg);

/**
 * @brief  [Core 1]  Receive frames from CAN1 (wiper side), pass-through,
 *         enqueue for CAN0 TX.
 */
void can_gateway_can1_rx_task(void *arg);

/**
 * @brief  [Core 1]  Drain the to_can0 queue → transmit on CAN0 (car side).
 */
void can_gateway_can0_tx_task(void *arg);
