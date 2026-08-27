/*
 * test_nvdb_access.c — nvDb phases 1 and 2: addressing and the write path.
 *
 * Every assertion here is about the surface a USER sees.  Nothing in this
 * file names a sector, a page or an erase except where it is checking that
 * nvDb did not perform one.
 */

#include "nvdb.h"
#include "nvdb_exceptions.h"
#include "nvdb_layout.h"
#include "nvdb_internal.h"
#include "nvdb_port.h"
#include "nvdb_test_util.h"
#include "test_util.h"

#include <stdlib.h>

extern int nvdb_stub_lockDepth;

/* ==========================================================================
 * Phase 1 — addressing
 * ========================================================================== */

static void test_before_init_everything_is_notInit(void)
{
    uint8_t  buff[4] = {0};
    uint32_t size    = 0;
    uint32_t addr    = 0;

    mock_flash_reset();
    nvdbInitDone = false;

    TEST_ASSERT(nvdbRes_notInit == NvDb_Read(nvdbUser_crashLog, buff, 0, 4));
    TEST_ASSERT(nvdbRes_notInit == NvDb_Write(nvdbUser_crashLog, buff, 0, 4));
    TEST_ASSERT(nvdbRes_notInit == NvDb_Delete(nvdbUser_crashLog, 0, 4, NULL));
    TEST_ASSERT(nvdbRes_notInit == NvDb_Wipe(nvdbUser_crashLog, NULL));
    TEST_ASSERT(nvdbRes_notInit == NvDb_GetSize(nvdbUser_crashLog, &size));
    TEST_ASSERT(nvdbRes_notInit ==
                NvDb_GetAbsoluteAddress(nvdbUser_crashLog, &addr, &size));
}

static void test_fresh_board_comes_up_on_the_built_in_layout(void)
{
    sNvDbStatus status;
    uint32_t    size = 0;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    TEST_ASSERT(nvdbRes_ok == NvDb_GetStatus(&status));
    TEST_ASSERT(0 == strcmp(status.layoutName, "periphnet"));
    /* Tracks the constant rather than pinning a literal, so a
     * legitimate layout bump does not read as a regression. */
    TEST_ASSERT(NVDB_TARGET_VER == status.layoutVer);
    TEST_ASSERT(0 == strcmp(status.layoutName, NVDB_TARGET_NAME));
    TEST_ASSERT(nvdbRes_ok == status.lastApplyResult);

    TEST_ASSERT(nvdbRes_ok == NvDb_GetSize(nvdbUser_crashLog, &size));
    TEST_ASSERT(0x1000u == size);
    TEST_ASSERT(nvdbRes_ok == NvDb_GetSize(nvdbUser_fwuStored, &size));
    TEST_ASSERT(0x7A000u == size);
    TEST_ASSERT(nvdbRes_ok == NvDb_GetSize(nvdbUser_mqttCfg, &size));
    TEST_ASSERT(0x1000u == size);
}

static void test_unknown_user_is_refused(void)
{
    uint8_t  buff[4] = {0};
    uint32_t size    = 0;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    TEST_ASSERT(nvdbRes_badUser == NvDb_GetSize(nvdbUser_undefined, &size));
    TEST_ASSERT(nvdbRes_badUser == NvDb_GetSize(nvdbUser_last, &size));
    TEST_ASSERT(nvdbRes_badUser == NvDb_Read(nvdbUser_undefined, buff, 0, 4));
    TEST_ASSERT(nvdbRes_badUser ==
                NvDb_Read((eNvDbUser)0x7Fu, buff, 0, 4));
}

static void test_bounds_at_every_edge(void)
{
    uint8_t  buff[8]  = {0};
    uint32_t size     = 0;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    TEST_ASSERT(nvdbRes_ok == NvDb_GetSize(nvdbUser_crashLog, &size));

    /* The last byte is reachable... */
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_crashLog, buff, size - 1u, 1));
    /* ...and one past it is not, even for a single byte. */
    TEST_ASSERT(nvdbRes_outOfBounds ==
                NvDb_Read(nvdbUser_crashLog, buff, size, 1));
    TEST_ASSERT(nvdbRes_outOfBounds ==
                NvDb_Read(nvdbUser_crashLog, buff, size - 1u, 2));
    TEST_ASSERT(nvdbRes_outOfBounds ==
                NvDb_Write(nvdbUser_crashLog, buff, size - 1u, 2));
    /* A length that would wrap the address must read as out of bounds, not
     * as a tiny request. */
    TEST_ASSERT(nvdbRes_outOfBounds ==
                NvDb_Read(nvdbUser_crashLog, buff, 8, 0xFFFFFFF8u));
    /* Nothing was asked for. */
    TEST_ASSERT(nvdbRes_noOperation == NvDb_Read(nvdbUser_crashLog, buff, 0, 0));
    TEST_ASSERT(nvdbRes_noOperation == NvDb_Write(nvdbUser_crashLog, buff, 0, 0));
    TEST_ASSERT(nvdbRes_noOperation ==
                NvDb_Delete(nvdbUser_crashLog, 0, 0, NULL));
}

static void test_never_written_space_reads_erased(void)
{
    uint8_t buff[64];
    uint32_t i;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    memset(buff, 0, sizeof(buff));
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_wgCfg, buff, 7, sizeof(buff)));
    for (i = 0; i < sizeof(buff); i++) {
        TEST_ASSERT(0xFF == buff[i]);
    }
}

static void test_one_byte_at_an_odd_offset(void)
{
    uint8_t in  = 0x5A;
    uint8_t out = 0x00;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_wgCfg, &in, 1023, 1));
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_wgCfg, &out, 1023, 1));
    TEST_ASSERT(0x5A == out);
    TEST_ASSERT(0xFF == mock_flash[NVDBT_WGCFG_ADDR + 1022]);
    TEST_ASSERT(0xFF == mock_flash[NVDBT_WGCFG_ADDR + 1024]);
}

static void test_absolute_address_is_for_the_flagged_users_only(void)
{
    uint32_t addr = 0;
    uint32_t size = 0;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());

    TEST_ASSERT(nvdbRes_ok ==
                NvDb_GetAbsoluteAddress(nvdbUser_crashLog, &addr, &size));
    TEST_ASSERT(NVDBT_CRASHLOG_ADDR == addr);
    TEST_ASSERT(0x1000u == size);

    TEST_ASSERT(nvdbRes_ok ==
                NvDb_GetAbsoluteAddress(nvdbUser_fwuGolden, &addr, &size));
    TEST_ASSERT(NVDBT_FWUGOLDEN_ADDR == addr);

    /* An ordinary user cannot resolve an address even by asking. */
    TEST_ASSERT(nvdbRes_badUser ==
                NvDb_GetAbsoluteAddress(nvdbUser_wgCfg, &addr, &size));
    TEST_ASSERT(nvdbRes_badUser ==
                NvDb_GetAbsoluteAddress(nvdbUser_modbusLutA, &addr, &size));

    /* And it takes no lock, so it is safe where the RTOS may be dead. */
    TEST_ASSERT(0 == nvdb_stub_lockDepth);
}

static void test_areas_never_share_an_erasable_unit(void)
{
    sNvDbLayoutInfo info;
    uint32_t        i;
    uint32_t        j;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    TEST_ASSERT(nvdbRes_ok == NvDb_GetLayout(&info));

    for (i = 1; i < (uint32_t)nvdbUser_last; i++) {
        uint32_t a = nvdbDir.entries[i].addr_bytes;
        uint32_t n = nvdbDir.entries[i].size_bytes;

        if (0 == n) {
            continue;
        }
        TEST_ASSERT(0 == (a % NVDB_UNIT_SIZE));
        TEST_ASSERT(0 == (n % NVDB_UNIT_SIZE));
        for (j = 1; j < i; j++) {
            uint32_t b = nvdbDir.entries[j].addr_bytes;
            uint32_t m = nvdbDir.entries[j].size_bytes;
            if (0 == m) {
                continue;
            }
            TEST_ASSERT(a >= (b + m) || b >= (a + n));
        }
    }
    TEST_ASSERT(info.freeSpace_bytes > 0x600000u);
}

/* ==========================================================================
 * Phase 2 — the write path
 * ========================================================================== */

static void test_write_into_erased_space_performs_no_erase(void)
{
    uint8_t payload[300];
    uint8_t back[300];

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    memset(payload, 0xA5, sizeof(payload));

    mock_flash_eraseCnt = 0;
    TEST_ASSERT(nvdbRes_ok ==
                NvDb_Write(nvdbUser_wgTime, payload, 0, sizeof(payload)));
    TEST_ASSERT(0 == mock_flash_eraseCnt);

    /* Append-shaped use stays on the fast path for as long as the space
     * ahead of it has never been written. */
    TEST_ASSERT(nvdbRes_ok ==
                NvDb_Write(nvdbUser_wgTime, payload, 300, sizeof(payload)));
    TEST_ASSERT(0 == mock_flash_eraseCnt);

    memset(back, 0, sizeof(back));
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_wgTime, back, 300,
                                        sizeof(back)));
    TEST_ASSERT_MEM_EQ(payload, back, sizeof(back));
}

static void test_clearing_bits_needs_no_erase(void)
{
    uint32_t word = 0xFFFFFFFFu;
    uint32_t back = 0u;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());

    /* The pattern boot_status and the Modbus selector are built on: a flag
     * word starts erased and gives up one bit at a time.  Every one of these
     * writes must be a plain program — not merely cheaper than an erase, but
     * ATOMIC, which erase-and-write-back is not. */
    mock_flash_eraseCnt = 0;

    word = 0xFFFFFFFEu;
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_bootStatus, &word, 64, 4));
    word = 0xFFFFFFFCu;
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_bootStatus, &word, 64, 4));
    word = 0xFFFFFFF8u;
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_bootStatus, &word, 64, 4));
    TEST_ASSERT(0 == mock_flash_eraseCnt);

    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_bootStatus, &back, 64, 4));
    TEST_ASSERT(0xFFFFFFF8u == back);

    /* Setting a bit back is the case that genuinely needs the erase, and it
     * still works — it just costs one. */
    word = 0xFFFFFFFFu;
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_bootStatus, &word, 64, 4));
    TEST_ASSERT(1 == mock_flash_eraseCnt);
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_bootStatus, &back, 64, 4));
    TEST_ASSERT(0xFFFFFFFFu == back);
}

static void test_rewriting_the_same_bytes_needs_no_erase(void)
{
    uint8_t payload[64];

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    memset(payload, 0x5A, sizeof(payload));
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_wgCfg, payload, 0,
                                         sizeof(payload)));

    mock_flash_eraseCnt = 0;
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_wgCfg, payload, 0,
                                         sizeof(payload)));
    TEST_ASSERT(0 == mock_flash_eraseCnt);
}

static void test_overwrite_preserves_the_rest_of_the_unit(void)
{
    uint8_t  neighbour[64];
    uint8_t  first[16];
    uint8_t  second[16];
    uint8_t  back[64];

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());

    memset(neighbour, 0x11, sizeof(neighbour));
    memset(first, 0x22, sizeof(first));
    memset(second, 0x33, sizeof(second));

    TEST_ASSERT(nvdbRes_ok ==
                NvDb_Write(nvdbUser_wgCfg, neighbour, 1000, sizeof(neighbour)));
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_wgCfg, first, 100,
                                         sizeof(first)));

    mock_flash_eraseCnt = 0;
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_wgCfg, second, 100,
                                         sizeof(second)));
    TEST_ASSERT(1 == mock_flash_eraseCnt);   /* one unit, once              */

    memset(back, 0, sizeof(back));
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_wgCfg, back, 100,
                                        sizeof(second)));
    TEST_ASSERT_MEM_EQ(second, back, sizeof(second));

    memset(back, 0, sizeof(back));
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_wgCfg, back, 1000,
                                        sizeof(neighbour)));
    TEST_ASSERT_MEM_EQ(neighbour, back, sizeof(neighbour));
}

static void test_write_across_an_erasable_unit_boundary(void)
{
    uint8_t *payload = malloc(9000);
    uint8_t *back    = malloc(9000);
    uint32_t i;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    for (i = 0; i < 9000u; i++) {
        payload[i] = (uint8_t)(i * 7u);
    }

    /* Starts inside one unit, ends inside a third. */
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_modbusLutA, payload,
                                         3000, 9000));
    memset(back, 0, 9000);
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_modbusLutA, back, 3000, 9000));
    TEST_ASSERT_MEM_EQ(payload, back, 9000);

    /* And it stayed inside its own area. */
    TEST_ASSERT(nvdbt_allBytes(NVDBT_LUTA_ADDR, 3000, 0xFF));
    TEST_ASSERT(nvdbt_allBytes(NVDBT_LUTB_ADDR, 0x4000, 0xFF));

    free(payload);
    free(back);
}

static void test_a_user_cannot_reach_its_neighbour(void)
{
    uint8_t payload[32];

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    memset(payload, 0xC3, sizeof(payload));

    /* Fill the whole of one small user... */
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_mqttCfg, payload,
                                         0x1000u - sizeof(payload),
                                         sizeof(payload)));
    /* ...and the user placed after it is untouched, erase included. */
    TEST_ASSERT(nvdbt_allBytes(NVDBT_TRICE_ADDR, 0x1000, 0xFF));
    TEST_ASSERT(nvdbt_allBytes(NVDBT_WGCFG_ADDR, 0x1000, 0xFF));
}

int main(void)
{
    RUN_TEST(test_before_init_everything_is_notInit);
    RUN_TEST(test_fresh_board_comes_up_on_the_built_in_layout);
    RUN_TEST(test_unknown_user_is_refused);
    RUN_TEST(test_bounds_at_every_edge);
    RUN_TEST(test_never_written_space_reads_erased);
    RUN_TEST(test_one_byte_at_an_odd_offset);
    RUN_TEST(test_absolute_address_is_for_the_flagged_users_only);
    RUN_TEST(test_areas_never_share_an_erasable_unit);

    RUN_TEST(test_write_into_erased_space_performs_no_erase);
    RUN_TEST(test_clearing_bits_needs_no_erase);
    RUN_TEST(test_rewriting_the_same_bytes_needs_no_erase);
    RUN_TEST(test_overwrite_preserves_the_rest_of_the_unit);
    RUN_TEST(test_write_across_an_erasable_unit_boundary);
    RUN_TEST(test_a_user_cannot_reach_its_neighbour);

    printf("%s: %d failure(s)\n", __FILE__, test_failures);
    return test_failures ? 1 : 0;
}
