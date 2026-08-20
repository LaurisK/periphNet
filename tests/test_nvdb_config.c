/*
 * test_nvdb_config.c — nvDb phase 4: the layout as JSON, and the status.
 *
 * The schema is tiny on purpose, so most of what is worth testing is what it
 * REFUSES: an unknown user is a typo that would otherwise cost that user its
 * space silently, and freeSpace is an answer the board gives rather than an
 * input it accepts.
 */

#include "nvdb.h"
#include "nvdb_config.h"
#include "nvdb_layout.h"
#include "nvdb_internal.h"
#include "image_mgmt.h"
#include "nvdb_test_util.h"
#include "test_util.h"

static const char *k_good =
    "{ \"name\": \"periphnet\", \"version\": 3, \"operation\": \"normal\","
    "  \"users\": { \"crashLog\": 4096, \"fwuStored\": 499712 } }";

static void test_a_good_document_parses(void)
{
    sNvDbLayoutCfg cfg;
    sNvDbCfgError  err;

    TEST_ASSERT(0 == NvDbCfg_Parse(k_good, 0, &cfg, &err));
    TEST_ASSERT(0 == strcmp(cfg.name, "periphnet"));
    TEST_ASSERT(3 == cfg.version);
    TEST_ASSERT(nvdbMode_normal == cfg.operation);
    TEST_ASSERT(4096u == cfg.size_bytes[nvdbUser_crashLog]);
    TEST_ASSERT(499712u == cfg.size_bytes[nvdbUser_fwuStored]);
    /* A user left out is size 0, which is the same fact NvDb_GetSize
     * reports — absence and emptiness are one state. */
    TEST_ASSERT(0u == cfg.size_bytes[nvdbUser_wgCfg]);
}

static void test_operation_defaults_to_normal(void)
{
    sNvDbLayoutCfg cfg;

    TEST_ASSERT(0 == NvDbCfg_Parse(
        "{\"name\":\"x\",\"version\":1,\"users\":{\"wgCfg\":4096}}",
        0, &cfg, NULL));
    TEST_ASSERT(nvdbMode_normal == cfg.operation);
}

static void test_forced_is_authored_not_requested(void)
{
    sNvDbLayoutCfg cfg;

    TEST_ASSERT(0 == NvDbCfg_Parse(
        "{\"name\":\"x\",\"version\":1,\"operation\":\"forced\","
        "\"users\":{\"wgCfg\":4096}}", 0, &cfg, NULL));
    TEST_ASSERT(nvdbMode_forced == cfg.operation);
}

static void test_the_reject_matrix(void)
{
    struct {
        const char *json;
        const char *field;
    } cases[] = {
        { "{\"name\":\"x\",\"version\":1,\"users\":{\"nosuchuser\":4096}}",
          "nosuchuser" },
        { "{\"name\":\"x\",\"version\":1,\"users\":{},\"freeSpace\":10}",
          "freeSpace" },
        { "{\"version\":1,\"users\":{}}",                    "name" },
        { "{\"name\":\"x\",\"users\":{}}",                   "version" },
        { "{\"name\":\"x\",\"version\":1}",                  "users" },
        { "{\"name\":\"x\",\"version\":1,\"users\":{},\"wat\":1}", "wat" },
        { "{\"name\":\"x\",\"version\":1,\"operation\":\"maybe\",\"users\":{}}",
          "operation" },
        { "{\"name\":\"x\",\"version\":70000,\"users\":{}}",  "version" },
        { "{\"name\":\"waaaaaaaaaaaaaaaaytoolong\",\"version\":1,\"users\":{}}",
          "name" },
        { "{\"name\":\"x\",\"version\":1,\"users\":{\"wgCfg\":4096,"
          "\"wgCfg\":8192}}", "wgCfg" },
        { "{\"name\":\"x\",\"version\":1,\"users\":{\"wgCfg\":\"4096\"}}",
          "wgCfg" },
        { "{\"name\":\"x\",\"version\":1,\"users\":{}} trailing", "json" },
        { "not json at all",                                 "json" },
    };
    uint32_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        sNvDbLayoutCfg cfg;
        sNvDbCfgError  err;

        if (0 == NvDbCfg_Parse(cases[i].json, 0, &cfg, &err)) {
            printf("  accepted what it should refuse: %s\n", cases[i].json);
            test_failures++;
            continue;
        }
        if (0 != strcmp(err.field, cases[i].field)) {
            printf("  blamed \"%s\", expected \"%s\" (%s)\n",
                   err.field, cases[i].field, err.reason);
            test_failures++;
        }
    }
}

static void test_round_trip_through_the_board(void)
{
    sNvDbLayoutCfg  cfg;
    sNvDbLayoutInfo info;
    sNvDbStatus     status;
    sNvDbLayoutCfg  back;
    char            json[1024];
    uint32_t        n = 0;
    uint32_t        i;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    TEST_ASSERT(nvdbRes_ok == NvDb_GetLayout(&info));
    TEST_ASSERT(nvdbRes_ok == NvDb_GetStatus(&status));

    n = NvDbCfg_Render(&info, &status, json, sizeof(json));
    TEST_ASSERT(n > 0);

    /* The read-back is deliberately almost what is supplied, so the two can
     * be diffed by eye.  What it adds — freeSpace, onboarding, the status —
     * is not accepted back, so strip it before re-parsing. */
    TEST_ASSERT(NULL != strstr(json, "\"freeSpace\":"));
    TEST_ASSERT(NULL != strstr(json, "\"onboarding\":\"none\""));
    TEST_ASSERT(NULL != strstr(json, "\"name\":\"periphnet\""));

    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.name, info.name);
    cfg.version   = info.version;
    cfg.operation = nvdbMode_normal;
    for (i = 1; i < (uint32_t)nvdbUser_last; i++) {
        cfg.size_bytes[i] = info.size_bytes[i];
    }

    /* struct -> JSON -> struct, and the sizes come back unchanged. */
    n = 0;
    n += (uint32_t)snprintf(json + n, sizeof(json) - n,
                            "{\"name\":\"%s\",\"version\":%u,\"users\":{",
                            cfg.name, cfg.version);
    for (i = 1; i < (uint32_t)nvdbUser_last; i++) {
        n += (uint32_t)snprintf(json + n, sizeof(json) - n, "%s\"%s\":%u",
                                (i > 1) ? "," : "", NvDb_UserName((eNvDbUser)i),
                                cfg.size_bytes[i]);
    }
    n += (uint32_t)snprintf(json + n, sizeof(json) - n, "}}");

    TEST_ASSERT(0 == NvDbCfg_Parse(json, 0, &back, NULL));
    for (i = 1; i < (uint32_t)nvdbUser_last; i++) {
        TEST_ASSERT(cfg.size_bytes[i] == back.size_bytes[i]);
    }
}

static void test_a_supplied_layout_shows_up_as_onboarding(void)
{
    sNvDbLayoutCfg  cfg;
    sNvDbLayoutInfo info;
    char            json[1024];

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());

    TEST_ASSERT(0 == NvDbCfg_Parse(
        "{\"name\":\"site-a\",\"version\":7,\"operation\":\"forced\","
        "\"users\":{\"wgCfg\":4096,\"crashLog\":4096}}", 0, &cfg, NULL));
    TEST_ASSERT(nvdbRes_ok == NvDb_SupplyLayout(&cfg));

    TEST_ASSERT(nvdbRes_ok == NvDb_GetLayout(&info));
    /* Waiting, not applied: the layout in force is still the built-in one. */
    TEST_ASSERT(0 == strcmp(info.name, "periphnet"));
    TEST_ASSERT(nvdbOnboard_forced == info.onboarding);
    TEST_ASSERT(0 == strcmp(info.received.name, "site-a"));
    TEST_ASSERT(7 == info.received.version);

    TEST_ASSERT(NvDbCfg_Render(&info, NULL, json, sizeof(json)) > 0);
    TEST_ASSERT(NULL != strstr(json, "\"onboarding\":\"forced\""));
    TEST_ASSERT(NULL != strstr(json, "\"received\":{\"name\":\"site-a\""));

    /* And it can be taken back off the board again. */
    TEST_ASSERT(nvdbRes_ok == NvDb_DropSuppliedLayout());
    TEST_ASSERT(nvdbRes_ok == NvDb_GetLayout(&info));
    TEST_ASSERT(nvdbOnboard_none == info.onboarding);
}

static void test_a_stored_layout_from_an_older_image_reads_short(void)
{
    sNvDbStageRecord rec;
    sNvDbLayoutInfo  info;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());

    /* An image that knew fewer users wrote fewer entries.  The record states
     * its own count, so a newer image reads it without guessing. */
    memset(&rec, 0, sizeof(rec));
    rec.magic     = NVDB_STAGE_MAGIC;
    rec.recordVer = NVDB_RECORD_VER;
    rec.entryCnt  = (uint16_t)nvdbUser_wgCfg;      /* stops short           */
    rec.version   = 2;
    rec.operation = (uint8_t)nvdbMode_normal;
    strcpy(rec.name, "older");
    rec.size_bytes[nvdbUser_crashLog] = 0x1000;
    rec.crc32 = ImgMgmt_Crc32((const uint8_t *)&rec + 4, sizeof(rec) - 4);

    TEST_ASSERT(nvdbRes_ok == NvDbInt_RawErase(NVDB_CONFIG_ADDR +
                                               NVDB_CFG_STAGE_OFF));
    TEST_ASSERT(nvdbRes_ok == NvDbInt_RawProgram(NVDB_CONFIG_ADDR +
                                                 NVDB_CFG_STAGE_OFF,
                                                 &rec, sizeof(rec)));

    TEST_ASSERT(nvdbRes_ok == NvDb_GetLayout(&info));
    TEST_ASSERT(nvdbOnboard_validated == info.onboarding);
    TEST_ASSERT(0 == strcmp(info.received.name, "older"));
}

static void test_render_reports_what_it_cannot_fit(void)
{
    sNvDbLayoutInfo info;
    char            small[16];

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    TEST_ASSERT(nvdbRes_ok == NvDb_GetLayout(&info));
    TEST_ASSERT(0 == NvDbCfg_Render(&info, NULL, small, sizeof(small)));
}

/* ==========================================================================
 * Wear and occupancy — reporting only (C13)
 * ========================================================================== */

static void test_wear_counts_every_erase_and_nothing_else(void)
{
    sNvDbMediumUsage before;
    sNvDbMediumUsage after;
    uint8_t          payload[64];

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    TEST_ASSERT(nvdbRes_ok == NvDb_GetMediumUsage(&before));

    memset(payload, 0x5A, sizeof(payload));

    /* A write into erased space performs no erase, so it counts none. */
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_wgCfg, payload, 0,
                                         sizeof(payload)));
    TEST_ASSERT(nvdbRes_ok == NvDb_GetMediumUsage(&after));
    TEST_ASSERT(before.eraseCntTotal == after.eraseCntTotal);

    /* Setting a bit back does, exactly once. */
    memset(payload, 0xFF, sizeof(payload));
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_wgCfg, payload, 0,
                                         sizeof(payload)));
    TEST_ASSERT(nvdbRes_ok == NvDb_GetMediumUsage(&after));
    TEST_ASSERT((before.eraseCntTotal + 1u) == after.eraseCntTotal);
}

/* Bring-up is the erase-heaviest thing the board ever does — first adoption
 * writes a directory, a relayout journals and moves, and every one of those
 * is an erase.  The counters have to be up before the layout is, or all of it
 * goes unrecorded. */
static void test_bringup_erases_are_counted(void)
{
    sNvDbMediumUsage usage;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    TEST_ASSERT(nvdbRes_ok == NvDb_GetMediumUsage(&usage));

    TEST_ASSERT(usage.eraseCntTotal > 0u);
    TEST_ASSERT(usage.eraseCntTotal == mock_flash_eraseCnt);
}

static void test_wear_is_charged_to_the_area_that_earned_it(void)
{
    sNvDbUsage lutA;
    sNvDbUsage lutB;
    uint8_t    payload[64];
    uint32_t   i;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());

    for (i = 0; i < 5u; i++) {
        memset(payload, (uint8_t)(0xF0 | i), sizeof(payload));
        TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_modbusLutA, payload, 0,
                                             sizeof(payload)));
    }

    TEST_ASSERT(nvdbRes_ok == NvDb_GetUsage(nvdbUser_modbusLutA, false, &lutA));
    TEST_ASSERT(nvdbRes_ok == NvDb_GetUsage(nvdbUser_modbusLutB, false, &lutB));

    /* A user with an arithmetic bug can wear only itself out. */
    TEST_ASSERT(lutA.eraseCntTotal > 0u);
    TEST_ASSERT(0u == lutB.eraseCntTotal);
    TEST_ASSERT(4u == lutA.units);
}

static void test_wear_survives_a_reboot(void)
{
    sNvDbMediumUsage before;
    sNvDbMediumUsage after;
    uint8_t          payload[64];
    uint32_t         i;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());

    for (i = 0; i < 6u; i++) {
        memset(payload, (uint8_t)(0xF0 | i), sizeof(payload));
        TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_wgCfg, payload, 0,
                                             sizeof(payload)));
    }
    TEST_ASSERT(nvdbRes_ok == NvDb_GetMediumUsage(&before));
    TEST_ASSERT(before.eraseCntUnsaved > 0u);

    /* They are written back once the collector has been idle long enough to
     * conclude the dirt is not going to grow. */
    (void)nvdbt_collectAll();
    nvdbt_idle(80u);
    TEST_ASSERT(nvdbRes_ok == NvDb_GetMediumUsage(&before));
    TEST_ASSERT(0u == before.eraseCntUnsaved);

    TEST_ASSERT(nvdbRes_ok == nvdbt_reboot());
    TEST_ASSERT(nvdbRes_ok == NvDb_GetMediumUsage(&after));
    TEST_ASSERT(after.eraseCntTotal >= before.eraseCntTotal);
}

static void test_losing_the_wear_area_costs_a_statistic(void)
{
    sNvDbMediumUsage usage;
    uint32_t         size = 0;
    uint8_t          payload[64];

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    memset(payload, 0x5A, sizeof(payload));
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_wgCfg, payload, 0,
                                         sizeof(payload)));
    (void)nvdbt_collectAll();

    /* Scribble over the whole wear area and reboot.  Everything still works;
     * the numbers are simply wrong, which is the price the design accepts. */
    memset(&mock_flash[NVDB_WEAR_ADDR], 0x00, NVDB_WEAR_SIZE);

    TEST_ASSERT(nvdbRes_ok == nvdbt_reboot());
    TEST_ASSERT(nvdbRes_ok == NvDb_GetMediumUsage(&usage));
    TEST_ASSERT(nvdbRes_ok == NvDb_GetSize(nvdbUser_wgCfg, &size));
    TEST_ASSERT(0x1000u == size);
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_wgCfg, payload, 0,
                                        sizeof(payload)));
    TEST_ASSERT(0x5A == payload[0]);
}

static void test_occupancy_is_observed_not_declared(void)
{
    sNvDbUsage usage;
    uint8_t    payload[16];

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());

    TEST_ASSERT(nvdbRes_ok == NvDb_GetUsage(nvdbUser_modbusLutA, true, &usage));
    TEST_ASSERT(0u == usage.occupied_bytes);
    TEST_ASSERT(0x4000u == usage.size_bytes);

    /* One byte written into the third unit makes three units occupied: the
     * measure is unit-granular and nvDb never asks what the user meant. */
    memset(payload, 0x77, sizeof(payload));
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_modbusLutA, payload, 0x2000,
                                         1u));
    TEST_ASSERT(nvdbRes_ok == NvDb_GetUsage(nvdbUser_modbusLutA, true, &usage));
    TEST_ASSERT(0x3000u == usage.occupied_bytes);

    /* Asking without a scan is not a lie, it is a different question. */
    TEST_ASSERT(nvdbRes_ok == NvDb_GetUsage(nvdbUser_modbusLutA, false, &usage));
    TEST_ASSERT(0u == usage.occupied_bytes);
    TEST_ASSERT(0x4000u == usage.size_bytes);
}

static void test_the_usage_report_renders(void)
{
    char     js[2048];
    uint32_t n;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    n = NvDbCfg_RenderUsage(false, js, sizeof(js));
    TEST_ASSERT(n > 0);
    TEST_ASSERT(NULL != strstr(js, "\"scanned\":false"));
    TEST_ASSERT(NULL != strstr(js, "\"unitSize\":4096"));
    TEST_ASSERT(NULL != strstr(js, "\"fwuGolden\":{\"size\":499712"));
    TEST_ASSERT(NULL != strstr(js, "\"eraseUnsaved\":"));

    /* And it says so when it cannot fit, rather than truncating. */
    TEST_ASSERT(0 == NvDbCfg_RenderUsage(false, js, 64u));
}

int main(void)
{
    RUN_TEST(test_a_good_document_parses);
    RUN_TEST(test_operation_defaults_to_normal);
    RUN_TEST(test_forced_is_authored_not_requested);
    RUN_TEST(test_the_reject_matrix);
    RUN_TEST(test_round_trip_through_the_board);
    RUN_TEST(test_a_supplied_layout_shows_up_as_onboarding);
    RUN_TEST(test_a_stored_layout_from_an_older_image_reads_short);
    RUN_TEST(test_render_reports_what_it_cannot_fit);

    RUN_TEST(test_wear_counts_every_erase_and_nothing_else);
    RUN_TEST(test_bringup_erases_are_counted);
    RUN_TEST(test_wear_is_charged_to_the_area_that_earned_it);
    RUN_TEST(test_wear_survives_a_reboot);
    RUN_TEST(test_losing_the_wear_area_costs_a_statistic);
    RUN_TEST(test_occupancy_is_observed_not_declared);
    RUN_TEST(test_the_usage_report_renders);

    printf("%s: %d failure(s)\n", __FILE__, test_failures);
    return test_failures ? 1 : 0;
}
