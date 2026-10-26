/**
 * @file can_gateway.h
 * @brief Bidirectional CAN man-in-the-middle gateway (two TWAI controllers).
 *
 * Tasks
 * ─────
 *   can_fwd car→act   blocks on TWAI0 RX, runs wiper_logic on each frame,
 *                     transmits on TWAI1
 *   can_fwd act→car   blocks on TWAI1 RX, transmits on TWAI0 (untouched)
 *   can_alert         watches both controllers for bus-off and recovers them
 *
 * Each forwarding task blocks on its own RX queue, so there is no polling
 * loop or sleep to tune, and one direction stalling can't starve the other.
 * Both forwarding tasks are registered with the task watchdog: if either
 * wedges, the chip resets (and the stock passthrough path comes back after
 * boot — see ROADMAP Phase 4 for the hardware fail-safe that covers the
 * reset window itself).
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>

typedef struct {
    uint32_t rx;          /**< frames received on the source bus           */
    uint32_t tx_drop;     /**< frames we failed to queue on the other bus   */
    uint32_t bus_off;     /**< bus-off events on the source controller      */
    uint32_t rx_missed;   /**< driver-reported lost frames (queue full)     */
    uint32_t rx_overrun;  /**< driver-reported HW FIFO overruns             */
} can_dir_stats_t;

typedef struct {
    can_dir_stats_t car_to_act;   /**< source = TWAI0 (car side)       */
    can_dir_stats_t act_to_car;   /**< source = TWAI1 (actuator side)  */
} can_gateway_stats_t;

/** @brief  Install + start both TWAI controllers.  Fatal on failure. */
esp_err_t can_gateway_init(void);

/** @brief  Spawn the two forwarding tasks and the alert/recovery task. */
esp_err_t can_gateway_start(void);

/** @brief  Snapshot of counters for the health log. */
void can_gateway_get_stats(can_gateway_stats_t *out);
