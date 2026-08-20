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

static int compile_to(const char *json, uint32_t base, sModbusCompileResult *res)
{
    sMemSrc src = { json, (uint32_t)strlen(json), 0 };
    return MbCfgCompile(mem_src, &src, base, NULL, res);
}

/* A config exercising every optional field the exporter must reproduce: the
 * dialect group, all three access forms, bounds present and ABSENT, ascii
 * length, bitfield, both endiannesses, holding+input, per-device line
 * parameters, and plans in non-contiguous slots.
 *
 * A field the exporter forgets is invisible until someone downloads a config
 * and re-uploads it, which is exactly what this test does (§9). */
static const char FULL_FEATURED[] =
    "{\"capabilities\":["
    "{\"id\":0,\"name\":\"inv\",\"addrStride\":1,\"writeFc\":6,"
     "\"maxReadRegs\":100,"
     "\"blocks\":[{\"base\":3000,\"regs\":32},{\"base\":43000,\"regs\":8}],"
     "\"points\":["
      "{\"id\":0,\"addr\":3000,\"fc\":\"input\",\"decodeType\":\"u16\","
       "\"scale\":0.1,\"unit\":\"V\",\"name\":\"voltage\"},"
      "{\"id\":1,\"addr\":3001,\"fc\":\"input\",\"decodeType\":\"s16\","
       "\"scale\":0.1,\"unit\":\"A\",\"name\":\"current\"},"
      "{\"id\":2,\"addr\":3002,\"fc\":\"input\",\"decodeType\":\"u32_be\","
       "\"scale\":1,\"unit\":\"Wh\",\"name\":\"energy\"},"
      "{\"id\":3,\"addr\":3004,\"fc\":\"input\",\"decodeType\":\"s32_le\","
       "\"scale\":1,\"unit\":\"W\",\"name\":\"power\"},"
      "{\"id\":4,\"addr\":3006,\"fc\":\"input\",\"decodeType\":\"bitfield\","
       "\"scale\":1,\"unit\":\"\",\"name\":\"status_bits\"},"
      "{\"id\":5,\"addr\":3007,\"fc\":\"input\",\"decodeType\":\"ascii\","
       "\"scale\":1,\"unit\":\"\",\"name\":\"serial\",\"length\":8},"
      "{\"id\":6,\"addr\":43000,\"fc\":\"holding\",\"decodeType\":\"u16\","
       "\"scale\":1,\"unit\":\"%\",\"name\":\"soc_limit\",\"access\":\"rw\","
       "\"writeMin\":5,\"writeMax\":40},"
      "{\"id\":7,\"addr\":43001,\"fc\":\"holding\",\"decodeType\":\"u16\","
       "\"scale\":1,\"unit\":\"%\",\"name\":\"free_knob\",\"access\":\"rw\"},"
      "{\"id\":8,\"addr\":43002,\"fc\":\"holding\",\"decodeType\":\"u16\","
       "\"scale\":1,\"unit\":\"%\",\"name\":\"write_only\",\"access\":\"w\"}"
     "]},"
    "{\"id\":1,\"name\":\"meter\",\"addrStride\":1,\"writeFc\":16,"
     "\"maxReadRegs\":125,\"blocks\":[{\"base\":0,\"regs\":8}],"
     "\"points\":["
      "{\"id\":0,\"addr\":0,\"fc\":\"input\",\"decodeType\":\"float32_be\","
       "\"scale\":0.01,\"unit\":\"kWh\",\"name\":\"total\"},"
      "{\"id\":1,\"addr\":2,\"fc\":\"input\",\"decodeType\":\"float32_le\","
       "\"scale\":1,\"unit\":\"Hz\",\"name\":\"freq\"}"
     "]}"
    "],"
    "\"devices\":["
    "{\"id\":0,\"slaveAddr\":1,\"capability\":0,\"baud\":19200,"
     "\"format\":\"8E1\",\"port\":\"rs485\",\"topicPrefix\":\"inv1\"},"
    "{\"id\":1,\"slaveAddr\":2,\"capability\":0,\"baud\":9600,"
     "\"format\":\"8N1\",\"port\":\"test\",\"topicPrefix\":\"inv2\"},"
    "{\"id\":2,\"slaveAddr\":7,\"capability\":1,\"baud\":115200,"
     "\"format\":\"8N2\",\"port\":\"rs485\",\"topicPrefix\":\"meter\"}"
    "],"
    "\"plans\":["
    "{\"id\":0,\"name\":\"inv_fast\",\"capability\":0,\"devices\":[0,1],"
     "\"timeTables\":[{\"id\":0,\"everySec\":5,\"points\":[0,1,2,3]},"
                     "{\"id\":1,\"everySec\":86400,\"points\":[5]}]},"
    "{\"id\":4,\"name\":\"inv_slow\",\"capability\":0,\"devices\":[],"
     "\"timeTables\":[{\"id\":0,\"everySec\":600,\"points\":[4,6]}]},"
    "{\"id\":7,\"name\":\"meter\",\"capability\":1,\"devices\":[2],"
     "\"timeTables\":[]}"
    "]}";

static void test_export_round_trip(void)
{
    sModbusCompileResult res;
    sStrSink         json1 = {0};

    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);

    /* compile original -> region A */
    TEST_ASSERT(compile_to(FULL_FEATURED, nvdbUser_modbusLutA,
                           &res) == 0);
    TEST_ASSERT(res.counts.capabilities == 2);
    TEST_ASSERT(res.counts.devices == 3);
    TEST_ASSERT(res.counts.plans == 3);
    TEST_ASSERT(res.counts.points == 11);

    /* export region A */
    TEST_ASSERT(MbCfgExport(nvdbUser_modbusLutA,
                            str_sink, &json1) == 0);
    TEST_ASSERT(json1.len > 0);

    /* re-compile the export -> region B */
    TEST_ASSERT(compile_to(json1.buf, nvdbUser_modbusLutB,
                           &res) == 0);

    /* record streams must be byte-identical */
    sModbusLutHeader ha, hb;
    NvDb_Read(nvdbUser_modbusLutA, &ha, 0u, sizeof(ha));
    NvDb_Read(nvdbUser_modbusLutB, &hb, 0u, sizeof(hb));
    TEST_ASSERT(ha.streamLen == hb.streamLen);
    TEST_ASSERT(ha.crc32 == hb.crc32);

    static uint8_t sa[8192], sb[8192];
    TEST_ASSERT(ha.streamLen <= sizeof(sa));
    NvDb_Read(nvdbUser_modbusLutA, sa, MODBUS_LUT_HEADER_SIZE, ha.streamLen);
    NvDb_Read(nvdbUser_modbusLutB, sb, MODBUS_LUT_HEADER_SIZE, hb.streamLen);
    TEST_ASSERT_MEM_EQ(sa, sb, ha.streamLen);

    /* and a second export must reproduce the same JSON */
    sStrSink json2 = {0};
    TEST_ASSERT(MbCfgExport(nvdbUser_modbusLutB,
                            str_sink, &json2) == 0);
    TEST_ASSERT(json1.len == json2.len);
    TEST_ASSERT(strcmp(json1.buf, json2.buf) == 0);
}

static void test_export_invalid_region(void)
{
    sStrSink out = {0};
    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);
    TEST_ASSERT(MbCfgExport(nvdbUser_modbusLutA, str_sink, &out) != 0);
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

/* Encode is decode's inverse: every writable type must round-trip. */
static void test_encode_round_trip(void)
{
    static const uint8_t types[] = {
        mbDecode_u16, mbDecode_s16, mbDecode_bitfield,
        mbDecode_u32Be, mbDecode_u32Le, mbDecode_s32Be, mbDecode_s32Le,
    };
    static const int32_t values[] = { 0, 1, 85, 32767, -1, -32768, 100000,
                                      -100000 };

    for (unsigned t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
        sModbusPointRecord p = mkpt(types[t], 0, 0);

        for (unsigned v = 0; v < sizeof(values) / sizeof(values[0]); v++) {
            uint16_t regs[2] = { 0, 0 };
            int      n = MbEncode_Scaled(&p, values[v], regs);

            if (n < 0) {
                continue;              /* out of this type's range: refused */
            }
            TEST_ASSERT(MbDecode_Scaled(&p, regs) == values[v]);
        }
    }
}

/* A value that does not fit is REFUSED, never truncated (§5.3). */
static void test_encode_refuses_what_does_not_fit(void)
{
    uint16_t regs[2];

    sModbusPointRecord u16 = mkpt(mbDecode_u16, 0, 0);
    TEST_ASSERT(MbEncode_Scaled(&u16, 65535, regs) == 1);
    TEST_ASSERT(MbEncode_Scaled(&u16, 65536, regs) < 0);
    TEST_ASSERT(MbEncode_Scaled(&u16, -1, regs) < 0);

    sModbusPointRecord s16 = mkpt(mbDecode_s16, 0, 0);
    TEST_ASSERT(MbEncode_Scaled(&s16, -32768, regs) == 1);
    TEST_ASSERT(MbEncode_Scaled(&s16, 32768, regs) < 0);

    sModbusPointRecord u32 = mkpt(mbDecode_u32Be, 0, 0);
    TEST_ASSERT(MbEncode_Scaled(&u32, -1, regs) < 0);

    /* ascii and float32 have no inverse at all. */
    sModbusPointRecord asc = mkpt(mbDecode_ascii, 0, 4);
    sModbusPointRecord flt = mkpt(mbDecode_float32Be, 0, 0);
    TEST_ASSERT(MbEncode_Scaled(&asc, 1, regs) < 0);
    TEST_ASSERT(MbEncode_Scaled(&flt, 1, regs) < 0);
}

/* Word order is the SAME rule decode uses — a be/le mix-up would round-trip
 * against itself and still be wrong on the wire, so pin the bytes. */
static void test_encode_word_order(void)
{
    uint16_t regs[2];

    sModbusPointRecord be = mkpt(mbDecode_u32Be, 0, 0);
    TEST_ASSERT(MbEncode_Scaled(&be, 0x12345678, regs) == 2);
    TEST_ASSERT(regs[0] == 0x1234 && regs[1] == 0x5678);

    sModbusPointRecord le = mkpt(mbDecode_u32Le, 0, 0);
    TEST_ASSERT(MbEncode_Scaled(&le, 0x12345678, regs) == 2);
    TEST_ASSERT(regs[0] == 0x5678 && regs[1] == 0x1234);
}

int main(void)
{
    RUN_TEST(test_export_round_trip);
    RUN_TEST(test_export_invalid_region);
    RUN_TEST(test_encode_round_trip);
    RUN_TEST(test_encode_refuses_what_does_not_fit);
    RUN_TEST(test_encode_word_order);
    RUN_TEST(test_decode_integers);
    RUN_TEST(test_decode_float_quantize);
    RUN_TEST(test_decode_ascii);
    RUN_TEST(test_format_scaled);
    RUN_TEST(test_parse_scaled);
    return test_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
