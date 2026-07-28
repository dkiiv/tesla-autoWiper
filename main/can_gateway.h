/**
 * @file can_gateway.h
 * @brief Bidirectional CAN MITM gateway - 4-task symmetric architecture.
 *
 * Both CAN buses forward to each other.  Traffic in the car→wiper direction
 * passes through wiper_logic_process_can_frame() so the wiper control frame
 * can be modified based on live rain sensor data.  Traffic in the wiper→car
 * direction is forwarded untouched.
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
 * @brief  CAN Gateway
 */
void can_gateway_task(void *arg);
