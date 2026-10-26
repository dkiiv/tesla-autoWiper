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
}

wiper_decision_t wiper_policy_step(wiper_policy_t *p, uint32_t now_ms,
                                   bool sensor_valid, uint32_t sensor_age_ms,
                                   uint8_t rain_intensity, uint8_t oem_speed)
{
    wiper_decision_t d = { .speed = oem_speed, .modify = false };

    /* 1. No trustworthy sensor data → hands off. */
    if (!sensor_valid || sensor_age_ms > p->cfg.stale_ms) {
        reset_state(p);
        d.reason = WIPER_DECISION_PASSTHROUGH_NO_SENSOR;
        return d;
    }

    /* 3. Map. (Step 2 is checked after hysteresis so state keeps tracking rain.) */
    uint8_t target = p->cfg.rain_to_speed[rain_intensity & 0x0F];

    /* 4. Hysteresis: rise fast, fall slow. */
    if (target >= p->current) {
        p->current      = target;
        p->fall_pending = false;
    } else if (!p->fall_pending) {
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

    /* 2. Car marked its own request invalid → don't touch the frame. */
    if (oem_speed == TESLA_WIPER_SPEED_INVALID) {
        d.reason = WIPER_DECISION_PASSTHROUGH_OEM_SNA;
        return d;
    }

    /* 5. Never slow the wipers below what the car asked for. */
    if (oem_speed >= p->current) {
        d.reason = WIPER_DECISION_PASSTHROUGH_OEM_HIGHER;
        return d;
    }

    d.speed  = p->current;
    d.modify = true;
    d.reason = WIPER_DECISION_OVERRIDE;
    return d;
}

const char *wiper_decision_reason_str(wiper_decision_reason_t r)
{
    switch (r) {
    case WIPER_DECISION_PASSTHROUGH_NO_SENSOR:  return "passthrough(no sensor)";
    case WIPER_DECISION_PASSTHROUGH_OEM_SNA:    return "passthrough(oem SNA)";
    case WIPER_DECISION_PASSTHROUGH_OEM_HIGHER: return "passthrough(oem >= ours)";
    case WIPER_DECISION_OVERRIDE:               return "override";
    }
    return "?";
}
