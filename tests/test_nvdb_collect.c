/*
 * test_nvdb_collect.c — nvDb phase 3: delete, wipe and the collector.
 *
 * The property under test is that erases stay OFF the write path, and that
 * the one thing a user is told — the completion callback — is told honestly:
 * it confirms an erase happened, and it never fires for one that did not.
 */

#include "nvdb.h"
#include "nvdb_layout.h"
#include "nvdb_internal.h"
#include "nvdb_port.h"
#include "nvdb_test_util.h"
#include "test_util.h"

#include <stdlib.h>

/* Whatever the last callback said. */
static int       s_cbCnt;
static eNvDbUser s_cbUser;
static uint32_t  s_cbOff;
static uint32_t  s_cbLen;
static eNvDbRes  s_cbRes;
static int       s_cbInsideDelete;
static int       s_cbReenterOk;

static void ResetCb(void)
{
    s_cbCnt          = 0;
    s_cbUser         = nvdbUser_undefined;
    s_cbOff          = 0;
    s_cbLen          = 0;
    s_cbRes          = nvdbRes_undefined;
    s_cbInsideDelete = 0;
    s_cbReenterOk    = 0;
}

static void OnDone(eNvDbUser user, uint32_t off_bytes, uint32_t len_bytes,
                   eNvDbRes result)
{
    s_cbCnt++;
    s_cbUser = user;
    s_cbOff  = off_bytes;
    s_cbLen  = len_bytes;
    s_cbRes  = result;
}

/* A callback that calls back into nvDb, which the contract permits because
 * completions are delivered outside the lock. */
static void OnDoneReenter(eNvDbUser user, uint32_t off_bytes,
                          uint32_t len_bytes, eNvDbRes result)
{
    uint32_t size = 0;

    (void)off_bytes; (void)len_bytes; (void)result;
    s_cbCnt++;
    s_cbReenterOk = (nvdbRes_ok == NvDb_GetSize(user, &size)) ? 1 : 0;
}

static void FillUser(eNvDbUser user, uint8_t val, uint32_t len)
{
    uint8_t *buff = malloc(len);
    memset(buff, val, len);
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(user, buff, 0, len));
    free(buff);
}

/* ==========================================================================
 * Delete is deferred, and real
 * ========================================================================== */

static void test_delete_returns_before_the_bytes_are_gone(void)
{
    uint8_t back[16];

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    ResetCb();
    FillUser(nvdbUser_wgCfg, 0x5A, 0x1000);

    mock_flash_eraseCnt = 0;
    TEST_ASSERT(nvdbRes_ok == NvDb_Delete(nvdbUser_wgCfg, 0, 0x1000, OnDone));
    TEST_ASSERT(0 == mock_flash_eraseCnt);   /* nothing happened yet        */
    TEST_ASSERT(0 == s_cbCnt);

    /* A subsequent read still sees the old bytes. */
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_wgCfg, back, 0, sizeof(back)));
    TEST_ASSERT(0x5A == back[0]);

    /* Then the collector arrives. */
    TEST_ASSERT(nvdbt_collectAll() > 0);
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_wgCfg, back, 0, sizeof(back)));
    TEST_ASSERT(0xFF == back[0]);
    TEST_ASSERT(1 == s_cbCnt);
    TEST_ASSERT(nvdbUser_wgCfg == s_cbUser);
    TEST_ASSERT(nvdbRes_ok == s_cbRes);
}

static void test_partial_delete_preserves_what_was_not_deleted(void)
{
    uint8_t back[0x1000];

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    ResetCb();
    FillUser(nvdbUser_wgCfg, 0x5A, 0x1000);

    /* Byte-granular, and squarely inside one erasable unit. */
    TEST_ASSERT(nvdbRes_ok == NvDb_Delete(nvdbUser_wgCfg, 100, 50, OnDone));
    TEST_ASSERT(nvdbt_collectAll() > 0);

    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_wgCfg, back, 0, sizeof(back)));
    TEST_ASSERT(0x5A == back[99]);
    TEST_ASSERT(0xFF == back[100]);
    TEST_ASSERT(0xFF == back[149]);
    TEST_ASSERT(0x5A == back[150]);
    TEST_ASSERT(1 == s_cbCnt);
    TEST_ASSERT(100u == s_cbOff);
    TEST_ASSERT(50u == s_cbLen);
}

static void test_a_deleted_range_is_cheap_to_write_again(void)
{
    uint8_t payload[64];

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    ResetCb();
    FillUser(nvdbUser_wgCfg, 0x5A, 0x1000);
    memset(payload, 0x77, sizeof(payload));

    TEST_ASSERT(nvdbRes_ok == NvDb_Delete(nvdbUser_wgCfg, 0, 0x1000, NULL));
    TEST_ASSERT(nvdbt_collectAll() > 0);

    /* This is what a user buys by deleting ahead of time. */
    mock_flash_eraseCnt = 0;
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_wgCfg, payload, 0,
                                         sizeof(payload)));
    TEST_ASSERT(0 == mock_flash_eraseCnt);
}

static void test_a_write_into_pending_space_pulls_its_erase_forward(void)
{
    uint8_t payload[64];
    uint8_t back[0x1000];

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    ResetCb();
    FillUser(nvdbUser_modbusLutA, 0x5A, 0x4000);
    memset(payload, 0x77, sizeof(payload));

    TEST_ASSERT(nvdbRes_ok == NvDb_Delete(nvdbUser_modbusLutA, 0, 0x4000,
                                          OnDone));

    /* The writer does not wait for a lowest-priority task to reach it: it
     * does that unit's erase itself and gets on with the write. */
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_modbusLutA, payload, 10,
                                         sizeof(payload)));
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_modbusLutA, back, 0, 0x1000));
    TEST_ASSERT(0xFF == back[0]);
    TEST_ASSERT(0x77 == back[10]);
    TEST_ASSERT(0xFF == back[500]);

    /* Only the units the write touched were pulled forward; the rest is
     * still the collector's business. */
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_modbusLutA, back, 0x2000,
                                        0x1000));
    TEST_ASSERT(0x5A == back[0]);
    TEST_ASSERT(0 == s_cbCnt);

    TEST_ASSERT(nvdbt_collectAll() > 0);
    TEST_ASSERT(1 == s_cbCnt);
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_modbusLutA, back, 0x2000,
                                        0x1000));
    TEST_ASSERT(0xFF == back[0]);
    /* ...and the bytes the write put down survived the collector. */
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_modbusLutA, back, 0, 0x1000));
    TEST_ASSERT(0x77 == back[10]);
}

/* The invariant a write into pending space has to hold, in every geometry:
 * bytes a write puts down are NEVER erased by a later collector step.  The
 * write pulls the erase of the units it touches forward; whatever is left of
 * the mark must be strictly the part the write did not cover. */
static void CheckWriteIntoPending(uint32_t markOff, uint32_t markLen,
                                  uint32_t writeOff, const char *what)
{
    uint8_t payload[64];
    uint8_t back[64];

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    ResetCb();
    FillUser(nvdbUser_modbusLutA, 0x5A, 0x4000);
    memset(payload, 0x77, sizeof(payload));

    TEST_ASSERT(nvdbRes_ok == NvDb_Delete(nvdbUser_modbusLutA, markOff,
                                          markLen, OnDone));
    TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_modbusLutA, payload,
                                         writeOff, sizeof(payload)));

    memset(back, 0, sizeof(back));
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_modbusLutA, back, writeOff,
                                        sizeof(back)));
    TEST_ASSERT_MEM_EQ(payload, back, sizeof(back));

    (void)nvdbt_collectAll();

    memset(back, 0, sizeof(back));
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_modbusLutA, back, writeOff,
                                        sizeof(back)));
    if (memcmp(payload, back, sizeof(back)) != 0) {
        printf("  %s: the collector erased the write (0x%02X at 0x%X)\n",
               what, back[0], writeOff);
        test_failures++;
    }
}

static void test_a_write_is_never_erased_by_the_collector(void)
{
    /* head of the mark */
    CheckWriteIntoPending(0x0000, 0x2000, 0x0000, "write at the mark head");
    /* middle — the mark must split */
    CheckWriteIntoPending(0x0000, 0x4000, 0x2000, "write in the mark middle");
    /* tail — the mark must keep only its head */
    CheckWriteIntoPending(0x0000, 0x2000, 0x1000, "write at the mark tail");
    /* the whole mark */
    CheckWriteIntoPending(0x1000, 0x1000, 0x1000, "write over the whole mark");
}

static void test_marks_coalesce(void)
{
    uint32_t i;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    ResetCb();
    FillUser(nvdbUser_fwuStored, 0x5A, 0x8000);

    /* Adjacent ranges merge, so an append-shaped deleter cannot exhaust the
     * list by asking one page at a time. */
    for (i = 0; i < 200u; i++) {
        TEST_ASSERT(nvdbRes_ok == NvDb_Delete(nvdbUser_fwuStored, i * 256u,
                                              256u, NULL));
    }
    TEST_ASSERT(nvdbt_collectAll() > 0);
    TEST_ASSERT(nvdbt_allBytes(NVDBT_FWUSTORED_ADDR, 200u * 256u, 0xFF));
}

static void test_mark_list_overflow_does_the_work_inline(void)
{
    uint32_t i;
    uint8_t  back[8];

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    ResetCb();
    FillUser(nvdbUser_fwuStored, 0x5A, 0x20000);

    /* Non-adjacent ranges cannot merge, so the list fills.  Nothing is
     * dropped: the delete that finds it full does its own erase, and its
     * callback fires before the call returns. */
    for (i = 0; i < 200u; i++) {
        eNvDbRes res = NvDb_Delete(nvdbUser_fwuStored, i * 0x400u, 8u, OnDone);
        TEST_ASSERT(nvdbRes_ok == res);
    }
    TEST_ASSERT(s_cbCnt > 0);            /* the list did overflow           */

    TEST_ASSERT(nvdbt_collectAll() > 0);
    for (i = 0; i < 200u; i++) {
        TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_fwuStored, back,
                                            i * 0x400u, 8u));
        TEST_ASSERT(0xFF == back[0]);
        TEST_ASSERT(0xFF == back[7]);
    }
    TEST_ASSERT(200 == s_cbCnt);
}

static void test_wipe_erases_the_whole_area_and_nothing_else(void)
{
    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    ResetCb();
    FillUser(nvdbUser_modbusLutA, 0x5A, 0x4000);
    FillUser(nvdbUser_modbusLutB, 0x3C, 0x4000);

    TEST_ASSERT(nvdbRes_ok == NvDb_Wipe(nvdbUser_modbusLutA, OnDone));
    TEST_ASSERT(nvdbt_collectAll() > 0);

    TEST_ASSERT(nvdbt_allBytes(NVDBT_LUTA_ADDR, 0x4000, 0xFF));
    TEST_ASSERT(nvdbt_allBytes(NVDBT_LUTB_ADDR, 0x4000, 0x3C));
    TEST_ASSERT(1 == s_cbCnt);
    TEST_ASSERT(nvdbUser_modbusLutA == s_cbUser);
}

static void test_a_callback_may_call_back_into_nvdb(void)
{
    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    ResetCb();
    FillUser(nvdbUser_wgCfg, 0x5A, 0x1000);

    TEST_ASSERT(nvdbRes_ok == NvDb_Delete(nvdbUser_wgCfg, 0, 0x1000,
                                          OnDoneReenter));
    TEST_ASSERT(nvdbt_collectAll() > 0);
    TEST_ASSERT(1 == s_cbCnt);
    TEST_ASSERT(1 == s_cbReenterOk);
}

static void test_marks_do_not_survive_a_reset(void)
{
    uint8_t back[8];

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    ResetCb();
    FillUser(nvdbUser_wgCfg, 0x5A, 0x1000);
    TEST_ASSERT(nvdbRes_ok == NvDb_Delete(nvdbUser_wgCfg, 0, 0x1000, OnDone));

    TEST_ASSERT(nvdbRes_ok == nvdbt_reboot());

    /* The request is gone, the bytes are still readable, and the callback
     * simply never fires.  This is the accepted risk, stated as a test. */
    TEST_ASSERT(0 == nvdbt_collectAll());
    TEST_ASSERT(0 == s_cbCnt);
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_wgCfg, back, 0, sizeof(back)));
    TEST_ASSERT(0x5A == back[0]);
}

/* What image_store does with a 488 KB blob, and what the deferred delete
 * exists for: declare the old one unwanted, then stream the new one straight
 * over the top.  Every write must land on erased space — either because the
 * collector got there first or because the write pulled that unit's erase
 * forward itself — and the total erase count must be one per unit, not two. */
static void test_wipe_then_stream_costs_one_erase_per_unit(void)
{
    const uint32_t blobLen = 64u * 1024u;
    uint8_t       *payload = malloc(blobLen);
    uint8_t       *back    = malloc(blobLen);
    uint32_t       off     = 0;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    ResetCb();

    for (uint32_t i = 0; i < blobLen; i++) {
        payload[i] = (uint8_t)(i * 31u);
    }

    /* An old blob is in the way. */
    FillUser(nvdbUser_fwuStored, 0x5A, blobLen);

    TEST_ASSERT(nvdbRes_ok == NvDb_Wipe(nvdbUser_fwuStored, NULL));

    mock_flash_eraseCnt = 0;
    for (off = 0; off < blobLen; off += 256u) {
        TEST_ASSERT(nvdbRes_ok == NvDb_Write(nvdbUser_fwuStored,
                                             &payload[off], off, 256u));
    }

    /* 16 units of 4 KB, each erased exactly once. */
    TEST_ASSERT((blobLen / 0x1000u) == mock_flash_eraseCnt);

    memset(back, 0, blobLen);
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_fwuStored, back, 0, blobLen));
    TEST_ASSERT_MEM_EQ(payload, back, blobLen);

    /* The area is far bigger than this blob, so the rest of the wipe is
     * still the collector's to finish — off the write path, which is the
     * whole point.  When it does, the tail reads erased and the blob is
     * untouched. */
    TEST_ASSERT(nvdbt_collectAll() > 0);
    memset(back, 0, blobLen);
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_fwuStored, back, 0, blobLen));
    TEST_ASSERT_MEM_EQ(payload, back, blobLen);
    TEST_ASSERT(nvdbRes_ok == NvDb_Read(nvdbUser_fwuStored, back, blobLen,
                                        4096u));
    TEST_ASSERT(0xFF == back[0]);
    TEST_ASSERT(0xFF == back[4095]);
    TEST_ASSERT(0 == nvdbt_collectAll());

    free(payload);
    free(back);
}

static void test_collector_takes_one_unit_per_call(void)
{
    uint32_t before = 0;

    TEST_ASSERT(nvdbRes_ok == nvdbt_freshBoot());
    ResetCb();
    FillUser(nvdbUser_modbusLutA, 0x5A, 0x4000);
    TEST_ASSERT(nvdbRes_ok == NvDb_Wipe(nvdbUser_modbusLutA, NULL));

    /* A waiting writer gets in between units, which is only true if the
     * collector gives the lock back that often. */
    mock_flash_eraseCnt = 0;
    TEST_ASSERT(NvDb_CollectStep());
    before = mock_flash_eraseCnt;
    TEST_ASSERT(1 == before);
    TEST_ASSERT(NvDb_CollectStep());
    TEST_ASSERT(2 == mock_flash_eraseCnt);
    TEST_ASSERT(nvdbt_collectAll() > 0);

    /* Four units of the area, and nothing else.  The wear counters are NOT
     * written back the moment the collector goes idle: one erase anywhere on
     * the medium would then cost a second one on the single pinned wear
     * sector, forever, to protect a number that is allowed to be lost. */
    TEST_ASSERT(4 == mock_flash_eraseCnt);
    TEST_ASSERT(!NvDb_CollectStep());
    TEST_ASSERT(4 == mock_flash_eraseCnt);

    /* It does get written back eventually, once idleness says the dirt is
     * not going to get any larger. */
    nvdbt_idle(80u);
    TEST_ASSERT(5 == mock_flash_eraseCnt);

    /* And then it stops — there is nothing new to say. */
    nvdbt_idle(80u);
    TEST_ASSERT(5 == mock_flash_eraseCnt);
}

int main(void)
{
    RUN_TEST(test_delete_returns_before_the_bytes_are_gone);
    RUN_TEST(test_partial_delete_preserves_what_was_not_deleted);
    RUN_TEST(test_a_deleted_range_is_cheap_to_write_again);
    RUN_TEST(test_a_write_into_pending_space_pulls_its_erase_forward);
    RUN_TEST(test_a_write_is_never_erased_by_the_collector);
    RUN_TEST(test_marks_coalesce);
    RUN_TEST(test_mark_list_overflow_does_the_work_inline);
    RUN_TEST(test_wipe_erases_the_whole_area_and_nothing_else);
    RUN_TEST(test_a_callback_may_call_back_into_nvdb);
    RUN_TEST(test_marks_do_not_survive_a_reset);
    RUN_TEST(test_wipe_then_stream_costs_one_erase_per_unit);
    RUN_TEST(test_collector_takes_one_unit_per_call);

    printf("%s: %d failure(s)\n", __FILE__, test_failures);
    return test_failures ? 1 : 0;
}
