/*
 * test_nvdb_layout.c — nvDb phase 5: relocation.
 *
 * This is where the module earns its keep: a layout change moves opaque bytes
 * and needs no per-user code, refusal is never a route to data loss, and a
 * relayout interrupted by a power cut finishes at the next init.
 */

#include "nvdb.h"
#include "nvdb_config.h"
#include "nvdb_exceptions.h"
#include "nvdb_layout.h"
#include "nvdb_internal.h"
#include "nvdb_port.h"
#include "nvdb_test_util.h"
#include "test_util.h"

#include "bl_app_contract.h"
#include "image_mgmt.h"

#include <stdlib.h>

/* ==========================================================================
 * Helpers
 * ========================================================================== */

/* Fill a span of the medium directly, the way the firmware that predates
 * nvDb left its bytes behind. */
static void PreloadRaw(uint32_t addr, uint8_t seed, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        mock_flash[addr + i] = (uint8_t)(seed + (uint8_t)i);
    }
}

static int CheckRaw(uint32_t addr, uint8_t seed, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        if (mock_flash[addr + i] != (uint8_t)(seed + (uint8_t)i)) {
            return 0;
        }
    }
    return 1;
}

/* Start from the built-in layout, then hand the board one of its own. */
static void SupplySizes(const char *name, uint16_t ver, eNvDbApplyMode mode,
                        const uint32_t *sizes)
{
    sNvDbLayoutCfg cfg;
    uint32_t       i;

    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.name, name, NVDB_LAYOUT_NAME_LEN - 1u);
    cfg.version   = ver;
    cfg.operation = mode;
    for (i = 0; i < (uint32_t)nvdbUser_last; i++) {
        cfg.size_bytes[i] = sizes[i];
    }
    TEST_ASSERT(nvdbRes_ok == NvDb_SupplyLayout(&cfg));
}

/* The layout in force, as a size table one can edit and hand back. */
static void CurrentSizes(uint32_t *out)
{
    sNvDbLayoutInfo info;
    uint32_t        i;

    TEST_ASSERT(nvdbRes_ok == NvDb_GetLayout(&info));
    for (i = 0; i < (uint32_t)nvdbUser_last; i++) {
        out[i] = info.size_bytes[i];
    }
}

/* ==========================================================================
 * First adoption (§4.4.1)
 * ========================================================================== */

static void test_a_factory_fresh_board_costs_nothing(void)
{
    sNvDbStatus status;

    mock_flash_reset();
    TEST_ASSERT(nvdbRes_ok == NvDb_Init());
    TEST_ASSERT(nvdbRes_ok == NvDb_GetStatus(&status));
    TEST_ASSERT(0 == strcmp(status.layoutName, "periphnet"));

    /* The assumed areas held erased space, so the relayout moved nothing. */
    TEST_ASSERT(nvdbt_allBytes(NVDBT_FWUSTORED_ADDR, 0x1000, 0xFF));
    TEST_ASSERT(nvdbt_allBytes(NVDBT_CRASHLOG_ADDR, 0x1000, 0xFF));
}

static void test_a_pre_nvdb_board_keeps_its_data(void)
{
    uint8_t back[64];

    mock_flash_reset();

    /* What the hand-assigned map left behind. */
    PreloadRaw(EXT_FLASH_FWU_STATUS_ADDR,   0x10, 64);
    PreloadRaw(EXT_FLASH_FWU_IMG_ADDR,      0x20, 4096);
    PreloadRaw(EXT_FLASH_GOLDEN_IMG_ADDR,   0x30, 4096);
    PreloadRaw(EXT_FLASH_IMG_META_ADDR,     0x40, 64);
    PreloadRaw(EXT_FLASH_CRASH_LOG_ADDR,    0x50, 64);
    PreloadRaw(EXT_FLASH_MODBUS_LUT_A_ADDR, 0x60, 4096);
    PreloadRaw(EXT_FLASH_MODBUS_LUT_B_ADDR, 0x70, 4096);
    PreloadRaw(EXT_FLASH_MODBUS_SEL_ADDR,   0x80, 64);
    PreloadRaw(EXT_FLASH_WG_TIME_ADDR,      0x90, 64);
    PreloadRaw(EXT_FLASH_WG_CFG_ADDR,       0xA0, 64);

    TEST_ASSERT(nvdbRes_ok == NvDb_Init());

    /* Every user reads back exactly what it wrote, at the offsets it wrote
     * them — and not one of them was told anything moved. */
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_bootStatus, back, 0, 64));
    TEST_ASSERT(CheckRaw(NVDBT_BOOTSTATUS_ADDR, 0x10, 64));
    TEST_ASSERT(CheckRaw(NVDBT_FWUSTORED_ADDR,  0x20, 4096));
    TEST_ASSERT(CheckRaw(NVDBT_FWUGOLDEN_ADDR,  0x30, 4096));
    TEST_ASSERT(CheckRaw(NVDBT_IMAGEMETA_ADDR,  0x40, 64));
    TEST_ASSERT(CheckRaw(NVDBT_CRASHLOG_ADDR,   0x50, 64));
    TEST_ASSERT(CheckRaw(NVDBT_LUTA_ADDR,       0x60, 4096));
    TEST_ASSERT(CheckRaw(NVDBT_LUTB_ADDR,       0x70, 4096));
    TEST_ASSERT(CheckRaw(NVDBT_SEL_ADDR,        0x80, 64));
    TEST_ASSERT(CheckRaw(NVDBT_WGTIME_ADDR,     0x90, 64));
    TEST_ASSERT(CheckRaw(NVDBT_WGCFG_ADDR,      0xA0, 64));

    /* The users that are new in this layout start out erased, not holding
     * whatever the old map had lying in that space. */
    TEST_ASSERT(nvdbt_allBytes(NVDBT_MQTT_ADDR, 0x1000, 0xFF));
    TEST_ASSERT(nvdbt_allBytes(NVDBT_TRICE_ADDR, 0x1000, 0xFF));

    /* And nothing moved.  The layout this image ships is chosen to land every
     * existing user where it already was, because ten modules still reach the
     * medium themselves — nvDb is the authority here before it is the only
     * caller. */
    TEST_ASSERT(NVDBT_BOOTSTATUS_ADDR == EXT_FLASH_FWU_STATUS_ADDR);
    TEST_ASSERT(NVDBT_FWUSTORED_ADDR  == EXT_FLASH_FWU_IMG_ADDR);
    TEST_ASSERT(NVDBT_FWUGOLDEN_ADDR  == EXT_FLASH_GOLDEN_IMG_ADDR);
    TEST_ASSERT(NVDBT_IMAGEMETA_ADDR  == EXT_FLASH_IMG_META_ADDR);
    TEST_ASSERT(NVDBT_CRASHLOG_ADDR   == EXT_FLASH_CRASH_LOG_ADDR);
    TEST_ASSERT(NVDBT_LUTA_ADDR       == EXT_FLASH_MODBUS_LUT_A_ADDR);
    TEST_ASSERT(NVDBT_LUTB_ADDR       == EXT_FLASH_MODBUS_LUT_B_ADDR);
    TEST_ASSERT(NVDBT_SEL_ADDR        == EXT_FLASH_MODBUS_SEL_ADDR);
    TEST_ASSERT(NVDBT_WGTIME_ADDR     == EXT_FLASH_WG_TIME_ADDR);
    TEST_ASSERT(NVDBT_WGCFG_ADDR      == EXT_FLASH_WG_CFG_ADDR);

    TEST_ASSERT(nvdbDir.entries[nvdbUser_crashLog].addr_bytes ==
                EXT_FLASH_CRASH_LOG_ADDR);
    TEST_ASSERT(nvdbDir.entries[nvdbUser_wgCfg].addr_bytes ==
                EXT_FLASH_WG_CFG_ADDR);
}

/* What phase 7 eventually ships: once no module reaches the medium itself,
 * the wasted hole in the hand-assigned map can be handed back and everything
 * after it pulled down.  Nobody needs new code for that — which is the whole
 * point of areas being opaque bytes. */
static void test_the_compacting_layout_moves_everything_safely(void)
{
    uint32_t sizes[nvdbUser_last];
    uint32_t i;
    uint8_t  back[16];

    mock_flash_reset();
    PreloadRaw(EXT_FLASH_CRASH_LOG_ADDR,    0x50, 4096);
    PreloadRaw(EXT_FLASH_MODBUS_LUT_A_ADDR, 0x60, 4096);
    PreloadRaw(EXT_FLASH_MODBUS_LUT_B_ADDR, 0x70, 4096);
    PreloadRaw(EXT_FLASH_MODBUS_SEL_ADDR,   0x80, 4096);
    PreloadRaw(EXT_FLASH_WG_TIME_ADDR,      0x90, 4096);
    PreloadRaw(EXT_FLASH_WG_CFG_ADDR,       0xA0, 4096);
    TEST_ASSERT(nvdbRes_ok == NvDb_Init());

    CurrentSizes(sizes);
    sizes[nvdbUser_imageMeta] = 0x1000;      /* give the hole back           */
    SupplySizes("periphnet", 2, nvdbMode_normal, sizes);
    TEST_ASSERT(nvdbRes_ok == nvdbt_reboot());

    /* Everything below the hole slid down by two erasable units, and every
     * user still reads its own bytes at its own offsets. */
    TEST_ASSERT(nvdbDir.entries[nvdbUser_crashLog].addr_bytes ==
                (EXT_FLASH_CRASH_LOG_ADDR - 0x2000u));
    for (i = 1; i < (uint32_t)nvdbUser_last; i++) {
        static const uint8_t seeds[] = {
            0, 0, 0, 0, 0, 0, 0, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0, 0, 0
        };
        uint32_t size = 0;

        if (i >= sizeof(seeds) || 0 == seeds[i]) {
            continue;
        }
        TEST_ASSERT(nvdbRes_ok == NvDb_GetSize((eNvDbUser)i, &size));
        TEST_ASSERT(nvdbRes_ok == NvDb_Read((eNvDbUser)i, back, 0,
                                            sizeof(back)));
        if (seeds[i] != back[0] || (uint8_t)(seeds[i] + 15u) != back[15]) {
            printf("  user %u came across wrong: 0x%02X\n", i, back[0]);
            test_failures++;
        }
    }
}

static void test_the_second_boot_does_nothing(void)
{
    uint32_t erases = 0;

    mock_flash_reset();
    PreloadRaw(EXT_FLASH_MODBUS_LUT_A_ADDR, 0x60, 4096);
    TEST_ASSERT(nvdbRes_ok == NvDb_Init());

    /* The directory already records the target, so the "did it already run"
     * question answers itself. */
    mock_flash_eraseCnt = 0;
    TEST_ASSERT(nvdbRes_ok == NvDb_Init());
    erases = mock_flash_eraseCnt;
    TEST_ASSERT(0 == erases);
    TEST_ASSERT(CheckRaw(NVDBT_LUTA_ADDR, 0x60, 4096));
}

/* ==========================================================================
 * Ordinary layout changes
 * ========================================================================== */

static void test_growing_an_area_keeps_the_data_and_erases_the_tail(void)
{
    uint32_t sizes[nvdbUser_last];
    uint8_t  payload[128];
    uint8_t  back[128];
    uint32_t size = 0;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    memset(payload, 0xC7, sizeof(payload));
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_wgCfg, payload, 0,
                                         sizeof(payload)));

    CurrentSizes(sizes);
    sizes[nvdbUser_wgCfg] = 0x4000;
    SupplySizes("bigger", 2, nvdbMode_normal, sizes);

    TEST_ASSERT(nvdbRes_ok == nvdbt_reboot());
    TEST_ASSERT(nvdbRes_ok == NvDb_GetSize(nvdbUser_wgCfg, &size));
    TEST_ASSERT(0x4000u == size);

    /* Offset 0x00 stays offset 0x00 — that is what makes relocation
     * invisible. */
    memset(back, 0, sizeof(back));
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_wgCfg, back, 0,
                                        sizeof(back)));
    TEST_ASSERT_MEM_EQ(payload, back, sizeof(back));

    /* If it grew, the tail is erased space. */
    memset(back, 0, sizeof(back));
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_wgCfg, back, 0x3F00,
                                        sizeof(back)));
    TEST_ASSERT(0xFF == back[0]);
    TEST_ASSERT(0xFF == back[127]);
}

static void test_every_user_survives_a_layout_that_shifts_them_all(void)
{
    uint32_t sizes[nvdbUser_last];
    uint32_t i;
    uint8_t  back[16];

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());

    /* Give every user something recognisable, at a recognisable offset. */
    for (i = 1; i < (uint32_t)nvdbUser_last; i++) {
        uint8_t payload[16];
        uint32_t size = 0;

        TEST_ASSERT(nvdbRes_ok == NvDb_GetSize((eNvDbUser)i, &size));
        if (0 == size || nvdbUser_nvdbConfig == i || nvdbUser_nvdbWear == i) {
            continue;
        }
        memset(payload, (uint8_t)(0x40 + i), sizeof(payload));
        TEST_ASSERT(nvdbRes_ok == NvDb_Write((eNvDbUser)i, payload, 0,
                                             sizeof(payload)));
    }

    /* Grow the very first user, which pushes every later one along. */
    CurrentSizes(sizes);
    sizes[nvdbUser_bootStatus] = 0x4000;
    SupplySizes("shifted", 2, nvdbMode_normal, sizes);
    TEST_ASSERT(nvdbRes_ok == nvdbt_reboot());

    for (i = 1; i < (uint32_t)nvdbUser_last; i++) {
        uint32_t size = 0;

        TEST_ASSERT(nvdbRes_ok == NvDb_GetSize((eNvDbUser)i, &size));
        if (0 == size || nvdbUser_nvdbConfig == i || nvdbUser_nvdbWear == i) {
            continue;
        }
        TEST_ASSERT(nvdbRes_ok == NvDb_Read((eNvDbUser)i, back, 0,
                                            sizeof(back)));
        if ((uint8_t)(0x40 + i) != back[0]) {
            printf("  user %u lost its bytes: 0x%02X\n", i, back[0]);
            test_failures++;
        }
    }
}

static void test_a_user_dropped_from_the_layout_has_no_space(void)
{
    uint32_t sizes[nvdbUser_last];
    uint32_t size = 0xFFFFFFFFu;
    uint8_t  buff[4];

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    CurrentSizes(sizes);
    sizes[nvdbUser_triceUdpCfg] = 0;
    SupplySizes("no-trice", 2, nvdbMode_normal, sizes);
    TEST_ASSERT(nvdbRes_ok == nvdbt_reboot());

    /* Absence and emptiness are the same state; there is no "unprovisioned"
     * flag and none is needed. */
    TEST_ASSERT(nvdbRes_ok == NvDb_GetSize(nvdbUser_triceUdpCfg, &size));
    TEST_ASSERT(0u == size);
    TEST_ASSERT(nvdbRes_outOfBounds ==
                NvDb_Read(nvdbUser_triceUdpCfg, buff, 0, 4));
    TEST_ASSERT(nvdbRes_outOfBounds ==
                NvDb_Write(nvdbUser_triceUdpCfg, buff, 0, 4));
    TEST_ASSERT(nvdbRes_outOfBounds == NvDb_Wipe(nvdbUser_triceUdpCfg, NULL));
}

/* ==========================================================================
 * Refusal, and forced
 * ========================================================================== */

static void test_a_shrink_below_the_occupied_extent_is_refused(void)
{
    uint32_t    sizes[nvdbUser_last];
    uint8_t     payload[64];
    uint8_t     back[64];
    sNvDbStatus status;
    uint32_t    size = 0;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    memset(payload, 0xD9, sizeof(payload));

    /* The shrink is safe when it is supplied — the area is empty — so the
     * advisory check passes.  This is exactly the case the doubled check
     * exists for: the binding one runs at the next init, by which time a user
     * has written where the new layout has no room. */
    CurrentSizes(sizes);
    sizes[nvdbUser_modbusLutA] = 0x1000;
    SupplySizes("too-small", 2, nvdbMode_normal, sizes);

    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_modbusLutA, payload, 0x2000,
                                         sizeof(payload)));

    TEST_ASSERT(nvdbRes_refused == nvdbt_reboot());

    /* The layout in force stays in force and every user keeps the area it
     * already had.  Refusal is never a route to data loss. */
    TEST_ASSERT(nvdbRes_ok == NvDb_GetSize(nvdbUser_modbusLutA, &size));
    TEST_ASSERT(0x4000u == size);
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_modbusLutA, back, 0x2000,
                                        sizeof(back)));
    TEST_ASSERT_MEM_EQ(payload, back, sizeof(back));

    /* And the operator can find out after the reboot that it did not take. */
    TEST_ASSERT(nvdbRes_ok == NvDb_GetStatus(&status));
    TEST_ASSERT(0 == strcmp(status.layoutName, "periphnet"));
    TEST_ASSERT(nvdbRes_refused == status.lastApplyResult);

    /* The refusal is not retried forever: the configuration was applied and
     * answered, so it is off the board. */
    TEST_ASSERT(nvdbRes_ok == nvdbt_reboot());
}

static void test_the_advisory_check_catches_what_it_can_see(void)
{
    uint32_t sizes[nvdbUser_last];
    uint8_t  payload[64];

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    memset(payload, 0xD9, sizeof(payload));
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_modbusLutA, payload, 0x2000,
                                         sizeof(payload)));

    /* Supplied against a board that already cannot take it, so the operator
     * hears about it now rather than after a reboot. */
    CurrentSizes(sizes);
    sizes[nvdbUser_modbusLutA] = 0x1000;
    {
        sNvDbLayoutCfg cfg;
        uint32_t       i;

        memset(&cfg, 0, sizeof(cfg));
        strcpy(cfg.name, "too-small");
        cfg.version   = 2;
        cfg.operation = nvdbMode_normal;
        for (i = 0; i < (uint32_t)nvdbUser_last; i++) {
            cfg.size_bytes[i] = sizes[i];
        }
        TEST_ASSERT(nvdbRes_refused == NvDb_SupplyLayout(&cfg));
    }
}

static void test_forced_truncates_instead_of_refusing(void)
{
    uint32_t    sizes[nvdbUser_last];
    uint8_t     payload[64];
    uint8_t     back[64];
    sNvDbStatus status;
    uint32_t    size = 0;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    memset(payload, 0xD9, sizeof(payload));
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_modbusLutA, payload, 0,
                                         sizeof(payload)));
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_modbusLutA, payload, 0x2000,
                                         sizeof(payload)));

    CurrentSizes(sizes);
    sizes[nvdbUser_modbusLutA] = 0x1000;
    SupplySizes("too-small", 2, nvdbMode_forced, sizes);

    TEST_ASSERT(nvdbRes_ok == nvdbt_reboot());
    TEST_ASSERT(nvdbRes_ok == NvDb_GetStatus(&status));
    TEST_ASSERT(0 == strcmp(status.layoutName, "too-small"));
    TEST_ASSERT(nvdbMode_forced == status.lastApplyMode);

    TEST_ASSERT(nvdbRes_ok == NvDb_GetSize(nvdbUser_modbusLutA, &size));
    TEST_ASSERT(0x1000u == size);
    /* What fitted came across; what did not is gone, knowingly. */
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_modbusLutA, back, 0,
                                        sizeof(back)));
    TEST_ASSERT_MEM_EQ(payload, back, sizeof(back));
    TEST_ASSERT(nvdbRes_outOfBounds ==
                NvDb_Read(nvdbUser_modbusLutA, back, 0x2000, sizeof(back)));
}

static void test_nvdbs_own_areas_cannot_be_moved_in_either_mode(void)
{
    sNvDbLayoutCfg cfg;
    uint32_t       sizes[nvdbUser_last];
    uint32_t       i;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    CurrentSizes(sizes);

    for (i = 0; i < 2u; i++) {
        eNvDbApplyMode mode = (0u == i) ? nvdbMode_normal : nvdbMode_forced;
        uint32_t       j;

        memset(&cfg, 0, sizeof(cfg));
        strcpy(cfg.name, "bad-anchor");
        cfg.version   = 9;
        cfg.operation = mode;
        for (j = 0; j < (uint32_t)nvdbUser_last; j++) {
            cfg.size_bytes[j] = sizes[j];
        }
        /* A forced layout may cost a user its data; it may never cost nvDb
         * its ability to find anything. */
        cfg.size_bytes[nvdbUser_nvdbConfig] = 0x8000;
        TEST_ASSERT(nvdbRes_refused == NvDb_SupplyLayout(&cfg));

        cfg.size_bytes[nvdbUser_nvdbConfig] = sizes[nvdbUser_nvdbConfig];
        cfg.size_bytes[nvdbUser_nvdbWear]   = 0x2000;
        TEST_ASSERT(nvdbRes_refused == NvDb_SupplyLayout(&cfg));
    }
}

static void test_a_layout_that_does_not_fit_is_refused_at_supply_time(void)
{
    sNvDbLayoutCfg cfg;
    uint32_t       sizes[nvdbUser_last];
    uint32_t       i;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    CurrentSizes(sizes);

    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.name, "too-big");
    cfg.version   = 3;
    cfg.operation = nvdbMode_normal;
    for (i = 0; i < (uint32_t)nvdbUser_last; i++) {
        cfg.size_bytes[i] = sizes[i];
    }
    cfg.size_bytes[nvdbUser_fwuStored] = NVDB_MEDIUM_SIZE;

    /* Checked when it is supplied, so an operator sees the mistake now
     * rather than after a reboot — even though that check is only advisory. */
    TEST_ASSERT(nvdbRes_refused == NvDb_SupplyLayout(&cfg));
}

/* ==========================================================================
 * A relayout interrupted by a power cut
 * ========================================================================== */

static void test_a_torn_relayout_finishes_at_the_next_init(void)
{
    uint32_t sizes[nvdbUser_last];
    uint32_t i;
    uint8_t  back[16];

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());

    for (i = 1; i < (uint32_t)nvdbUser_last; i++) {
        uint8_t  payload[16];
        uint32_t size = 0;

        TEST_ASSERT(nvdbRes_ok == NvDb_GetSize((eNvDbUser)i, &size));
        if (0 == size || nvdbUser_nvdbConfig == i || nvdbUser_nvdbWear == i) {
            continue;
        }
        memset(payload, (uint8_t)(0x40 + i), sizeof(payload));
        TEST_ASSERT(nvdbRes_ok == NvDb_Write((eNvDbUser)i, payload, 0,
                                             sizeof(payload)));
    }

    CurrentSizes(sizes);
    sizes[nvdbUser_bootStatus] = 0x4000;
    SupplySizes("shifted", 2, nvdbMode_normal, sizes);

    /* The supply goes down cleanly; the power goes away partway through the
     * relayout that follows. */
    mock_flash_failAfter = 12;
    TEST_ASSERT(nvdbRes_ok != NvDb_Init());

    /* Power back on. */
    mock_flash_failAfter = 0;
    mock_flash_reset_power();
    TEST_ASSERT(nvdbRes_ok == NvDb_Init());

    {
        sNvDbStatus status;
        TEST_ASSERT(nvdbRes_ok == NvDb_GetStatus(&status));
        TEST_ASSERT(0 == strcmp(status.layoutName, "shifted"));
    }

    for (i = 1; i < (uint32_t)nvdbUser_last; i++) {
        uint32_t size = 0;

        TEST_ASSERT(nvdbRes_ok == NvDb_GetSize((eNvDbUser)i, &size));
        if (0 == size || nvdbUser_nvdbConfig == i || nvdbUser_nvdbWear == i) {
            continue;
        }
        TEST_ASSERT(nvdbRes_ok == NvDb_Read((eNvDbUser)i, back, 0,
                                            sizeof(back)));
        if ((uint8_t)(0x40 + i) != back[0]) {
            printf("  user %u lost its bytes across the cut: 0x%02X\n",
                   i, back[0]);
            test_failures++;
        }
    }
}

static void test_the_older_directory_slot_can_be_lost(void)
{
    uint32_t       sizes[nvdbUser_last];
    sNvDbDirRecord a;
    sNvDbDirRecord b;
    sNvDbStatus    status;
    uint32_t       stale = 0;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    CurrentSizes(sizes);
    sizes[nvdbUser_wgCfg] = 0x2000;
    SupplySizes("two-slots", 2, nvdbMode_normal, sizes);
    TEST_ASSERT(nvdbRes_ok == nvdbt_reboot());

    /* Every directory write goes to the slot the record in force did NOT
     * come from, so the previous one is still readable.  Destroy it and the
     * board is unmoved: the higher sequence number wins. */
    TEST_ASSERT(nvdbRes_ok == NvDbInt_RawRead(NVDB_CONFIG_ADDR +
                                              NVDB_CFG_DIR_A_OFF, &a, sizeof(a)));
    TEST_ASSERT(nvdbRes_ok == NvDbInt_RawRead(NVDB_CONFIG_ADDR +
                                              NVDB_CFG_DIR_B_OFF, &b, sizeof(b)));
    stale = (a.seq < b.seq) ? (NVDB_CONFIG_ADDR + NVDB_CFG_DIR_A_OFF)
                            : (NVDB_CONFIG_ADDR + NVDB_CFG_DIR_B_OFF);
    memset(&mock_flash[stale], 0x00, 0x1000);

    TEST_ASSERT(nvdbRes_ok == NvDb_Init());
    TEST_ASSERT(nvdbRes_ok == NvDb_GetStatus(&status));
    TEST_ASSERT(0 == strcmp(status.layoutName, "two-slots"));
}

/* Cut the power at every step of a relayout in turn.  Whatever the cut
 * interrupted, the next init must leave the board on a layout it can read
 * every user through — that is the whole promise the journal makes. */
static void test_a_cut_anywhere_in_a_relayout_is_survivable(void)
{
    uint32_t cut;

    for (cut = 1u; cut <= 60u; cut++) {
        uint32_t sizes[nvdbUser_last];
        uint32_t i;
        uint8_t  back[16];

        TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
        for (i = 1; i < (uint32_t)nvdbUser_last; i++) {
            uint8_t  payload[16];
            uint32_t size = 0;

            TEST_ASSERT(nvdbRes_ok == NvDb_GetSize((eNvDbUser)i, &size));
            if (0 == size || nvdbUser_nvdbConfig == i ||
                nvdbUser_nvdbWear == i) {
                continue;
            }
            memset(payload, (uint8_t)(0x40 + i), sizeof(payload));
            TEST_ASSERT(nvdbRes_ok == NvDb_Write((eNvDbUser)i, payload, 0,
                                                 sizeof(payload)));
        }

        CurrentSizes(sizes);
        sizes[nvdbUser_bootStatus] = 0x3000;
        sizes[nvdbUser_wgCfg]      = 0x2000;
        SupplySizes("shifted", 2, nvdbMode_normal, sizes);

        mock_flash_failAfter = cut;
        (void)NvDb_Init();
        mock_flash_reset_power();

        /* Power back on, possibly more than once — a board can be unlucky
         * twice, and finishing must not depend on being lucky. */
        TEST_ASSERT(nvdbRes_ok == NvDb_Init());
        TEST_ASSERT(nvdbRes_ok == NvDb_Init());

        {
            sNvDbStatus st;
            TEST_ASSERT(nvdbRes_ok == NvDb_GetStatus(&st));
            if (0 != strcmp(st.layoutName, "shifted")) {
                printf("  cut %u: gave up on the layout (%s)\n", cut,
                       st.layoutName);
                test_failures++;
            }
        }

        for (i = 1; i < (uint32_t)nvdbUser_last; i++) {
            uint32_t size = 0;

            TEST_ASSERT(nvdbRes_ok == NvDb_GetSize((eNvDbUser)i, &size));
            if (0 == size || nvdbUser_nvdbConfig == i ||
                nvdbUser_nvdbWear == i) {
                continue;
            }
            TEST_ASSERT(nvdbRes_ok == NvDb_Read((eNvDbUser)i, back, 0,
                                                sizeof(back)));
            if ((uint8_t)(0x40 + i) != back[0]) {
                printf("  cut %u: user %u reads 0x%02X\n", cut, i, back[0]);
                test_failures++;
            }
        }
    }
}

static void test_a_directory_that_makes_no_sense_is_not_adopted(void)
{
    sNvDbDirRecord rec;
    sNvDbStatus    status;
    uint32_t       size = 0;
    uint32_t       slot = 0;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());

    /* A record that passes its own CRC but describes two users on top of each
     * other.  It is not a candidate at all: a board that cannot say where
     * anything is is worse than one built on the assumed map. */
    for (slot = 0; slot < 2u; slot++) {
        uint32_t off = (0u == slot) ? NVDB_CFG_DIR_A_OFF : NVDB_CFG_DIR_B_OFF;

        memset(&rec, 0, sizeof(rec));
        rec.magic     = NVDB_DIR_MAGIC;
        rec.recordVer = NVDB_RECORD_VER;
        rec.seq       = 99u + slot;
        rec.entryCnt  = (uint16_t)nvdbUser_last;
        strcpy(rec.layoutName, "nonsense");
        rec.entries[nvdbUser_nvdbConfig].addr_bytes = NVDB_CONFIG_ADDR;
        rec.entries[nvdbUser_nvdbConfig].size_bytes = NVDB_CONFIG_SIZE;
        rec.entries[nvdbUser_nvdbWear].addr_bytes   = NVDB_WEAR_ADDR;
        rec.entries[nvdbUser_nvdbWear].size_bytes   = NVDB_WEAR_SIZE;
        rec.entries[nvdbUser_crashLog].addr_bytes   = 0x10000u;
        rec.entries[nvdbUser_crashLog].size_bytes   = 0x2000u;
        rec.entries[nvdbUser_wgCfg].addr_bytes      = 0x11000u;
        rec.entries[nvdbUser_wgCfg].size_bytes      = 0x2000u;
        rec.crc32 = ImgMgmt_Crc32((const uint8_t *)&rec + 4, sizeof(rec) - 4);

        TEST_ASSERT(nvdbRes_ok == NvDbInt_RawErase(NVDB_CONFIG_ADDR + off));
        TEST_ASSERT(nvdbRes_ok == NvDbInt_RawProgram(NVDB_CONFIG_ADDR + off,
                                                     &rec, sizeof(rec)));
    }

    /* Boot always succeeds. */
    TEST_ASSERT(nvdbRes_ok == NvDb_Init());
    TEST_ASSERT(nvdbRes_ok == NvDb_GetStatus(&status));
    TEST_ASSERT(0 == strcmp(status.layoutName, "periphnet"));
    TEST_ASSERT(nvdbRes_ok == NvDb_GetSize(nvdbUser_crashLog, &size));
    TEST_ASSERT(0x1000u == size);
    TEST_ASSERT(nvdbDir.entries[nvdbUser_crashLog].addr_bytes ==
                NVDBT_CRASHLOG_ADDR);
}

/* A medium that fails once and then behaves is not a power cut, and the two
 * must not be confused: the journal of a half-run relayout is the only record
 * of where everybody's bytes are, so nothing may consume the sequence number
 * the resume test keys on.  Recording "it went wrong" is worth less than
 * being able to finish. */
static void test_a_transient_medium_error_does_not_strand_a_relayout(void)
{
    uint32_t sizes[nvdbUser_last];
    uint32_t glitchAt;
    uint32_t i;

    for (glitchAt = 8u; glitchAt <= 30u; glitchAt += 2u) {
        uint8_t back[16];

        TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
        for (i = 1; i < (uint32_t)nvdbUser_last; i++) {
            uint8_t  payload[16];
            uint32_t size = 0;

            TEST_ASSERT(nvdbRes_ok == NvDb_GetSize((eNvDbUser)i, &size));
            if (0 == size || nvdbUser_nvdbConfig == i ||
                nvdbUser_nvdbWear == i) {
                continue;
            }
            memset(payload, (uint8_t)(0x40 + i), sizeof(payload));
            TEST_ASSERT(nvdbRes_ok == NvDb_Write((eNvDbUser)i, payload, 0,
                                                 sizeof(payload)));
        }

        CurrentSizes(sizes);
        sizes[nvdbUser_bootStatus] = 0x3000;
        SupplySizes("glitched", 2, nvdbMode_normal, sizes);

        /* One erase times out partway through the relayout, then the medium
         * is fine again. */
        {
            uint32_t seqBefore = nvdbDir.seq;

            mock_flash_glitchAfter = glitchAt;
            mock_flash_glitchOps   = 1u;
            (void)NvDb_Init();
            mock_flash_glitchAfter = 0u;
            mock_flash_glitchOps   = 0u;

            /* The mechanism, pinned directly: the journal of a half-run
             * relayout is recognised by targetSeq == nvdbDir.seq + 1, so
             * NOTHING may consume that sequence number on the way out.  A
             * note recording the failure would, the resume test would miss,
             * and the plan would be thrown away with users' bytes already
             * moved. */
            if (0 != strcmp(nvdbDir.layoutName, "glitched") &&
                nvdbDir.seq != seqBefore) {
                printf("  glitch at %u: apply did not complete yet seq "
                       "advanced %u -> %u, so the journal can no longer be "
                       "recognised\n", glitchAt, seqBefore, nvdbDir.seq);
                test_failures++;
            }
        }

        /* The next boot must FINISH the relayout, not discard it. */
        TEST_ASSERT(nvdbRes_ok == NvDb_Init());
        {
            sNvDbStatus st;
            TEST_ASSERT(nvdbRes_ok == NvDb_GetStatus(&st));
            if (0 != strcmp(st.layoutName, "glitched")) {
                printf("  glitch at %u: gave up on the layout (%s)\n",
                       glitchAt, st.layoutName);
                test_failures++;
            }
        }

        for (i = 1; i < (uint32_t)nvdbUser_last; i++) {
            uint32_t size = 0;

            TEST_ASSERT(nvdbRes_ok == NvDb_GetSize((eNvDbUser)i, &size));
            if (0 == size || nvdbUser_nvdbConfig == i ||
                nvdbUser_nvdbWear == i) {
                continue;
            }
            TEST_ASSERT(nvdbRes_ok == NvDb_Read((eNvDbUser)i, back, 0,
                                                sizeof(back)));
            if ((uint8_t)(0x40 + i) != back[0]) {
                printf("  glitch at %u: user %u reads 0x%02X\n", glitchAt, i,
                       back[0]);
                test_failures++;
            }
        }
    }
}

/* Wear is indication only.  It must never reach an allocation, a relocation,
 * a write or a result code (C13) — which is exactly what allows the counters
 * to be cheap and lossy in the first place. */
static void test_wear_never_influences_a_placement(void)
{
    uint32_t sizes[nvdbUser_last];
    uint32_t placedFresh[nvdbUser_last];
    uint32_t i;

    /* Place a layout on a board that has never erased anything... */
    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    CurrentSizes(sizes);
    sizes[nvdbUser_wgCfg] = 0x2000;
    SupplySizes("worn", 2, nvdbMode_normal, sizes);
    TEST_ASSERT(nvdbRes_ok == nvdbt_reboot());
    for (i = 1; i < (uint32_t)nvdbUser_last; i++) {
        placedFresh[i] = nvdbDir.entries[i].addr_bytes;
    }

    /* ...and again on one where a single area has been hammered. */
    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    for (i = 0; i < 40u; i++) {
        uint8_t payload[32];
        memset(payload, (uint8_t)i, sizeof(payload));
        TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_modbusLutA, payload, 0,
                                             sizeof(payload)));
        memset(payload, 0xFF, sizeof(payload));
        TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_modbusLutA, payload, 0,
                                             sizeof(payload)));
    }
    {
        sNvDbUsage u;
        TEST_ASSERT(nvdbRes_ok == NvDb_GetUsage(nvdbUser_modbusLutA, false, &u));
        TEST_ASSERT(u.eraseCntMax > 10u);   /* genuinely worn */
    }

    CurrentSizes(sizes);
    sizes[nvdbUser_wgCfg] = 0x2000;
    SupplySizes("worn", 2, nvdbMode_normal, sizes);
    TEST_ASSERT(nvdbRes_ok == nvdbt_reboot());

    for (i = 1; i < (uint32_t)nvdbUser_last; i++) {
        if (placedFresh[i] != nvdbDir.entries[i].addr_bytes) {
            printf("  wear moved user %u: 0x%X -> 0x%X\n", i, placedFresh[i],
                   nvdbDir.entries[i].addr_bytes);
            test_failures++;
        }
    }
}

/* ==========================================================================
 * Ownership
 * ========================================================================== */

static void test_an_operator_layout_is_not_taken_back_by_the_image(void)
{
    uint32_t    sizes[nvdbUser_last];
    sNvDbStatus status;
    uint32_t    i;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    CurrentSizes(sizes);
    sizes[nvdbUser_wgCfg] = 0x2000;
    SupplySizes("site-a", 1, nvdbMode_normal, sizes);
    TEST_ASSERT(nvdbRes_ok == nvdbt_reboot());

    for (i = 0; i < 3u; i++) {
        TEST_ASSERT(nvdbRes_ok == nvdbt_reboot());
        TEST_ASSERT(nvdbRes_ok == NvDb_GetStatus(&status));
        TEST_ASSERT(0 == strcmp(status.layoutName, "site-a"));
    }
}

int main(void)
{
    RUN_TEST(test_a_factory_fresh_board_costs_nothing);
    RUN_TEST(test_a_pre_nvdb_board_keeps_its_data);
    RUN_TEST(test_the_compacting_layout_moves_everything_safely);
    RUN_TEST(test_the_second_boot_does_nothing);
    RUN_TEST(test_growing_an_area_keeps_the_data_and_erases_the_tail);
    RUN_TEST(test_every_user_survives_a_layout_that_shifts_them_all);
    RUN_TEST(test_a_user_dropped_from_the_layout_has_no_space);
    RUN_TEST(test_a_shrink_below_the_occupied_extent_is_refused);
    RUN_TEST(test_the_advisory_check_catches_what_it_can_see);
    RUN_TEST(test_forced_truncates_instead_of_refusing);
    RUN_TEST(test_nvdbs_own_areas_cannot_be_moved_in_either_mode);
    RUN_TEST(test_a_layout_that_does_not_fit_is_refused_at_supply_time);
    RUN_TEST(test_a_torn_relayout_finishes_at_the_next_init);
    RUN_TEST(test_the_older_directory_slot_can_be_lost);
    RUN_TEST(test_a_cut_anywhere_in_a_relayout_is_survivable);
    RUN_TEST(test_a_directory_that_makes_no_sense_is_not_adopted);
    RUN_TEST(test_a_transient_medium_error_does_not_strand_a_relayout);
    RUN_TEST(test_wear_never_influences_a_placement);
    RUN_TEST(test_an_operator_layout_is_not_taken_back_by_the_image);

    printf("%s: %d failure(s)\n", __FILE__, test_failures);
    return test_failures ? 1 : 0;
}
