/**
 * @file wiper_policy.c
 * @brief Rain → wiper speed policy.  Pure C (host-tested); see header for rules.
 */

#include "wiper_policy.h"
#include "protocol.h"

#include <string.h>

void wiper_policy_init(wiper_policy_t *p, const wiper_policy_config_t *cfg)
{
    memset(p, 0, sizeof(*p));
    p->cfg = *cfg;
    for (int i = 0; i < WIPER_POLICY_MAP_LEN; i++) {
        if (p->cfg.rain_to_speed[i] > TESLA_WIPER_SPEED_MAX_VALID) {
            p->cfg.rain_to_speed[i] = TESLA_WIPER_SPEED_MAX_VALID;
        }
    }
}

static void reset_state(wiper_policy_t *p)
{
    p->current      = TESLA_WIPER_SPEED_OFF;
    p->fall_pending = false;
    p->fall_max     = 0;
    p->rise_pending = false;
    p->rise_min     = 0;
    p->healthy      = false;   /* any dropout or fault restarts the warm-up */
}

wiper_decision_t wiper_policy_step(wiper_policy_t *p, uint32_t now_ms,
                                   bool sensor_valid, uint32_t sensor_age_ms,
                                   uint8_t sensor_status,
                                   uint8_t rain_intensity, uint8_t oem_speed)
{
    wiper_decision_t d = { .speed = oem_speed, .modify = false };

    /* 1. No trustworthy sensor data → hands off. */
    if (!sensor_valid || sensor_age_ms > p->cfg.stale_ms) {
        reset_state(p);
        d.reason = WIPER_DECISION_PASSTHROUGH_NO_SENSOR;
        return d;
    }
    /* 1b. Sensor reports a fault / init state → same as no sensor. */
    if ((sensor_status & p->cfg.status_fault_mask) != 0) {
        reset_state(p);
        d.reason = WIPER_DECISION_PASSTHROUGH_SENSOR_FAULT;
        return d;
    }

    if (!p->healthy) {
        p->healthy          = true;
        p->healthy_since_ms = now_ms;
    }

    /* 3. Map. (Steps 1c and 2 are checked after hysteresis so the state keeps
     *    tracking the rain while we are not allowed to act on it.) */
    uint8_t target = p->cfg.rain_to_speed[rain_intensity & 0x0F];

    /* 4. Hysteresis: debounced rise, slow fall. */
    if (target == p->current) {
        p->fall_pending = false;
        p->rise_pending = false;
    } else if (target > p->current) {
        p->fall_pending = false;
        if (p->cfg.rise_hold_ms == 0) {
            p->current = target;
        } else if (!p->rise_pending) {
            p->rise_pending  = true;
            p->rise_since_ms = now_ms;
            p->rise_min      = target;
        } else {
            if (target < p->rise_min) {
                p->rise_min = target;
            }
            if ((uint32_t)(now_ms - p->rise_since_ms) >= p->cfg.rise_hold_ms) {
                p->current      = p->rise_min;
                p->rise_pending = false;
            }
        }
    } else {
        p->rise_pending = false;
        if (!p->fall_pending) {
            p->fall_pending  = true;
            p->fall_since_ms = now_ms;
            p->fall_max      = target;
        } else {
            if (target > p->fall_max) {
                p->fall_max = target;
            }
            if ((uint32_t)(now_ms - p->fall_since_ms) >= p->cfg.fall_hold_ms) {
                p->current      = p->fall_max;
                p->fall_pending = false;
            }
        }
    }

    /* 1c. Sensor hasn't been healthy long enough to overrule the car. */
    if ((uint32_t)(now_ms - p->healthy_since_ms) < p->cfg.warmup_ms) {
        d.reason = WIPER_DECISION_PASSTHROUGH_WARMUP;
        return d;
    }

    /* 2. Car marked its own request invalid → don't touch the frame. */
    if (oem_speed == TESLA_WIPER_SPEED_INVALID) {
        d.reason = WIPER_DECISION_PASSTHROUGH_OEM_SNA;
        return d;
    }

    /* 5a. Raise-only mode: never lower what the car asked for. */
    if (p->cfg.raise_only && p->current < oem_speed) {
        d.reason = WIPER_DECISION_PASSTHROUGH_RAISE_ONLY;
        return d;
    }

    /* 5. Authoritative: our speed goes on the bus, higher or lower. */
    d.speed = p->current;
    if (oem_speed == p->current) {
        d.modify = false;                  /* identical bytes — nothing to rewrite */
        d.reason = WIPER_DECISION_AGREE;
    } else {
        d.modify = true;
        d.reason = (p->current > oem_speed) ? WIPER_DECISION_OVERRIDE_UP
                                            : WIPER_DECISION_OVERRIDE_DOWN;
    }
    return d;
}

const char *wiper_decision_reason_str(wiper_decision_reason_t r)
{
    switch (r) {
    case WIPER_DECISION_PASSTHROUGH_NO_SENSOR: return "passthrough(no sensor)";
    case WIPER_DECISION_PASSTHROUGH_OEM_SNA:   return "passthrough(oem SNA)";
    case WIPER_DECISION_AGREE:                 return "agree";
    case WIPER_DECISION_OVERRIDE_UP:           return "override(up)";
    case WIPER_DECISION_OVERRIDE_DOWN:         return "override(down)";
    case WIPER_DECISION_PASSTHROUGH_SENSOR_FAULT: return "passthrough(sensor fault)";
    case WIPER_DECISION_PASSTHROUGH_RAISE_ONLY:   return "passthrough(raise-only)";
    case WIPER_DECISION_PASSTHROUGH_WARMUP:       return "passthrough(warm-up)";
    }
    return "?";
}
