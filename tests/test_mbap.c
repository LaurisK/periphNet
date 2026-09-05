/**
 * @file    test_mbap.c
 * @brief   The Modbus TCP codec, against what `solis_modbus` really sends.
 *
 * This is the half of the gateway that can be tested without a board, a
 * network or an inverter: `App/Gw/mbap.c` is pure, so every frame Home
 * Assistant will put on the wire can be built here, decoded, answered, and
 * the answer re-parsed the way pymodbus would parse it.
 *
 * THE FIXTURE IS NOT INVENTED.  `tests/fixtures/solis_modbus_groups.h` is
 * generated from upstream's own `hybrid_sensors.py` by
 * `tools/extract_solis_groups.py`, using upstream's own grouping rule, and its
 * register values come from a real read-out of the sodas inverter.  A test
 * written against frames somebody imagined would agree with an implementation
 * written by the same person against the same imagination.
 *
 * WHAT THIS DOES NOT COVER, and cannot: the listener, the unit-id map, the
 * engine budget and the 0x06 / 0x0A / 0x0B exceptions all need state and a
 * wire.  They are hardware acceptance items
 * (docs/design_solis_modbus_link.md §10).
 */

#include "App/Gw/mbap.h"
#include "fixtures/solis_modbus_groups.h"
#include "test_util.h"

#include <stdlib.h>

/* --------------------------------------------------------------------------
 * A pymodbus-shaped client, in 30 lines
 *
 * Everything below builds requests the way `AsyncModbusTcpClient` does and
 * reads replies the way it does, so a disagreement here is a disagreement
 * with the far end rather than with a hand-written expectation.
 * -------------------------------------------------------------------------- */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

/** Build a read request (FC03/FC04).  @return frame length. */
static uint32_t client_read(uint8_t *out, uint16_t txn, uint8_t unit,
                            uint8_t fc, uint16_t addr, uint16_t count)
{
    wr16(&out[0], txn);
    wr16(&out[2], 0u);
    wr16(&out[4], 6u);          /* unit + fc + addr + count */
    out[6] = unit;
    out[7] = fc;
    wr16(&out[8], addr);
    wr16(&out[10], count);
    return 12u;
}

/** Build an FC06 request. */
static uint32_t client_write_single(uint8_t *out, uint16_t txn, uint8_t unit,
                                    uint16_t addr, uint16_t value)
{
    wr16(&out[0], txn);
    wr16(&out[2], 0u);
    wr16(&out[4], 6u);
    out[6] = unit;
    out[7] = 6u;
    wr16(&out[8], addr);
    wr16(&out[10], value);
    return 12u;
}

/** Build an FC16 request. */
static uint32_t client_write_multi(uint8_t *out, uint16_t txn, uint8_t unit,
                                   uint16_t addr, const uint16_t *vals,
                                   uint16_t count)
{
    uint8_t bc = (uint8_t)(count * 2u);

    wr16(&out[0], txn);
    wr16(&out[2], 0u);
    wr16(&out[4], (uint16_t)(7u + bc));   /* unit + fc + addr + cnt + bc + data */
    out[6] = unit;
    out[7] = 16u;
    wr16(&out[8], addr);
    wr16(&out[10], count);
    out[12] = bc;
    for (uint16_t i = 0; i < count; i++) {
        wr16(&out[13 + i * 2u], vals[i]);
    }
    return 13u + (uint32_t)bc;
}

/** A value from the real read-out, or a synthesized one for an address the
 *  dump does not cover — the dump reaches 33299 and 43199, the groups reach
 *  further. */
static uint16_t dumped(uint16_t addr)
{
    for (uint32_t i = 0; i < SOLIS_DUMP_COUNT; i++) {
        if (kSolisDump[i].addr == addr) {
            return kSolisDump[i].val;
        }
    }
    return (uint16_t)(addr ^ 0xA55Au);
}

/* --------------------------------------------------------------------------
 * Every group solis_modbus asks for, round-tripped
 * -------------------------------------------------------------------------- */

static void test_every_solis_group_round_trips(void)
{
    uint8_t  req[MBAP_MAX_FRAME];
    uint8_t  rsp[MBAP_MAX_FRAME];
    uint16_t regs[SOLIS_GROUP_MAX_REGS];
    uint32_t served = 0;

    TEST_ASSERT(SOLIS_GROUP_COUNT == 48u);
    TEST_ASSERT(SOLIS_GROUP_MAX_REGS <= MBAP_MAX_READ_REGS);

    for (uint32_t g = 0; g < SOLIS_GROUP_COUNT; g++) {
        const sSolisGroup *grp = &kSolisGroups[g];
        uint16_t           txn = (uint16_t)(0x1000u + g);
        sMbapReq           r;
        uint32_t           reqLen, rspLen;
        int                pduLen;

        reqLen = client_read(req, txn, 1u, grp->fc, grp->start, grp->count);

        /* The board reads the header first, then exactly the PDU it promises.
         * A mismatch here would desynchronise the stream on a real socket. */
        pduLen = Mbap_PduLen(req);
        TEST_ASSERT(pduLen > 0);
        TEST_ASSERT((uint32_t)pduLen + MBAP_HDR_LEN == reqLen);

        TEST_ASSERT(Mbap_Decode(req, (uint16_t)pduLen, &r) == 0u);
        TEST_ASSERT(r.fc    == grp->fc);
        TEST_ASSERT(r.addr  == grp->start);
        TEST_ASSERT(r.count == grp->count);
        TEST_ASSERT(r.unit  == 1u);
        TEST_ASSERT(r.txnId == txn);

        for (uint16_t i = 0; i < grp->count; i++) {
            regs[i] = dumped((uint16_t)(grp->start + i));
        }
        rspLen = Mbap_BuildReadReply(&r, regs, rsp);

        /* Now read it back as the client does. */
        TEST_ASSERT(rspLen <= MBAP_MAX_FRAME);
        TEST_ASSERT(rd16(&rsp[0]) == txn);            /* txn id echoed      */
        TEST_ASSERT(rd16(&rsp[2]) == 0u);             /* protocol id        */
        TEST_ASSERT(rd16(&rsp[4]) == rspLen - 6u);    /* length excludes 6  */
        TEST_ASSERT(rsp[6] == 1u);                    /* unit id echoed     */
        TEST_ASSERT(rsp[7] == grp->fc);               /* NOT an exception   */
        TEST_ASSERT(rsp[8] == (uint8_t)(grp->count * 2u));
        TEST_ASSERT(rspLen == 9u + (uint32_t)grp->count * 2u);

        for (uint16_t i = 0; i < grp->count; i++) {
            TEST_ASSERT(rd16(&rsp[9 + i * 2u]) == regs[i]);
        }
        served++;
    }
    TEST_ASSERT(served == SOLIS_GROUP_COUNT);
}

/* The largest group upstream asks for must fit the board's buffers, and the
 * protocol's own ceiling must fit too — the buffer is sized from the protocol,
 * so a group growing past 125 would be upstream's problem, not ours. */
static void test_largest_frames_fit(void)
{
    uint8_t  rsp[MBAP_MAX_FRAME];
    uint16_t regs[MBAP_MAX_READ_REGS];
    sMbapReq r = { 0x0001u, 33000u, MBAP_MAX_READ_REGS, NULL, 1u, 4u };

    for (uint16_t i = 0; i < MBAP_MAX_READ_REGS; i++) {
        regs[i] = i;
    }
    TEST_ASSERT(Mbap_BuildReadReply(&r, regs, rsp) == 9u + 250u);
    TEST_ASSERT(9u + 250u <= MBAP_MAX_FRAME);

    /* And the worst request: FC16 with 123 registers. */
    TEST_ASSERT(13u + 246u <= MBAP_MAX_FRAME);
    TEST_ASSERT(MBAP_MAX_FRAME == MBAP_HDR_LEN + MBAP_MAX_PDU);
}

/* --------------------------------------------------------------------------
 * The write half — sunSale's control surface
 * -------------------------------------------------------------------------- */

/* The Remote Dispatch block, the one place FC16 atomicity is load-bearing:
 * scattered single-register writes are silently dropped by the inverter
 * (docs/design_solis_modbus_link.md §3.5).  Upstream writes it as two blocks,
 * global 44100-44104 and realtime 44105-44112. */
static void test_remote_dispatch_fc16_blocks(void)
{
    const struct { uint16_t addr, count; } blocks[] = {
        { 44100u, 5u },      /* global   */
        { 44105u, 8u },      /* realtime */
    };

    for (uint32_t b = 0; b < 2u; b++) {
        uint8_t  req[MBAP_MAX_FRAME];
        uint8_t  rsp[MBAP_MAX_FRAME];
        uint16_t vals[16];
        uint16_t taken[16];
        sMbapReq r;
        uint32_t reqLen;
        int      pduLen;

        for (uint16_t i = 0; i < blocks[b].count; i++) {
            vals[i] = (uint16_t)(0x0100u * (b + 1u) + i);
        }
        reqLen = client_write_multi(req, 0x2000u, 1u, blocks[b].addr,
                                    vals, blocks[b].count);

        pduLen = Mbap_PduLen(req);
        TEST_ASSERT(pduLen > 0);
        TEST_ASSERT((uint32_t)pduLen + MBAP_HDR_LEN == reqLen);

        TEST_ASSERT(Mbap_Decode(req, (uint16_t)pduLen, &r) == 0u);
        TEST_ASSERT(r.fc    == 16u);
        TEST_ASSERT(r.addr  == blocks[b].addr);
        TEST_ASSERT(r.count == blocks[b].count);
        TEST_ASSERT(r.values != NULL);

        /* THE WHOLE BLOCK ARRIVES AS ONE REQUEST, which is what lets the
         * gateway hand it to the wire as one frame. */
        Mbap_TakeValues(&r, taken);
        for (uint16_t i = 0; i < blocks[b].count; i++) {
            TEST_ASSERT(taken[i] == vals[i]);
        }

        TEST_ASSERT(Mbap_BuildWriteReply(&r, rsp) == 12u);
        TEST_ASSERT(rd16(&rsp[0]) == 0x2000u);
        TEST_ASSERT(rsp[7] == 16u);
        TEST_ASSERT(rd16(&rsp[8])  == blocks[b].addr);
        TEST_ASSERT(rd16(&rsp[10]) == blocks[b].count);
    }
}

/* Every single-register control sunSale writes, from docs/solis_control.md.
 * FC06 answers by echoing the value, which is how pymodbus confirms a write. */
static void test_single_register_controls(void)
{
    const uint16_t addrs[] = { 43110u, 43071u, 43073u, 43074u, 43117u, 43118u,
                               43128u, 43132u, 43135u, 43136u, 43282u };

    for (uint32_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
        uint8_t  req[MBAP_MAX_FRAME];
        uint8_t  rsp[MBAP_MAX_FRAME];
        uint16_t taken[2] = { 0u, 0u };
        sMbapReq r;
        int      pduLen;
        uint16_t value = (uint16_t)(1000u + i);

        (void)client_write_single(req, 0x3000u, 1u, addrs[i], value);
        /* fc + addr + value = 5 PDU bytes; the MBAP length field says 6
         * because it counts the unit id too. */
        pduLen = Mbap_PduLen(req);
        TEST_ASSERT(pduLen == 5);

        TEST_ASSERT(Mbap_Decode(req, (uint16_t)pduLen, &r) == 0u);
        TEST_ASSERT(r.fc    == 6u);
        TEST_ASSERT(r.addr  == addrs[i]);
        TEST_ASSERT(r.count == 1u);       /* FC06 is always one register */

        Mbap_TakeValues(&r, taken);
        TEST_ASSERT(taken[0] == value);
        TEST_ASSERT(taken[1] == 0u);      /* nothing written past count */

        TEST_ASSERT(Mbap_BuildWriteReply(&r, rsp) == 12u);
        TEST_ASSERT(rsp[7] == 6u);
        TEST_ASSERT(rd16(&rsp[8])  == addrs[i]);
        TEST_ASSERT(rd16(&rsp[10]) == value);   /* the VALUE, not the count */
    }
}

/* --------------------------------------------------------------------------
 * What must be refused, and with which code
 * -------------------------------------------------------------------------- */

static void test_header_rejects(void)
{
    /* A whole frame, not MBAP_HDR_LEN: client_read() writes 12 bytes and only
     * the first 7 are the header under test. */
    uint8_t hdr[MBAP_MAX_FRAME];

    /* A protocol id that is not zero is not Modbus TCP.  The stream cannot be
     * resynchronised, so the caller must close rather than skip. */
    (void)client_read(hdr, 1u, 1u, 4u, 33000u, 1u);
    wr16(&hdr[2], 1u);
    TEST_ASSERT(Mbap_PduLen(hdr) < 0);

    /* Length counts the unit id, so 0 and 1 leave no PDU at all. */
    (void)client_read(hdr, 1u, 1u, 4u, 33000u, 1u);
    wr16(&hdr[4], 0u);
    TEST_ASSERT(Mbap_PduLen(hdr) < 0);
    wr16(&hdr[4], 1u);
    TEST_ASSERT(Mbap_PduLen(hdr) < 0);

    /* Two is the shortest legal frame: unit id plus a bare function code. */
    wr16(&hdr[4], 2u);
    TEST_ASSERT(Mbap_PduLen(hdr) == 1);

    /* And a length that would overrun the buffer must not be believed. */
    wr16(&hdr[4], (uint16_t)(MBAP_MAX_PDU + 2u));
    TEST_ASSERT(Mbap_PduLen(hdr) < 0);
    wr16(&hdr[4], (uint16_t)(MBAP_MAX_PDU + 1u));
    TEST_ASSERT(Mbap_PduLen(hdr) == (int)MBAP_MAX_PDU);
    wr16(&hdr[4], 0xFFFFu);
    TEST_ASSERT(Mbap_PduLen(hdr) < 0);
}

static void test_decode_rejects(void)
{
    uint8_t  req[MBAP_MAX_FRAME];
    uint16_t vals[130];
    sMbapReq r;

    /* An unsupported function code — FC05 write-coil, FC01 read-coils, FC23 */
    (void)client_read(req, 1u, 1u, 5u, 0u, 1u);
    TEST_ASSERT(Mbap_Decode(req, 6u, &r) == MBAP_EXC_ILLEGAL_FUNCTION);
    (void)client_read(req, 1u, 1u, 1u, 0u, 1u);
    TEST_ASSERT(Mbap_Decode(req, 6u, &r) == MBAP_EXC_ILLEGAL_FUNCTION);
    (void)client_read(req, 1u, 1u, 23u, 0u, 1u);
    TEST_ASSERT(Mbap_Decode(req, 6u, &r) == MBAP_EXC_ILLEGAL_FUNCTION);

    /* A bare function code is a legal frame and a malformed request — and the
     * function code is still checked FIRST, so a supported fc with no
     * arguments is 0x02 while an unsupported one is 0x01. */
    (void)client_read(req, 1u, 1u, 4u, 0u, 1u);
    TEST_ASSERT(Mbap_Decode(req, 1u, &r) == MBAP_EXC_ILLEGAL_ADDRESS);
    (void)client_read(req, 1u, 1u, 5u, 0u, 1u);
    TEST_ASSERT(Mbap_Decode(req, 1u, &r) == MBAP_EXC_ILLEGAL_FUNCTION);

    /* Register counts outside the protocol's own limits. */
    (void)client_read(req, 1u, 1u, 4u, 33000u, 0u);
    TEST_ASSERT(Mbap_Decode(req, 6u, &r) == MBAP_EXC_ILLEGAL_VALUE);
    (void)client_read(req, 1u, 1u, 3u, 43000u, MBAP_MAX_READ_REGS + 1u);
    TEST_ASSERT(Mbap_Decode(req, 6u, &r) == MBAP_EXC_ILLEGAL_VALUE);
    (void)client_read(req, 1u, 1u, 3u, 43000u, MBAP_MAX_READ_REGS);
    TEST_ASSERT(Mbap_Decode(req, 6u, &r) == 0u);

    /* FC16 whose byte count disagrees with its register count.  That is a
     * malformed FRAME (0x02), not an out-of-range value: believing it would
     * read past what the client sent. */
    for (uint16_t i = 0; i < 130u; i++) {
        vals[i] = i;
    }
    (void)client_write_multi(req, 1u, 1u, 44100u, vals, 4u);
    req[12] = 6u;                     /* claim 3 registers' worth of bytes */
    TEST_ASSERT(Mbap_Decode(req, 13u, &r) == MBAP_EXC_ILLEGAL_ADDRESS);

    /* FC16 truncated: byte count agrees with the register count, but the
     * frame is shorter than both say. */
    (void)client_write_multi(req, 1u, 1u, 44100u, vals, 8u);
    TEST_ASSERT(Mbap_Decode(req, 12u, &r) == MBAP_EXC_ILLEGAL_ADDRESS);

    /* Exactly at the write limit — the largest frame a client can legally
     * send, and it must fit the board's buffer with nothing to spare. */
    (void)client_write_multi(req, 1u, 1u, 44100u, vals, MBAP_MAX_WRITE_REGS);
    TEST_ASSERT(13u + MBAP_MAX_WRITE_REGS * 2u <= MBAP_MAX_FRAME);
    TEST_ASSERT(Mbap_PduLen(req) == (int)(6u + MBAP_MAX_WRITE_REGS * 2u));
    TEST_ASSERT(Mbap_Decode(req, (uint16_t)(6u + MBAP_MAX_WRITE_REGS * 2u),
                            &r) == 0u);

    /* ONE PAST IT IS REJECTED BY THE HEADER, NOT BY DECODE, and that matters:
     * such a frame is 261 bytes, one more than the board's receive buffer, so
     * the length check is what stops it being read at all.  Writing it needs
     * a bigger buffer HERE than the board has THERE, which is the point. */
    {
        uint8_t big[MBAP_MAX_FRAME + 16];

        (void)client_write_multi(big, 1u, 1u, 44100u, vals,
                                 (uint16_t)(MBAP_MAX_WRITE_REGS + 1u));
        TEST_ASSERT(Mbap_PduLen(big) < 0);
    }

    /* Decode's own register-count guard is therefore UNREACHABLE from the
     * wire — no frame that passes the header can carry a count above the
     * limit.  Tested directly anyway, by handing decode a length the header
     * would never have produced: a guard nothing reaches today is one line
     * away from being reached by tomorrow's caller. */
    {
        uint8_t big[MBAP_MAX_FRAME + 16];

        (void)client_write_multi(big, 1u, 1u, 44100u, vals,
                                 (uint16_t)(MBAP_MAX_WRITE_REGS + 1u));
        TEST_ASSERT(Mbap_Decode(big,
                                (uint16_t)(6u + (MBAP_MAX_WRITE_REGS + 1u) * 2u),
                                &r) == MBAP_EXC_ILLEGAL_VALUE);
    }
}

/* AN ADDRESS THE INVERTER DOES NOT IMPLEMENT IS NOT THIS CODEC'S BUSINESS.
 * `solis_modbus` bisects a failing block on exception 2 and disables the
 * sensors behind the bad register — so the exception has to come from the
 * SLAVE, which means the request has to reach the wire.  A codec that guessed
 * would turn a one-time adaptation into a permanent wrong answer. */
static void test_unknown_addresses_are_accepted_for_the_wire(void)
{
    uint8_t  req[MBAP_MAX_FRAME];
    sMbapReq r;

    const uint16_t nonsense[] = { 0u, 1u, 9999u, 39999u, 40000u, 65535u };

    for (uint32_t i = 0; i < sizeof(nonsense) / sizeof(nonsense[0]); i++) {
        (void)client_read(req, 1u, 1u, 4u, nonsense[i], 1u);
        TEST_ASSERT(Mbap_Decode(req, 6u, &r) == 0u);
        TEST_ASSERT(r.addr == nonsense[i]);
    }
}

static void test_exception_frame(void)
{
    uint8_t req[MBAP_MAX_FRAME];
    uint8_t rsp[MBAP_MAX_FRAME];

    (void)client_read(req, 0xBEEFu, 7u, 4u, 33000u, 20u);

    /* Every code the gateway can answer with, including the ones this file
     * does not originate — building the frame is the same operation. */
    const uint8_t codes[] = { 0x01u, 0x02u, 0x03u, 0x04u, 0x06u, 0x0Au, 0x0Bu };

    for (uint32_t i = 0; i < sizeof(codes); i++) {
        TEST_ASSERT(Mbap_BuildException(req, codes[i], rsp) == 9u);
        TEST_ASSERT(rd16(&rsp[0]) == 0xBEEFu);   /* txn id echoed          */
        TEST_ASSERT(rd16(&rsp[2]) == 0u);
        TEST_ASSERT(rd16(&rsp[4]) == 3u);        /* unit + fc + code       */
        TEST_ASSERT(rsp[6] == 7u);               /* unit id echoed         */
        TEST_ASSERT(rsp[7] == (4u | 0x80u));     /* fc with the high bit   */
        TEST_ASSERT(rsp[8] == codes[i]);
    }

    /* An exception to an FC16 marks FC16, not the read code. */
    uint16_t vals[2] = { 1u, 2u };
    (void)client_write_multi(req, 0x0042u, 1u, 44100u, vals, 2u);
    TEST_ASSERT(Mbap_BuildException(req, 0x0Bu, rsp) == 9u);
    TEST_ASSERT(rsp[7] == (16u | 0x80u));
    TEST_ASSERT(rd16(&rsp[0]) == 0x0042u);
}

/* pymodbus matches a reply to its request by transaction id and drops
 * anything else, so an id that did not survive the round trip is a stall on
 * the far end rather than a visible error. */
static void test_transaction_ids_survive(void)
{
    const uint16_t ids[] = { 0u, 1u, 0x00FFu, 0x0100u, 0x7FFFu, 0x8000u, 0xFFFFu };
    uint8_t        req[MBAP_MAX_FRAME];
    uint8_t        rsp[MBAP_MAX_FRAME];
    uint16_t       regs[4] = { 1u, 2u, 3u, 4u };

    for (uint32_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        sMbapReq r;

        (void)client_read(req, ids[i], 1u, 4u, 33000u, 4u);
        TEST_ASSERT(Mbap_Decode(req, 6u, &r) == 0u);
        TEST_ASSERT(r.txnId == ids[i]);

        (void)Mbap_BuildReadReply(&r, regs, rsp);
        TEST_ASSERT(rd16(&rsp[0]) == ids[i]);

        TEST_ASSERT(Mbap_BuildException(req, 0x06u, rsp) == 9u);
        TEST_ASSERT(rd16(&rsp[0]) == ids[i]);
    }
}

/* The unit id is the slave address and the board resolves a device from it, so
 * it must survive decode untouched — including 0, which some clients use as
 * "broadcast" and which must reach the device lookup rather than being
 * special-cased here. */
static void test_unit_ids_pass_through(void)
{
    uint8_t  req[MBAP_MAX_FRAME];
    sMbapReq r;

    for (uint32_t u = 0; u <= 255u; u++) {
        (void)client_read(req, 1u, (uint8_t)u, 4u, 33000u, 1u);
        TEST_ASSERT(Mbap_Decode(req, 6u, &r) == 0u);
        TEST_ASSERT(r.unit == (uint8_t)u);
    }
}

int main(void)
{
    RUN_TEST(test_every_solis_group_round_trips);
    RUN_TEST(test_largest_frames_fit);
    RUN_TEST(test_remote_dispatch_fc16_blocks);
    RUN_TEST(test_single_register_controls);
    RUN_TEST(test_header_rejects);
    RUN_TEST(test_decode_rejects);
    RUN_TEST(test_unknown_addresses_are_accepted_for_the_wire);
    RUN_TEST(test_exception_frame);
    RUN_TEST(test_transaction_ids_survive);
    RUN_TEST(test_unit_ids_pass_through);
    return test_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
