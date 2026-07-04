#include "bl_app_contract.h"
#include "image_mgmt.h"
#include "hmac_sha256.h"
#include "aes_gcm.h"
#include "secrets.h"
#include "boot_status.h"
#include "w25q128.h"
#include <string.h>

#define BOOTLOADER_VERSION_MAJOR    1
#define BOOTLOADER_VERSION_MINOR    1
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
 * verify_hmac — HMAC-SHA256 verification of a RAM buffer
 * -------------------------------------------------------------------------- */

static bool bl_verify_hmac(const uint8_t *data, uint32_t size,
                           const uint8_t expected[DFU_HMAC_SIZE])
{
    if (!data || size == 0 || !expected) {
        return false;
    }

    uint8_t mac[HMAC_SHA256_SIZE];
    hmac_sha256(GLB_hmacKey, sizeof(GLB_hmacKey), data, size, mac);

    uint8_t diff = 0;
    for (uint32_t i = 0; i < DFU_HMAC_SIZE; i++) {
        diff |= mac[i] ^ expected[i];
    }

    return diff == 0;
}

/* --------------------------------------------------------------------------
 * verify_image_hmac — streaming HMAC verification of a flash image
 *
 * Reads the image in 256-byte chunks, zeroing the HMAC field during
 * computation, then compares with the expected HMAC.
 * Uses GLB_blKey (compile-time BL key).
 * -------------------------------------------------------------------------- */

#define HMAC_CHUNK_SIZE 256u

eFwuRes bl_verify_image_hmac(uint32_t flash_addr, bool is_external,
                                     uint32_t size,
                                     const uint8_t expected[DFU_HMAC_SIZE])
{
    if (size == 0 || !expected) {
        return FWU_ERR_IMAGE_HMAC;
    }

    /* Check for all-0xFF placeholder (unsigned image) */
    bool all_ff = true;
    for (uint32_t i = 0; i < DFU_HMAC_SIZE; i++) {
        if (expected[i] != 0xFFu) {
            all_ff = false;
            break;
        }
    }
    if (all_ff) {
        return FWU_OK;  /* unsigned image, skip verification */
    }

    sHmacSha256Ctx ctx;
    hmac_sha256_init(&ctx, GLB_hmacKey, sizeof(GLB_hmacKey));

    uint8_t chunk[HMAC_CHUNK_SIZE];
    uint32_t offset = 0;

    while (offset < size) {
        uint32_t n = size - offset;
        if (n > HMAC_CHUNK_SIZE) {
            n = HMAC_CHUNK_SIZE;
        }

        /* Read from flash */
        if (is_external) {
            if (W25Q128_Read(flash_addr + offset, chunk, n) != W25Q128_OK) {
                return FWU_ERR_FLASH_READ;
            }
        } else {
            memcpy(chunk, (const void *)(flash_addr + offset), n);
        }

        /* Zero the HMAC field region within this chunk */
        uint32_t hmac_start = FW_OFFSET_IMAGE_HMAC;
        uint32_t hmac_end   = FW_OFFSET_IMAGE_HMAC + DFU_HMAC_SIZE;
        uint32_t chunk_end  = offset + n;

        if (offset < hmac_end && chunk_end > hmac_start) {
            uint32_t zero_start = (hmac_start > offset) ? (hmac_start - offset) : 0;
            uint32_t zero_end   = (hmac_end < chunk_end) ? (hmac_end - offset) : n;
            memset(chunk + zero_start, 0, zero_end - zero_start);
        }

        hmac_sha256_update(&ctx, chunk, n);
        offset += n;
    }

    uint8_t computed[HMAC_SHA256_SIZE];
    hmac_sha256_final(&ctx, computed);

    /* Constant-time comparison */
    uint8_t diff = 0;
    for (uint32_t i = 0; i < DFU_HMAC_SIZE; i++) {
        diff |= computed[i] ^ expected[i];
    }

    return (diff == 0) ? FWU_OK : FWU_ERR_IMAGE_HMAC;
}

/* --------------------------------------------------------------------------
 * decrypt_blob — AES-128-GCM decryption of a RAM buffer
 *
 * Blob layout: [nonce:12][ciphertext:N][tag:16]
 * Uses the compile-time BL key; the key never leaves the bootloader.
 * -------------------------------------------------------------------------- */

static eFwuRes bl_decrypt_blob(uint8_t *data, uint32_t size)
{
    if (!data || size <= AES_GCM_IV_SIZE + AES_GCM_TAG_SIZE) {
        return FWU_ERR_DECRYPT;
    }

    return aes_gcm_decrypt(GLB_blKey, data, size, NULL, 0)
        ? FWU_OK : FWU_ERR_DECRYPT;
}

/* --------------------------------------------------------------------------
 * API table — placed at 0x08007F00 by linker (.bl_api section)
 * -------------------------------------------------------------------------- */

__attribute__((section(".bl_api")))
__attribute__((used))
const sBootloaderApi bootloader_api = {
    .magic               = BL_API_MAGIC,
    .version             = 3,
    .get_bl_version      = bl_get_bl_version,
    .verify_internal_app = bl_verify_internal_app,
    .verify_hmac         = bl_verify_hmac,
    .decrypt_blob        = bl_decrypt_blob,
    .verify_image_hmac   = bl_verify_image_hmac,
    .reserved            = {0},
};
