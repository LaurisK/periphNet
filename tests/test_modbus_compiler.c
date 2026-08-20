/**
 * Unit tests for Shared/Modbus/modbus_config_compiler.c (v2): the
 * docs/modbus.md §6 worked example must compile byte-identically to the
 * hand-assembled record stream (under any chunking of the byte source), and
 * every §7.4 validation rule must reject at the right object.
 *
 * THE COMPILE PASS IS THE VALIDATION, so this file is also the accept/reject
 * matrix for the whole schema.
 */

#include "test_util.h"
#include "modbus_test_stream.h"
#include "modbus_config_compiler.h"
#include "modbus_blocks.h"

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

static int compile_str(const char *json, uint32_t chunk,
                       sModbusCompileResult *res)
{
    sMemSource src = { json, (uint32_t)strlen(json), 0, chunk };
    return MbCfgCompile(mem_source, &src, nvdbUser_modbusLutA,
                        NULL, res);
}

/* The §6 worked example: two capabilities, three devices (two sharing one
 * capability), three plans in non-contiguous slots. */
static const char WORKED_EXAMPLE[] =
    "{\n"
    "  \"capabilities\": [\n"
    "    { \"id\": 0, \"name\": \"solis\",\n"
    "      \"addrStride\": 1, \"writeFc\": 6, \"maxReadRegs\": 125,\n"
    "      \"blocks\": [ { \"base\": 3132, \"regs\": 16 } ],\n"
    "      \"points\": [\n"
    "        { \"id\": 0, \"addr\": 3132, \"fc\": \"input\","
    "          \"decodeType\": \"u16\", \"scale\": 0.1,\n"
    "          \"unit\": \"V\", \"name\": \"battery_voltage\" },\n"
    "        { \"id\": 1, \"addr\": 3133, \"fc\": \"input\","
    "          \"decodeType\": \"s16\", \"scale\": 0.1,\n"
    "          \"unit\": \"A\", \"name\": \"battery_current\" },\n"
    "        { \"id\": 2, \"addr\": 3138, \"fc\": \"input\","
    "          \"decodeType\": \"u16\", \"scale\": 1,\n"
    "          \"unit\": \"%\", \"name\": \"battery_soc\" },\n"
    "        { \"id\": 3, \"addr\": 3139, \"fc\": \"holding\","
    "          \"decodeType\": \"u16\", \"scale\": 1,\n"
    "          \"unit\": \"%\", \"name\": \"overdischarge_soc_set\",\n"
    "          \"access\": \"rw\", \"writeMin\": 5, \"writeMax\": 40 }\n"
    "      ] },\n"
    "    { \"id\": 1, \"name\": \"meter\",\n"
    "      \"addrStride\": 1, \"writeFc\": 6, \"maxReadRegs\": 125,\n"
    "      \"blocks\": [ { \"base\": 0, \"regs\": 8 } ],\n"
    "      \"points\": [\n"
    "        { \"id\": 0, \"addr\": 0, \"fc\": \"input\","
    "          \"decodeType\": \"float32_be\", \"scale\": 1,\n"
    "          \"unit\": \"W\", \"name\": \"power\" }\n"
    "      ] }\n"
    "  ],\n"
    "  \"devices\": [\n"
    "    { \"id\": 0, \"slaveAddr\": 1, \"capability\": 0,"
    "      \"topicPrefix\": \"periphnet\" },\n"
    "    { \"id\": 1, \"slaveAddr\": 3, \"capability\": 0,"
    "      \"topicPrefix\": \"spare\" },\n"
    "    { \"id\": 2, \"slaveAddr\": 2, \"capability\": 1,"
    "      \"topicPrefix\": \"periphnet_meter\" }\n"
    "  ],\n"
    "  \"plans\": [\n"
    "    { \"id\": 0, \"name\": \"inv_fast\", \"capability\": 0,"
    "      \"devices\": [0],\n"
    "      \"timeTables\": [ { \"id\": 0, \"everySec\": 5,"
    "                          \"points\": [0, 1, 2] },\n"
    "                        { \"id\": 1, \"everySec\": 60,"
    "                          \"points\": [3] } ] },\n"
    "    { \"id\": 2, \"name\": \"inv_lazy\", \"capability\": 0,"
    "      \"devices\": [1],\n"
    "      \"timeTables\": [ { \"id\": 0, \"everySec\": 300,"
    "                          \"points\": [0] } ] },\n"
    "    { \"id\": 3, \"name\": \"meter_plan\", \"capability\": 1,"
    "      \"devices\": [2],\n"
    "      \"timeTables\": [ { \"id\": 0, \"everySec\": 10,"
    "                          \"points\": [0] } ] }\n"
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
    TEST_ASSERT(NvDb_Read(nvdbUser_modbusLutA, &hdr, 0u,
                          sizeof(hdr)) == nvdbRes_ok);
    TEST_ASSERT(hdr.version == MODBUS_LUT_VERSION);
    TEST_ASSERT(hdr.streamLen == expect.len);

    static uint8_t got[8192];
    TEST_ASSERT(NvDb_Read(nvdbUser_modbusLutA, got, MODBUS_LUT_HEADER_SIZE,
                          expect.len) == nvdbRes_ok);
    TEST_ASSERT_MEM_EQ(got, expect.buf, expect.len);
}

static void test_worked_example_compiles(void)
{
    sModbusCompileResult res;

    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);
    TEST_ASSERT(MbCfgStore_Init() == 0);

    TEST_ASSERT(compile_str(WORKED_EXAMPLE, 0, &res) == 0);
    TEST_ASSERT(res.ok == 1);
    TEST_ASSERT(res.counts.capabilities == 2);
    TEST_ASSERT(res.counts.devices == 3);
    TEST_ASSERT(res.counts.plans == 3);
    TEST_ASSERT(res.counts.points == 5);

    TEST_ASSERT(MbCfgStore_RegionValid(nvdbUser_modbusLutA));
    assert_worked_example_stream();
}

static void test_one_byte_chunks(void)
{
    sModbusCompileResult res;

    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);

    /* 1-byte reads exercise every token/window boundary */
    TEST_ASSERT(compile_str(WORKED_EXAMPLE, 1, &res) == 0);
    TEST_ASSERT(res.ok == 1);
    TEST_ASSERT(MbCfgStore_RegionValid(nvdbUser_modbusLutA));
    assert_worked_example_stream();

    /* 7-byte chunks for an odd stride across the 128-byte window */
    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);
    TEST_ASSERT(compile_str(WORKED_EXAMPLE, 7, &res) == 0);
    assert_worked_example_stream();
}

/* ============================================================================
 * The dialect group — data, never a code path
 * ============================================================================ */

static void test_jk_dialect_compiles(void)
{
    static const char json[] =
        "{\"capabilities\":[{\"id\":0,\"name\":\"jk_pb\","
        "\"addrStride\":2,\"writeFc\":16,\"maxReadRegs\":123,"
        "\"blocks\":[{\"base\":4096,\"regs\":147},"
                    "{\"base\":4608,\"regs\":147},"
                    "{\"base\":5120,\"regs\":147}],"
        "\"points\":["
        "{\"id\":0,\"addr\":5120,\"fc\":\"holding\",\"decodeType\":\"ascii\","
        "\"length\":8,\"unit\":\"\",\"name\":\"model\"},"
        "{\"id\":1,\"addr\":4096,\"fc\":\"holding\",\"decodeType\":\"u32_be\","
        "\"scale\":0.001,\"unit\":\"V\",\"name\":\"cell_ovp\","
        "\"access\":\"rw\"}"
        "]}],"
        "\"devices\":[{\"id\":0,\"slaveAddr\":1,\"capability\":0,"
        "\"baud\":115200,\"format\":\"8N1\",\"topicPrefix\":\"bms1\"}],"
        "\"plans\":[{\"id\":0,\"name\":\"pack\",\"capability\":0,"
        "\"devices\":[0],\"timeTables\":[{\"id\":0,\"everySec\":5,"
        "\"points\":[0,1]}]}]}";

    sModbusCompileResult res;
    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);
    TEST_ASSERT(compile_str(json, 0, &res) == 0);

    sMbCfgCursor            c;
    sModbusCapabilityRecord cap;
    sModbusBlockRecord      blocks[MB_MAX_BLOCKS_PER_CAP];
    sModbusPointRecord      pt;
    sModbusDeviceRecord     dev;

    TEST_ASSERT(MbCfg_OpenCapability(nvdbUser_modbusLutA, 0, &c, &cap,
                                     blocks, MB_MAX_BLOCKS_PER_CAP) == 0);
    TEST_ASSERT(cap.addrStride == 2 && cap.writeFc == 16);
    TEST_ASSERT(cap.maxReadRegs == 123 && cap.blockCount == 3);
    TEST_ASSERT(blocks[2].base == 5120 && blocks[2].regs == 147);

    /* addr is stored VERBATIM — no stride baked in at compile time */
    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 1);
    TEST_ASSERT(pt.addr == 5120 && pt.length == 8);
    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 1);
    TEST_ASSERT(pt.addr == 4096);
    /* A 2-register writable point is legal under FC16 and only under FC16 */
    TEST_ASSERT((pt.flags & MB_PT_WRITE) != 0);

    TEST_ASSERT(MbCfg_FindDevice(nvdbUser_modbusLutA, 0, &dev) == 0);
    TEST_ASSERT(MbRecords_BaudFromCode(dev.baudCode) == 115200u);
    TEST_ASSERT(dev.format == mbFmt_8N1);
    TEST_ASSERT(dev.portId == mbPort_rs485);      /* default */
}

static void test_device_line_parameters(void)
{
    static const char json[] =
        "{\"capabilities\":[{\"id\":0,\"name\":\"c\",\"blocks\":"
        "[{\"base\":0,\"regs\":4}],\"points\":[{\"id\":0,\"addr\":0,"
        "\"fc\":\"input\",\"decodeType\":\"u16\",\"scale\":1,\"unit\":\"V\","
        "\"name\":\"a\"}]}],"
        "\"devices\":[{\"id\":0,\"slaveAddr\":1,\"capability\":0,"
        "\"baud\":9600,\"format\":\"8E1\",\"port\":\"test\","
        "\"topicPrefix\":\"d0\"}]}";

    sModbusCompileResult res;
    sModbusDeviceRecord  dev;

    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);
    TEST_ASSERT(compile_str(json, 0, &res) == 0);
    TEST_ASSERT(res.counts.plans == 0);           /* zero plans is legal */
    TEST_ASSERT(MbCfg_FindDevice(nvdbUser_modbusLutA, 0, &dev) == 0);
    TEST_ASSERT(dev.baudCode == 0);               /* 9600 is code 0 */
    TEST_ASSERT(dev.format == mbFmt_8E1);
    TEST_ASSERT(dev.portId == mbPort_test);
}

static void test_scale_and_bounds_forms(void)
{
    /* "1.0", "10", "0.001" are powers of ten; a writable point with NO bounds
     * is accepted and keeps MB_PT_BOUNDED clear — absent bounds mean
     * unconstrained, not forbidden (§4.6). */
    static const char json[] =
        "{\"capabilities\":[{\"id\":0,\"name\":\"c\","
        "\"blocks\":[{\"base\":0,\"regs\":8}],\"points\":["
        "{\"id\":0,\"addr\":0,\"fc\":\"holding\",\"decodeType\":\"u16\","
        "\"scale\":1.0,\"unit\":\"V\",\"name\":\"a\"},"
        "{\"id\":1,\"addr\":1,\"fc\":\"holding\",\"decodeType\":\"u16\","
        "\"scale\":10,\"unit\":\"W\",\"name\":\"b\"},"
        "{\"id\":2,\"addr\":2,\"fc\":\"holding\",\"decodeType\":\"s16\","
        "\"scale\":0.001,\"unit\":\"kWh\",\"name\":\"c\",\"access\":\"rw\"},"
        "{\"id\":3,\"addr\":3,\"fc\":\"holding\",\"decodeType\":\"u16\","
        "\"scale\":1,\"unit\":\"V\",\"name\":\"d\",\"access\":\"w\"}"
        "]}]}";

    sModbusCompileResult res;
    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);
    TEST_ASSERT(compile_str(json, 0, &res) == 0);

    sMbCfgCursor            c;
    sModbusCapabilityRecord cap;
    sModbusPointRecord      pt;

    TEST_ASSERT(MbCfg_OpenCapability(nvdbUser_modbusLutA, 0, &c, &cap,
                                     NULL, 0) == 0);
    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 1);
    TEST_ASSERT(pt.scalePow10 == 0);
    TEST_ASSERT(pt.flags == MB_PT_READ);          /* access defaults to r */
    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 1);
    TEST_ASSERT(pt.scalePow10 == 1);
    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 1);
    TEST_ASSERT(pt.scalePow10 == -3);
    TEST_ASSERT((pt.flags & MB_PT_WRITE) && (pt.flags & MB_PT_READ));
    TEST_ASSERT((pt.flags & MB_PT_BOUNDED) == 0);
    TEST_ASSERT(pt.writeMin == 0 && pt.writeMax == 0);
    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 1);
    TEST_ASSERT((pt.flags & MB_PT_WRITE) && !(pt.flags & MB_PT_READ));

    /* int32 bounds round-trip past +-32767, which int16 could not hold */
    /* writeFc 16 is what makes a multi-register point writable at all. */
    static const char wide[] =
        "{\"capabilities\":[{\"id\":0,\"name\":\"c\",\"writeFc\":16,"
        "\"blocks\":[{\"base\":0,\"regs\":8}],\"points\":["
        "{\"id\":0,\"addr\":0,\"fc\":\"holding\",\"decodeType\":\"s32_be\","
        "\"scale\":1,\"unit\":\"W\",\"name\":\"p\",\"access\":\"rw\","
        "\"writeMin\":-100000,\"writeMax\":100000}"
        "]}]}";
    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);
    TEST_ASSERT(compile_str(wide, 0, &res) == 0);
    TEST_ASSERT(MbCfg_FindPoint(nvdbUser_modbusLutA, 0, 0, &pt) == 0);
    TEST_ASSERT(pt.writeMin == -100000 && pt.writeMax == 100000);
    TEST_ASSERT(pt.flags & MB_PT_BOUNDED);
}

/* ============================================================================
 * Rejection matrix
 * ============================================================================ */

static void expect_reject(const char *json, const char *expectField,
                          const char *expectReasonSub)
{
    sModbusCompileResult res;

    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);
    TEST_ASSERT(compile_str(json, 0, &res) != 0);
    TEST_ASSERT(res.ok == 0);
    if (strcmp(res.field, expectField) != 0) {
        printf("  field: got '%s' want '%s'\n", res.field, expectField);
        test_failures++;
    }
    if (strstr(res.reason, expectReasonSub) == NULL) {
        printf("  reason: got '%s' want substring '%s'\n",
               res.reason, expectReasonSub);
        test_failures++;
    }
    TEST_ASSERT(!MbCfgStore_RegionValid(nvdbUser_modbusLutA));
}

/* Wrap one point into an otherwise valid single-capability config. */
static void reject_point(const char *pointJson, const char *expectField,
                         const char *expectReasonSub)
{
    static char json[1024];
    snprintf(json, sizeof(json),
             "{\"capabilities\":[{\"id\":0,\"name\":\"c\",\"writeFc\":6,"
             "\"blocks\":[{\"base\":100,\"regs\":16}],\"points\":[%s]}]}",
             pointJson);
    expect_reject(json, expectField, expectReasonSub);
}

static void test_reject_bad_points(void)
{
    reject_point("{\"id\":0,\"addr\":100,\"fc\":\"input\","
                 "\"decodeType\":\"u16\",\"scale\":0.5,"
                 "\"unit\":\"V\",\"name\":\"x\"}",
                 "scale", "power of ten");

    reject_point("{\"id\":0,\"addr\":100,\"fc\":\"input\","
                 "\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"volts\",\"name\":\"x\"}",
                 "unit", "unknown unit");

    reject_point("{\"id\":0,\"addr\":100,\"fc\":\"input\","
                 "\"decodeType\":\"u99\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\"}",
                 "decodeType", "unknown decode type");

    reject_point("{\"id\":0,\"addr\":100,\"fc\":\"input\","
                 "\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"this_name_is_way_too_long_ok\"}",
                 "name", "1..23 chars");

    /* A point outside every declared block, and one that ends past its
     * block's regs — the two things authoring blocks buys at compile time. */
    reject_point("{\"id\":0,\"addr\":9000,\"fc\":\"input\","
                 "\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\"}",
                 "addr", "outside every declared block");

    reject_point("{\"id\":0,\"addr\":115,\"fc\":\"input\","
                 "\"decodeType\":\"u32_be\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\"}",
                 "addr", "past its block");

    /* An id that does not continue the run */
    reject_point("{\"id\":1,\"addr\":100,\"fc\":\"input\","
                 "\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\"}",
                 "id", "must run 0, 1, 2");

    /* Access rules */
    reject_point("{\"id\":0,\"addr\":100,\"fc\":\"input\","
                 "\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\",\"access\":\"rw\"}",
                 "access", "holding");

    reject_point("{\"id\":0,\"addr\":100,\"fc\":\"holding\","
                 "\"decodeType\":\"u32_be\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\",\"access\":\"rw\"}",
                 "access", "one register");

    reject_point("{\"id\":0,\"addr\":100,\"fc\":\"input\","
                 "\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\",\"writeMin\":5,"
                 "\"writeMax\":9}",
                 "writeMin", "write access");

    reject_point("{\"id\":0,\"addr\":100,\"fc\":\"holding\","
                 "\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\",\"access\":\"rw\","
                 "\"writeMin\":5}",
                 "writeMin", "requires the other");

    reject_point("{\"id\":0,\"addr\":100,\"fc\":\"holding\","
                 "\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\",\"access\":\"rw\","
                 "\"writeMin\":50,\"writeMax\":10}",
                 "writeMin", "writeMin > writeMax");

    reject_point("{\"id\":0,\"addr\":100,\"fc\":\"holding\","
                 "\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\",\"access\":\"rw\","
                 "\"writeMin\":-40000,\"writeMax\":10}",
                 "writeMin", "outside the decode type");

    reject_point("{\"id\":0,\"addr\":100,\"fc\":\"input\","
                 "\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\",\"length\":4}",
                 "length", "ascii");

    reject_point("{\"id\":0,\"addr\":100,\"fc\":\"input\","
                 "\"decodeType\":\"ascii\",\"unit\":\"\",\"name\":\"x\"}",
                 "length", "required for ascii");

    /* An old config carrying "publish" fails BY NAME (§6) */
    reject_point("{\"id\":0,\"addr\":100,\"fc\":\"input\","
                 "\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\","
                 "\"publish\":{\"threshold\":1}}",
                 "publish", "unknown point key");

    /* And so does the v1 offset/writable spelling */
    reject_point("{\"id\":0,\"offset\":0,\"fc\":\"input\","
                 "\"decodeType\":\"u16\",\"scale\":1,"
                 "\"unit\":\"V\",\"name\":\"x\"}",
                 "offset", "unknown point key");

    reject_point("{\"id\":0,\"addr\":100,\"decodeType\":\"u16\","
                 "\"scale\":1,\"unit\":\"V\",\"name\":\"x\"}",
                 "fc", "missing");
}

static void test_reject_capabilities_and_devices(void)
{
    /* A capability with no blocks */
    expect_reject("{\"capabilities\":[{\"id\":0,\"name\":\"c\","
                  "\"points\":[]}]}",
                  "points", "blocks must precede points");

    /* Blocks after points */
    expect_reject("{\"capabilities\":[{\"id\":0,\"name\":\"c\","
                  "\"points\":[],\"blocks\":[{\"base\":0,\"regs\":4}]}]}",
                  "points", "blocks must precede points");

    /* Capability ids must be dense */
    expect_reject("{\"capabilities\":[{\"id\":1,\"name\":\"c\","
                  "\"blocks\":[{\"base\":0,\"regs\":4}],\"points\":[]}]}",
                  "id", "must run 0, 1, 2");

    /* writeFc outside {6, 16} */
    expect_reject("{\"capabilities\":[{\"id\":0,\"name\":\"c\",\"writeFc\":3,"
                  "\"blocks\":[{\"base\":0,\"regs\":4}],\"points\":[]}]}",
                  "writeFc", "must be 6 or 16");

    static const char capOk[] =
        "{\"capabilities\":[{\"id\":0,\"name\":\"c\","
        "\"blocks\":[{\"base\":0,\"regs\":4}],\"points\":[{\"id\":0,"
        "\"addr\":0,\"fc\":\"input\",\"decodeType\":\"u16\",\"scale\":1,"
        "\"unit\":\"V\",\"name\":\"a\"}]}],\"devices\":[";
    static char json[1024];

    /* A device naming a capability that does not exist */
    snprintf(json, sizeof(json), "%s{\"id\":0,\"slaveAddr\":1,"
             "\"capability\":7,\"topicPrefix\":\"d\"}]}", capOk);
    expect_reject(json, "capability", "no such capability");

    snprintf(json, sizeof(json), "%s{\"id\":0,\"slaveAddr\":248,"
             "\"capability\":0,\"topicPrefix\":\"d\"}]}", capOk);
    expect_reject(json, "slaveAddr", "1..247");

    snprintf(json, sizeof(json), "%s{\"id\":0,\"slaveAddr\":1,"
             "\"capability\":0,\"baud\":31337,\"topicPrefix\":\"d\"}]}", capOk);
    expect_reject(json, "baud", "supported rates");

    snprintf(json, sizeof(json), "%s{\"id\":0,\"slaveAddr\":1,"
             "\"capability\":0,\"format\":\"7E1\",\"topicPrefix\":\"d\"}]}",
             capOk);
    expect_reject(json, "format", "8N1");

    snprintf(json, sizeof(json), "%s{\"id\":0,\"slaveAddr\":1,"
             "\"capability\":0,\"port\":\"uart6\",\"topicPrefix\":\"d\"}]}",
             capOk);
    expect_reject(json, "port", "no such port");

    /* Section order is forced by the single pass */
    expect_reject("{\"devices\":[],\"capabilities\":[]}",
                  "devices", "capabilities must come first");
}

static void test_reject_plans(void)
{
    static const char base[] =
        "{\"capabilities\":[{\"id\":0,\"name\":\"c\","
        "\"blocks\":[{\"base\":0,\"regs\":8}],\"points\":["
        "{\"id\":0,\"addr\":0,\"fc\":\"input\",\"decodeType\":\"u16\","
        "\"scale\":1,\"unit\":\"V\",\"name\":\"a\"},"
        "{\"id\":1,\"addr\":1,\"fc\":\"holding\",\"decodeType\":\"u16\","
        "\"scale\":1,\"unit\":\"V\",\"name\":\"b\",\"access\":\"w\"}"
        "]},"
        "{\"id\":1,\"name\":\"c2\",\"blocks\":[{\"base\":0,\"regs\":8}],"
        "\"points\":[{\"id\":0,\"addr\":0,\"fc\":\"input\","
        "\"decodeType\":\"u16\",\"scale\":1,\"unit\":\"V\",\"name\":\"z\"}]}],"
        "\"devices\":[{\"id\":0,\"slaveAddr\":1,\"capability\":0,"
        "\"topicPrefix\":\"d0\"},{\"id\":1,\"slaveAddr\":2,\"capability\":1,"
        "\"topicPrefix\":\"d1\"}],\"plans\":[";
    static char json[1536];

    /* A plan naming a device that implements a DIFFERENT capability */
    snprintf(json, sizeof(json), "%s{\"id\":0,\"name\":\"p\","
             "\"capability\":0,\"devices\":[1],\"timeTables\":[]}]}", base);
    expect_reject(json, "devices", "another capability");

    /* A device that does not exist */
    snprintf(json, sizeof(json), "%s{\"id\":0,\"name\":\"p\","
             "\"capability\":0,\"devices\":[5],\"timeTables\":[]}]}", base);
    expect_reject(json, "devices", "no such device");

    /* A write-only point cannot be watched */
    snprintf(json, sizeof(json), "%s{\"id\":0,\"name\":\"p\","
             "\"capability\":0,\"devices\":[0],\"timeTables\":["
             "{\"id\":0,\"everySec\":5,\"points\":[1]}]}]}", base);
    expect_reject(json, "points", "write-only");

    /* A point id past the capability's point count */
    snprintf(json, sizeof(json), "%s{\"id\":0,\"name\":\"p\","
             "\"capability\":0,\"devices\":[0],\"timeTables\":["
             "{\"id\":0,\"everySec\":5,\"points\":[9]}]}]}", base);
    expect_reject(json, "points", "no such point id");

    /* A point in two time tables OF ONE PLAN */
    snprintf(json, sizeof(json), "%s{\"id\":0,\"name\":\"p\","
             "\"capability\":0,\"devices\":[0],\"timeTables\":["
             "{\"id\":0,\"everySec\":5,\"points\":[0]},"
             "{\"id\":1,\"everySec\":9,\"points\":[0]}]}]}", base);
    expect_reject(json, "points", "twice in this plan");

    /* period 0 */
    snprintf(json, sizeof(json), "%s{\"id\":0,\"name\":\"p\","
             "\"capability\":0,\"devices\":[0],\"timeTables\":["
             "{\"id\":0,\"everySec\":0,\"points\":[0]}]}]}", base);
    expect_reject(json, "everySec", ">= 1");

    /* Duplicate and out-of-range plan slots */
    snprintf(json, sizeof(json), "%s{\"id\":0,\"name\":\"p\","
             "\"capability\":0,\"devices\":[0],\"timeTables\":[]},"
             "{\"id\":0,\"name\":\"q\",\"capability\":0,\"devices\":[0],"
             "\"timeTables\":[]}]}", base);
    expect_reject(json, "id", "duplicate plan id");

    snprintf(json, sizeof(json), "%s{\"id\":9,\"name\":\"p\","
             "\"capability\":0,\"devices\":[0],\"timeTables\":[]}]}", base);
    expect_reject(json, "id", "must be 0..7");

    /* Slots must ascend, so the stream stays in slot order with no blanks */
    snprintf(json, sizeof(json), "%s{\"id\":3,\"name\":\"p\","
             "\"capability\":0,\"devices\":[0],\"timeTables\":[]},"
             "{\"id\":1,\"name\":\"q\",\"capability\":0,\"devices\":[0],"
             "\"timeTables\":[]}]}", base);
    expect_reject(json, "id", "ascending");
}

/* An empty device set and an empty time-table list are both legal: they are
 * the two ways of saying "capable but unmonitored" (§3.1). */
static void test_accept_empty_plan_sets(void)
{
    static const char json[] =
        "{\"capabilities\":[{\"id\":0,\"name\":\"c\","
        "\"blocks\":[{\"base\":0,\"regs\":8}],\"points\":[{\"id\":0,"
        "\"addr\":0,\"fc\":\"input\",\"decodeType\":\"u16\",\"scale\":1,"
        "\"unit\":\"V\",\"name\":\"a\"}]}],"
        "\"devices\":[{\"id\":0,\"slaveAddr\":1,\"capability\":0,"
        "\"topicPrefix\":\"d0\"}],"
        "\"plans\":[{\"id\":0,\"name\":\"none\",\"capability\":0,"
        "\"devices\":[],\"timeTables\":[]}]}";

    sModbusCompileResult res;
    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);
    TEST_ASSERT(compile_str(json, 0, &res) == 0);

    sMbPlanHeader hdr[MB_MAX_PLANS];
    TEST_ASSERT(MbCfg_ReadPlanHeaders(nvdbUser_modbusLutA, hdr) == 1);
    TEST_ASSERT(hdr[0].used && hdr[0].rec.devices == 0);
    TEST_ASSERT(hdr[0].tableCount == 0);
}

static void test_reject_structure(void)
{
    /* Truncated document */
    expect_reject("{\"capabilities\":[{\"id\":0,\"nam", "json", "");

    /* Trailing data after a complete config — rejected by the lexer or by the
     * eof check, either way as a json-level failure. */
    expect_reject("{\"capabilities\":[{\"id\":0,\"name\":\"c\","
                  "\"blocks\":[{\"base\":0,\"regs\":4}],\"points\":[]}]}"
                  "{\"devices\":[]}",
                  "json", "trailing");

    /* Unknown top-level key */
    expect_reject("{\"transactions\":[]}", "transactions", "unknown top-level");

    /* No capabilities at all */
    expect_reject("{\"devices\":[]}", "devices", "capabilities must come first");
}

static void test_reject_budgets(void)
{
    static char json[8192];
    int n;

    /* 9 capabilities exceeds MB_MAX_CAPABILITIES */
    n = snprintf(json, sizeof(json), "{\"capabilities\":[");
    for (int i = 0; i < 9; i++) {
        n += snprintf(json + n, sizeof(json) - (size_t)n,
            "%s{\"id\":%d,\"name\":\"c%d\",\"blocks\":[{\"base\":0,"
            "\"regs\":4}],\"points\":[]}", i ? "," : "", i, i);
    }
    snprintf(json + n, sizeof(json) - (size_t)n, "]}");
    expect_reject(json, "capabilities", "too many");
}

/* ============================================================================
 * The integration fixture
 *
 * tests/integration uploads tests/fixtures/modbus_solis.json to a live board
 * before it runs.  A fixture the compiler rejects is a broken test run that
 * only fails on hardware, so it is compiled here too.
 * ============================================================================ */

static void test_integration_fixture_compiles(void)
{
    static char json[8192];
    FILE       *f = fopen(MB_FIXTURE_PATH, "rb");
    size_t      n;

    TEST_ASSERT(f != NULL);
    if (f == NULL) {
        return;
    }
    n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[n] = '\0';
    TEST_ASSERT(n > 0);

    sModbusCompileResult res;
    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);
    if (compile_str(json, 0, &res) != 0) {
        printf("  fixture rejected: %s / %s\n", res.field, res.reason);
        test_failures++;
        return;
    }
    TEST_ASSERT(res.counts.capabilities == 1);
    TEST_ASSERT(res.counts.devices == 1);
    TEST_ASSERT(res.counts.plans == 1);
    TEST_ASSERT(res.counts.points == 5);

    /* The points the injected frames land on, at the addresses the frames
     * assume: reg 0, reg 1 and reg 6 of a 20-register read at 3132. */
    sModbusPointRecord pt;
    TEST_ASSERT(MbCfg_FindPoint(nvdbUser_modbusLutA, 0, 0, &pt) == 0);
    TEST_ASSERT(pt.addr == 3132 && pt.scalePow10 == -1);
    TEST_ASSERT(strcmp(pt.name, "battery_voltage") == 0);
    TEST_ASSERT(MbCfg_FindPoint(nvdbUser_modbusLutA, 0, 2, &pt) == 0);
    TEST_ASSERT(pt.addr == 3138);
    TEST_ASSERT(strcmp(pt.name, "battery_soc") == 0);

    /* And the two writable points the set-topic tests drive, with the bounds
     * those tests depend on (15 accepted, 99 refused). */
    TEST_ASSERT(MbCfg_FindPoint(nvdbUser_modbusLutA, 0, 4, &pt) == 0);
    TEST_ASSERT(strcmp(pt.name, "overdischarge_soc") == 0);
    TEST_ASSERT(pt.addr == 3010);
    TEST_ASSERT((pt.flags & MB_PT_WRITE) && (pt.flags & MB_PT_BOUNDED));
    TEST_ASSERT(pt.writeMin == 5 && pt.writeMax == 40);
    TEST_ASSERT(MbCfg_FindPoint(nvdbUser_modbusLutA, 0, 3, &pt) == 0);
    TEST_ASSERT(strcmp(pt.name, "max_charge_soc") == 0);
    TEST_ASSERT(pt.writeMin == 70 && pt.writeMax == 100);
}

/* The JK capability, which is the module's whole claim: the next device after
 * JK costs a JSON file, not a .c file (§2.3).  It compiling — with stride 2,
 * FC16 writes, a 123-register ceiling and three 147-register blocks — is what
 * says the dialect group is data and not a code path. */
static void test_jk_fixture_compiles(void)
{
    static char json[16384];
    FILE       *f = fopen(MB_JK_FIXTURE_PATH, "rb");
    size_t      n;

    TEST_ASSERT(f != NULL);
    if (f == NULL) {
        return;
    }
    n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[n] = '\0';

    sModbusCompileResult res;
    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);
    if (compile_str(json, 0, &res) != 0) {
        printf("  JK fixture rejected: %s / %s\n", res.field, res.reason);
        test_failures++;
        return;
    }
    TEST_ASSERT(res.counts.capabilities == 1);
    TEST_ASSERT(res.counts.devices == 2);      /* two packs, ONE capability */
    TEST_ASSERT(res.counts.plans == 2);

    sMbCfgCursor            c;
    sModbusCapabilityRecord cap;
    sModbusBlockRecord      blocks[MB_MAX_BLOCKS_PER_CAP];

    TEST_ASSERT(MbCfg_OpenCapability(nvdbUser_modbusLutA, 0, &c, &cap,
                                     blocks, MB_MAX_BLOCKS_PER_CAP) == 0);
    TEST_ASSERT(cap.addrStride == 2);
    TEST_ASSERT(cap.writeFc == 16);
    TEST_ASSERT(cap.maxReadRegs == 123);
    TEST_ASSERT(cap.blockCount == 3);
    TEST_ASSERT(blocks[0].regs == 147);

    /* A 2-register writable point is legal ONLY because writeFc is 16 — under
     * FC06 not one JK setpoint would be writable (§3.2). */
    sModbusPointRecord pt;
    TEST_ASSERT(MbCfg_FindPoint(nvdbUser_modbusLutA, 0, 10, &pt) == 0);
    TEST_ASSERT(strcmp(pt.name, "cell_ovp") == 0);
    TEST_ASSERT(pt.flags & MB_PT_WRITE);
    TEST_ASSERT(MbRecords_RegWidth(pt.decodeType, pt.length) == 2);

    /* Both packs share the capability: the same ptOrd is the same point on a
     * different slave, which is what makes four packs one register map. */
    sModbusDeviceRecord d0, d1;
    TEST_ASSERT(MbCfg_FindDevice(nvdbUser_modbusLutA, 0, &d0) == 0);
    TEST_ASSERT(MbCfg_FindDevice(nvdbUser_modbusLutA, 1, &d1) == 0);
    TEST_ASSERT(d0.capId == d1.capId);
    TEST_ASSERT(d0.slaveAddr != d1.slaveAddr);
    TEST_ASSERT(MbRecords_BaudFromCode(d0.baudCode) == 115200u);
}

/* Derivation over the JK's byte-addressed registers: the quantity is in
 * REGISTERS while the addresses step by two. */
static void test_jk_fixture_derives_blocks(void)
{
    static char json[16384];
    FILE       *f = fopen(MB_JK_FIXTURE_PATH, "rb");
    size_t      n;

    if (f == NULL) {
        return;
    }
    n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[n] = '\0';

    sModbusCompileResult res;
    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);
    TEST_ASSERT(compile_str(json, 0, &res) == 0);

    sMbCfgCursor            c;
    sModbusCapabilityRecord cap;
    sModbusBlockRecord      blocks[MB_MAX_BLOCKS_PER_CAP];
    sModbusPointRecord      pt;
    sModbusPointSpan        spans[8];
    sModbusReadBlock        out[8];
    uint16_t                nSpans = 0;

    TEST_ASSERT(MbCfg_OpenCapability(nvdbUser_modbusLutA, 0, &c, &cap,
                                     blocks, MB_MAX_BLOCKS_PER_CAP) == 0);

    /* The realtime points of pack_fast: ids 5..8 */
    uint16_t ord = 0;
    while (MbCfg_NextPoint(&c, &pt) == 1) {
        if (ord >= 5 && ord <= 8) {
            spans[nSpans].addr  = pt.addr;
            spans[nSpans].ptOrd = ord;
            spans[nSpans].regs  = MbRecords_RegWidth(pt.decodeType, pt.length);
            spans[nSpans].fc    = pt.functionCode;
            nSpans++;
        }
        ord++;
    }
    TEST_ASSERT(nSpans == 4);

    int blockCount = MbBlocks_Derive(&cap, blocks, cap.blockCount, spans,
                                     nSpans, out, 8);
    TEST_ASSERT(blockCount == 1);        /* all inside the 0x1200 block */
    TEST_ASSERT(out[0].addr == 4736);
    /* 4736..4755 inclusive is an address span of 20 = 10 REGISTERS, not 20. */
    TEST_ASSERT(out[0].regs == 10);
    TEST_ASSERT(out[0].regs <= cap.maxReadRegs);
}

static void test_failed_compile_invalidates_previous(void)
{
    sTestStream          s;
    sModbusCompileResult res;

    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);

    /* Region A holds a valid config... */
    ts_build_worked_example(&s);
    ts_write_region(&s, nvdbUser_modbusLutA);
    TEST_ASSERT(MbCfgStore_RegionValid(nvdbUser_modbusLutA));

    /* ...a failing compile into it must leave it invalid, not stale-valid.
     * That is exactly the cost Modbus_ConfigVerify exists to avoid (§4.9). */
    TEST_ASSERT(compile_str("{\"devices\":[]}", 0, &res) != 0);
    TEST_ASSERT(!MbCfgStore_RegionValid(nvdbUser_modbusLutA));
}

/* ============================================================================
 * Verify — the same pass, writing nothing (docs/modbus.md §4.9)
 * ============================================================================ */

static void test_verify_writes_nothing(void)
{
    sTestStream          s;
    sModbusCompileResult res;
    sMemSource           src;

    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);
    TEST_ASSERT(MbCfgStore_Init() == 0);

    /* Region A holds a config, and a verify must leave it EXACTLY there —
     * that is the whole reason verify exists: a compile consumes the region
     * whether it succeeds or fails, and that region is the fallback. */
    ts_build_worked_example(&s);
    ts_write_region(&s, nvdbUser_modbusLutA);

    static uint8_t before[8192];
    TEST_ASSERT(NvDb_Read(nvdbUser_modbusLutA, before, 0u,
                          sizeof(before)) == nvdbRes_ok);

    src = (sMemSource){ WORKED_EXAMPLE, (uint32_t)strlen(WORKED_EXAMPLE), 0, 0 };
    TEST_ASSERT(MbCfgVerify(mem_source, &src, &res) == 0);
    TEST_ASSERT(res.ok == 1);
    TEST_ASSERT(res.counts.capabilities == 2 && res.counts.points == 5);

    static uint8_t after[8192];
    TEST_ASSERT(NvDb_Read(nvdbUser_modbusLutA, after, 0u,
                          sizeof(after)) == nvdbRes_ok);
    TEST_ASSERT_MEM_EQ(after, before, sizeof(before));
    TEST_ASSERT(MbCfgStore_RegionValid(nvdbUser_modbusLutA));

    /* A bad config reports the same failure a compile would, still writing
     * nothing — same pass, same rules, counting sink. */
    static const char bad[] =
        "{\"capabilities\":[{\"id\":0,\"name\":\"c\","
        "\"blocks\":[{\"base\":0,\"regs\":4}],\"points\":[{\"id\":0,"
        "\"addr\":0,\"fc\":\"input\",\"decodeType\":\"u16\",\"scale\":0.5,"
        "\"unit\":\"V\",\"name\":\"x\"}]}]}";
    src = (sMemSource){ bad, (uint32_t)strlen(bad), 0, 0 };
    TEST_ASSERT(MbCfgVerify(mem_source, &src, &res) != 0);
    TEST_ASSERT(strcmp(res.field, "scale") == 0);
    TEST_ASSERT(MbCfgStore_RegionValid(nvdbUser_modbusLutA));
}

/* The point ceiling is a COUNTING cap; the real ceiling is the 16 KB REGION,
 * and which one bites depends on what else the config carries.  Both
 * boundaries are asserted here because §6 states only the first one.
 */
static char s_bigJson[64 * 1024];

static int build_big_config(int points, int planTables, int perTable)
{
    int n = snprintf(s_bigJson, sizeof(s_bigJson),
                     "{\"capabilities\":[{\"id\":0,\"name\":\"big\","
                     "\"blocks\":[{\"base\":0,\"regs\":400}],\"points\":[");
    for (int i = 0; i < points; i++) {
        n += snprintf(s_bigJson + n, sizeof(s_bigJson) - (size_t)n,
                      "%s{\"id\":%d,\"addr\":%d,\"fc\":\"input\","
                      "\"decodeType\":\"u16\",\"scale\":1,\"unit\":\"V\","
                      "\"name\":\"p%d\"}", i ? "," : "", i, i, i);
    }
    n += snprintf(s_bigJson + n, sizeof(s_bigJson) - (size_t)n, "]}]");

    if (planTables > 0) {
        n += snprintf(s_bigJson + n, sizeof(s_bigJson) - (size_t)n,
                      ",\"devices\":[],\"plans\":[{\"id\":0,\"name\":\"all\","
                      "\"capability\":0,\"devices\":[],\"timeTables\":[");
        int id = 0;
        for (int t = 0; t < planTables; t++) {
            n += snprintf(s_bigJson + n, sizeof(s_bigJson) - (size_t)n,
                          "%s{\"id\":%d,\"everySec\":%d,\"points\":[",
                          t ? "," : "", t, t + 1);
            for (int k = 0; k < perTable; k++, id++) {
                n += snprintf(s_bigJson + n, sizeof(s_bigJson) - (size_t)n,
                              "%s%d", k ? "," : "", id);
            }
            n += snprintf(s_bigJson + n, sizeof(s_bigJson) - (size_t)n, "]}");
        }
        n += snprintf(s_bigJson + n, sizeof(s_bigJson) - (size_t)n, "]}]");
    }
    n += snprintf(s_bigJson + n, sizeof(s_bigJson) - (size_t)n, "}");
    return n;
}

static void test_region_budget_is_the_real_ceiling(void)
{
    sModbusCompileResult res;
    sMemSource           src;

    /* MB_MAX_POINTS_TOTAL points and nothing else DOES fit a 16 KB region. */
    build_big_config(MB_MAX_POINTS_TOTAL, 0, 0);
    src = (sMemSource){ s_bigJson, (uint32_t)strlen(s_bigJson), 0, 0 };
    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);
    if (MbCfgVerify(mem_source, &src, &res) != 0) {
        printf("  maximal point config rejected: %s / %s\n",
               res.field, res.reason);
        test_failures++;
    }
    TEST_ASSERT(res.counts.points == MB_MAX_POINTS_TOTAL);

    /* Add a plan watching all of them and the REGION is what runs out — the
     * count caps are per-section, and nothing sums them.  The compiler must
     * say so rather than run off the end of the region. */
    build_big_config(MB_MAX_POINTS_TOTAL, 6, 64);
    src = (sMemSource){ s_bigJson, (uint32_t)strlen(s_bigJson), 0, 0 };
    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);
    TEST_ASSERT(MbCfgVerify(mem_source, &src, &res) != 0);
    if (strstr(res.reason, "exceeds region size") == NULL) {
        printf("  got '%s' / '%s'\n", res.field, res.reason);
        test_failures++;
    }
}

int main(void)
{
    RUN_TEST(test_worked_example_compiles);
    RUN_TEST(test_one_byte_chunks);
    RUN_TEST(test_jk_dialect_compiles);
    RUN_TEST(test_device_line_parameters);
    RUN_TEST(test_scale_and_bounds_forms);
    RUN_TEST(test_reject_bad_points);
    RUN_TEST(test_reject_capabilities_and_devices);
    RUN_TEST(test_reject_plans);
    RUN_TEST(test_accept_empty_plan_sets);
    RUN_TEST(test_reject_structure);
    RUN_TEST(test_reject_budgets);
    RUN_TEST(test_verify_writes_nothing);
    RUN_TEST(test_region_budget_is_the_real_ceiling);
    RUN_TEST(test_integration_fixture_compiles);
    RUN_TEST(test_jk_fixture_compiles);
    RUN_TEST(test_jk_fixture_derives_blocks);
    RUN_TEST(test_failed_compile_invalidates_previous);
    return test_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
