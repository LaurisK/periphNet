#include "bl_app_contract.h"
#include "image_mgmt.h"

#define BOOTLOADER_VERSION_MAJOR    1
#define BOOTLOADER_VERSION_MINOR    0
#define BOOTLOADER_VERSION_PATCH    0

/* --------------------------------------------------------------------------
 * get_bl_version
 * -------------------------------------------------------------------------- */

static void bl_get_bl_version(uint32_t *major, uint32_t *minor, uint32_t *patch)
{
    if (major) *major = BOOTLOADER_VERSION_MAJOR;
    if (minor) *minor = BOOTLOADER_VERSION_MINOR;
    if (patch) *patch = BOOTLOADER_VERSION_PATCH;
}

/* --------------------------------------------------------------------------
 * verify_internal_app — validate the application in internal flash
 * -------------------------------------------------------------------------- */

static int bl_verify_internal_app(void)
{
    uint8_t work_buf[sizeof(sAppInfo)];

    eFwuRes res = ImgMgmt_Validate(APPLICATION_START_ADDR, false,
                                   work_buf, sizeof(work_buf));
    if (res != FWU_OK) {
        return BL_WRONG_MAGIC;
    }

    return BL_OK;
}

/* --------------------------------------------------------------------------
 * verify_hmac — HMAC-SHA256 verification (stub: always passes)
 *
 * Future: compute HMAC-SHA256 over data using key from boot status,
 * compare with expected[].
 * -------------------------------------------------------------------------- */

static bool bl_verify_hmac(const uint8_t *data, uint32_t size,
                           const uint8_t expected[DFU_HMAC_SIZE])
{
    (void)data;
    (void)size;
    (void)expected;
    return true;
}

/* --------------------------------------------------------------------------
 * decrypt_blob — AES-GCM decryption (stub: no-op)
 *
 * Future: decrypt data in-place using key from boot status.
 * Blob layout: [nonce:12][ciphertext:N][tag:16]
 * -------------------------------------------------------------------------- */

static eFwuRes bl_decrypt_blob(uint8_t *data, uint32_t size)
{
    (void)data;
    (void)size;
    return FWU_OK;
}

/* --------------------------------------------------------------------------
 * API table — placed at 0x08007F00 by linker (.bl_api section)
 * -------------------------------------------------------------------------- */

__attribute__((section(".bl_api")))
__attribute__((used))
const sBootloaderApi bootloader_api = {
    .magic               = BL_API_MAGIC,
    .version             = 2,
    .get_bl_version      = bl_get_bl_version,
    .verify_internal_app = bl_verify_internal_app,
    .verify_hmac         = bl_verify_hmac,
    .decrypt_blob        = bl_decrypt_blob,
    .reserved            = {0},
};
