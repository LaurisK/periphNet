#include "image_mgmt.h"
#include "version.h"
#include "w25q128.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * CRC32 (standard polynomial, used for header CRC and image CRC)
 * -------------------------------------------------------------------------- */

uint32_t ImgMgmt_Crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
        }
    }

    return ~crc;
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

    /* ---- HMAC verification (stub: always passes) ---- */
    /*
     * Future: read image in chunks, compute HMAC-SHA256, compare with
     * info->image_hmac[]. The HMAC key will be stored in boot status
     * (ext flash) and accessible only via BL API.
     *
     * For now, skip HMAC — images are considered valid if magic + size OK.
     */

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
