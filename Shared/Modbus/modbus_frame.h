/**
 * @file    modbus_frame.h
 * @brief   Modbus RTU frame build and parse — pure byte functions.
 *
 * THE ENGINE DECIDES WHAT A VALID REPLY IS, not the driver (docs/modbus.md
 * §5.1): a short frame and a CRC-bad frame are both `mbPortDone_frame`, and
 * this is where they are told apart.  One place decides, so a second transport
 * cannot disagree with the first about what came back.
 *
 * It lives in Shared/ for the reason §2.2 gives: it is a pure function of
 * bytes, so host tests reach it, and CRC + exception handling is exactly the
 * kind of code that should never first be exercised against a live slave.
 *
 * Scope is the master side of FC03/04/06/16 — what the module puts on a wire.
 */
#ifndef MODBUS_FRAME_H_
#define MODBUS_FRAME_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest RTU frame: addr + fc + byteCount + 250 data + CRC. */
#define MB_FRAME_MAX        256u
#define MB_FRAME_MIN_REPLY    4u   /* addr + fc + 1 + CRC would be shorter  */

/* How a reply failed to be a reply.  These map onto the API's eModbusErr in
 * App/Modbus; Shared/ does not know the API's code set. */
typedef enum {
    mbFrame_ok            =  0,
    mbFrame_errShort      = -1,  /* truncated, or shorter than it claims    */
    mbFrame_errCrc        = -2,
    mbFrame_errAddr       = -3,  /* a different slave answered              */
    mbFrame_errFunction   = -4,  /* not the function code we asked for      */
    mbFrame_errCount      = -5,  /* byte count disagrees with the request   */
    mbFrame_errBadArg     = -6,
    /* The slave's own exception codes, kept apart because telling
     * illegal-function from illegal-address from illegal-value is most of the
     * diagnosis when bringing up an unfamiliar slave (§4.6). */
    mbFrame_exception     = -10, /* + the code is in *excOut                */
} eModbusFrameErr;

/** CRC16/Modbus (poly 0xA001, init 0xFFFF). */
uint16_t MbFrame_Crc16(const uint8_t *data, size_t len);

/**
 * @brief  Build a read request (FC03 holding / FC04 input).
 * @return frame length, or a negative eModbusFrameErr
 */
int MbFrame_BuildRead(uint8_t slave, uint8_t fc, uint16_t addr, uint16_t count,
                      uint8_t *out, uint16_t outSize);

/** @brief  Build an FC06 single-register write. */
int MbFrame_BuildWrite(uint8_t slave, uint16_t reg, uint16_t value,
                       uint8_t *out, uint16_t outSize);

/**
 * @brief  Build an FC16 multiple-register write.
 *
 *         Needed by a capability whose dialect says `writeFc: 16` — a slave
 *         with no FC06 handler, which the JK is.  The engine starts using it
 *         at §10 step 12; the frame exists here from step 8 so the format is
 *         proven before anything depends on it.
 */
int MbFrame_BuildWriteMulti(uint8_t slave, uint16_t addr,
                            const uint16_t *values, uint16_t count,
                            uint8_t *out, uint16_t outSize);

/**
 * @brief  Parse a read reply against the request that produced it.
 *
 *         Checks length, CRC, slave address, function code and byte count,
 *         then decodes big-endian registers into `regs`.
 *
 * @param  excOut  set to the slave's exception code when the return is
 *                 mbFrame_exception; 0 otherwise.  May be NULL.
 * @return mbFrame_ok, or a negative eModbusFrameErr
 */
int MbFrame_ParseRead(const uint8_t *frame, uint16_t len,
                      uint8_t slave, uint8_t fc, uint16_t count,
                      uint16_t *regs, uint8_t *excOut);

/**
 * @brief  Parse a write reply (FC06/FC16 both echo address + value/count).
 */
int MbFrame_ParseWrite(const uint8_t *frame, uint16_t len,
                       uint8_t slave, uint8_t fc, uint8_t *excOut);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_FRAME_H_ */
