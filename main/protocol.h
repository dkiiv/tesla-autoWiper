/**
 * @file protocol.h
 * @brief Pure bus-protocol math: LIN framing and Tesla DAS_bodyControls packing.
 *
 * Everything in here is plain C with NO ESP-IDF dependencies, so it can be
 * compiled and unit-tested on any host (see test/).  Keep it that way: if a
 * function needs a driver, a log call or a FreeRTOS primitive, it belongs in
 * lin_sensor.c / wiper_logic.c instead.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ─── LIN 2.x ──────────────────────────────────────────────────────────── */

/**
 * @brief  Protected identifier (PID) from a 6-bit LIN frame ID.
 *
 *   P0 = ID0 ^ ID1 ^ ID2 ^ ID4
 *   P1 = ~(ID1 ^ ID3 ^ ID4 ^ ID5)
 *
 * Example: frame 0x21 → PID 0x61.
 */
uint8_t lin_calc_pid(uint8_t frame_id);

/**
 * @brief  LIN 2.x "enhanced" checksum: PID + data, carry folded back, inverted.
 */
uint8_t lin_calc_checksum_enhanced(uint8_t pid, const uint8_t *data, uint8_t len);

/* ─── Tesla DAS_bodyControls (0x3E9) ──────────────────────────────────── */

/*
 * Signal layout, from commaai/opendbc tesla_model3_vehicle.dbc
 * (BO_ 1001 DAS_bodyControls, all Intel/little-endian, unsigned):
 *
 *   DAS_wiperSpeed            4|4   → byte 0, upper nibble
 *   DAS_bodyControlsCounter  52|4   → byte 6, upper nibble  (we never touch it)
 *   DAS_bodyControlsChecksum 56|8   → byte 7
 */
#define TESLA_BODYCTRL_CAN_ID          0x3E9
#define TESLA_BODYCTRL_LEN             8
#define TESLA_BODYCTRL_CHECKSUM_BYTE   7

#define TESLA_WIPER_SPEED_OFF          0
#define TESLA_WIPER_SPEED_MAX_VALID    14   /* 1..14 = speed levels          */
#define TESLA_WIPER_SPEED_INVALID      15   /* DAS_WIPER_SPEED_INVALID (SNA) */

/**
 * @brief  Tesla CAN checksum (same algorithm as opendbc tesla_checksum()):
 *         low byte of address + high byte of address + every data byte except
 *         the checksum byte itself, truncated to 8 bits.
 */
uint8_t tesla_checksum(uint16_t address, const uint8_t *data, int len, int checksum_byte);

/** @brief  Read DAS_wiperSpeed (0..15) out of an 8-byte 0x3E9 payload. */
uint8_t tesla_wiper_speed_get(const uint8_t *data);

/**
 * @brief  Write DAS_wiperSpeed into an 8-byte 0x3E9 payload and recompute
 *         the checksum.  Only byte 0 bits [7:4] and byte 7 are modified.
 *
 * Speed is clamped to TESLA_WIPER_SPEED_MAX_VALID: this function can never
 * put DAS_WIPER_SPEED_INVALID on the bus.
 */
void tesla_wiper_speed_set(uint8_t *data, uint8_t speed);
