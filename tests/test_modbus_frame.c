/**
 * Unit tests for Shared/Modbus/modbus_frame.c.
 *
 * CRC and exception handling are exactly the code that should never first be
 * exercised against a live slave, which is why frame build/parse is a pure
 * function in Shared/ (docs/modbus.md §2.2).
 *
 * The read vector is the one the integration suite injects, so the two agree
 * about what a 20-register reply at 3132 looks like.
 */

#include "test_util.h"
#include "modbus_frame.h"

#include <stdlib.h>
#include <string.h>

static void test_crc_vector(void)
{
    /* Vectors computed independently (poly 0xA001, init 0xFFFF, LSB-first).
     * The value is the CRC register; on the wire it goes low byte first. */
    const uint8_t a[] = { 0x01, 0x04, 0x0C, 0x3C, 0x00, 0x02 };
    const uint8_t b[] = { 0x01, 0x03, 0x00, 0x01, 0x00, 0x01 };
    const uint8_t c[] = { 0x02, 0x07 };

    TEST_ASSERT(MbFrame_Crc16(a, sizeof(a)) == 0x97B2u);
    TEST_ASSERT(MbFrame_Crc16(b, sizeof(b)) == 0xCAD5u);
    TEST_ASSERT(MbFrame_Crc16(c, sizeof(c)) == 0x1241u);

    /* An empty message is the seed. */
    TEST_ASSERT(MbFrame_Crc16(a, 0) == 0xFFFFu);
}

static void test_build_read(void)
{
    uint8_t buf[16];
    int     n = MbFrame_BuildRead(1, 0x04, 3132, 20, buf, sizeof(buf));

    TEST_ASSERT(n == 8);
    TEST_ASSERT(buf[0] == 1 && buf[1] == 0x04);
    TEST_ASSERT(buf[2] == 0x0C && buf[3] == 0x3C);      /* 3132 = 0x0C3C */
    TEST_ASSERT(buf[4] == 0x00 && buf[5] == 20);
    /* The CRC seals what precedes it, so re-running it over the whole frame
     * (CRC included) yields zero — the property the parser relies on. */
    TEST_ASSERT(MbFrame_Crc16(buf, 6) ==
                ((uint16_t)buf[6] | (uint16_t)(buf[7] << 8)));

    TEST_ASSERT(MbFrame_BuildRead(1, 0x06, 0, 1, buf, sizeof(buf)) ==
                mbFrame_errBadArg);
    TEST_ASSERT(MbFrame_BuildRead(1, 0x04, 0, 0, buf, sizeof(buf)) ==
                mbFrame_errBadArg);
    TEST_ASSERT(MbFrame_BuildRead(1, 0x04, 0, 1, buf, 4) ==
                mbFrame_errBadArg);
}

static void test_build_writes(void)
{
    uint8_t buf[32];

    TEST_ASSERT(MbFrame_BuildWrite(1, 3009, 95, buf, sizeof(buf)) == 8);
    TEST_ASSERT(buf[1] == 0x06);
    TEST_ASSERT(buf[2] == 0x0B && buf[3] == 0xC1);      /* 3009 = 0x0BC1 */
    TEST_ASSERT(buf[4] == 0x00 && buf[5] == 95);

    /* FC16 for a slave with no FC06 handler — the JK's dialect. */
    const uint16_t vals[] = { 0x1234, 0x5678 };
    int n = MbFrame_BuildWriteMulti(2, 0x1000, vals, 2, buf, sizeof(buf));
    TEST_ASSERT(n == 9 + 4);
    TEST_ASSERT(buf[1] == 0x10);
    TEST_ASSERT(buf[4] == 0 && buf[5] == 2);            /* count            */
    TEST_ASSERT(buf[6] == 4);                           /* byte count       */
    TEST_ASSERT(buf[7] == 0x12 && buf[8] == 0x34);
    TEST_ASSERT(buf[9] == 0x56 && buf[10] == 0x78);
}

/* Build a valid reply carrying `count` registers, for the parser tests. */
static uint16_t make_read_reply(uint8_t *out, uint8_t slave, uint8_t fc,
                                const uint16_t *regs, uint8_t count)
{
    uint16_t crc;
    int      n = 0;

    out[n++] = slave;
    out[n++] = fc;
    out[n++] = (uint8_t)(count * 2u);
    for (uint8_t i = 0; i < count; i++) {
        out[n++] = (uint8_t)(regs[i] >> 8);
        out[n++] = (uint8_t)(regs[i] & 0xFFu);
    }
    crc = MbFrame_Crc16(out, (size_t)n);
    out[n++] = (uint8_t)(crc & 0xFFu);
    out[n++] = (uint8_t)(crc >> 8);
    return (uint16_t)n;
}

static void test_parse_read_ok(void)
{
    const uint16_t src[] = { 0x0200, 0xFFCE, 0x0000 };
    uint8_t        frame[32];
    uint16_t       len = make_read_reply(frame, 1, 0x04, src, 3);
    uint16_t       regs[3] = { 0, 0, 0 };
    uint8_t        exc = 0xEE;

    TEST_ASSERT(MbFrame_ParseRead(frame, len, 1, 0x04, 3, regs, &exc) ==
                mbFrame_ok);
    TEST_ASSERT(regs[0] == 0x0200);
    TEST_ASSERT(regs[1] == 0xFFCE);       /* -50 as s16 */
    TEST_ASSERT(regs[2] == 0x0000);
    TEST_ASSERT(exc == 0);
}

static void test_parse_read_rejects(void)
{
    const uint16_t src[] = { 1, 2, 3 };
    uint8_t        frame[32];
    uint16_t       len = make_read_reply(frame, 1, 0x04, src, 3);
    uint16_t       regs[3];

    /* Truncated */
    TEST_ASSERT(MbFrame_ParseRead(frame, 3, 1, 0x04, 3, regs, NULL) ==
                mbFrame_errShort);

    /* One flipped payload bit fails the CRC */
    uint8_t bad[32];
    memcpy(bad, frame, len);
    bad[4] ^= 0x01u;
    TEST_ASSERT(MbFrame_ParseRead(bad, len, 1, 0x04, 3, regs, NULL) ==
                mbFrame_errCrc);

    /* A different slave answering is not our reply */
    TEST_ASSERT(MbFrame_ParseRead(frame, len, 2, 0x04, 3, regs, NULL) ==
                mbFrame_errAddr);

    /* A reply to a different function code */
    TEST_ASSERT(MbFrame_ParseRead(frame, len, 1, 0x03, 3, regs, NULL) ==
                mbFrame_errFunction);

    /* Fewer registers than asked for: the byte count disagrees */
    TEST_ASSERT(MbFrame_ParseRead(frame, len, 1, 0x04, 2, regs, NULL) ==
                mbFrame_errCount);

    /* More than the frame holds */
    TEST_ASSERT(MbFrame_ParseRead(frame, len, 1, 0x04, 8, regs, NULL) ==
                mbFrame_errShort);
}

static void test_parse_exception(void)
{
    uint8_t  frame[8];
    uint16_t crc;
    uint8_t  exc = 0;
    uint16_t regs[4];

    frame[0] = 1;
    frame[1] = 0x84u;                     /* FC04 + error bit */
    frame[2] = 2;                         /* illegal data address */
    crc = MbFrame_Crc16(frame, 3);
    frame[3] = (uint8_t)(crc & 0xFFu);
    frame[4] = (uint8_t)(crc >> 8);

    TEST_ASSERT(MbFrame_ParseRead(frame, 5, 1, 0x04, 4, regs, &exc) ==
                mbFrame_exception);
    TEST_ASSERT(exc == 2);

    /* A device answering exceptions IS answering — the code survives so the
     * per-item result can say which one it got (§4.6). */
    frame[2] = 1;
    crc = MbFrame_Crc16(frame, 3);
    frame[3] = (uint8_t)(crc & 0xFFu);
    frame[4] = (uint8_t)(crc >> 8);
    TEST_ASSERT(MbFrame_ParseRead(frame, 5, 1, 0x04, 4, regs, &exc) ==
                mbFrame_exception);
    TEST_ASSERT(exc == 1);
}

static void test_parse_write(void)
{
    uint8_t buf[16];
    int     n = MbFrame_BuildWrite(1, 3009, 95, buf, sizeof(buf));

    /* An FC06 slave echoes the request verbatim. */
    TEST_ASSERT(MbFrame_ParseWrite(buf, (uint16_t)n, 1, 0x06, NULL) ==
                mbFrame_ok);
    TEST_ASSERT(MbFrame_ParseWrite(buf, (uint16_t)n, 2, 0x06, NULL) ==
                mbFrame_errAddr);
    TEST_ASSERT(MbFrame_ParseWrite(buf, 5, 1, 0x06, NULL) != mbFrame_ok);
}

/* The exact frame tests/integration injects, parsed here so the two agree. */
static void test_integration_vector(void)
{
    static const uint8_t frame[] = {
        0x01, 0x04, 0x28,
        0x02, 0x00, 0xff, 0xce, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x55, 0x00, 0x63, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x86, 0x68
    };
    uint16_t regs[20];

    TEST_ASSERT(sizeof(frame) == 45);
    TEST_ASSERT(MbFrame_ParseRead(frame, sizeof(frame), 1, 0x04, 20, regs,
                                  NULL) == mbFrame_ok);
    TEST_ASSERT(regs[0] == 512);          /* 51.2 V at scale 0.1 */
    TEST_ASSERT((int16_t)regs[1] == -50); /* -5.0 A              */
    TEST_ASSERT(regs[6] == 85);           /* SOC                 */
}

int main(void)
{
    RUN_TEST(test_crc_vector);
    RUN_TEST(test_build_read);
    RUN_TEST(test_build_writes);
    RUN_TEST(test_parse_read_ok);
    RUN_TEST(test_parse_read_rejects);
    RUN_TEST(test_parse_exception);
    RUN_TEST(test_parse_write);
    RUN_TEST(test_integration_vector);
    return test_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
