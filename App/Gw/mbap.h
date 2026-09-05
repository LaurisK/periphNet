/**
 * @file    mbap.h
 * @brief   Modbus TCP (MBAP) frame codec — pure, and deliberately so.
 *
 * WHY THIS IS ITS OWN FILE.  `modbus_tcp.c` is welded to lwIP, FreeRTOS and
 * the Modbus engine, so nothing in it can be reached by a host test.  The
 * framing is the half most likely to be wrong in a way no counter shows —
 * a byte-order slip, a length field off by the unit id, an exception frame
 * that echoes the wrong transaction id — and it is also the half that needs
 * no board at all.  So it lives here: **libc only, no lwIP, no RTOS, no
 * globals, no I/O**, and `tests/test_mbap.c` drives it against the 48 register
 * groups `solis_modbus` actually asks for.
 *
 * WHAT IT DOES NOT DO, on purpose: resolve a unit id to a device, touch the
 * bus, or decide whether the board is busy.  Those need state and a wire, and
 * they are `modbus_tcp.c`'s.  This file turns bytes into a request and a
 * result back into bytes.
 *
 * THE FRAME, so the offsets below are readable:
 *
 *     0 1   transaction id   echoed verbatim; the client's, not ours
 *     2 3   protocol id      always 0.  Anything else is not Modbus TCP
 *     4 5   length           bytes AFTER this field — unit id INCLUDED
 *     6     unit id          the slave address
 *     7...  PDU              function code, then its arguments
 *
 * The length field covering the unit id is the classic off-by-one here, so it
 * is spelled out: a frame is `6 + length` bytes, and the PDU is `length - 1`.
 */

#ifndef MBAP_H_
#define MBAP_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MBAP_HDR_LEN         7u    /* txnId, protoId, length, unitId        */
#define MBAP_MAX_PDU       253u    /* FC16 with 123 registers is the worst  */
#define MBAP_MAX_FRAME     (MBAP_HDR_LEN + MBAP_MAX_PDU)

/* The PROTOCOL's register limits, not this board's.  They must agree with
 * MB_RAW_MAX_READ_REGS / MB_RAW_MAX_WRITE_REGS in App/Modbus/modbus.h; a
 * compile-time check in modbus_tcp.c holds them together, because the two
 * headers are deliberately independent of each other. */
#define MBAP_MAX_READ_REGS   125u
#define MBAP_MAX_WRITE_REGS  123u

/* Exception codes this codec can ORIGINATE.  The gateway adds its own
 * (0x06 busy, 0x0A/0x0B path and target) — those need state this file has
 * none of.  See modbus_tcp.h for the full map and why each is the one owed. */
#define MBAP_EXC_ILLEGAL_FUNCTION   0x01u
#define MBAP_EXC_ILLEGAL_ADDRESS    0x02u
#define MBAP_EXC_ILLEGAL_VALUE      0x03u

/** One decoded request.  `values` points INTO the caller's frame buffer for
 *  fc 6 and 16 and is NULL otherwise — borrowed, like everything else here. */
typedef struct {
    uint16_t       txnId;
    uint16_t       addr;
    uint16_t       count;    /* registers; always 1 for fc 6                */
    const uint8_t *values;   /* big-endian pairs, `count` of them, or NULL  */
    uint8_t        unit;
    uint8_t        fc;       /* 3, 4, 6 or 16                               */
} sMbapReq;

/**
 * @brief  Read the 7-byte header and say how long the PDU is.
 *
 *         This is the only place that knows the length field includes the
 *         unit id.  A frame whose protocol id is not zero is not Modbus TCP
 *         and cannot be resynchronised — the caller must close, not skip.
 *
 * @param  hdr  at least MBAP_HDR_LEN bytes
 * @return PDU length in bytes (1..MBAP_MAX_PDU), or -1 if the header is not
 *         a Modbus TCP header this codec will parse.
 */
int Mbap_PduLen(const uint8_t *hdr);

/**
 * @brief  Decode a complete frame into a request.
 *
 *         Validates SHAPE ONLY — the function code is one this gateway
 *         serves, the register count is inside the protocol's limits, and an
 *         FC16's own byte count agrees with its register count.  What an
 *         address MEANS is the slave's business: an address the inverter does
 *         not implement must reach the wire and come back as the slave's own
 *         exception 2, because that is what upstream adapts to.
 *
 * @param  frame   the whole frame, header included
 * @param  pduLen  from Mbap_PduLen()
 * @param  out     filled on success
 * @return 0 on success, or the exception code to answer with.
 */
uint8_t Mbap_Decode(const uint8_t *frame, uint16_t pduLen, sMbapReq *out);

/**
 * @brief  Unpack a write request's values into a register array.
 *         `out` must hold at least `req->count` entries.  No-op for a read.
 */
void Mbap_TakeValues(const sMbapReq *req, uint16_t *out);

/**
 * @brief  Build the response to a successful FC03/FC04.
 * @param  out  at least MBAP_MAX_FRAME bytes
 * @return frame length.
 */
uint32_t Mbap_BuildReadReply(const sMbapReq *req, const uint16_t *regs,
                             uint8_t *out);

/**
 * @brief  Build the response to a successful FC06/FC16.
 *
 *         FC06 echoes the request; FC16 answers address and count.  Both are
 *         built from the DECODED request rather than copied from the inbound
 *         bytes, so a malformed-but-accepted frame cannot be reflected back.
 * @return frame length.
 */
uint32_t Mbap_BuildWriteReply(const sMbapReq *req, uint8_t *out);

/**
 * @brief  Build an exception response.
 *
 *         Takes the raw frame rather than a decoded request, because the
 *         cases that most need an exception are the ones that would not
 *         decode.  Only the transaction id, unit id and function code are
 *         read from it, and all three exist in any frame long enough to have
 *         reached here.
 * @return frame length (always 9).
 */
uint32_t Mbap_BuildException(const uint8_t *frame, uint8_t code, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MBAP_H_ */
