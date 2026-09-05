/**
 * @file    mbap.c
 * @brief   Modbus TCP frame codec — see mbap.h.
 *
 * libc only.  If this file ever needs lwIP, an RTOS call or a global, the
 * thing being added belongs in modbus_tcp.c instead — the whole value of this
 * file is that a host test can run it.
 */

#include "App/Gw/mbap.h"

#include <string.h>

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

int Mbap_PduLen(const uint8_t *hdr)
{
    uint16_t length;

    if (hdr == NULL) {
        return -1;
    }
    if (be16(&hdr[2]) != 0u) {
        return -1;                  /* not Modbus TCP */
    }

    length = be16(&hdr[4]);

    /* `length` counts the unit id plus the PDU, so a frame carrying a bare
     * function code is 2 and anything below that is nonsense. */
    if (length < 2u || length > (MBAP_MAX_PDU + 1u)) {
        return -1;
    }
    return (int)(length - 1u);
}

uint8_t Mbap_Decode(const uint8_t *frame, uint16_t pduLen, sMbapReq *out)
{
    const uint8_t *pdu;

    if (frame == NULL || out == NULL || pduLen == 0u) {
        return MBAP_EXC_ILLEGAL_ADDRESS;
    }
    pdu = &frame[MBAP_HDR_LEN];

    memset(out, 0, sizeof(*out));
    out->txnId = be16(&frame[0]);
    out->unit  = frame[6];
    out->fc    = pdu[0];

    /* THE FUNCTION CODE IS CHECKED FIRST, before anything reads past it: a
     * one-byte PDU is a legal frame and its only meaningful field is this. */
    if (out->fc != 3u && out->fc != 4u && out->fc != 6u && out->fc != 16u) {
        return MBAP_EXC_ILLEGAL_FUNCTION;
    }

    /* Every function this gateway serves carries at least address + one
     * 16-bit argument. */
    if (pduLen < 5u) {
        return MBAP_EXC_ILLEGAL_ADDRESS;
    }
    out->addr = be16(&pdu[1]);

    switch (out->fc) {
    case 3u:
    case 4u:
        out->count = be16(&pdu[3]);
        if (out->count == 0u || out->count > MBAP_MAX_READ_REGS) {
            return MBAP_EXC_ILLEGAL_VALUE;
        }
        return 0u;

    case 6u:
        /* FC06 lands exactly one register and its argument IS the value. */
        out->count  = 1u;
        out->values = &pdu[3];
        return 0u;

    default:      /* 16 */
        out->count = be16(&pdu[3]);
        /* The byte count is the request's own statement about itself.  A
         * disagreement is a malformed frame, which is a different fact from
         * an address the slave does not have — hence 0x02 rather than 0x03,
         * and hence checking it BEFORE the register limit: a frame that lies
         * about its own length was never a valid request to bound. */
        if (pduLen < 6u || pdu[5] != (uint8_t)(out->count * 2u) ||
            (uint32_t)pduLen < 6u + (uint32_t)out->count * 2u) {
            return MBAP_EXC_ILLEGAL_ADDRESS;
        }
        if (out->count == 0u || out->count > MBAP_MAX_WRITE_REGS) {
            return MBAP_EXC_ILLEGAL_VALUE;
        }
        out->values = &pdu[6];
        return 0u;
    }
}

void Mbap_TakeValues(const sMbapReq *req, uint16_t *out)
{
    if (req == NULL || out == NULL || req->values == NULL) {
        return;
    }
    for (uint16_t i = 0; i < req->count; i++) {
        out[i] = be16(&req->values[i * 2u]);
    }
}

uint32_t Mbap_BuildReadReply(const sMbapReq *req, const uint16_t *regs,
                             uint8_t *out)
{
    put16(&out[0], req->txnId);
    put16(&out[2], 0u);
    /* unit + fc + byte count + data */
    put16(&out[4], (uint16_t)(3u + (uint32_t)req->count * 2u));
    out[6] = req->unit;
    out[7] = req->fc;
    out[8] = (uint8_t)(req->count * 2u);

    for (uint16_t i = 0; i < req->count; i++) {
        put16(&out[9 + i * 2u], regs[i]);
    }
    return 9u + (uint32_t)req->count * 2u;
}

uint32_t Mbap_BuildWriteReply(const sMbapReq *req, uint8_t *out)
{
    put16(&out[0], req->txnId);
    put16(&out[2], 0u);
    put16(&out[4], 6u);              /* unit + fc + 2 + 2, both cases */
    out[6] = req->unit;
    out[7] = req->fc;
    put16(&out[8], req->addr);

    /* FC06 answers the VALUE it wrote; FC16 answers how many it wrote. */
    if (req->fc == 6u) {
        put16(&out[10], (req->values != NULL) ? be16(req->values) : 0u);
    } else {
        put16(&out[10], req->count);
    }
    return 12u;
}

uint32_t Mbap_BuildException(const uint8_t *frame, uint8_t code, uint8_t *out)
{
    put16(&out[0], be16(&frame[0]));    /* transaction id, echoed */
    put16(&out[2], 0u);
    put16(&out[4], 3u);                 /* unit + fc + code */
    out[6] = frame[6];                  /* unit id, echoed */
    out[7] = (uint8_t)(frame[7] | 0x80u);
    out[8] = code;
    return 9u;
}
