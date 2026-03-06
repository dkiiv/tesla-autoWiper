/**
 * @file config.h
 * @brief Central configuration for the ESP32-P4 rain-sensor wiper controller.
 *
 * Edit this file to match your hardware wiring before compiling.
 *
 * Hardware overview
 * -----------------
 *  TWAI0  ──► CAN transceiver 0 ──► Car-side CAN bus        (bidirectional)
 *  TWAI1  ──► CAN transceiver 1 ──► Wiper-actuator CAN bus  (bidirectional)
 *  UART1  ──► LIN transceiver   ──► Rain/light sensor        (master role)
 *
 * CAN gateway data flow
 * ---------------------
 *  CAN0 RX  →  (wiper frame modified by rain sensor data if ID matches)  →  CAN1 TX
 *  CAN1 RX  →  (pass-through, no modification)                           →  CAN0 TX
 */

#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

/* ─── CAN (TWAI) ─────────────────────────────────────────────────────────── */

/* TWAI0  –  Car-side bus (we read incoming wiper frames from here) */
#define CAN_CAR_TX_PIN          GPIO_NUM_4
#define CAN_CAR_RX_PIN          GPIO_NUM_5

/* TWAI1  –  Wiper-actuator bus (we write potentially-modified frames here) */
#define CAN_WIPER_TX_PIN        GPIO_NUM_6
#define CAN_WIPER_RX_PIN        GPIO_NUM_7

/* Both buses run at the same rate.  Adjust if your car uses a different speed. */
#define CAN_BAUD_RATE           TWAI_TIMING_CONFIG_1MBITS()

/*
 * CAN ID of the wiper control message you want to intercept.
 * Set this to the frame ID you discovered via CAN sniffing.
 * All other IDs are forwarded unchanged.
 */
#define WIPER_CAN_MSG_ID        0x3E9   /* DAS_bodyControls */
#define WIPER_CAN_MSG_LEN       8       /* expected DLC of that frame */

/* ─── LIN bus ────────────────────────────────────────────────────────────── */

#define LIN_UART_NUM            UART_NUM_1
#define LIN_TX_PIN              GPIO_NUM_8
#define LIN_RX_PIN              GPIO_NUM_9
#define LIN_BAUD                19200   /* standard LIN baud rate */

/*
 * VAG rain/light sensor LIN frame ID (6-bit, before parity).
 * 0x21 is standard for the Bosch / Continental sensor fitted to
 * VW / Audi / Skoda / SEAT platforms.  Verify against your sensor datasheet.
 */
#define LIN_RAIN_SENSOR_FRAME_ID    0x21

/*
 * Response payload length returned by the sensor (not including checksum byte).
 * Adjust if your sensor variant returns a different length.
 */
#define LIN_RAIN_SENSOR_DATA_LEN    7

/* ─── Polling rate ───────────────────────────────────────────────────────── */

/*
 * How often to query the LIN sensor.
 * 10 Hz  → 100 ms   (relaxed, lower bus load)
 * 20 Hz  →  50 ms
 * 50 Hz  →  20 ms   (aggressive, near the sensor's own update rate)
 *
 * Pick one and comment-out the others.
 */
#define LIN_POLL_INTERVAL_MS    50      /* 20 Hz – good starting point */
// #define LIN_POLL_INTERVAL_MS 100     /* 10 Hz */
// #define LIN_POLL_INTERVAL_MS  20     /* 50 Hz */

/* ─── FreeRTOS task priorities & stack sizes ────────────────────────────── */

/*
 * CAN RX tasks are highest priority – we must not drop frames.
 * TX tasks match RX so a burst of received frames drains quickly.
 * LIN polling is slightly lower; a missed poll just uses the previous value.
 * Wiper logic runs at background priority.
 */
#define TASK_PRIORITY_CAN_RX        10
#define TASK_PRIORITY_CAN_TX        10
#define TASK_PRIORITY_LIN_POLL       8
#define TASK_PRIORITY_WIPER_LOGIC    6

#define TASK_STACK_CAN_RX         4096
#define TASK_STACK_CAN_TX         4096
#define TASK_STACK_LIN_POLL       4096
#define TASK_STACK_WIPER_LOGIC    2048

/* ─── Debug / logging ───────────────────────────────────────────────────── */

/*
 * Set to 1 to print every CAN frame that is forwarded.
 * Disable in production to reduce UART overhead.
 */
#define DEBUG_CAN_GATEWAY       0

/*
 * Set to 1 to print raw LIN response bytes.
 */
#define DEBUG_LIN_SENSOR        1
