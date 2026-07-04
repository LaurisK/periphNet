#include "image_mgmt.h"
#include "version.h"
#include "w25q128.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * CRC32 (standard polynomial, used for header CRC and image CRC)
 * -------------------------------------------------------------------------- */

uint32_t ImgMgmt_Crc32Init(void)
{
    return 0xFFFFFFFFu;
}

uint32_t ImgMgmt_Crc32Update(uint32_t state, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        state ^= data[i];
        for (int j = 0; j < 8; j++) {
            state = (state >> 1) ^ (0xEDB88320u & -(state & 1u));
        }
    }

    return state;
}

uint32_t ImgMgmt_Crc32Final(uint32_t state)
{
    return ~state;
}

uint32_t ImgMgmt_Crc32(const uint8_t *data, uint32_t len)
{
    return ImgMgmt_Crc32Final(ImgMgmt_Crc32Update(ImgMgmt_Crc32Init(), data, len));
}

/* --------------------------------------------------------------------------
 * Flash read helper
 * -------------------------------------------------------------------------- */

static bool flash_read(uint32_t addr, bool is_external, void *buf, uint32_t len)
{
    if (is_external) {
        return W25Q128_Read(addr, (uint8_t *)buf, len) == W25Q128_OK;
    }

    /* Internal flash — memory-mapped */
    memcpy(buf, (const void *)addr, len);
    return true;
}

/* --------------------------------------------------------------------------
 * ImgMgmt_GetVersion
 * -------------------------------------------------------------------------- */

bool ImgMgmt_GetVersion(uint32_t base, bool is_external, sFwVerArea *out)
{
    if (!out) {
        return false;
    }

    return flash_read(base + FW_OFFSET_FW_VERSION, is_external,
                      out, sizeof(sFwVerArea));
}

/* --------------------------------------------------------------------------
 * ImgMgmt_Validate
 *
 * 1. Read sAppInfo header
 * 2. Check magic
 * 3. Check image_size bounds
 * 4. Verify HMAC (stub: always passes)
 * -------------------------------------------------------------------------- */

eFwuRes ImgMgmt_Validate(uint32_t base, bool is_external,
                         uint8_t *work_buf, uint32_t buf_size)
{
    if (!work_buf || buf_size < sizeof(sAppInfo)) {
        return FWU_ERR_IMAGE_SIZE;
    }

    sAppInfo *info = (sAppInfo *)work_buf;

    /* Read app info header */
    if (!flash_read(base + FW_OFFSET_APP_HEADER, is_external,
                    info, sizeof(sAppInfo))) {
        return FWU_ERR_FLASH_READ;
    }

    /* Check magic */
    if (info->magic != APP_INFO_MAGIC) {
        return FWU_ERR_WRONG_MAGIC;
    }

    /* Check image size bounds.
     * 0xFFFFFFFF = unpatched placeholder (no post-build tool yet) — allowed. */
    if (info->image_size == 0) {
        return FWU_ERR_IMAGE_SIZE;
    }
    if (info->image_size != 0xFFFFFFFFu && info->image_size > APPLICATION_SIZE) {
        return FWU_ERR_IMAGE_SIZE;
    }

    /* ---- HMAC verification ---- */

    /* Skip if image_size is unpatched (no post-build tool) */
    if (info->image_size != 0xFFFFFFFFu) {
        /* Check for all-0xFF placeholder (unsigned image) */
        bool hmac_is_placeholder = true;
        for (uint32_t i = 0; i < DFU_HMAC_SIZE; i++) {
            if (info->image_hmac[i] != 0xFFu) {
                hmac_is_placeholder = false;
                break;
            }
        }

        /* HMAC only applies to plaintext internal-flash images; external
         * flash holds opaque encrypted blobs authenticated via GCM. */
        if (!hmac_is_placeholder && !is_external) {
#ifdef BOOTLOADER_BUILD
            /* BL has direct access to the key — declared in boot_api.c */
            extern eFwuRes bl_verify_image_hmac(uint32_t flash_addr, bool is_external,
                                                 uint32_t size,
                                                 const uint8_t expected[DFU_HMAC_SIZE]);
            eFwuRes hmac_res = bl_verify_image_hmac(base, false,
                                                     info->image_size,
                                                     info->image_hmac);
            if (hmac_res != FWU_OK) {
                return FWU_ERR_IMAGE_HMAC;
            }
#else
            const sBootloaderApi *bl_api =
                (const sBootloaderApi *)BL_API_TABLE_ADDR;
            if (bl_api->magic == BL_API_MAGIC &&
                bl_api->version >= 3 &&
                bl_api->verify_image_hmac != NULL) {
                eFwuRes hmac_res = bl_api->verify_image_hmac(
                    base, false, info->image_size, info->image_hmac);
                if (hmac_res != FWU_OK) {
                    return FWU_ERR_IMAGE_HMAC;
                }
            }
#endif
        }
    }

    return FWU_OK;
}

/* --------------------------------------------------------------------------
 * ImgMgmt_CheckVerForFwu
 * -------------------------------------------------------------------------- */

eFwuRes ImgMgmt_CheckVerForFwu(const sFwVerArea *current_ver,
                                const sFwVerArea *incoming_ver)
{
    if (!current_ver || !incoming_ver) {
        return FWU_ERR_NO_IMAGE;
    }

    return ver_checkCompatibility(&current_ver->ver, &incoming_ver->ver);
}
