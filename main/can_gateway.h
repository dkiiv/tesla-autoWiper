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
 * Sniffer build (CAN_LISTEN_ONLY, `SNIFFER=1`): both controllers listen-only,
 * one sniff task per controller instead of the forwarding tasks.  Nothing is
 * transmitted; 0x3E9 frames are checked (checksum, counter, interval) and
 * logged.  Connect in PARALLEL to the bus in this build, never inline.
 *
 * Each forwarding task blocks on its own RX queue, so there is no polling
 * loop or sleep to tune, and one direction stalling can't starve the other.
 * All three tasks are registered with the task watchdog: if any of them
 * wedges, the chip resets and forwarding resumes after boot.  During the
 * reset and boot window NO frames pass at all; only the hardware bypass in
 * ROADMAP Phase 4 can cover that window.
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

/** 0x3E9 observations in the sniffer build (per controller). */
typedef struct {
    uint32_t bodyctrl_seen;    /**< 0x3E9 frames received                     */
    uint32_t bad_dlc;          /**< ... with a length other than 8            */
    uint32_t bad_checksum;     /**< ... whose byte 7 != tesla_checksum         */
    uint32_t counter_skips;    /**< ... whose counter wasn't previous + 1     */
    uint32_t min_interval_ms;  /**< shortest gap between 0x3E9 frames         */
    uint32_t max_interval_ms;  /**< longest gap between 0x3E9 frames          */
    uint8_t  last_speed;       /**< last DAS_wiperSpeed seen                  */
} can_sniff_stats_t;

typedef struct {
    can_dir_stats_t car_to_act;   /**< source = TWAI0 (car side)       */
    can_dir_stats_t act_to_car;   /**< source = TWAI1 (actuator side)  */
    can_sniff_stats_t sniff[2];   /**< sniffer build only, per TWAI    */
} can_gateway_stats_t;

/** @brief  Install + start both TWAI controllers.  Fatal on failure. */
esp_err_t can_gateway_init(void);

/** @brief  Spawn the two forwarding tasks and the alert/recovery task. */
esp_err_t can_gateway_start(void);

/** @brief  Snapshot of counters for the health log. */
void can_gateway_get_stats(can_gateway_stats_t *out);
