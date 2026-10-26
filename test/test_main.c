/**
 * @file test_main.c
 * @brief Host-side unit tests for the pure-C modules (protocol.c, wiper_policy.c).
 *
 * No ESP-IDF, no framework: build and run with `make -C test`.
 * Each CHECK prints the failing expression; exit code = number of failures.
 */

#include "protocol.h"
#include "wiper_policy.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;

#define CHECK_EQ(actual, expected) do {                                          \
    long _a = (long)(actual), _e = (long)(expected);                            \
    if (_a != _e) {                                                             \
        printf("  FAIL %s:%d  %s == %ld, expected %ld\n",                      \
               __FILE__, __LINE__, #actual, _a, _e);                            \
        g_fail++;                                                               \
    } else {                                                                    \
        g_pass++;                                                               \
    }                                                                           \
} while (0)

/* ─── Reference implementations (independent of protocol.c) ─────────────── */

/* LIN 2.x spec: PID parity bits. Straight from the spec text, bit by bit. */
static uint8_t ref_lin_pid(uint8_t id)
{
    int b[6];
    for (int i = 0; i < 6; i++) b[i] = (id >> i) & 1;
    int p0 = b[0] ^ b[1] ^ b[2] ^ b[4];
    int p1 = !(b[1] ^ b[3] ^ b[4] ^ b[5]);
    return (uint8_t)((id & 0x3F) | (p0 << 6) | (p1 << 7));
}

/* LIN 2.x enhanced checksum, spec form: 8-bit sum with end-around carry, inverted. */
static uint8_t ref_lin_cs(uint8_t pid, const uint8_t *d, int n)
{
    unsigned sum = pid;
    for (int i = 0; i < n; i++) {
        sum += d[i];
        sum = (sum & 0xFF) + (sum >> 8);
    }
    return (uint8_t)~sum;
}

/* opendbc tesla_checksum() transliterated from opendbc/car/tesla/teslacan.py */
static uint8_t ref_tesla_cs(unsigned addr, const uint8_t *d, int n, int cs_byte)
{
    unsigned c = (addr & 0xFF) + ((addr >> 8) & 0xFF);
    for (int i = 0; i < n; i++) if (i != cs_byte) c += d[i];
    return (uint8_t)(c & 0xFF);
}

/* ─── LIN tests ─────────────────────────────────────────────────────────── */

static void test_lin_pid(void)
{
    puts("lin_calc_pid");
    CHECK_EQ(lin_calc_pid(0x21), 0x61);   /* VAG rain sensor, documented in README */
    CHECK_EQ(lin_calc_pid(0x3C), 0x3C);   /* master request diag frame (well-known) */
    CHECK_EQ(lin_calc_pid(0x3D), 0x7D);   /* slave response diag frame (well-known) */
    CHECK_EQ(lin_calc_pid(0x00), 0x80);
    for (int id = 0; id < 64; id++) {
        CHECK_EQ(lin_calc_pid((uint8_t)id), ref_lin_pid((uint8_t)id));
    }
    CHECK_EQ(lin_calc_pid(0x21 | 0xC0), 0x61);   /* upper bits ignored */
}

static void test_lin_checksum(void)
{
    puts("lin_calc_checksum_enhanced");
    const uint8_t zeros[7] = {0};
    CHECK_EQ(lin_calc_checksum_enhanced(0x61, zeros, 7), (uint8_t)~0x61);

    /* Forces several carries. */
    const uint8_t ff[7] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK_EQ(lin_calc_checksum_enhanced(0x61, ff, 7), ref_lin_cs(0x61, ff, 7));

    /* Pseudo-random sweep against the reference. */
    uint32_t x = 12345;
    for (int t = 0; t < 2000; t++) {
        uint8_t d[7];
        for (int i = 0; i < 7; i++) { x = x * 1103515245u + 12345u; d[i] = (uint8_t)(x >> 16); }
        CHECK_EQ(lin_calc_checksum_enhanced(0x61, d, 7), ref_lin_cs(0x61, d, 7));
    }

    /* Property: a valid frame (data + checksum) sums to 0xFF under end-around carry. */
    const uint8_t d[7] = {0x03, 0x80, 0x00, 0x12, 0x34, 0x56, 0x78};
    uint8_t cs = lin_calc_checksum_enhanced(0x61, d, 7);
    unsigned sum = 0x61;
    for (int i = 0; i < 7; i++) { sum += d[i]; sum = (sum & 0xFF) + (sum >> 8); }
    sum += cs; sum = (sum & 0xFF) + (sum >> 8);
    CHECK_EQ(sum, 0xFF);
}

/* ─── Tesla tests ───────────────────────────────────────────────────────── */

static void test_tesla_checksum(void)
{
    puts("tesla_checksum");
    const uint8_t zeros[8] = {0};
    CHECK_EQ(tesla_checksum(0x3E9, zeros, 8, 7), 0xEC);   /* seed = 0xE9 + 0x03 */

    uint8_t d[8] = {0x41, 0x22, 0x00, 0x01, 0x00, 0x00, 0xA0, 0x55};
    CHECK_EQ(tesla_checksum(0x3E9, d, 8, 7), ref_tesla_cs(0x3E9, d, 8, 7));

    /* Checksum byte content must not influence the result. */
    d[7] = 0x00; uint8_t a = tesla_checksum(0x3E9, d, 8, 7);
    d[7] = 0xFF; uint8_t b = tesla_checksum(0x3E9, d, 8, 7);
    CHECK_EQ(a, b);

    uint32_t x = 999;
    for (int t = 0; t < 2000; t++) {
        uint8_t r[8];
        for (int i = 0; i < 8; i++) { x = x * 1103515245u + 12345u; r[i] = (uint8_t)(x >> 16); }
        CHECK_EQ(tesla_checksum(0x3E9, r, 8, 7), ref_tesla_cs(0x3E9, r, 8, 7));
    }
}

static void test_wiper_speed_packing(void)
{
    puts("tesla_wiper_speed_get/set");
    uint8_t d[8] = {0xA7, 0x11, 0x22, 0x33, 0x44, 0x55, 0xB6, 0x00};
    uint8_t orig[8]; memcpy(orig, d, 8);

    CHECK_EQ(tesla_wiper_speed_get(d), 0xA);

    tesla_wiper_speed_set(d, 5);
    CHECK_EQ(tesla_wiper_speed_get(d), 5);
    CHECK_EQ(d[0] & 0x0F, orig[0] & 0x0F);         /* headlight/hazard bits untouched */
    for (int i = 1; i <= 6; i++) CHECK_EQ(d[i], orig[i]);   /* incl. counter in byte 6 */
    CHECK_EQ(d[6] >> 4, 0xB);                       /* DAS_bodyControlsCounter preserved */
    CHECK_EQ(d[7], ref_tesla_cs(0x3E9, d, 8, 7));    /* checksum valid after rewrite */

    /* Never emit 15 (DAS_WIPER_SPEED_INVALID) — clamp everything above 14. */
    tesla_wiper_speed_set(d, 15);
    CHECK_EQ(tesla_wiper_speed_get(d), 14);
    tesla_wiper_speed_set(d, 200);
    CHECK_EQ(tesla_wiper_speed_get(d), 14);
    CHECK_EQ(d[7], ref_tesla_cs(0x3E9, d, 8, 7));

    tesla_wiper_speed_set(d, 0);
    CHECK_EQ(tesla_wiper_speed_get(d), 0);
    CHECK_EQ(d[0] & 0x0F, orig[0] & 0x0F);
}

/* ─── Wiper policy tests ───────────────────────────────────────────────── */

static wiper_policy_t make_policy(void)
{
    wiper_policy_config_t cfg = {
        .rain_to_speed = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
        .fall_hold_ms  = 3000,
        .stale_ms      = 500,
    };
    wiper_policy_t p;
    wiper_policy_init(&p, &cfg);
    return p;
}

static void test_policy_passthrough(void)
{
    puts("wiper_policy: passthrough cases");
    wiper_policy_t p = make_policy();

    wiper_decision_t d = wiper_policy_step(&p, 0, false, 0, 8, 3);
    CHECK_EQ(d.modify, 0);
    CHECK_EQ(d.reason, WIPER_DECISION_PASSTHROUGH_NO_SENSOR);

    d = wiper_policy_step(&p, 0, true, 501, 8, 3);      /* stale reading */
    CHECK_EQ(d.modify, 0);
    CHECK_EQ(d.reason, WIPER_DECISION_PASSTHROUGH_NO_SENSOR);

    d = wiper_policy_step(&p, 0, true, 500, 8, 3);      /* exactly at limit = fresh */
    CHECK_EQ(d.modify, 1);

    d = wiper_policy_step(&p, 10, true, 0, 8, 15);      /* car says SNA */
    CHECK_EQ(d.modify, 0);
    CHECK_EQ(d.reason, WIPER_DECISION_PASSTHROUGH_OEM_SNA);

    d = wiper_policy_step(&p, 20, true, 0, 9, 9);       /* car agrees exactly */
    CHECK_EQ(d.modify, 0);
    CHECK_EQ(d.speed, 9);
    CHECK_EQ(d.reason, WIPER_DECISION_AGREE);

    /* Still inside the 3 s fall hold from speed 9 → we keep overriding. */
    d = wiper_policy_step(&p, 30, true, 0, 0, 0);
    CHECK_EQ(d.modify, 1);
    CHECK_EQ(d.speed, 9);
    CHECK_EQ(d.reason, WIPER_DECISION_OVERRIDE_UP);

    p = make_policy();                                  /* fresh: dry + car off */
    d = wiper_policy_step(&p, 0, true, 0, 0, 0);
    CHECK_EQ(d.modify, 0);                              /* equal → nothing to rewrite */
    CHECK_EQ(d.reason, WIPER_DECISION_AGREE);
}

/*
 * The module is AUTHORITATIVE: when the sensor is healthy, Tesla's request
 * never wins.  This is the core reason the project exists — the camera-based
 * auto-wipers fire when the glass is dry.
 */
static void test_policy_authoritative(void)
{
    puts("wiper_policy: authoritative (suppresses phantom wipes)");
    wiper_policy_t p = make_policy();

    /* Dry glass, car wants to wipe → we force OFF. */
    wiper_decision_t d = wiper_policy_step(&p, 0, true, 0, 0, 5);
    CHECK_EQ(d.modify, 1);
    CHECK_EQ(d.speed, 0);
    CHECK_EQ(d.reason, WIPER_DECISION_OVERRIDE_DOWN);

    /* Every possible car request on dry glass → always 0. */
    for (int oem = 1; oem <= 14; oem++) {
        d = wiper_policy_step(&p, 10, true, 0, 0, (uint8_t)oem);
        CHECK_EQ(d.modify, 1);
        CHECK_EQ(d.speed, 0);
    }

    /* Light rain (speed 2), car wants max → we hold 2. */
    d = wiper_policy_step(&p, 20, true, 0, 2, 14);
    CHECK_EQ(d.modify, 1);
    CHECK_EQ(d.speed, 2);
    CHECK_EQ(d.reason, WIPER_DECISION_OVERRIDE_DOWN);

    /* Heavy rain, car thinks dry → we raise. */
    d = wiper_policy_step(&p, 30, true, 0, 10, 0);
    CHECK_EQ(d.modify, 1);
    CHECK_EQ(d.speed, 10);
    CHECK_EQ(d.reason, WIPER_DECISION_OVERRIDE_UP);

    /* End-to-end through the frame encoder: car's phantom wipe is erased. */
    p = make_policy();
    uint8_t frame[8] = {0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00};  /* oem speed 5 */
    frame[7] = tesla_checksum(0x3E9, frame, 8, 7);
    d = wiper_policy_step(&p, 0, true, 0, 0, tesla_wiper_speed_get(frame));
    if (d.modify) tesla_wiper_speed_set(frame, d.speed);
    CHECK_EQ(tesla_wiper_speed_get(frame), 0);
    CHECK_EQ(frame[0] & 0x0F, 0x1);                        /* low bits untouched */
    CHECK_EQ(frame[6], 0x30);                              /* counter untouched */
    CHECK_EQ(frame[7], ref_tesla_cs(0x3E9, frame, 8, 7));  /* valid checksum */

    /* Fail-safe still holds: sensor gone → car's value passes through. */
    d = wiper_policy_step(&p, 100, false, 0, 0, 5);
    CHECK_EQ(d.modify, 0);
    CHECK_EQ(d.reason, WIPER_DECISION_PASSTHROUGH_NO_SENSOR);
}

static void test_policy_map_clamp(void)
{
    puts("wiper_policy: map clamps to 14");
    wiper_policy_t p = make_policy();
    CHECK_EQ(p.cfg.rain_to_speed[15], 14);
    wiper_decision_t d = wiper_policy_step(&p, 0, true, 0, 15, 0);
    CHECK_EQ(d.modify, 1);
    CHECK_EQ(d.speed, 14);
}

static void test_policy_hysteresis(void)
{
    puts("wiper_policy: rise fast, fall slow");
    wiper_policy_t p = make_policy();

    wiper_decision_t d = wiper_policy_step(&p, 0, true, 0, 6, 0);
    CHECK_EQ(d.speed, 6);                                  /* immediate rise */

    d = wiper_policy_step(&p, 100, true, 0, 9, 0);
    CHECK_EQ(d.speed, 9);                                  /* immediate rise */

    d = wiper_policy_step(&p, 200, true, 0, 2, 0);
    CHECK_EQ(d.speed, 9);                                  /* hold */
    d = wiper_policy_step(&p, 1200, true, 0, 4, 0);
    CHECK_EQ(d.speed, 9);                                  /* still holding, max-in-window=4 */
    d = wiper_policy_step(&p, 3199, true, 0, 3, 0);
    CHECK_EQ(d.speed, 9);                                  /* 2999 ms — not yet */
    d = wiper_policy_step(&p, 3200, true, 0, 3, 0);
    CHECK_EQ(d.speed, 4);                                  /* drop to highest seen in window */

    /* A rise during a pending fall cancels the fall. */
    d = wiper_policy_step(&p, 3300, true, 0, 1, 0);
    CHECK_EQ(d.speed, 4);
    d = wiper_policy_step(&p, 3400, true, 0, 7, 0);
    CHECK_EQ(d.speed, 7);
    d = wiper_policy_step(&p, 6000, true, 0, 7, 0);
    CHECK_EQ(d.speed, 7);

    /* Dry for long enough → off (equal to OEM 0 → passthrough, speed 0). */
    wiper_policy_step(&p, 7000, true, 0, 0, 0);
    d = wiper_policy_step(&p, 10000, true, 0, 0, 0);
    CHECK_EQ(p.current, 0);
    CHECK_EQ(d.modify, 0);

    /* Sensor loss resets state: no stale high speed when it comes back. */
    wiper_policy_step(&p, 11000, true, 0, 12, 0);
    CHECK_EQ(p.current, 12);
    wiper_policy_step(&p, 11100, false, 0, 0, 0);
    d = wiper_policy_step(&p, 11200, true, 0, 2, 0);
    CHECK_EQ(d.speed, 2);
}

static void test_policy_time_wrap(void)
{
    puts("wiper_policy: millisecond counter wrap");
    wiper_policy_t p = make_policy();
    uint32_t t0 = 0xFFFFF000u;
    wiper_policy_step(&p, t0, true, 0, 8, 0);
    wiper_policy_step(&p, t0 + 100, true, 0, 1, 0);
    wiper_decision_t d = wiper_policy_step(&p, t0 + 100 + 3000, true, 0, 1, 0);  /* wraps */
    CHECK_EQ(d.speed, 1);
}

int main(void)
{
    test_lin_pid();
    test_lin_checksum();
    test_tesla_checksum();
    test_wiper_speed_packing();
    test_policy_passthrough();
    test_policy_authoritative();
    test_policy_map_clamp();
    test_policy_hysteresis();
    test_policy_time_wrap();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
