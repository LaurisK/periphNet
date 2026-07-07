/**
 * Unit tests for Shared/Modbus/modbus_config_store.c over the NOR-faithful
 * flash mock: A/B selector lifecycle (bit-clear swap flag, power-fail
 * recovery), region CRC validation, and the record cursor over the
 * design-doc §2 worked example.
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

    sMbCfgCounts counts;
    TEST_ASSERT(MbCfg_Count(EXT_FLASH_MODBUS_LUT_A_ADDR, &counts) != 0);

    /* Init must be idempotent */
    TEST_ASSERT(MbCfgStore_Init() == 0);
}

/* ============================================================================
 * Region validity + cursor traversal of the worked example
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

    sMbCfgCounts counts;
    TEST_ASSERT(MbCfg_Count(EXT_FLASH_MODBUS_LUT_A_ADDR, &counts) == 0);
    TEST_ASSERT(counts.devices == 2);
    TEST_ASSERT(counts.transactions == 2);
    TEST_ASSERT(counts.points == 5);

    /* Corrupting one stream byte (NOR-legal bit clear of slaveAddr=1's
     * set bit) must fail the CRC */
    mock_flash[EXT_FLASH_MODBUS_LUT_A_ADDR + MODBUS_LUT_HEADER_SIZE] &= 0xFE;
    TEST_ASSERT(!MbCfgStore_RegionValid(EXT_FLASH_MODBUS_LUT_A_ADDR));
}

static void test_cursor_traversal(void)
{
    sTestStream s;

    mock_flash_reset();
    TEST_ASSERT(MbCfgStore_Init() == 0);
    ts_build_worked_example(&s);
    ts_write_region(&s, EXT_FLASH_MODBUS_LUT_A_ADDR);

    sMbCfgCursor             c;
    sModbusDeviceRecord      dev;
    sModbusTransactionRecord txn;
    sModbusPointRecord       pt;

    TEST_ASSERT(MbCfg_Open(EXT_FLASH_MODBUS_LUT_A_ADDR, &c) == 0);

    /* Device 1 */
    TEST_ASSERT(MbCfg_NextDevice(&c, &dev) == 1);
    TEST_ASSERT(dev.slaveAddr == 1);
    TEST_ASSERT(strcmp(dev.topicPrefix, "periphnet") == 0);

    TEST_ASSERT(MbCfg_NextTransaction(&c, &txn) == 1);
    TEST_ASSERT(txn.count == 8);                 /* derived: 7 + 1 */
    TEST_ASSERT(txn.functionCode == MB_FC_INPUT);
    TEST_ASSERT(txn.startAddr == 3132);
    TEST_ASSERT(txn.readPeriodS == 5);

    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 1);
    TEST_ASSERT(pt.decodeType == MB_DECODE_U16);
    TEST_ASSERT(pt.scalePow10 == -1);
    TEST_ASSERT(strcmp(pt.name, "battery_voltage") == 0);

    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 1);
    TEST_ASSERT(pt.decodeType == MB_DECODE_S16);
    TEST_ASSERT(strcmp(pt.name, "battery_current") == 0);

    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 1);
    TEST_ASSERT(strcmp(pt.name, "battery_soc") == 0);
    TEST_ASSERT(pt.publishThreshold == 1);
    TEST_ASSERT(pt.publishHeartbeatS == 300);

    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 1);
    TEST_ASSERT(strcmp(pt.name, "overdischarge_soc_set") == 0);
    TEST_ASSERT(pt.flags & MB_POINT_FLAG_WRITABLE);
    TEST_ASSERT(pt.writeMin == 5 && pt.writeMax == 40);

    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 0);       /* point sentinel */
    TEST_ASSERT(MbCfg_NextTransaction(&c, &txn) == 0); /* txn sentinel  */

    /* Device 2 */
    TEST_ASSERT(MbCfg_NextDevice(&c, &dev) == 1);
    TEST_ASSERT(dev.slaveAddr == 2);
    TEST_ASSERT(strcmp(dev.topicPrefix, "periphnet_meter") == 0);

    TEST_ASSERT(MbCfg_NextTransaction(&c, &txn) == 1);
    TEST_ASSERT(txn.count == 2);                 /* float32 = 2 regs */
    TEST_ASSERT(txn.startAddr == 0);

    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 1);
    TEST_ASSERT(pt.decodeType == MB_DECODE_FLOAT32_BE);
    TEST_ASSERT(strcmp(pt.name, "power") == 0);

    TEST_ASSERT(MbCfg_NextPoint(&c, &pt) == 0);
    TEST_ASSERT(MbCfg_NextTransaction(&c, &txn) == 0);

    TEST_ASSERT(MbCfg_NextDevice(&c, &dev) == 0);      /* end of config */
}

static void test_truncated_stream_detected(void)
{
    sTestStream s;

    mock_flash_reset();
    TEST_ASSERT(MbCfgStore_Init() == 0);

    /* Stream missing the final device sentinel */
    ts_build_worked_example(&s);
    s.len -= sizeof(sModbusDeviceRecord);
    ts_write_region(&s, EXT_FLASH_MODBUS_LUT_A_ADDR);

    /* CRC still matches (header written over the truncated stream), but a
     * structural walk must fail instead of running off the end. */
    TEST_ASSERT(MbCfgStore_RegionValid(EXT_FLASH_MODBUS_LUT_A_ADDR));
    sMbCfgCounts counts;
    TEST_ASSERT(MbCfg_Count(EXT_FLASH_MODBUS_LUT_A_ADDR, &counts) != 0);
}

/* ============================================================================
 * Writable point lookup
 * ============================================================================ */

static void test_find_writable_point(void)
{
    sTestStream s;

    mock_flash_reset();
    TEST_ASSERT(MbCfgStore_Init() == 0);
    ts_build_worked_example(&s);
    ts_write_region(&s, EXT_FLASH_MODBUS_LUT_A_ADDR);

    sMbPointLookup lk;
    TEST_ASSERT(MbCfg_FindWritablePoint("periphnet",
                                        "overdischarge_soc_set", &lk) == 0);
    TEST_ASSERT(lk.slaveAddr == 1);
    TEST_ASSERT(lk.regAddr == 3132 + 7);
    TEST_ASSERT(lk.point.writeMin == 5 && lk.point.writeMax == 40);

    /* Non-writable point, wrong prefix, unknown name → no match */
    TEST_ASSERT(MbCfg_FindWritablePoint("periphnet", "battery_soc", &lk) != 0);
    TEST_ASSERT(MbCfg_FindWritablePoint("periphnet_meter",
                                        "overdischarge_soc_set", &lk) != 0);
    TEST_ASSERT(MbCfg_FindWritablePoint("periphnet", "nonexistent", &lk) != 0);
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

    /* The bit-clear must not have corrupted the CRC-protected header */
    TEST_ASSERT(MbCfgStore_Init() == 0);
    TEST_ASSERT(MbCfgStore_ActiveBase() == EXT_FLASH_MODBUS_LUT_A_ADDR);
    TEST_ASSERT(MbCfgStore_IsSwapPending());

    /* Walker commits at its lap boundary */
    TEST_ASSERT(MbCfgStore_CommitSwap() == 0);
    TEST_ASSERT(MbCfgStore_ActiveBase() == EXT_FLASH_MODBUS_LUT_B_ADDR);
    TEST_ASSERT(MbCfgStore_InactiveBase() == EXT_FLASH_MODBUS_LUT_A_ADDR);
    TEST_ASSERT(!MbCfgStore_IsSwapPending());

    /* Survives re-init */
    TEST_ASSERT(MbCfgStore_Init() == 0);
    TEST_ASSERT(MbCfgStore_ActiveBase() == EXT_FLASH_MODBUS_LUT_B_ADDR);
}

static void test_selector_recovery_prefers_valid_region(void)
{
    sTestStream s;

    mock_flash_reset();
    ts_build_worked_example(&s);

    /* Power fail during CommitSwap: selector sector erased, only B valid */
    ts_write_region(&s, EXT_FLASH_MODBUS_LUT_B_ADDR);
    TEST_ASSERT(MbCfgStore_Init() == 0);
    TEST_ASSERT(MbCfgStore_ActiveBase() == EXT_FLASH_MODBUS_LUT_B_ADDR);

    /* Corrupt selector header (NOR-legal bit clear) → same recovery */
    mock_flash[EXT_FLASH_MODBUS_SEL_ADDR + 1] &= 0xFE;
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
    RUN_TEST(test_cursor_traversal);
    RUN_TEST(test_truncated_stream_detected);
    RUN_TEST(test_find_writable_point);
    RUN_TEST(test_swap_lifecycle);
    RUN_TEST(test_selector_recovery_prefers_valid_region);
    return test_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
