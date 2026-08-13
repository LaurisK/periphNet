/**
 * Unit tests for Shared/Modbus/modbus_config_store.c over the NOR-faithful
 * flash mock: A/B selector lifecycle (bit-clear swap flag, power-fail
 * recovery), region CRC validation, and the v2 record cursor over the
 * docs/modbus.md §6 worked example.
 */

#include "test_util.h"
#include "modbus_test_stream.h"

#include <stdlib.h>

/* ============================================================================
 * Blank flash
 * ============================================================================ */

static void test_blank_flash_init(void)
{
    mock_flash_reset();

    TEST_ASSERT(MbCfgStore_Init() == 0);
    TEST_ASSERT(MbCfgStore_ActiveBase() == EXT_FLASH_MODBUS_LUT_A_ADDR);
    TEST_ASSERT(MbCfgStore_InactiveBase() == EXT_FLASH_MODBUS_LUT_B_ADDR);
    TEST_ASSERT(!MbCfgStore_RegionValid(MbCfgStore_ActiveBase()));
    TEST_ASSERT(!MbCfgStore_IsSwapPending());

    sMbCfgCursor c;
    TEST_ASSERT(MbCfg_Open(EXT_FLASH_MODBUS_LUT_A_ADDR, &c) != 0);

    sModbusConfigCounts counts;
    TEST_ASSERT(MbCfg_Count(EXT_FLASH_MODBUS_LUT_A_ADDR, &counts) != 0);

    /* Init must be idempotent */
    TEST_ASSERT(MbCfgStore_Init() == 0);
}

/* ============================================================================
 * Region validity + counts
 * ============================================================================ */

static void test_region_valid_and_counts(void)
{
    sTestStream s;

    mock_flash_reset();
    TEST_ASSERT(MbCfgStore_Init() == 0);

    ts_build_worked_example(&s);
    ts_write_region(&s, EXT_FLASH_MODBUS_LUT_A_ADDR);

    TEST_ASSERT(MbCfgStore_RegionValid(EXT_FLASH_MODBUS_LUT_A_ADDR));
    TEST_ASSERT(!MbCfgStore_RegionValid(EXT_FLASH_MODBUS_LUT_B_ADDR));

    sModbusConfigCounts counts;
    TEST_ASSERT(MbCfg_Count(EXT_FLASH_MODBUS_LUT_A_ADDR, &counts) == 0);
    TEST_ASSERT(counts.capabilities == 2);
    TEST_ASSERT(counts.devices == 3);
    TEST_ASSERT(counts.plans == 3);
    TEST_ASSERT(counts.points == 5);      /* records, NOT instances */

    /* Corrupting one stream byte (NOR-legal bit clear) must fail the CRC */
    mock_flash[EXT_FLASH_MODBUS_LUT_A_ADDR + MODBUS_LUT_HEADER_SIZE] &= 0xFE;
    TEST_ASSERT(!MbCfgStore_RegionValid(EXT_FLASH_MODBUS_LUT_A_ADDR));
}

/* "Invalid is erased, not repaired" (§4.2) collapses to one state. */
static void test_region_erase(void)
{
    sTestStream s;

    mock_flash_reset();
    TEST_ASSERT(MbCfgStore_Init() == 0);
    ts_build_worked_example(&s);
    ts_write_region(&s, EXT_FLASH_MODBUS_LUT_A_ADDR);
    TEST_ASSERT(MbCfgStore_RegionValid(EXT_FLASH_MODBUS_LUT_A_ADDR));

    TEST_ASSERT(MbCfgStore_EraseRegion(EXT_FLASH_MODBUS_LUT_A_ADDR) == 0);
    TEST_ASSERT(!MbCfgStore_RegionValid(EXT_FLASH_MODBUS_LUT_A_ADDR));
}

/* ============================================================================
 * Cursor traversal — the stream's own order
 * ============================================================================ */

static void test_cursor_traversal(void)
{
    sTestStream s;

    mock_flash_reset();
    TEST_ASSERT(MbCfgStore_Init() == 0);
    ts_build_worked_example(&s);
    ts_write_region(&s, EXT_FLASH_MODBUS_LUT_A_ADDR);

    sMbCfgCursor            c;
    sModbusCapabilityRecord cap;
    sModbusBlockRecord      blocks[MB_MAX_BLOCKS_PER_CAP];
    sModbusPointRecord      pt;
    sModbusDeviceRecord     dev;
    sModbusPlanRecord       plan;
    sModbusTimeTableRecord  tt;
    uint16_t                ids[8];

    TEST_ASSERT(MbCfg_Open(EXT_FLASH_MODBUS_LUT_A_ADDR, &c) == 0);

    /* capability 0 */
    TEST_ASSERT(MbCfg_NextCapability(&c, &cap) == 1);
    TEST_ASSERT(strcmp(cap.name, "solis") == 0);
    TEST_ASSERT(cap.addrStride == 1 && cap.writeFc == 6);
    TEST_ASSERT(cap.blockCount == 1);
    TEST_ASSERT(MbCfg_ReadBlocks(&c, blocks, cap.blockCount) == 0);
    TEST_ASSERT(blocks[0].base == 3132 && blocks[0].regs == 16);

    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 1);
    TEST_ASSERT(pt.addr == 3132 && pt.scalePow10 == -1);
    TEST_ASSERT(pt.functionCode == mbFc_input);
    TEST_ASSERT(strcmp(pt.name, "battery_voltage") == 0);

    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 1);
    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 1);
    TEST_ASSERT(strcmp(pt.name, "battery_soc") == 0);

    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 1);
    TEST_ASSERT(strcmp(pt.name, "overdischarge_soc_set") == 0);
    TEST_ASSERT(pt.flags & MB_PT_WRITE);
    TEST_ASSERT(pt.flags & MB_PT_BOUNDED);
    TEST_ASSERT(pt.functionCode == mbFc_holding);   /* w/rw needs holding */
    TEST_ASSERT(pt.writeMin == 5 && pt.writeMax == 40);

    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 0);     /* point sentinel */

    /* capability 1, then the sentinel */
    TEST_ASSERT(MbCfg_NextCapability(&c, &cap) == 1);
    TEST_ASSERT(strcmp(cap.name, "meter") == 0);
    TEST_ASSERT(MbCfg_ReadBlocks(&c, blocks, cap.blockCount) == 0);
    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 1);
    TEST_ASSERT(pt.decodeType == mbDecode_float32Be);
    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 0);
    TEST_ASSERT(MbCfg_NextCapability(&c, &cap) == 0);

    /* devices — two of them share capability 0 */
    TEST_ASSERT(MbCfg_NextDevice(&c, &dev) == 1);
    TEST_ASSERT(dev.slaveAddr == 1 && dev.capId == 0);
    TEST_ASSERT(strcmp(dev.topicPrefix, "periphnet") == 0);
    TEST_ASSERT(MbCfg_NextDevice(&c, &dev) == 1);
    TEST_ASSERT(dev.slaveAddr == 3 && dev.capId == 0);
    TEST_ASSERT(MbCfg_NextDevice(&c, &dev) == 1);
    TEST_ASSERT(dev.slaveAddr == 2 && dev.capId == 1);
    TEST_ASSERT(MbCfg_NextDevice(&c, &dev) == 0);

    /* plans */
    TEST_ASSERT(MbCfg_NextPlan(&c, &plan) == 1);
    TEST_ASSERT(strcmp(plan.name, "inv_fast") == 0);
    TEST_ASSERT(plan.planId == 0 && plan.capId == 0 && plan.devices == 0x01);

    TEST_ASSERT(MbCfg_NextTimeTable(&c, &tt) == 1);
    TEST_ASSERT(tt.period_sec == 5 && tt.entryCount == 3);
    TEST_ASSERT(MbCfg_ReadPointIds(&c, ids, tt.entryCount) == 0);
    TEST_ASSERT(ids[0] == 0 && ids[1] == 1 && ids[2] == 2);

    TEST_ASSERT(MbCfg_NextTimeTable(&c, &tt) == 1);
    TEST_ASSERT(tt.period_sec == 60 && tt.entryCount == 1);
    TEST_ASSERT(MbCfg_ReadPointIds(&c, ids, tt.entryCount) == 0);
    TEST_ASSERT(ids[0] == 3);
    TEST_ASSERT(MbCfg_NextTimeTable(&c, &tt) == 0);

    TEST_ASSERT(MbCfg_NextPlan(&c, &plan) == 1);
    TEST_ASSERT(plan.planId == 2);
    TEST_ASSERT(MbCfg_NextTimeTable(&c, &tt) == 1);
    TEST_ASSERT(MbCfg_ReadPointIds(&c, ids, tt.entryCount) == 0);
    TEST_ASSERT(MbCfg_NextTimeTable(&c, &tt) == 0);

    TEST_ASSERT(MbCfg_NextPlan(&c, &plan) == 1);
    TEST_ASSERT(plan.planId == 3 && plan.capId == 1);
    TEST_ASSERT(MbCfg_NextTimeTable(&c, &tt) == 1);
    TEST_ASSERT(MbCfg_ReadPointIds(&c, ids, tt.entryCount) == 0);
    TEST_ASSERT(MbCfg_NextTimeTable(&c, &tt) == 0);

    TEST_ASSERT(MbCfg_NextPlan(&c, &plan) == 0);    /* end of stream */
}

static void test_truncated_stream_detected(void)
{
    sTestStream s;

    mock_flash_reset();
    TEST_ASSERT(MbCfgStore_Init() == 0);

    /* Stream missing the final plan sentinel */
    ts_build_worked_example(&s);
    s.len -= sizeof(sModbusPlanRecord);
    ts_write_region(&s, EXT_FLASH_MODBUS_LUT_A_ADDR);

    /* CRC still matches (header written over the truncated stream), but a
     * structural walk must fail instead of running off the end. */
    TEST_ASSERT(MbCfgStore_RegionValid(EXT_FLASH_MODBUS_LUT_A_ADDR));
    sModbusConfigCounts counts;
    TEST_ASSERT(MbCfg_Count(EXT_FLASH_MODBUS_LUT_A_ADDR, &counts) != 0);
}

/* ============================================================================
 * Section navigation and ordinal lookup — the addressing the API uses
 * ============================================================================ */

static void test_open_capability_and_find(void)
{
    sTestStream s;

    mock_flash_reset();
    TEST_ASSERT(MbCfgStore_Init() == 0);
    ts_build_worked_example(&s);
    ts_write_region(&s, EXT_FLASH_MODBUS_LUT_A_ADDR);

    sMbCfgCursor            c;
    sModbusCapabilityRecord cap;
    sModbusBlockRecord      blocks[MB_MAX_BLOCKS_PER_CAP];

    TEST_ASSERT(MbCfg_OpenCapability(EXT_FLASH_MODBUS_LUT_A_ADDR, 1, &c, &cap,
                                     blocks, MB_MAX_BLOCKS_PER_CAP) == 0);
    TEST_ASSERT(strcmp(cap.name, "meter") == 0);
    TEST_ASSERT(blocks[0].base == 0 && blocks[0].regs == 8);

    sModbusPointRecord pt;
    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 1);     /* cursor left at points */
    TEST_ASSERT(strcmp(pt.name, "power") == 0);

    TEST_ASSERT(MbCfg_OpenCapability(EXT_FLASH_MODBUS_LUT_A_ADDR, 2, &c, &cap,
                                     NULL, 0) != 0);

    sModbusDeviceRecord dev;
    TEST_ASSERT(MbCfg_FindDevice(EXT_FLASH_MODBUS_LUT_A_ADDR, 2, &dev) == 0);
    TEST_ASSERT(dev.slaveAddr == 2 && dev.capId == 1);
    TEST_ASSERT(MbCfg_FindDevice(EXT_FLASH_MODBUS_LUT_A_ADDR, 3, &dev) != 0);

    TEST_ASSERT(MbCfg_FindPoint(EXT_FLASH_MODBUS_LUT_A_ADDR, 0, 3, &pt) == 0);
    TEST_ASSERT(strcmp(pt.name, "overdischarge_soc_set") == 0);
    TEST_ASSERT(MbCfg_FindPoint(EXT_FLASH_MODBUS_LUT_A_ADDR, 0, 4, &pt) != 0);
}

static void test_resolve_point_by_ordinal(void)
{
    sTestStream s;

    mock_flash_reset();
    TEST_ASSERT(MbCfgStore_Init() == 0);
    ts_build_worked_example(&s);
    ts_write_region(&s, EXT_FLASH_MODBUS_LUT_A_ADDR);

    sMbPointLookup lk;

    TEST_ASSERT(MbCfg_ResolvePoint(0, 0, &lk) == 0);
    TEST_ASSERT(lk.slaveAddr == 1);
    TEST_ASSERT(lk.functionCode == mbFc_input);
    TEST_ASSERT(lk.regAddr == 3132);        /* verbatim, no offset arithmetic */
    TEST_ASSERT(lk.addrStride == 1);
    TEST_ASSERT(strcmp(lk.point.name, "battery_voltage") == 0);

    /* Devices 0 and 1 SHARE capability 0, so the same ptOrd is the same point
     * on a different slave — the cardinality that makes four packs one map. */
    TEST_ASSERT(MbCfg_ResolvePoint(1, 0, &lk) == 0);
    TEST_ASSERT(lk.slaveAddr == 3);
    TEST_ASSERT(strcmp(lk.point.name, "battery_voltage") == 0);

    /* Device 2 has its own capability, so ptOrd 0 is a different point. */
    TEST_ASSERT(MbCfg_ResolvePoint(2, 0, &lk) == 0);
    TEST_ASSERT(lk.slaveAddr == 2);
    TEST_ASSERT(strcmp(lk.point.name, "power") == 0);

    TEST_ASSERT(MbCfg_ResolvePoint(0, 4, &lk) != 0);
    TEST_ASSERT(MbCfg_ResolvePoint(3, 0, &lk) != 0);
}

/* ============================================================================
 * Plan headers — the resident table, and the gap that would truncate it
 * ============================================================================ */

static void test_plan_headers_with_a_gap(void)
{
    sTestStream s;

    mock_flash_reset();
    TEST_ASSERT(MbCfgStore_Init() == 0);
    ts_build_worked_example(&s);
    ts_write_region(&s, EXT_FLASH_MODBUS_LUT_A_ADDR);

    sMbPlanHeader hdr[MB_MAX_PLANS];
    TEST_ASSERT(MbCfg_ReadPlanHeaders(EXT_FLASH_MODBUS_LUT_A_ADDR, hdr) == 3);

    /* Slots 0, 2 and 3 are used; 1 is a hole and simply has no record.  This
     * is THE case a blank record would break: a stored blank would look like
     * the name[0] == 0 sentinel and truncate the section, so assert that the
     * plans AFTER the hole are still found. */
    TEST_ASSERT(hdr[0].used && strcmp(hdr[0].rec.name, "inv_fast") == 0);
    TEST_ASSERT(hdr[0].tableCount == 2);
    TEST_ASSERT(!hdr[1].used);
    TEST_ASSERT(hdr[2].used && strcmp(hdr[2].rec.name, "inv_lazy") == 0);
    TEST_ASSERT(hdr[2].tableCount == 1);
    TEST_ASSERT(hdr[3].used && strcmp(hdr[3].rec.name, "meter_plan") == 0);
    TEST_ASSERT(hdr[3].rec.capId == 1);
    TEST_ASSERT(!hdr[4].used && !hdr[7].used);
}

/* ============================================================================
 * Selector swap lifecycle
 * ============================================================================ */

static void test_swap_lifecycle(void)
{
    sTestStream s;

    mock_flash_reset();
    TEST_ASSERT(MbCfgStore_Init() == 0);
    ts_build_worked_example(&s);
    ts_write_region(&s, EXT_FLASH_MODBUS_LUT_A_ADDR);

    /* Inactive (B) holds nothing valid → apply must refuse */
    TEST_ASSERT(MbCfgStore_SetSwapPending() != 0);
    TEST_ASSERT(!MbCfgStore_IsSwapPending());

    /* Upload a config into the inactive region, then apply */
    ts_write_region(&s, EXT_FLASH_MODBUS_LUT_B_ADDR);
    TEST_ASSERT(MbCfgStore_SetSwapPending() == 0);
    TEST_ASSERT(MbCfgStore_IsSwapPending());
    TEST_ASSERT(MbCfgStore_ActiveBase() == EXT_FLASH_MODBUS_LUT_A_ADDR);

    TEST_ASSERT(MbCfgStore_CommitSwap() == 0);
    TEST_ASSERT(MbCfgStore_ActiveBase() == EXT_FLASH_MODBUS_LUT_B_ADDR);
    TEST_ASSERT(!MbCfgStore_IsSwapPending());

    /* Survives a "reboot" */
    TEST_ASSERT(MbCfgStore_Init() == 0);
    TEST_ASSERT(MbCfgStore_ActiveBase() == EXT_FLASH_MODBUS_LUT_B_ADDR);
}

static void test_selector_recovery_prefers_valid_region(void)
{
    sTestStream s;

    mock_flash_reset();
    TEST_ASSERT(MbCfgStore_Init() == 0);
    ts_build_worked_example(&s);
    ts_write_region(&s, EXT_FLASH_MODBUS_LUT_B_ADDR);

    /* Wipe the selector, as a power cut during CommitSwap would */
    W25Q128_EraseSector(EXT_FLASH_MODBUS_SEL_ADDR);

    TEST_ASSERT(MbCfgStore_Init() == 0);
    TEST_ASSERT(MbCfgStore_ActiveBase() == EXT_FLASH_MODBUS_LUT_B_ADDR);

    /* Both regions valid → prefer A (deterministic) */
    ts_write_region(&s, EXT_FLASH_MODBUS_LUT_A_ADDR);
    W25Q128_EraseSector(EXT_FLASH_MODBUS_SEL_ADDR);
    TEST_ASSERT(MbCfgStore_Init() == 0);
    TEST_ASSERT(MbCfgStore_ActiveBase() == EXT_FLASH_MODBUS_LUT_A_ADDR);
}

int main(void)
{
    RUN_TEST(test_blank_flash_init);
    RUN_TEST(test_region_valid_and_counts);
    RUN_TEST(test_region_erase);
    RUN_TEST(test_cursor_traversal);
    RUN_TEST(test_truncated_stream_detected);
    RUN_TEST(test_open_capability_and_find);
    RUN_TEST(test_resolve_point_by_ordinal);
    RUN_TEST(test_plan_headers_with_a_gap);
    RUN_TEST(test_swap_lifecycle);
    RUN_TEST(test_selector_recovery_prefers_valid_region);
    return test_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
