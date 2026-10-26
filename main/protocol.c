/**
 * @file protocol.c
 * @brief Pure bus-protocol math.  No ESP-IDF includes allowed (host-tested).
 */

#include "protocol.h"

/* ─── LIN ──────────────────────────────────────────────────────────────── */

uint8_t lin_calc_pid(uint8_t frame_id)
{
    uint8_t id = frame_id & 0x3F;
    uint8_t p0 = ((id >> 0) ^ (id >> 1) ^ (id >> 2) ^ (id >> 4)) & 0x01;
    uint8_t p1 = (~((id >> 1) ^ (id >> 3) ^ (id >> 4) ^ (id >> 5))) & 0x01;
    return (uint8_t)(id | (p0 << 6) | (p1 << 7));
}

uint8_t lin_calc_checksum_enhanced(uint8_t pid, const uint8_t *data, uint8_t len)
{
    uint16_t sum = pid;
    for (int i = 0; i < len; i++) {
        sum += data[i];
        if (sum > 0xFF) {
            sum -= 0xFF;   /* fold the carry back in (== +1 after dropping 0x100) */
        }
    }
    return (uint8_t)(~sum);
}

/* ─── Tesla ────────────────────────────────────────────────────────────── */

uint8_t tesla_checksum(uint16_t address, const uint8_t *data, int len, int checksum_byte)
{
    uint8_t sum = (uint8_t)((address & 0xFF) + ((address >> 8) & 0xFF));
    for (int i = 0; i < len; i++) {
        if (i != checksum_byte) {
            sum = (uint8_t)(sum + data[i]);
        }
    }
    return sum;
}

uint8_t tesla_wiper_speed_get(const uint8_t *data)
{
    return (uint8_t)((data[0] >> 4) & 0x0F);
}

void tesla_wiper_speed_set(uint8_t *data, uint8_t speed)
{
    if (speed > TESLA_WIPER_SPEED_MAX_VALID) {
        speed = TESLA_WIPER_SPEED_MAX_VALID;
    }
    data[0] = (uint8_t)((data[0] & 0x0F) | (speed << 4));
    data[TESLA_BODYCTRL_CHECKSUM_BYTE] =
        tesla_checksum(TESLA_BODYCTRL_CAN_ID, data, TESLA_BODYCTRL_LEN,
                       TESLA_BODYCTRL_CHECKSUM_BYTE);
}
