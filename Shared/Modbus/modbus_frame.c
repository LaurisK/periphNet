/**
 * @file    modbus_frame.c
 * @brief   Modbus RTU frame build and parse — see modbus_frame.h.
 */

#include "modbus_frame.h"

uint16_t MbFrame_Crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u)
                             : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/* Append the CRC to a frame body of `n` bytes and return the total length. */
static int seal(uint8_t *out, int n)
{
    uint16_t crc = MbFrame_Crc16(out, (size_t)n);

    out[n]     = (uint8_t)(crc & 0xFFu);
    out[n + 1] = (uint8_t)(crc >> 8);
    return n + 2;
}

int MbFrame_BuildRead(uint8_t slave, uint8_t fc, uint16_t addr, uint16_t count,
                      uint8_t *out, uint16_t outSize)
{
    if (out == NULL || outSize < 8u || count == 0u ||
        (fc != 0x03u && fc != 0x04u)) {
        return mbFrame_errBadArg;
    }

    out[0] = slave;
    out[1] = fc;
    out[2] = (uint8_t)(addr >> 8);
    out[3] = (uint8_t)(addr & 0xFFu);
    out[4] = (uint8_t)(count >> 8);
    out[5] = (uint8_t)(count & 0xFFu);
    return seal(out, 6);
}

int MbFrame_BuildWrite(uint8_t slave, uint16_t reg, uint16_t value,
                       uint8_t *out, uint16_t outSize)
{
    if (out == NULL || outSize < 8u) {
        return mbFrame_errBadArg;
    }

    out[0] = slave;
    out[1] = 0x06u;
    out[2] = (uint8_t)(reg >> 8);
    out[3] = (uint8_t)(reg & 0xFFu);
    out[4] = (uint8_t)(value >> 8);
    out[5] = (uint8_t)(value & 0xFFu);
    return seal(out, 6);
}

int MbFrame_BuildWriteMulti(uint8_t slave, uint16_t addr,
                            const uint16_t *values, uint16_t count,
                            uint8_t *out, uint16_t outSize)
{
    int n;

    if (out == NULL || values == NULL || count == 0u || count > 123u ||
        outSize < (uint16_t)(9u + count * 2u)) {
        return mbFrame_errBadArg;
    }

    out[0] = slave;
    out[1] = 0x10u;
    out[2] = (uint8_t)(addr >> 8);
    out[3] = (uint8_t)(addr & 0xFFu);
    out[4] = (uint8_t)(count >> 8);
    out[5] = (uint8_t)(count & 0xFFu);
    out[6] = (uint8_t)(count * 2u);
    n = 7;
    for (uint16_t i = 0; i < count; i++) {
        out[n++] = (uint8_t)(values[i] >> 8);
        out[n++] = (uint8_t)(values[i] & 0xFFu);
    }
    return seal(out, n);
}

/* Shared prologue: length, CRC, address, and the exception case.  Returns
 * mbFrame_ok when `frame` is a well-formed non-exception reply from `slave`
 * to `fc`. */
static int check_reply(const uint8_t *frame, uint16_t len,
                       uint8_t slave, uint8_t fc, uint8_t *excOut)
{
    if (excOut != NULL) {
        *excOut = 0u;
    }
    if (frame == NULL || len < MB_FRAME_MIN_REPLY) {
        return mbFrame_errShort;
    }

    uint16_t rxCrc = (uint16_t)frame[len - 2] |
                     (uint16_t)((uint16_t)frame[len - 1] << 8);
    if (rxCrc != MbFrame_Crc16(frame, (size_t)len - 2u)) {
        return mbFrame_errCrc;
    }

    /* A reply from another slave is not our reply, whatever it says. */
    if (frame[0] != slave) {
        return mbFrame_errAddr;
    }

    if ((frame[1] & 0x80u) != 0u) {
        if (len < 5u) {
            return mbFrame_errShort;
        }
        if (excOut != NULL) {
            *excOut = frame[2];
        }
        return mbFrame_exception;
    }

    if (frame[1] != fc) {
        return mbFrame_errFunction;
    }
    return mbFrame_ok;
}

int MbFrame_ParseRead(const uint8_t *frame, uint16_t len,
                      uint8_t slave, uint8_t fc, uint16_t count,
                      uint16_t *regs, uint8_t *excOut)
{
    int r = check_reply(frame, len, slave, fc, excOut);

    if (r != mbFrame_ok) {
        return r;
    }
    if (regs == NULL || count == 0u) {
        return mbFrame_errBadArg;
    }

    /* addr + fc + byteCount + data + CRC */
    if (len < (uint16_t)(5u + count * 2u)) {
        return mbFrame_errShort;
    }
    if (frame[2] != (uint8_t)(count * 2u)) {
        return mbFrame_errCount;
    }

    for (uint16_t i = 0; i < count; i++) {
        regs[i] = (uint16_t)((uint16_t)frame[3 + i * 2] << 8) |
                  (uint16_t)frame[4 + i * 2];
    }
    return mbFrame_ok;
}

int MbFrame_ParseWrite(const uint8_t *frame, uint16_t len,
                       uint8_t slave, uint8_t fc, uint8_t *excOut)
{
    int r = check_reply(frame, len, slave, fc, excOut);

    if (r != mbFrame_ok) {
        return r;
    }
    /* Both FC06 and FC16 echo four bytes after the function code. */
    if (len < 8u) {
        return mbFrame_errShort;
    }
    return mbFrame_ok;
}
