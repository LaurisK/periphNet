/**
 * Unit tests for modbus_config_export.c and modbus_decode.c:
 * compile -> export -> re-compile must yield a byte-identical record stream
 * (the design's data-faithful round trip), plus decode/format/parse vectors
 * for every decode type.
 */

#include "test_util.h"
#include "modbus_test_stream.h"
#include "modbus_config_compiler.h"
#include "modbus_config_export.h"
#include "modbus_decode.h"

#include <stdlib.h>

/* ============================================================================
 * Round trip
 * ============================================================================ */

typedef struct {
    char     buf[8192];
    uint32_t len;
    int      full;
} sStrSink;

static int str_sink(void *ctx, const char *data, uint32_t len)
{
    sStrSink *s = (sStrSink *)ctx;
    if (s->len + len >= sizeof(s->buf)) {
        s->full = 1;
        return -1;
    }
    memcpy(&s->buf[s->len], data, len);
    s->len += len;
    s->buf[s->len] = '\0';
    return 0;
}

typedef struct {
    const char *data;
    uint32_t    len;
    uint32_t    pos;
} sMemSrc;

static int mem_src(void *ctx, uint8_t *buf, uint32_t maxLen)
{
    sMemSrc *m = (sMemSrc *)ctx;
    uint32_t n = m->len - m->pos;
    if (n > maxLen) {
        n = maxLen;
    }
    memcpy(buf, &m->data[m->pos], n);
    m->pos += n;
    return (int)n;
}

static int compile_to(const char *json, uint32_t base, sMbCompileResult *res)
{
    sMemSrc src = { json, (uint32_t)strlen(json), 0 };
    return MbCfgCompile(mem_src, &src, base, NULL, res);
}

/* A config exercising every optional field: publish, writable with and
 * without range, ascii length, bitfield, both endiannesses, holding+input */
static const char FULL_FEATURED[] =
    "{\"devices\":["
    "{\"slaveAddr\":1,\"topicPrefix\":\"inv\",\"transactions\":["
      "{\"startAddr\":3000,\"functionCode\":\"input\",\"readPeriodS\":5,"
       "\"points\":["
        "{\"offset\":0,\"decodeType\":\"u16\",\"scale\":0.1,\"unit\":\"V\","
         "\"name\":\"voltage\"},"
        "{\"offset\":1,\"decodeType\":\"s16\",\"scale\":0.1,\"unit\":\"A\","
         "\"name\":\"current\",\"publish\":{\"threshold\":5}},"
        "{\"offset\":2,\"decodeType\":\"u32_be\",\"scale\":1,\"unit\":\"Wh\","
         "\"name\":\"energy\",\"publish\":{\"heartbeatS\":600}},"
        "{\"offset\":4,\"decodeType\":\"s32_le\",\"scale\":1,\"unit\":\"W\","
         "\"name\":\"power\",\"publish\":{\"threshold\":10,\"heartbeatS\":60}},"
        "{\"offset\":6,\"decodeType\":\"bitfield\",\"scale\":1,\"unit\":\"\","
         "\"name\":\"status_bits\"},"
        "{\"offset\":7,\"decodeType\":\"ascii\",\"scale\":1,\"unit\":\"\","
         "\"name\":\"serial\",\"length\":8}"
       "]},"
      "{\"startAddr\":43000,\"functionCode\":\"holding\",\"readPeriodS\":60,"
       "\"points\":["
        "{\"offset\":0,\"decodeType\":\"u16\",\"scale\":1,\"unit\":\"%\","
         "\"name\":\"soc_limit\",\"writable\":true,"
         "\"writeMin\":5,\"writeMax\":40},"
        "{\"offset\":1,\"decodeType\":\"u16\",\"scale\":1,\"unit\":\"%\","
         "\"name\":\"free_knob\",\"writable\":true}"
       "]}"
    "]},"
    "{\"slaveAddr\":7,\"topicPrefix\":\"meter\",\"transactions\":["
      "{\"startAddr\":0,\"functionCode\":\"input\",\"readPeriodS\":2,"
       "\"points\":["
        "{\"offset\":0,\"decodeType\":\"float32_be\",\"scale\":0.01,"
         "\"unit\":\"kWh\",\"name\":\"total\"},"
        "{\"offset\":2,\"decodeType\":\"float32_le\",\"scale\":1,"
         "\"unit\":\"Hz\",\"name\":\"freq\"}"
       "]}"
    "]}"
    "]}";

static void test_export_round_trip(void)
{
    sMbCompileResult res;
    sStrSink         json1 = {0};

    mock_flash_reset();

    /* compile original -> region A */
    TEST_ASSERT(compile_to(FULL_FEATURED, EXT_FLASH_MODBUS_LUT_A_ADDR,
                           &res) == 0);
    TEST_ASSERT(res.counts.devices == 2 && res.counts.transactions == 3 &&
                res.counts.points == 10);

    /* export region A */
    TEST_ASSERT(MbCfgExport(EXT_FLASH_MODBUS_LUT_A_ADDR,
                            str_sink, &json1) == 0);
    TEST_ASSERT(json1.len > 0);

    /* re-compile the export -> region B */
    TEST_ASSERT(compile_to(json1.buf, EXT_FLASH_MODBUS_LUT_B_ADDR,
                           &res) == 0);

    /* record streams must be byte-identical */
    sModbusLutHeader ha, hb;
    W25Q128_Read(EXT_FLASH_MODBUS_LUT_A_ADDR, (uint8_t *)&ha, sizeof(ha));
    W25Q128_Read(EXT_FLASH_MODBUS_LUT_B_ADDR, (uint8_t *)&hb, sizeof(hb));
    TEST_ASSERT(ha.streamLen == hb.streamLen);
    TEST_ASSERT(ha.crc32 == hb.crc32);

    static uint8_t sa[8192], sb[8192];
    TEST_ASSERT(ha.streamLen <= sizeof(sa));
    W25Q128_Read(EXT_FLASH_MODBUS_LUT_A_ADDR + MODBUS_LUT_HEADER_SIZE,
                 sa, ha.streamLen);
    W25Q128_Read(EXT_FLASH_MODBUS_LUT_B_ADDR + MODBUS_LUT_HEADER_SIZE,
                 sb, hb.streamLen);
    TEST_ASSERT_MEM_EQ(sa, sb, ha.streamLen);

    /* and a second export must reproduce the same JSON */
    sStrSink json2 = {0};
    TEST_ASSERT(MbCfgExport(EXT_FLASH_MODBUS_LUT_B_ADDR,
                            str_sink, &json2) == 0);
    TEST_ASSERT(json1.len == json2.len);
    TEST_ASSERT(strcmp(json1.buf, json2.buf) == 0);
}

static void test_export_invalid_region(void)
{
    sStrSink out = {0};
    mock_flash_reset();
    TEST_ASSERT(MbCfgExport(EXT_FLASH_MODBUS_LUT_A_ADDR, str_sink, &out) != 0);
}

/* ============================================================================
 * Decode vectors
 * ============================================================================ */

static sModbusPointRecord mkpt(uint8_t type, int8_t pow10, uint8_t len)
{
    sModbusPointRecord p;
    memset(&p, 0, sizeof(p));
    p.decodeType = type;
    p.scalePow10 = pow10;
    p.length     = len;
    return p;
}

static void test_decode_integers(void)
{
    sModbusPointRecord p;
    uint16_t           regs[2];

    p = mkpt(mbDecode_u16, -1, 0);
    regs[0] = 512;
    TEST_ASSERT(MbDecode_Scaled(&p, regs) == 512);

    p = mkpt(mbDecode_s16, -1, 0);
    regs[0] = 0xFFF6;                        /* -10 */
    TEST_ASSERT(MbDecode_Scaled(&p, regs) == -10);

    p = mkpt(mbDecode_bitfield, 0, 0);
    regs[0] = 0xABCD;
    TEST_ASSERT(MbDecode_Scaled(&p, regs) == 0xABCD);

    p = mkpt(mbDecode_u32Be, 0, 0);
    regs[0] = 0x0001; regs[1] = 0x0002;      /* 0x00010002 */
    TEST_ASSERT(MbDecode_Scaled(&p, regs) == 0x10002);

    p = mkpt(mbDecode_u32Le, 0, 0);
    TEST_ASSERT(MbDecode_Scaled(&p, regs) == 0x20001);

    p = mkpt(mbDecode_s32Be, 0, 0);
    regs[0] = 0xFFFF; regs[1] = 0xFFFE;      /* -2 */
    TEST_ASSERT(MbDecode_Scaled(&p, regs) == -2);

    p = mkpt(mbDecode_s32Le, 0, 0);
    regs[0] = 0xFFFE; regs[1] = 0xFFFF;      /* -2, word-swapped */
    TEST_ASSERT(MbDecode_Scaled(&p, regs) == -2);

    /* u32 above int32 clamps */
    p = mkpt(mbDecode_u32Be, 0, 0);
    regs[0] = 0xFFFF; regs[1] = 0xFFFF;
    TEST_ASSERT(MbDecode_Scaled(&p, regs) == INT32_MAX);
}

static void test_decode_float_quantize(void)
{
    sModbusPointRecord p;
    uint16_t           regs[2];
    float              f;
    uint32_t           bits;

    /* 51.25 W at scale 0.1 -> scaled 512 (round-to-nearest 512.5 -> 513) */
    f = 51.25f;
    memcpy(&bits, &f, sizeof(bits));
    regs[0] = (uint16_t)(bits >> 16);
    regs[1] = (uint16_t)bits;
    p = mkpt(mbDecode_float32Be, -1, 0);
    TEST_ASSERT(MbDecode_Scaled(&p, regs) == 513);

    /* same value little-word order */
    regs[1] = (uint16_t)(bits >> 16);
    regs[0] = (uint16_t)bits;
    p = mkpt(mbDecode_float32Le, -1, 0);
    TEST_ASSERT(MbDecode_Scaled(&p, regs) == 513);

    /* negative with scale 1 */
    f = -230.4f;
    memcpy(&bits, &f, sizeof(bits));
    regs[0] = (uint16_t)(bits >> 16);
    regs[1] = (uint16_t)bits;
    p = mkpt(mbDecode_float32Be, 0, 0);
    TEST_ASSERT(MbDecode_Scaled(&p, regs) == -230);

    /* scale 10: 1500.0 -> scaled 150 */
    f = 1500.0f;
    memcpy(&bits, &f, sizeof(bits));
    regs[0] = (uint16_t)(bits >> 16);
    regs[1] = (uint16_t)bits;
    p = mkpt(mbDecode_float32Be, 1, 0);
    TEST_ASSERT(MbDecode_Scaled(&p, regs) == 150);
}

static void test_decode_ascii(void)
{
    sModbusPointRecord p = mkpt(mbDecode_ascii, 0, 4);
    uint16_t regs[4] = { ('S' << 8) | 'N', ('1' << 8) | '2',
                         ('3' << 8) | ' ', (' ' << 8) | ' ' };
    char out[16];

    TEST_ASSERT(MbDecode_Ascii(&p, regs, out, sizeof(out)) == 5);
    TEST_ASSERT(strcmp(out, "SN123") == 0);

    char tiny[4];
    TEST_ASSERT(MbDecode_Ascii(&p, regs, tiny, sizeof(tiny)) == -1);
}

static void test_format_scaled(void)
{
    char buf[24];

    MbFormat_Scaled(buf, sizeof(buf), 512, -1);
    TEST_ASSERT(strcmp(buf, "51.2") == 0);
    MbFormat_Scaled(buf, sizeof(buf), -512, -1);
    TEST_ASSERT(strcmp(buf, "-51.2") == 0);
    MbFormat_Scaled(buf, sizeof(buf), -5, -1);
    TEST_ASSERT(strcmp(buf, "-0.5") == 0);
    MbFormat_Scaled(buf, sizeof(buf), 12345, -2);
    TEST_ASSERT(strcmp(buf, "123.45") == 0);
    MbFormat_Scaled(buf, sizeof(buf), 7, -3);
    TEST_ASSERT(strcmp(buf, "0.007") == 0);
    MbFormat_Scaled(buf, sizeof(buf), 42, 0);
    TEST_ASSERT(strcmp(buf, "42") == 0);
    MbFormat_Scaled(buf, sizeof(buf), 42, 2);
    TEST_ASSERT(strcmp(buf, "4200") == 0);
    MbFormat_Scaled(buf, sizeof(buf), -42, 1);
    TEST_ASSERT(strcmp(buf, "-420") == 0);
}

static void test_parse_scaled(void)
{
    int32_t v;

    TEST_ASSERT(MbParse_Scaled("51.2", -1, &v) == 0 && v == 512);
    TEST_ASSERT(MbParse_Scaled("-51.2", -1, &v) == 0 && v == -512);
    TEST_ASSERT(MbParse_Scaled("51", -1, &v) == 0 && v == 510);
    TEST_ASSERT(MbParse_Scaled("51.20", -1, &v) == 0 && v == 512);
    TEST_ASSERT(MbParse_Scaled("123.45", -2, &v) == 0 && v == 12345);
    TEST_ASSERT(MbParse_Scaled("42", 0, &v) == 0 && v == 42);
    TEST_ASSERT(MbParse_Scaled("4200", 2, &v) == 0 && v == 42);

    /* not representable at this scale */
    TEST_ASSERT(MbParse_Scaled("51.25", -1, &v) != 0);
    TEST_ASSERT(MbParse_Scaled("4210", 2, &v) != 0);
    TEST_ASSERT(MbParse_Scaled("1.5", 0, &v) != 0);
    /* junk */
    TEST_ASSERT(MbParse_Scaled("", -1, &v) != 0);
    TEST_ASSERT(MbParse_Scaled("12a", -1, &v) != 0);
    TEST_ASSERT(MbParse_Scaled("-", -1, &v) != 0);
}

int main(void)
{
    RUN_TEST(test_export_round_trip);
    RUN_TEST(test_export_invalid_region);
    RUN_TEST(test_decode_integers);
    RUN_TEST(test_decode_float_quantize);
    RUN_TEST(test_decode_ascii);
    RUN_TEST(test_format_scaled);
    RUN_TEST(test_parse_scaled);
    return test_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
