/**
 * Unit tests for Shared/Fwu/boot_status.c over a NOR-faithful RAM flash mock
 * (programming can only clear bits — catches any accidental 0→1 write),
 * plus the CRC32 helpers from image_mgmt.c.
 */

#include "test_util.h"

#include "boot_status.h"
#include "image_mgmt.h"
#include "nvdb.h"
#include "w25q128_mock.h"

#include <stdlib.h>

/* ============================================================================
 * CRC32
 * ============================================================================ */

static void test_crc32(void)
{
    /* Standard check value for polynomial 0xEDB88320 */
    TEST_ASSERT(ImgMgmt_Crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);

    /* Streaming must match one-shot */
    uint32_t st = ImgMgmt_Crc32Init();
    st = ImgMgmt_Crc32Update(st, (const uint8_t *)"1234", 4);
    st = ImgMgmt_Crc32Update(st, (const uint8_t *)"56789", 5);
    TEST_ASSERT(ImgMgmt_Crc32Final(st) == 0xCBF43926u);
}

/* ============================================================================
 * Boot status lifecycle
 * ============================================================================ */

static void test_ensure_valid_on_blank_flash(void)
{
    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);

    sBootStatus st;
    TEST_ASSERT(BootStatus_Read(&st) != 0);         /* blank → invalid */
    TEST_ASSERT(BootStatus_EnsureValid() == 0);
    TEST_ASSERT(BootStatus_Read(&st) == 0);

    TEST_ASSERT(st.last_fwu_result == fwuRes_noResult);
    TEST_ASSERT(st.flags.word == 0xFFFFFFFFu);      /* all flags erased */
    TEST_ASSERT(BootStatus_AttemptsRemaining() == BOOT_ATTEMPTS_MAX);
    TEST_ASSERT(BootStatus_GetFwuAction() == fwuAction_none);
}

static void test_request_fwu_arms_install(void)
{
    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);
    TEST_ASSERT(BootStatus_EnsureValid() == 0);

    TEST_ASSERT(BootStatus_RequestFwu() == 0);
    TEST_ASSERT(BootStatus_GetFwuAction() == fwuAction_install);

    /* Bit-clear must not have invalidated the CRC-protected header */
    sBootStatus st;
    TEST_ASSERT(BootStatus_Read(&st) == 0);
    TEST_ASSERT(st.flags.bits.fwu_requested == 0);
}

static void test_unconfirmed_attempts_to_rollback(void)
{
    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);

    /* Successful install leaves the image unconfirmed */
    TEST_ASSERT(BootStatus_FinishFwu(fwuRes_ok, false) == 0);
    TEST_ASSERT(BootStatus_IsUnconfirmed());
    TEST_ASSERT(BootStatus_GetFwuAction() == fwuAction_none);
    TEST_ASSERT(BootStatus_AttemptsRemaining() == 3);

    TEST_ASSERT(BootStatus_ConsumeBootAttempt() == 0);
    TEST_ASSERT(BootStatus_AttemptsRemaining() == 2);
    TEST_ASSERT(BootStatus_GetFwuAction() == fwuAction_none);

    TEST_ASSERT(BootStatus_ConsumeBootAttempt() == 0);
    TEST_ASSERT(BootStatus_ConsumeBootAttempt() == 0);
    TEST_ASSERT(BootStatus_AttemptsRemaining() == 0);

    /* Unconfirmed with all attempts exhausted → rollback */
    TEST_ASSERT(BootStatus_GetFwuAction() == fwuAction_rollback);

    sBootStatus st;
    TEST_ASSERT(BootStatus_Read(&st) == 0);
    TEST_ASSERT(st.last_fwu_result == fwuRes_ok);
}

static void test_confirm_stops_attempt_counting(void)
{
    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);

    TEST_ASSERT(BootStatus_FinishFwu(fwuRes_ok, false) == 0);
    TEST_ASSERT(BootStatus_ConsumeBootAttempt() == 0);
    TEST_ASSERT(BootStatus_ConsumeBootAttempt() == 0);
    TEST_ASSERT(BootStatus_ConsumeBootAttempt() == 0);

    /* Confirm just in time — no rollback even with attempts gone */
    TEST_ASSERT(BootStatus_ConfirmApp() == 0);
    TEST_ASSERT(!BootStatus_IsUnconfirmed());
    TEST_ASSERT(BootStatus_GetFwuAction() == fwuAction_none);
}

static void test_finish_fwu_preconfirmed(void)
{
    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);

    /* Rollback restores golden image which is known-good → pre-confirmed */
    TEST_ASSERT(BootStatus_FinishFwu(fwuRes_rollback, true) == 0);
    TEST_ASSERT(!BootStatus_IsUnconfirmed());
    TEST_ASSERT(BootStatus_GetFwuAction() == fwuAction_none);

    sBootStatus st;
    TEST_ASSERT(BootStatus_Read(&st) == 0);
    TEST_ASSERT(st.last_fwu_result == fwuRes_rollback);
    TEST_ASSERT(BootStatus_AttemptsRemaining() == 3);   /* attempts restored */
}

static void test_corrupt_header_detected_and_recovered(void)
{
    mock_flash_reset();
    TEST_ASSERT(NvDb_Init() == nvdbRes_ok);
    TEST_ASSERT(BootStatus_EnsureValid() == 0);

    /* Clear a bit inside the CRC-protected header region (NOR-legal) */
    {
        /* A NOR-legal bit flip inside the CRC'd header, through the front
         * door: clearing bits is exactly the write nvDb programs in place. */
        uint8_t b = 0;
        TEST_ASSERT(NvDb_Read(nvdbUser_bootStatus, &b, 8u, 1u) == nvdbRes_ok);
        b &= 0x7Fu;
        TEST_ASSERT(NvDb_Write(nvdbUser_bootStatus, &b, 8u, 1u) == nvdbRes_ok);
    }

    sBootStatus st;
    TEST_ASSERT(BootStatus_Read(&st) != 0);

    /* EnsureValid must rewrite a fresh default */
    TEST_ASSERT(BootStatus_EnsureValid() == 0);
    TEST_ASSERT(BootStatus_Read(&st) == 0);
    TEST_ASSERT(st.last_fwu_result == fwuRes_noResult);
}

int main(void)
{
    RUN_TEST(test_crc32);
    RUN_TEST(test_ensure_valid_on_blank_flash);
    RUN_TEST(test_request_fwu_arms_install);
    RUN_TEST(test_unconfirmed_attempts_to_rollback);
    RUN_TEST(test_confirm_stops_attempt_counting);
    RUN_TEST(test_finish_fwu_preconfirmed);
    RUN_TEST(test_corrupt_header_detected_and_recovered);
    return test_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
