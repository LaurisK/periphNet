/**
 * Unit tests for Shared/Modbus/modbus_blocks.c — read-block derivation.
 *
 * The stride cases want a JK-SHAPED VECTOR SPECIFICALLY (docs/modbus.md §9):
 * addrStride 2 is where a contiguity test or a quantity computed in the
 * address domain passes every stride-1 test and is wrong by a factor of two.
 */

#include "test_util.h"
#include "modbus_blocks.h"

#include <stdlib.h>
#include <string.h>

static sModbusCapabilityRecord mkcap(uint8_t stride, uint16_t maxReadRegs)
{
    sModbusCapabilityRecord c;
    memset(&c, 0, sizeof(c));
    strcpy(c.name, "cap");
    c.addrStride  = stride;
    c.writeFc     = 6;
    c.maxReadRegs = maxReadRegs;
    return c;
}

static sModbusPointSpan span(uint16_t addr, uint8_t regs, uint8_t fc)
{
    sModbusPointSpan s = { addr, 0, regs, fc };
    return s;
}

/* ============================================================================
 * Stride 1 — the ordinary slave
 * ============================================================================ */

static void test_contiguous_points_coalesce(void)
{
    sModbusCapabilityRecord cap = mkcap(1, 125);
    sModbusBlockRecord      blk[] = { { 3000, 200 } };
    sModbusPointSpan        sel[] = {
        span(3000, 1, mbFc_input),
        span(3001, 1, mbFc_input),
        span(3002, 2, mbFc_input),
    };
    sModbusReadBlock        out[8];

    TEST_ASSERT(MbBlocks_Derive(&cap, blk, 1, sel, 3, out, 8) == 1);
    TEST_ASSERT(out[0].addr == 3000);
    TEST_ASSERT(out[0].regs == 4);          /* 1 + 1 + 2 */
    TEST_ASSERT(out[0].fc == mbFc_input);
}

/* A derived read may span anything INSIDE one block, including addresses no
 * point selects: two bytes per register beats a whole extra round trip. */
static void test_gaps_inside_a_block_are_bridged(void)
{
    sModbusCapabilityRecord cap = mkcap(1, 125);
    sModbusBlockRecord      blk[] = { { 3000, 200 } };
    sModbusPointSpan        sel[] = {
        span(3000, 1, mbFc_input),
        span(3010, 1, mbFc_input),
    };
    sModbusReadBlock        out[8];

    TEST_ASSERT(MbBlocks_Derive(&cap, blk, 1, sel, 2, out, 8) == 1);
    TEST_ASSERT(out[0].addr == 3000 && out[0].regs == 11);
}

/* Unsorted input is sorted in place: the caller hands over point ids in
 * authoring order, which says nothing about address order. */
static void test_unsorted_input_is_sorted(void)
{
    sModbusCapabilityRecord cap = mkcap(1, 125);
    sModbusBlockRecord      blk[] = { { 3000, 200 } };
    sModbusPointSpan        sel[] = {
        span(3010, 1, mbFc_input),
        span(3000, 1, mbFc_input),
        span(3005, 1, mbFc_input),
    };
    sModbusReadBlock        out[8];

    TEST_ASSERT(MbBlocks_Derive(&cap, blk, 1, sel, 3, out, 8) == 1);
    TEST_ASSERT(out[0].addr == 3000 && out[0].regs == 11);
    TEST_ASSERT(sel[0].addr == 3000 && sel[2].addr == 3010);
}

/* Holding and input are different address spaces, so they never share a read. */
static void test_function_codes_never_share_a_block(void)
{
    sModbusCapabilityRecord cap = mkcap(1, 125);
    sModbusBlockRecord      blk[] = { { 3000, 200 } };
    sModbusPointSpan        sel[] = {
        span(3000, 1, mbFc_input),
        span(3001, 1, mbFc_holding),
    };
    sModbusReadBlock        out[8];

    TEST_ASSERT(MbBlocks_Derive(&cap, blk, 1, sel, 2, out, 8) == 2);
    TEST_ASSERT(out[0].fc == mbFc_holding);   /* 3 sorts before 4 */
    TEST_ASSERT(out[1].fc == mbFc_input);
}

static void test_split_at_max_read_regs(void)
{
    sModbusCapabilityRecord cap = mkcap(1, 10);
    sModbusBlockRecord      blk[] = { { 3000, 200 } };
    sModbusPointSpan        sel[] = {
        span(3000, 1, mbFc_input),
        span(3009, 1, mbFc_input),          /* exactly 10 registers */
        span(3010, 1, mbFc_input),          /* would make 11 -> split */
    };
    sModbusReadBlock        out[8];

    TEST_ASSERT(MbBlocks_Derive(&cap, blk, 1, sel, 3, out, 8) == 2);
    TEST_ASSERT(out[0].addr == 3000 && out[0].regs == 10);
    TEST_ASSERT(out[1].addr == 3010 && out[1].regs == 1);
}

static void test_never_crosses_a_block_boundary(void)
{
    sModbusCapabilityRecord cap = mkcap(1, 125);
    sModbusBlockRecord      blk[] = { { 3000, 10 }, { 4000, 10 } };
    sModbusPointSpan        sel[] = {
        span(3000, 1, mbFc_input),
        span(4000, 1, mbFc_input),
    };
    sModbusReadBlock        out[8];

    TEST_ASSERT(MbBlocks_Derive(&cap, blk, 2, sel, 2, out, 8) == 2);
    TEST_ASSERT(out[0].addr == 3000 && out[0].regs == 1);
    TEST_ASSERT(out[1].addr == 4000 && out[1].regs == 1);
}

static void test_point_outside_every_block_is_rejected(void)
{
    sModbusCapabilityRecord cap = mkcap(1, 125);
    sModbusBlockRecord      blk[] = { { 3000, 10 } };
    sModbusPointSpan        sel[] = { span(3100, 1, mbFc_input) };
    sModbusReadBlock        out[4];

    TEST_ASSERT(MbBlocks_Derive(&cap, blk, 1, sel, 1, out, 4) ==
                mbBlocks_errNoBlock);
}

static void test_point_past_its_blocks_regs_is_rejected(void)
{
    sModbusCapabilityRecord cap = mkcap(1, 125);
    sModbusBlockRecord      blk[] = { { 3000, 10 } };
    /* Starts inside the block (register 9 of 10) but is two registers wide. */
    sModbusPointSpan        sel[] = { span(3009, 2, mbFc_input) };
    sModbusReadBlock        out[4];

    TEST_ASSERT(MbBlocks_Derive(&cap, blk, 1, sel, 1, out, 4) ==
                mbBlocks_errPastBlock);
}

static void test_out_of_space_is_reported(void)
{
    sModbusCapabilityRecord cap = mkcap(1, 125);
    sModbusBlockRecord      blk[] = { { 0, 10 }, { 100, 10 }, { 200, 10 } };
    sModbusPointSpan        sel[] = {
        span(0, 1, mbFc_input),
        span(100, 1, mbFc_input),
        span(200, 1, mbFc_input),
    };
    sModbusReadBlock        out[2];

    TEST_ASSERT(MbBlocks_Derive(&cap, blk, 3, sel, 3, out, 2) ==
                mbBlocks_errTooMany);
}

static void test_empty_selection(void)
{
    sModbusCapabilityRecord cap = mkcap(1, 125);
    sModbusBlockRecord      blk[] = { { 3000, 10 } };
    sModbusPointSpan        sel[1];
    sModbusReadBlock        out[4];

    TEST_ASSERT(MbBlocks_Derive(&cap, blk, 1, sel, 0, out, 4) == 0);
}

/* ============================================================================
 * Stride 2 — the JK, where the address and register domains differ
 * ============================================================================ */

/* Consecutive JK registers sit at 0x1400, 0x1402, 0x1404: three registers
 * spanning an address delta of 6.  A quantity computed on raw addresses would
 * say 6 and be wrong by exactly addrStride. */
static void test_jk_stride_quantity_is_in_registers(void)
{
    sModbusCapabilityRecord cap = mkcap(2, 123);
    sModbusBlockRecord      blk[] = { { 0x1400, 147 } };
    sModbusPointSpan        sel[] = {
        span(0x1400, 1, mbFc_holding),
        span(0x1402, 1, mbFc_holding),
        span(0x1404, 1, mbFc_holding),
    };
    sModbusReadBlock        out[8];

    TEST_ASSERT(MbBlocks_Derive(&cap, blk, 1, sel, 3, out, 8) == 1);
    TEST_ASSERT(out[0].addr == 0x1400);
    TEST_ASSERT(out[0].regs == 3);          /* registers, not address units */
}

/* A 32-bit JK field occupies two registers = four address units. */
static void test_jk_stride_wide_point(void)
{
    sModbusCapabilityRecord cap = mkcap(2, 123);
    sModbusBlockRecord      blk[] = { { 0x1000, 147 } };
    sModbusPointSpan        sel[] = {
        span(0x1000, 2, mbFc_holding),
        span(0x1004, 2, mbFc_holding),
    };
    sModbusReadBlock        out[8];

    TEST_ASSERT(MbBlocks_Derive(&cap, blk, 1, sel, 2, out, 8) == 1);
    TEST_ASSERT(out[0].addr == 0x1000);
    TEST_ASSERT(out[0].regs == 4);          /* 0x1000..0x1007 = 4 registers */
}

/* The JK's three blocks are 147 registers = 294 address units apart, so a
 * containment test in the address domain would put 0x1200 inside 0x1000. */
static void test_jk_blocks_are_distinguished(void)
{
    sModbusCapabilityRecord cap = mkcap(2, 123);
    sModbusBlockRecord      blk[] = {
        { 0x1000, 147 }, { 0x1200, 147 }, { 0x1400, 147 }
    };
    sModbusPointSpan        sel[] = {
        span(0x1000, 1, mbFc_holding),
        span(0x1200, 1, mbFc_holding),
        span(0x1400, 1, mbFc_holding),
    };
    sModbusReadBlock        out[8];

    TEST_ASSERT(MbBlocks_Find(&cap, blk, 3, 0x1000) == 0);
    TEST_ASSERT(MbBlocks_Find(&cap, blk, 3, 0x1200) == 1);
    TEST_ASSERT(MbBlocks_Find(&cap, blk, 3, 0x1400) == 2);
    /* 0x1000 + 147*2 = 0x1126, so 0x1126 is past block 0 and before block 1 */
    TEST_ASSERT(MbBlocks_Find(&cap, blk, 3, 0x1126) < 0);

    TEST_ASSERT(MbBlocks_Derive(&cap, blk, 3, sel, 3, out, 8) == 3);
}

/* The JK's read ceiling is "quantity + wordOffset < 147" from the block base,
 * which `regs` states: a point at register 146 that is two wide is refused. */
static void test_jk_read_span_limit(void)
{
    sModbusCapabilityRecord cap = mkcap(2, 123);
    sModbusBlockRecord      blk[] = { { 0x1400, 147 } };
    sModbusPointSpan        sel[] = { span((uint16_t)(0x1400 + 146 * 2), 2,
                                           mbFc_holding) };
    sModbusReadBlock        out[4];

    TEST_ASSERT(MbBlocks_Derive(&cap, blk, 1, sel, 1, out, 4) ==
                mbBlocks_errPastBlock);
}

static void test_reg_index_into_the_reply(void)
{
    sModbusCapabilityRecord cap = mkcap(2, 123);
    sModbusReadBlock        blk = { 0x1400, 10, mbFc_holding };

    TEST_ASSERT(MbBlocks_RegIndex(&cap, &blk, 0x1400) == 0);
    TEST_ASSERT(MbBlocks_RegIndex(&cap, &blk, 0x1402) == 1);
    TEST_ASSERT(MbBlocks_RegIndex(&cap, &blk, 0x1412) == 9);
    TEST_ASSERT(MbBlocks_RegIndex(&cap, &blk, 0x1414) < 0);   /* past the end */
    TEST_ASSERT(MbBlocks_RegIndex(&cap, &blk, 0x13FE) < 0);   /* before it    */
}

int main(void)
{
    RUN_TEST(test_contiguous_points_coalesce);
    RUN_TEST(test_gaps_inside_a_block_are_bridged);
    RUN_TEST(test_unsorted_input_is_sorted);
    RUN_TEST(test_function_codes_never_share_a_block);
    RUN_TEST(test_split_at_max_read_regs);
    RUN_TEST(test_never_crosses_a_block_boundary);
    RUN_TEST(test_point_outside_every_block_is_rejected);
    RUN_TEST(test_point_past_its_blocks_regs_is_rejected);
    RUN_TEST(test_out_of_space_is_reported);
    RUN_TEST(test_empty_selection);
    RUN_TEST(test_jk_stride_quantity_is_in_registers);
    RUN_TEST(test_jk_stride_wide_point);
    RUN_TEST(test_jk_blocks_are_distinguished);
    RUN_TEST(test_jk_read_span_limit);
    RUN_TEST(test_reg_index_into_the_reply);
    return test_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
