#ifndef BL_APP_CONTRACT_H
#define BL_APP_CONTRACT_H

#include "dfu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * Memory addresses
 * ========================================================================== */

#define BL_API_TABLE_ADDR            0x08007F00u
#define APP_INFO_HEADER_ADDR         0x08008200u
#define BOOTLOADER_START_ADDR        0x08000000u
#define BOOTLOADER_SIZE              (32u * 1024u)
#define APPLICATION_START_ADDR       0x08008000u
#define APPLICATION_SIZE             (480u * 1024u)

/* ==========================================================================
 * External flash layout
 * ========================================================================== */

/* Blob areas hold encrypted .pnfw blobs (max image 480 KB + 96 B blob
 * overhead → 488 KB, 4 KB-sector aligned). */
#define EXT_FLASH_BASE               0x00000000u
#define EXT_FLASH_FWU_STATUS_ADDR    0x00000000u
#define EXT_FLASH_FWU_STATUS_SIZE    0x00001000u
#define EXT_FLASH_FWU_IMG_ADDR       0x00001000u
#define EXT_FLASH_FWU_IMG_SIZE       0x0007A000u
#define EXT_FLASH_GOLDEN_IMG_ADDR    0x0007B000u
#define EXT_FLASH_GOLDEN_IMG_SIZE    0x0007A000u
#define EXT_FLASH_IMG_META_ADDR      0x000F5000u  /* app-side image metadata */
#define EXT_FLASH_IMG_META_SIZE      0x00001000u
#define EXT_FLASH_CRASH_LOG_ADDR     0x000F8000u
#define EXT_FLASH_CRASH_LOG_SIZE     0x00001000u

/* Modbus register-config LUT: two 16 KB record-stream regions (A/B roles —
 * the selector sector says which one is active; uploads always compile into
 * the inactive one) + a 4 KB selector sector (NOR bit-clear pattern like the
 * boot status). Application-owned, never touched by the bootloader.
 * See Shared/Modbus/modbus_records.h. */
#define EXT_FLASH_MODBUS_LUT_A_ADDR  0x000F9000u
#define EXT_FLASH_MODBUS_LUT_B_ADDR  0x000FD000u
#define EXT_FLASH_MODBUS_LUT_SIZE    0x00004000u
#define EXT_FLASH_MODBUS_SEL_ADDR    0x00101000u
#define EXT_FLASH_MODBUS_SEL_SIZE    0x00001000u

/* WireGuard monotonic time base: one 4 KB sector used as an append-only slot
 * ring, so the TAI64N handshake timestamp never goes backwards across a
 * reboot (the hub rejects replayed/older timestamps).  Application-owned,
 * never touched by the bootloader.  See App/Net/wg_time.h. */
#define EXT_FLASH_WG_TIME_ADDR       0x00102000u
#define EXT_FLASH_WG_TIME_SIZE       0x00001000u

/* ==========================================================================
 * Magic numbers
 * ========================================================================== */

#define BL_API_MAGIC                 0x424C4150u   /* "BLAP" */
#define APP_INFO_MAGIC               0x41505049u   /* "APPI" */
#define BOOT_STATUS_MAGIC            0x424F4F54u   /* "BOOT" */
#define MODBUS_LUT_MAGIC             0x4D424346u   /* "MBCF" */
#define MODBUS_SEL_MAGIC             0x4D42534Cu   /* "MBSL" */
#define WG_TIME_MAGIC                0x5747544Du   /* "WGTM" */

/* ==========================================================================
 * Feature flags  (sAppInfo.features)
 * ========================================================================== */

#define APP_FEATURE_ETHERNET         (1u << 0)
#define APP_FEATURE_MQTT             (1u << 1)
#define APP_FEATURE_MODBUS           (1u << 2)
#define APP_FEATURE_OTA              (1u << 3)
#define APP_FEATURE_TRACING          (1u << 4)
#define APP_FEATURE_RTC              (1u << 5)
#define APP_FEATURE_CAN              (1u << 6)

/* ==========================================================================
 * Bootloader API  (function pointer table at BL_API_TABLE_ADDR)
 * ========================================================================== */

typedef void    (*fGetBlVersion)(uint32_t *major, uint32_t *minor, uint32_t *patch);
typedef int     (*fVerifyInternalApp)(void);
typedef bool    (*fVerifyHmac)(const uint8_t *data, uint32_t size,
                               const uint8_t expected[DFU_HMAC_SIZE]);
typedef eFwuRes (*fDecryptBlob)(uint8_t *data, uint32_t size);
typedef eFwuRes (*fVerifyImageHmac)(uint32_t flash_addr, bool is_external,
                                    uint32_t size,
                                    const uint8_t expected[DFU_HMAC_SIZE]);

typedef struct {
    uint32_t            magic;               /* BL_API_MAGIC                 */
    uint32_t            version;             /* API version (3)              */
    fGetBlVersion       get_bl_version;      /* return BL version            */
    fVerifyInternalApp  verify_internal_app; /* validate app in int. flash   */
    fVerifyHmac         verify_hmac;         /* HMAC-SHA256 verify (RAM)     */
    fDecryptBlob        decrypt_blob;        /* AES-GCM decrypt (RAM blob)   */
    fVerifyImageHmac    verify_image_hmac;   /* HMAC verify over flash image */
    uint32_t            reserved[9];         /* future expansion             */
} sBootloaderApi;

/* ==========================================================================
 * BL simple return codes  (for verify_internal_app)
 * ========================================================================== */

#define BL_OK            0
#define BL_ERROR         1
#define BL_WRONG_MAGIC   2

#ifdef __cplusplus
}
#endif

#endif /* BL_APP_CONTRACT_H */
