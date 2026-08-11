#ifndef IMAGE_MGMT_H
#define IMAGE_MGMT_H

#include "bl_app_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Read firmware version from a flash image.
 *
 * @param base         Base address of the image.
 * @param is_external  true = external SPI flash, false = internal flash (memory-mapped).
 * @param out          Output version area.
 * @return true on success.
 */
bool ImgMgmt_GetVersion(uint32_t base, bool is_external, sFwVerArea *out);

/**
 * Validate a plaintext firmware image (magic + size + HMAC-SHA256).
 *
 * HMAC applies to internal flash images only; external flash holds
 * encrypted blobs which are authenticated via their GCM tag instead.
 *
 * @param base         Base address of the image.
 * @param is_external  true = external SPI flash, false = internal flash.
 * @param work_buf     Scratch buffer (>= 256 bytes).
 * @param buf_size     Size of work_buf.
 * @return fwuRes_ok if image is valid.
 */
eFwuRes ImgMgmt_Validate(uint32_t base, bool is_external, uint8_t *work_buf, uint32_t buf_size);

/**
 * Check incoming version compatibility against current version.
 *
 * @param current_ver  Currently running firmware version.
 * @param incoming_ver Candidate firmware version (from staged image).
 * @return fwuRes_ok if compatible.
 */
eFwuRes ImgMgmt_CheckVerForFwu(const sFwVerArea *current_ver, const sFwVerArea *incoming_ver);

/**
 * Software CRC32 (standard polynomial 0xEDB88320, zlib-compatible).
 */
uint32_t ImgMgmt_Crc32(const uint8_t *data, uint32_t len);

/**
 * Streaming CRC32: state = Init(); state = Update(state, ...); Final(state).
 */
uint32_t ImgMgmt_Crc32Init(void);
uint32_t ImgMgmt_Crc32Update(uint32_t state, const uint8_t *data, uint32_t len);
uint32_t ImgMgmt_Crc32Final(uint32_t state);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_MGMT_H */
