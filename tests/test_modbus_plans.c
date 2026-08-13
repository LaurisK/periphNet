/**
 * Unit tests for Shared/Modbus/modbus_config_plans.c — runtime plan edits.
 *
 * The load-bearing assertion is that a rewrite PERTURBS NOTHING IT DID NOT
 * TOUCH: capabilities, devices and every other plan must come out
 * byte-identical, with a correct header CRC and streamLen.  A rewrite that
 * disturbs an untouched section is the failure mode that would otherwise be
 * found on hardware, after a swap (docs/modbus.md §9).
 */

#include "test_util.h"
#include "modbus_test_stream.h"
#include "modbus_config_plans.h"

#include <stdlib.h>

#define REG_A EXT_FLASH_MODBUS_LUT_A_ADDR
#define REG_B EXT_FLASH_MODBUS_LUT_B_ADDR

static void load_worked_example(void)
{
    sTestStream s;

    mock_flash_reset();
    TEST_ASSERT(MbCfgStore_Init() == 0);
    ts_build_worked_example(&s);
    ts_write_region(&s, REG_A);
}

/* Read a region's stream and header. */
static uint32_t read_stream(uint32_t base, uint8_t *out, uint32_t max,
                            sModbusLutHeader *hdr)
{
    TEST_ASSERT(W25Q128_Read(base, (uint8_t *)hdr, sizeof(*hdr)) == w25q_ok);
    TEST_ASSERT(hdr->streamLen <= max);
    TEST_ASSERT(W25Q128_Read(base + MODBUS_LUT_HEADER_SIZE, out,
                             hdr->streamLen) == w25q_ok);
    return hdr->streamLen;
}

/* Offset at which the plan section starts, i.e. how much of the stream must
 * survive an edit byte for byte. */
static uint32_t plan_section_offset(uint32_t base)
{
    sMbCfgCursor c;
    TEST_ASSERT(MbCfg_SeekPlans(base, &c) == 0);
    return c.off;
}

static void assert_region_valid(uint32_t base)
{
    static uint8_t     buf[8192];
    sModbusLutHeader   hdr;
    uint32_t           len = read_stream(base, buf, sizeof(buf), &hdr);

    TEST_ASSERT(hdr.magic == MODBUS_LUT_MAGIC);
    TEST_ASSERT(hdr.version == MODBUS_LUT_VERSION);
    TEST_ASSERT(hdr.crc32 == ImgMgmt_Crc32(buf, len));
    TEST_ASSERT(MbCfgStore_RegionValid(base));
}

/* ============================================================================
 * Validation
 * ============================================================================ */

static void test_validate_accepts_and_rejects(void)
{
    static const uint16_t ok[]    = { 0, 1 };
    static const uint16_t dup[]   = { 0, 0 };
    static const uint16_t wOnly[] = { 3 };   /* overdischarge is rw, readable */
    static const uint16_t bad[]   = { 9 };

    load_worked_example();

    sModbusTimeTableSpec tt = { 5, ok, 2 };
    sModbusPlanSpec      spec = { "p", &tt, 0, 1, 0x01 };

    TEST_ASSERT(MbCfgPlans_Validate(REG_A, &spec) == mbPlan_ok);

    /* A device that does not exist, and one that implements another
     * capability (device 2 is the meter, capability 1) */
    spec.devices = 0x80;
    TEST_ASSERT(MbCfgPlans_Validate(REG_A, &spec) == mbPlan_errNoDevice);
    spec.devices = 0x04;
    TEST_ASSERT(MbCfgPlans_Validate(REG_A, &spec) == mbPlan_errDeviceCap);
    spec.devices = 0x01;

    /* No such capability */
    spec.capId = 7;
    TEST_ASSERT(MbCfgPlans_Validate(REG_A, &spec) == mbPlan_errNoCap);
    spec.capId = 0;

    /* A point id past the capability's count */
    sModbusTimeTableSpec ttBad = { 5, bad, 1 };
    spec.tables = &ttBad;
    TEST_ASSERT(MbCfgPlans_Validate(REG_A, &spec) == mbPlan_errNoPoint);

    /* The same point twice in one plan */
    sModbusTimeTableSpec ttDup = { 5, dup, 2 };
    spec.tables = &ttDup;
    TEST_ASSERT(MbCfgPlans_Validate(REG_A, &spec) == mbPlan_errDuplicate);

    /* period 0 */
    sModbusTimeTableSpec ttZero = { 0, ok, 2 };
    spec.tables = &ttZero;
    TEST_ASSERT(MbCfgPlans_Validate(REG_A, &spec) == mbPlan_errPeriod);

    /* An empty device set and no time tables at all are BOTH legal: they are
     * the two ways of saying "capable but unmonitored" (§3.1). */
    sModbusPlanSpec empty = { "idle", NULL, 0, 0, 0x00 };
    TEST_ASSERT(MbCfgPlans_Validate(REG_A, &empty) == mbPlan_ok);

    /* A readable point in a table is fine; the worked example has no w-only
     * point, so the write-only rule is covered by the compiler suite. */
    sModbusTimeTableSpec ttRw = { 60, wOnly, 1 };
    spec.tables = &ttRw;
    TEST_ASSERT(MbCfgPlans_Validate(REG_A, &spec) == mbPlan_ok);
}

/* ============================================================================
 * Rewrite — what must NOT change
 * ============================================================================ */

static void test_modify_leaves_everything_else_byte_identical(void)
{
    static uint8_t before[8192], after[8192];
    sModbusLutHeader h1, h2;

    load_worked_example();

    uint32_t planOff = plan_section_offset(REG_A);
    uint32_t lenA    = read_stream(REG_A, before, sizeof(before), &h1);
    TEST_ASSERT(planOff > 0 && planOff < lenA);

    /* Retune plan 0: a different device set and a different period. */
    static const uint16_t pts[] = { 0, 2 };
    sModbusTimeTableSpec  tt    = { 15, pts, 2 };
    sModbusPlanSpec       spec  = { "inv_fast", &tt, 0, 1, 0x03 };

    TEST_ASSERT(MbCfgPlans_Rewrite(REG_A, REG_B, 0, &spec, NULL) == mbPlan_ok);
    assert_region_valid(REG_B);

    read_stream(REG_B, after, sizeof(after), &h2);

    /* Everything before the plan section is untouched, byte for byte. */
    TEST_ASSERT_MEM_EQ(after, before, planOff);

    /* And the other two plans survived with their slots and bodies. */
    sMbPlanHeader hdr[MB_MAX_PLANS];
    TEST_ASSERT(MbCfg_ReadPlanHeaders(REG_B, hdr) == 3);
    TEST_ASSERT(hdr[0].used && strcmp(hdr[0].rec.name, "inv_fast") == 0);
    TEST_ASSERT(hdr[0].rec.devices == 0x03);
    TEST_ASSERT(hdr[0].tableCount == 1);
    TEST_ASSERT(!hdr[1].used);
    TEST_ASSERT(hdr[2].used && strcmp(hdr[2].rec.name, "inv_lazy") == 0);
    TEST_ASSERT(hdr[2].tableCount == 1);
    TEST_ASSERT(hdr[3].used && strcmp(hdr[3].rec.name, "meter_plan") == 0);
    TEST_ASSERT(hdr[3].rec.capId == 1);

    /* The edited plan's new time table is what we asked for. */
    sMbCfgCursor           c;
    sModbusPlanRecord      plan;
    sModbusTimeTableRecord ttr;
    uint16_t               ids[8];

    TEST_ASSERT(MbCfg_SeekPlans(REG_B, &c) == 0);
    TEST_ASSERT(MbCfg_NextPlan(&c, &plan) == 1);
    TEST_ASSERT(plan.planId == 0);
    TEST_ASSERT(MbCfg_NextTimeTable(&c, &ttr) == 1);
    TEST_ASSERT(ttr.period_sec == 15 && ttr.entryCount == 2);
    TEST_ASSERT(MbCfg_ReadPointIds(&c, ids, 2) == 0);
    TEST_ASSERT(ids[0] == 0 && ids[1] == 2);
    TEST_ASSERT(MbCfg_NextTimeTable(&c, &ttr) == 0);
}

static void test_delete_frees_a_slot_and_moves_no_other(void)
{
    static uint8_t before[8192], after[8192];
    sModbusLutHeader h1, h2;

    load_worked_example();
    uint32_t planOff = plan_section_offset(REG_A);
    read_stream(REG_A, before, sizeof(before), &h1);

    TEST_ASSERT(MbCfgPlans_Rewrite(REG_A, REG_B, 2, NULL, NULL) == mbPlan_ok);
    assert_region_valid(REG_B);
    read_stream(REG_B, after, sizeof(after), &h2);
    TEST_ASSERT_MEM_EQ(after, before, planOff);

    /* Slot 2 is gone; slots 0 and 3 kept their ids — THAT is what makes a slot
     * a slot: deleting one never re-points a subscriber's mask bit (§3.5). */
    sMbPlanHeader hdr[MB_MAX_PLANS];
    TEST_ASSERT(MbCfg_ReadPlanHeaders(REG_B, hdr) == 2);
    TEST_ASSERT(hdr[0].used && strcmp(hdr[0].rec.name, "inv_fast") == 0);
    TEST_ASSERT(!hdr[2].used);
    TEST_ASSERT(hdr[3].used && strcmp(hdr[3].rec.name, "meter_plan") == 0);

    /* The stream shrank by exactly the deleted plan's records. */
    TEST_ASSERT(h2.streamLen < h1.streamLen);
}

static void test_create_lands_in_slot_order(void)
{
    load_worked_example();

    /* Slot 1 is the hole in the worked example; a create there must be written
     * BETWEEN slots 0 and 2, because plans are stored in ascending slot
     * order. */
    static const uint16_t pts[] = { 1 };
    sModbusTimeTableSpec  tt    = { 30, pts, 1 };
    sModbusPlanSpec       spec  = { "inserted", &tt, 0, 1, 0x02 };

    TEST_ASSERT(MbCfgPlans_Rewrite(REG_A, REG_B, 1, &spec, NULL) == mbPlan_ok);
    assert_region_valid(REG_B);

    sMbCfgCursor      c;
    sModbusPlanRecord plan;
    uint8_t           order[4];
    int               n = 0;

    TEST_ASSERT(MbCfg_SeekPlans(REG_B, &c) == 0);
    while (MbCfg_NextPlan(&c, &plan) == 1 && n < 4) {
        sModbusTimeTableRecord ttr;
        int rt;

        order[n++] = plan.planId;
        while ((rt = MbCfg_NextTimeTable(&c, &ttr)) == 1) {
            TEST_ASSERT(MbCfg_ReadPointIds(&c, NULL, ttr.entryCount) == 0);
        }
        TEST_ASSERT(rt == 0);
    }
    TEST_ASSERT(n == 4);
    TEST_ASSERT(order[0] == 0 && order[1] == 1 && order[2] == 2 &&
                order[3] == 3);

    sMbPlanHeader hdr[MB_MAX_PLANS];
    TEST_ASSERT(MbCfg_ReadPlanHeaders(REG_B, hdr) == 4);
    TEST_ASSERT(hdr[1].used && strcmp(hdr[1].rec.name, "inserted") == 0);
}

/* A create into the highest slot has no plan to land before. */
static void test_create_at_the_end(void)
{
    load_worked_example();

    static const uint16_t pts[] = { 0 };
    sModbusTimeTableSpec  tt    = { 42, pts, 1 };
    sModbusPlanSpec       spec  = { "last", &tt, 0, 1, 0x01 };

    TEST_ASSERT(MbCfgPlans_Rewrite(REG_A, REG_B, 7, &spec, NULL) == mbPlan_ok);
    assert_region_valid(REG_B);

    sMbPlanHeader hdr[MB_MAX_PLANS];
    TEST_ASSERT(MbCfg_ReadPlanHeaders(REG_B, hdr) == 4);
    TEST_ASSERT(hdr[7].used && strcmp(hdr[7].rec.name, "last") == 0);
    TEST_ASSERT(hdr[7].tableCount == 1);
}

/* An invalid edit must not touch the destination at all. */
static void test_invalid_edit_writes_nothing(void)
{
    load_worked_example();

    static const uint16_t pts[] = { 99 };
    sModbusTimeTableSpec  tt    = { 5, pts, 1 };
    sModbusPlanSpec       spec  = { "bogus", &tt, 0, 1, 0x01 };

    TEST_ASSERT(MbCfgPlans_Rewrite(REG_A, REG_B, 4, &spec, NULL) ==
                mbPlan_errNoPoint);
    TEST_ASSERT(!MbCfgStore_RegionValid(REG_B));
    TEST_ASSERT(MbCfgStore_RegionValid(REG_A));   /* the source is untouched */
}

/* Edits compose: modify, swap, modify again off the new active region. */
static void test_successive_edits(void)
{
    load_worked_example();

    static const uint16_t pts[] = { 0 };
    sModbusTimeTableSpec  tt    = { 7, pts, 1 };
    sModbusPlanSpec       spec  = { "one", &tt, 0, 1, 0x01 };

    TEST_ASSERT(MbCfgPlans_Rewrite(REG_A, REG_B, 0, &spec, NULL) == mbPlan_ok);
    TEST_ASSERT(MbCfgStore_SetSwapPending() == 0);
    TEST_ASSERT(MbCfgStore_CommitSwap() == 0);
    TEST_ASSERT(MbCfgStore_ActiveBase() == REG_B);

    sModbusPlanSpec spec2 = { "two", &tt, 0, 1, 0x02 };
    TEST_ASSERT(MbCfgPlans_Rewrite(REG_B, REG_A, 2, &spec2, NULL) ==
                mbPlan_ok);
    assert_region_valid(REG_A);

    sMbPlanHeader hdr[MB_MAX_PLANS];
    TEST_ASSERT(MbCfg_ReadPlanHeaders(REG_A, hdr) == 3);
    TEST_ASSERT(strcmp(hdr[0].rec.name, "one") == 0);
    TEST_ASSERT(strcmp(hdr[2].rec.name, "two") == 0);
    TEST_ASSERT(hdr[2].rec.devices == 0x02);
}

int main(void)
{
    RUN_TEST(test_validate_accepts_and_rejects);
    RUN_TEST(test_modify_leaves_everything_else_byte_identical);
    RUN_TEST(test_delete_frees_a_slot_and_moves_no_other);
    RUN_TEST(test_create_lands_in_slot_order);
    RUN_TEST(test_create_at_the_end);
    RUN_TEST(test_invalid_edit_writes_nothing);
    RUN_TEST(test_successive_edits);
    return test_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
