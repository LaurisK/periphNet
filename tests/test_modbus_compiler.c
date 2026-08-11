/**
 * Unit tests for Shared/Modbus/modbus_config_compiler.c: the design-doc §2
 * worked example must compile byte-identically to the hand-assembled record
 * stream (any chunking of the byte source), and every §4 validation rule
 * must reject with the exact {device, txn, point, field} location.
 */

#include "test_util.h"
#include "modbus_test_stream.h"
#include "modbus_config_compiler.h"

#include <stdio.h>
#include <stdlib.h>

/* ============================================================================
 * Memory byte source with configurable chunk size
 * ============================================================================ */

typedef struct {
    const char *data;
    uint32_t    len;
    uint32_t    pos;
    uint32_t    chunk;      /* max bytes per read; 0 = unlimited */
} sMemSource;

static int mem_source(void *ctx, uint8_t *buf, uint32_t maxLen)
{
    sMemSource *m = (sMemSource *)ctx;
    uint32_t    n = m->len - m->pos;

    if (n > maxLen) {
        n = maxLen;
    }
    if (m->chunk != 0u && n > m->chunk) {
        n = m->chunk;
    }
    memcpy(buf, &m->data[m->pos], n);
    m->pos += n;
    return (int)n;
}

static int compile_str(const char *json, uint32_t chunk, sMbCompileResult *res)
{
    sMemSource src = { json, (uint32_t)strlen(json), 0, chunk };
    return MbCfgCompile(mem_source, &src, EXT_FLASH_MODBUS_LUT_A_ADDR,
                        NULL, res);
}

/* The design-doc §2 worked example, verbatim structure */
static const char WORKED_EXAMPLE[] =
    "{\n"
    "  \"devices\": [\n"
    "    {\n"
    "      \"slaveAddr\": 1,\n"
    "      \"topicPrefix\": \"periphnet\",\n"
    "      \"transactions\": [\n"
    "        {\n"
    "          \"startAddr\": 3132,\n"
    "          \"functionCode\": \"input\",\n"
    "          \"readPeriodS\": 5,\n"
    "          \"points\": [\n"
    "            { \"offset\": 0, \"decodeType\": \"u16\", \"scale\": 0.1,\n"
    "              \"unit\": \"V\", \"name\": \"battery_voltage\" },\n"
    "            { \"offset\": 1, \"decodeType\": \"s16\", \"scale\": 0.1,\n"
    "              \"unit\": \"A\", \"name\": \"battery_current\" },\n"
    "            { \"offset\": 6, \"decodeType\": \"u16\", \"scale\": 1,\n"
    "              \"unit\": \"%\", \"name\": \"battery_soc\",\n"
    "              \"publish\": { \"threshold\": 1, \"heartbeatS\": 300 } },\n"
    "            { \"offset\": 7, \"decodeType\": \"u16\", \"scale\": 1,\n"
    "              \"unit\": \"%\", \"name\": \"overdischarge_soc_set\",\n"
    "              \"writable\": true, \"writeMin\": 5, \"writeMax\": 40 }\n"
    "          ]\n"
    "        }\n"
    "      ]\n"
    "    },\n"
    "    {\n"
    "      \"slaveAddr\": 2,\n"
    "      \"topicPrefix\": \"periphnet_meter\",\n"
    "      \"transactions\": [\n"
    "        {\n"
    "          \"startAddr\": 0,\n"
    "          \"functionCode\": \"input\",\n"
    "          \"readPeriodS\": 5,\n"
    "          \"points\": [\n"
    "            { \"offset\": 0, \"decodeType\": \"float32_be\", \"scale\": 1,\n"
    "              \"unit\": \"W\", \"name\": \"power\" }\n"
    "          ]\n"
    "        }\n"
    "      ]\n"
    "    }\n"
    "  ]\n"
    "}\n";

/* ============================================================================
 * Acceptance
 * ============================================================================ */

static void assert_worked_example_stream(void)
{
    sTestStream expect;
    ts_build_worked_example(&expect);

    sModbusLutHeader hdr;
    TEST_ASSERT(W25Q128_Read(EXT_FLASH_MODBUS_LUT_A_ADDR,
                             (uint8_t *)&hdr, sizeof(hdr)) == w25q_ok);
    TEST_ASSERT(hdr.streamLen == expect.len);

    static uint8_t got[4096];
    TEST_ASSERT(W25Q128_Read(EXT_FLASH_MODBUS_LUT_A_ADDR +
                             MODBUS_LUT_HEADER_SIZE,
                             got, expect.len) == w25q_ok);
    TEST_ASSERT_MEM_EQ(got, expect.buf, expect.len);
}

static void test_worked_example_compiles(void)
{
    sMbCompileResult res;

    mock_flash_reset();
    TEST_ASSERT(MbCfgStore_Init() == 0);

    TEST_ASSERT(compile_str(WORKED_EXAMPLE, 0, &res) == 0);
    TEST_ASSERT(res.ok == 1);
    TEST_ASSERT(res.counts.devices == 2);
    TEST_ASSERT(res.counts.transactions == 2);
    TEST_ASSERT(res.counts.points == 5);

    TEST_ASSERT(MbCfgStore_RegionValid(EXT_FLASH_MODBUS_LUT_A_ADDR));
    assert_worked_example_stream();
}

static void test_one_byte_chunks(void)
{
    sMbCompileResult res;

    mock_flash_reset();

    /* 1-byte reads exercise every token/window boundary */
    TEST_ASSERT(compile_str(WORKED_EXAMPLE, 1, &res) == 0);
    TEST_ASSERT(res.ok == 1);
    TEST_ASSERT(MbCfgStore_RegionValid(EXT_FLASH_MODBUS_LUT_A_ADDR));
    assert_worked_example_stream();

    /* 7-byte chunks for an odd stride across the 128-byte window */
    mock_flash_reset();
    TEST_ASSERT(compile_str(WORKED_EXAMPLE, 7, &res) == 0);
    assert_worked_example_stream();
}

static void test_scale_forms(void)
{
    /* "1.0", "10", "0.001" are powers of ten; also writable without an
     * explicit range gets the full-int16 "no validation" range */
    static const char json[] =
        "{\"devices\":[{\"slaveAddr\":1,\"topicPrefix\":\"p\","
        "\"transactions\":[{\"startAddr\":0,\"functionCode\":\"holding\","
        "\"readPeriodS\":1,\"points\":["
        "{\"offset\":0,\"decodeType\":\"u16\",\"scale\":1.0,"
        "\"unit\":\"V\",\"name\":\"a\"},"
        "{\"offset\":1,\"decodeType\":\"u16\",\"scale\":10,"
        "\"unit\":\"W\",\"name\":\"b\"},"
        "{\"offset\":2,\"decodeType\":\"s16\",\"scale\":0.001,"
        "\"unit\":\"kWh\",\"name\":\"c\",\"writable\":true}"
        "]}]}]}";

    sMbCompileResult res;
    mock_flash_reset();
    TEST_ASSERT(compile_str(json, 0, &res) == 0);

    sMbCfgCursor             c;
    sModbusDeviceRecord      dev;
    sModbusTransactionRecord txn;
    sModbusPointRecord       pt;

    TEST_ASSERT(MbCfg_Open(EXT_FLASH_MODBUS_LUT_A_ADDR, &c) == 0);
    TEST_ASSERT(MbCfg_NextDevice(&c, &dev) == 1);
    TEST_ASSERT(MbCfg_NextTransaction(&c, &txn) == 1);
    TEST_ASSERT(txn.functionCode == mbFc_holding);
    TEST_ASSERT(txn.count == 3);

    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 1);
    TEST_ASSERT(pt.scalePow10 == 0);
    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 1);
    TEST_ASSERT(pt.scalePow10 == 1);
    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 1);
    TEST_ASSERT(pt.scalePow10 == -3);
    TEST_ASSERT(pt.flags & MB_POINT_FLAG_WRITABLE);
    TEST_ASSERT(pt.writeMin == INT16_MIN && pt.writeMax == INT16_MAX);
}

/* ============================================================================
 * Rejection matrix — each case pins {deviceIdx, txnIdx, pointIdx, field}
 * ============================================================================ */

/* Wrap a single point JSON into a valid one-device one-txn config */
static void reject_point(const char *pointJson, const char *expectField,
                         const char *expectReasonSub)
{
    static char json[1024];
    snprintf(json, sizeof(json),
             "{\"devices\":[{\"slaveAddr\":9,\"topicPrefix\":\"pp\","
             "\"transactions\":[{\"startAddr\":100,"
             "\"functionCode\":\"input\",\"readPeriodS\":5,"
             "\"points\":[%s]}]}]}", pointJson);

    sMbCompileResult res;
    mock_flash_reset();
    TEST_ASSERT(compile_str(json, 0, &res) != 0);
    TEST_ASSERT(res.ok == 0);
    TEST_ASSERT(res.deviceIdx == 0);
    TEST_ASSERT(res.txnIdx == 0);
    TEST_ASSERT(res.pointIdx == 0);
    if (strcmp(res.field, expectField) != 0) {
        printf("  field: got '%s' want '%s'\n", res.field, expectField);
        test_failures++;
    }
    if (strstr(res.reason, expectReasonSub) == NULL) {
        printf("  reason: got '%s' want substring '%s'\n",
               res.reason, expectReasonSub);
        test_failures++;
    }
    TEST_ASSERT(!MbCfgStore_RegionValid(EXT_FLASH_MODBUS_LUT_A_ADDR));
}

static void test_reject_bad_points(void)
{
    reject_point("{\"offset\":0,\"decodeType\":\"u16\",\"scale\":0.5,"
                 "\"unit\":\"V\",\"name\":\"x\"}",
                 "scale", "power of ten");

    reject_point("{\"offset\":0,\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"volts\",\"name\":\"x\"}",
                 "unit", "unknown unit");

    reject_point("{\"offset\":0,\"decodeType\":\"u99\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\"}",
                 "decodeType", "unknown decode type");

    reject_point("{\"offset\":0,\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"this_name_is_way_too_long_ok\"}",
                 "name", "1..23 chars");

    reject_point("{\"offset\":0,\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"has space\"}",
                 "name", "1..23 chars");

    /* offset+width past the FC03/04 ceiling (124 + 2 regs > 125) */
    reject_point("{\"offset\":124,\"decodeType\":\"u32_be\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\"}",
                 "offset", "125");

    reject_point("{\"offset\":0,\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\",\"writeMin\":5}",
                 "writeMin", "writable");

    reject_point("{\"offset\":0,\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\",\"writable\":true,"
                 "\"writeMin\":50,\"writeMax\":10}",
                 "writeMin", "exceeds writeMax");

    reject_point("{\"offset\":0,\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\",\"writable\":true,"
                 "\"writeMin\":-40000,\"writeMax\":10}",
                 "writeMin", "int16");

    reject_point("{\"offset\":0,\"decodeType\":\"u32_be\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\",\"writable\":true}",
                 "writable", "1-register");

    reject_point("{\"offset\":0,\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\",\"length\":4}",
                 "length", "ascii");

    reject_point("{\"offset\":0,\"decodeType\":\"ascii\",\"scale\":1,"
                 "\"unit\":\"\",\"name\":\"x\"}",
                 "length", "required");

    reject_point("{\"offset\":0,\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\","
                 "\"publish\":{\"threshold\":70000}}",
                 "threshold", "65535");

    reject_point("{\"offset\":0,\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\",\"frobnicate\":1}",
                 "frobnicate", "unknown point key");

    reject_point("{\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\"}",
                 "point", "required");
}

static void test_reject_structure(void)
{
    sMbCompileResult res;

    /* Bad slave address */
    mock_flash_reset();
    TEST_ASSERT(compile_str(
        "{\"devices\":[{\"slaveAddr\":248,\"topicPrefix\":\"p\","
        "\"transactions\":[]}]}", 0, &res) != 0);
    TEST_ASSERT(strcmp(res.field, "slaveAddr") == 0);
    TEST_ASSERT(res.deviceIdx == 0);

    /* Bad function code */
    mock_flash_reset();
    TEST_ASSERT(compile_str(
        "{\"devices\":[{\"slaveAddr\":1,\"topicPrefix\":\"p\","
        "\"transactions\":[{\"startAddr\":0,\"functionCode\":\"coil\","
        "\"readPeriodS\":5,\"points\":[{\"offset\":0,\"decodeType\":\"u16\","
        "\"scale\":1,\"unit\":\"V\",\"name\":\"x\"}]}]}]}", 0, &res) != 0);
    TEST_ASSERT(strcmp(res.field, "functionCode") == 0);
    TEST_ASSERT(res.txnIdx == 0);

    /* Missing readPeriodS */
    mock_flash_reset();
    TEST_ASSERT(compile_str(
        "{\"devices\":[{\"slaveAddr\":1,\"topicPrefix\":\"p\","
        "\"transactions\":[{\"startAddr\":0,\"functionCode\":\"input\","
        "\"points\":[{\"offset\":0,\"decodeType\":\"u16\",\"scale\":1,"
        "\"unit\":\"V\",\"name\":\"x\"}]}]}]}", 0, &res) != 0);
    TEST_ASSERT(strcmp(res.field, "transaction") == 0);

    /* Empty devices array */
    mock_flash_reset();
    TEST_ASSERT(compile_str("{\"devices\":[]}", 0, &res) != 0);
    TEST_ASSERT(strcmp(res.field, "devices") == 0);

    /* Truncated document */
    mock_flash_reset();
    TEST_ASSERT(compile_str(
        "{\"devices\":[{\"slaveAddr\":1,\"topicPre", 0, &res) != 0);
    TEST_ASSERT(strcmp(res.field, "json") == 0);

    /* Trailing garbage */
    mock_flash_reset();
    TEST_ASSERT(compile_str(
        "{\"devices\":[{\"slaveAddr\":1,\"topicPrefix\":\"p\","
        "\"transactions\":[{\"startAddr\":0,\"functionCode\":\"input\","
        "\"readPeriodS\":5,\"points\":[{\"offset\":0,\"decodeType\":\"u16\","
        "\"scale\":1,\"unit\":\"V\",\"name\":\"x\"}]}]}]}garbage",
        0, &res) != 0);
    TEST_ASSERT(strcmp(res.field, "json") == 0);
    TEST_ASSERT(strstr(res.reason, "trailing") != NULL);
}

static void test_reject_budgets(void)
{
    /* 9 devices exceeds MB_MAX_DEVICES */
    static char json[4096];
    int n = snprintf(json, sizeof(json), "{\"devices\":[");
    for (int i = 0; i < 9; i++) {
        n += snprintf(json + n, sizeof(json) - (size_t)n,
            "%s{\"slaveAddr\":%d,\"topicPrefix\":\"p%d\","
            "\"transactions\":[{\"startAddr\":0,\"functionCode\":\"input\","
            "\"readPeriodS\":5,\"points\":[{\"offset\":0,"
            "\"decodeType\":\"u16\",\"scale\":1,\"unit\":\"V\","
            "\"name\":\"x\"}]}]}",
            i ? "," : "", i + 1, i);
    }
    snprintf(json + n, sizeof(json) - (size_t)n, "]}");

    sMbCompileResult res;
    mock_flash_reset();
    TEST_ASSERT(compile_str(json, 0, &res) != 0);
    TEST_ASSERT(strcmp(res.field, "devices") == 0);
    TEST_ASSERT(res.deviceIdx == 8);
    TEST_ASSERT(strstr(res.reason, "too many devices") != NULL);
}

static void test_failed_compile_invalidates_previous(void)
{
    sTestStream s;
    sMbCompileResult res;

    mock_flash_reset();

    /* Region A holds a valid config... */
    ts_build_worked_example(&s);
    ts_write_region(&s, EXT_FLASH_MODBUS_LUT_A_ADDR);
    TEST_ASSERT(MbCfgStore_RegionValid(EXT_FLASH_MODBUS_LUT_A_ADDR));

    /* ...a failing compile into it must leave it invalid, not stale-valid */
    TEST_ASSERT(compile_str("{\"devices\":[]}", 0, &res) != 0);
    TEST_ASSERT(!MbCfgStore_RegionValid(EXT_FLASH_MODBUS_LUT_A_ADDR));
}

int main(void)
{
    RUN_TEST(test_worked_example_compiles);
    RUN_TEST(test_one_byte_chunks);
    RUN_TEST(test_scale_forms);
    RUN_TEST(test_reject_bad_points);
    RUN_TEST(test_reject_structure);
    RUN_TEST(test_reject_budgets);
    RUN_TEST(test_failed_compile_invalidates_previous);
    return test_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
