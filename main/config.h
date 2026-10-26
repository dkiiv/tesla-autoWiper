/**
 * @file config.h
 * @brief Central configuration for the rain-sensor wiper controller.
 *
 * Edit this file to match your hardware wiring before compiling.
 * Every tunable number in the project lives here — no magic numbers in .c files.
 *
 * Supported chips
 * ---------------
 *  ESP32-C6  — the PRODUCTION target (the in-car module). Default.
 *  ESP32-P4  — the current bench/dev board. Kept building so existing
 *              hardware stays useful.  P4 boards come in several hardware
 *              revisions, which is why it is not the production target.
 *  Select with IDF_TARGET (./idf.sh build uses esp32c6 unless overridden:
 *  `IDF_TARGET=esp32p4 ./idf.sh build`).  Both chips have the two TWAI
 *  controllers + spare UART this design needs.
 *
 * Hardware overview
 * -----------------
 *  TWAI0  ──► CAN transceiver 0 ──► Car-side CAN bus        (bidirectional)
 *  TWAI1  ──► CAN transceiver 1 ──► Wiper-actuator CAN bus  (bidirectional)
 *  UART1  ──► LIN transceiver   ──► Rain/light sensor        (master role)
 *
 * CAN gateway data flow
 * ---------------------
 *  CAN0 RX  →  (0x3E9 rewritten by wiper policy, everything else untouched)  →  CAN1 TX
 *  CAN1 RX  →  (pass-through, no modification)                               →  CAN0 TX
 */

#pragma once

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/uart.h"

/* ─── Pins (per chip) ───────────────────────────────────────────────────── */

#if CONFIG_IDF_TARGET_ESP32C6
/*
 * ESP32-C6 (production).  Chosen from pins broken out on ESP32-C6-DevKitC-1
 * that are NOT strapping (8, 9, 15), USB-JTAG (12, 13), console UART0
 * (16, 17) or SPI flash.  UNVERIFIED on real hardware yet — change freely
 * to match the module's PCB.
 */
#define CAN_CAR_TX_PIN          GPIO_NUM_4
#define CAN_CAR_RX_PIN          GPIO_NUM_5
#define CAN_WIPER_TX_PIN        GPIO_NUM_6
#define CAN_WIPER_RX_PIN        GPIO_NUM_7
#define LIN_TX_PIN              GPIO_NUM_10
#define LIN_RX_PIN              GPIO_NUM_11

#elif CONFIG_IDF_TARGET_ESP32P4
/* ESP32-P4 bench breadboard (soldered, as wired today). */
#define CAN_CAR_TX_PIN          GPIO_NUM_35
#define CAN_CAR_RX_PIN          GPIO_NUM_36
#define CAN_WIPER_TX_PIN        GPIO_NUM_31
#define CAN_WIPER_RX_PIN        GPIO_NUM_34
#define LIN_TX_PIN              GPIO_NUM_20
#define LIN_RX_PIN              GPIO_NUM_21

#else
#error "Unsupported target: add a pin block for this chip in config.h"
#endif

/* ─── CAN (TWAI) ─────────────────────────────────────────────────────────── */

/*
 * Both buses run at the same rate.
 *
 * !! CHECK BEFORE INSTALLING IN THE CAR !!
 * Tesla Model 3/Y vehicle/party buses are generally reported as 500 kbit/s.
 * 1 Mbit/s was set for bench work.  A wrong bitrate means the gateway
 * receives nothing and the wipers lose their command path.  Confirm with a
 * sniffer on the actual harness (ROADMAP Phase 3, step 1) and set this to match.
 */
#define CAN_BAUD_RATE           TWAI_TIMING_CONFIG_1MBITS()
// #define CAN_BAUD_RATE        TWAI_TIMING_CONFIG_500KBITS()

/* Driver RX/TX queue depth per controller.  Frames, not bytes. */
#define CAN_RX_QUEUE_LEN        64
#define CAN_TX_QUEUE_LEN        64

/* The frame we rewrite.  Signal layout lives in protocol.h. */
#define WIPER_CAN_MSG_ID        0x3E9   /* DAS_bodyControls */
#define WIPER_CAN_MSG_LEN       8       /* expected DLC; other lengths pass untouched */

/* ─── LIN bus ────────────────────────────────────────────────────────────── */

#define LIN_UART_NUM            UART_NUM_1
#define LIN_BAUD                19200   /* standard LIN baud rate */

/*
 * VAG rain/light sensor LIN frame ID (6-bit, before parity).
 * 0x21 is standard for the Bosch / Continental sensor fitted to
 * VW / Audi / Skoda / SEAT platforms.  Verify against your sensor.
 */
#define LIN_RAIN_SENSOR_FRAME_ID    0x21

/* Response payload length (not including checksum byte). */
#define LIN_RAIN_SENSOR_DATA_LEN    7

/*
 * 1 = the LIN transceiver echoes our own TX back onto RX (true for every
 *     normal single-wire transceiver: TJA1021/1027, MCP2003B).  The driver
 *     then reads the echoed sync+PID and verifies them before the response.
 * 0 = no echo (e.g. a split TX/RX bench setup).
 */
#define LIN_EXPECT_ECHO         1

/* ─── Polling rate ───────────────────────────────────────────────────────── */

#define LIN_POLL_INTERVAL_MS    50      /* 20 Hz */

/* ─── Wiper policy (see wiper_policy.h for the rules) ──────────────────── */

/*
 * VAG rain intensity (index 0..15) → Tesla DAS_wiperSpeed (0 = off, 1..14).
 * INITIAL GUESS — identity-ish with the top end clamped.  What each Tesla
 * speed value actually looks like is not yet observed; calibrate in the car
 * (ROADMAP Phase 3, step 5).  Values > 14 are clamped automatically.
 */
#define WIPER_RAIN_TO_SPEED_MAP  { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 14 }

/* A lower rain target must persist this long before the wipers slow down. */
#define WIPER_FALL_HOLD_MS      3000

/* A LIN reading older than this is treated as "no sensor" → passthrough. */
#define LIN_STALE_MS            500

/* ─── FreeRTOS task priorities & stack sizes ────────────────────────────── */

/*
 * One forwarding task per direction; each blocks on its RX queue, so no
 * polling/sleep tuning.  Highest priority — must not drop frames.
 * The alert task handles bus-off recovery; LIN polling is lower (a missed
 * poll just ages the reading a little).
 */
#define TASK_PRIORITY_CAN_FWD        10
#define TASK_PRIORITY_CAN_ALERT      9
#define TASK_PRIORITY_LIN_POLL       8
#define TASK_PRIORITY_HEALTH         1

#define TASK_STACK_CAN_FWD        4096
#define TASK_STACK_CAN_ALERT      3072
#define TASK_STACK_LIN_POLL       4096
#define TASK_STACK_HEALTH         3072

#define HEALTH_LOG_INTERVAL_MS    5000

/* ─── Debug / logging ───────────────────────────────────────────────────── */

/* Global log level at boot.  ESP_LOG_INFO for the car; VERBOSE/DEBUG on the bench. */
#define APP_LOG_LEVEL           ESP_LOG_INFO

/* 1 = log every forwarded CAN frame (bench only — far too slow for a real bus). */
#define DEBUG_CAN_GATEWAY       0

/* 1 = log raw LIN response bytes on every poll. */
#define DEBUG_LIN_SENSOR        0

/* 1 = log every wiper policy decision (one line per 0x3E9 frame). */
#define DEBUG_WIPER_POLICY      0
