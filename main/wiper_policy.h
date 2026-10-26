/**
 * @file wiper_policy.h
 * @brief Decides what DAS_wiperSpeed to send, given the rain sensor and the
 *        speed the Autopilot computer originally asked for.
 *
 * Pure C, no ESP-IDF dependencies — unit-tested on the host (test/).
 * The caller (wiper_logic.c) supplies time and sensor data; this module
 * only holds the hysteresis state.
 *
 * Rules, in priority order
 * ────────────────────────
 *  1. Sensor invalid or stale            → pass the original frame through.
 *  2. Original speed is SNA/INVALID (15) → pass through (don't "fix" a frame
 *                                          the car itself marked invalid).
 *  3. Map rain intensity → target speed with a lookup table.
 *  4. Hysteresis: rises apply immediately; falls only after the lower
 *     target has persisted for fall_hold_ms (then drop to the highest target
 *     seen during that hold window).  Stops speed "hunting".
 *  5. Never go below the original request: out = max(original, ours).
 *     If the AP computer wants faster wipes than we do, it wins.
 *
 * What this does NOT know (yet)
 * ─────────────────────────────
 *  0x3E9 is the AP computer's *auto-wiper* request.  Manual stalk settings
 *  and washer cycles are believed to be handled elsewhere by the body
 *  controller, which would make it ignore DAS_wiperSpeed outside of auto
 *  mode.  That is UNVERIFIED — see ROADMAP Phase 3.  If it turns out wrong,
 *  extra inputs (another CAN ID) have to be added here.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define WIPER_POLICY_MAP_LEN 16   /* one entry per rain intensity 0..15 */

typedef struct {
    uint8_t  rain_to_speed[WIPER_POLICY_MAP_LEN];  /**< target Tesla speed per rain level */
    uint32_t fall_hold_ms;   /**< lower target must persist this long before slowing */
    uint32_t stale_ms;       /**< sensor reading older than this = invalid           */
} wiper_policy_config_t;

typedef enum {
    WIPER_DECISION_PASSTHROUGH_NO_SENSOR = 0,  /**< sensor invalid/stale         */
    WIPER_DECISION_PASSTHROUGH_OEM_SNA,        /**< original frame says INVALID  */
    WIPER_DECISION_PASSTHROUGH_OEM_HIGHER,     /**< car already asks for ≥ ours  */
    WIPER_DECISION_OVERRIDE,                   /**< we raised the speed          */
} wiper_decision_reason_t;

typedef struct {
    uint8_t                 speed;     /**< speed to put on the bus (0..14) */
    bool                    modify;    /**< true → rewrite the frame         */
    wiper_decision_reason_t reason;
} wiper_decision_t;

typedef struct {
    wiper_policy_config_t cfg;
    uint8_t  current;          /**< speed we are currently holding            */
    bool     fall_pending;     /**< a lower target is waiting out the hold    */
    uint32_t fall_since_ms;
    uint8_t  fall_max;         /**< highest target seen during the hold       */
} wiper_policy_t;

/** @brief Reset state and copy config.  Out-of-range map entries are clamped to 14. */
void wiper_policy_init(wiper_policy_t *p, const wiper_policy_config_t *cfg);

/**
 * @brief  Run once per received 0x3E9 frame.
 *
 * @param now_ms          monotonic time
 * @param sensor_valid    last LIN poll produced a good frame
 * @param sensor_age_ms   now - time of that good frame
 * @param rain_intensity  0..15 from the sensor
 * @param oem_speed       DAS_wiperSpeed in the frame as received from the car
 */
wiper_decision_t wiper_policy_step(wiper_policy_t *p, uint32_t now_ms,
                                   bool sensor_valid, uint32_t sensor_age_ms,
                                   uint8_t rain_intensity, uint8_t oem_speed);

/** @brief Human-readable reason string for logs. */
const char *wiper_decision_reason_str(wiper_decision_reason_t r);
