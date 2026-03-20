#ifndef DFU_TYPES_H
#define DFU_TYPES_H

#include "aes128.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * Firmware version
 * ========================================================================== */

typedef enum {
    fwDev_periphnet = 'P',
} eFwDeviceType;

typedef enum {
    fwTarget_release = 'v',   /* Release — production use              */
    fwTarget_dev     = 'd',   /* Development — internal testing        */
    fwTarget_local   = 'l',   /* Local build — uncontrolled            */
} eFwTarget;

#define FW_VER_SIZE       8u
#define FW_VER_AREA_SIZE  32u

/**
 * Firmware version — packed, same layout in binary and in ext flash.
 *
 * String format: "Pv1.2.3"  (deviceType target major.minor.patch)
 * With hwId:     "Pv1.2.3_ABCD"
 */
typedef struct {
    uint8_t  deviceType;   /* eFwDeviceType                            */
    uint8_t  target;       /* eFwTarget                                */
    uint16_t major;
    uint8_t  minor;
    uint8_t  patch;
    uint16_t hwId;         /* hardware variant (0 = generic)           */
} __attribute__((packed)) sFwVer;

_Static_assert(sizeof(sFwVer) == FW_VER_SIZE, "sFwVer size mismatch");

/** Padded version area — always 32 bytes in flash. */
typedef struct {
    sFwVer  ver;
    uint8_t reserved[FW_VER_AREA_SIZE - FW_VER_SIZE];
} __attribute__((packed)) sFwVerArea;

_Static_assert(sizeof(sFwVerArea) == FW_VER_AREA_SIZE, "sFwVerArea size mismatch");

/* ==========================================================================
 * FWU result codes
 * ========================================================================== */

typedef enum {
    FWU_OK                  = 0,
    FWU_ERR_IMAGE_SIZE      = 1,
    FWU_ERR_IMAGE_HMAC      = 2,
    FWU_ERR_VER_NOT_NEWER   = 3,
    FWU_ERR_VER_DEVICE_TYPE = 4,
    FWU_ERR_VER_HW_ID       = 5,
    FWU_ERR_FLASH_READ      = 6,
    FWU_ERR_FLASH_WRITE     = 7,
    FWU_ERR_NO_IMAGE        = 8,
    FWU_ERR_INSTALL         = 9,
    FWU_ERR_DECRYPT         = 10,
    FWU_ERR_BOOT_STATUS     = 11,
    FWU_ERR_WRONG_MAGIC     = 12,
    FWU_ROLLBACK            = 13,
} eFwuRes;

/* ==========================================================================
 * Application info header  (placed at APP_INFO_HEADER_ADDR by linker)
 * ========================================================================== */

#define DFU_HMAC_SIZE  32u

typedef struct {
    uint32_t    magic;                      /* APP_INFO_MAGIC                */
    sFwVerArea  fw_version;                 /* 32 B: firmware version        */
    uint32_t    image_size;                 /* binary size (0xFFFFFFFF = raw)*/
    uint8_t     image_hmac[DFU_HMAC_SIZE];  /* HMAC-SHA256  (stub: 0xFF)    */
    uint32_t    features;                   /* APP_FEATURE_* flags           */
    uint32_t    min_bl_version;             /* minimum BL API version        */
    uint32_t    reserved[8];                /* future use                    */
} sAppInfo;

/* ==========================================================================
 * Boot status  (external flash, first 4 KB sector)
 *
 * NOR-flash flag semantics: erased = 0xFF (all 1s).
 * A bit is "set" by clearing it to 0 (no erase required).
 * ========================================================================== */

typedef union {
    uint32_t word;                          /* erased value = 0xFFFFFFFF     */
    struct __attribute__((packed)) {
        uint32_t fwu_requested  : 1;        /* 0 = FWU requested by APP     */
        uint32_t confirmed      : 1;        /* 0 = APP confirmed healthy    */
        uint32_t boot_attempt_0 : 1;        /* 0 = 1st unconfirmed boot     */
        uint32_t boot_attempt_1 : 1;        /* 0 = 2nd                      */
        uint32_t boot_attempt_2 : 1;        /* 0 = 3rd                      */
        uint32_t boot_attempt_3 : 1;        /* 0 = 4th (rollback trigger)   */
        uint32_t _reserved      : 26;
    } bits;
} sBootFlags;

#define BOOT_ATTEMPTS_MAX  4u

typedef struct {
    uint32_t    magic;                      /* BOOT_STATUS_MAGIC             */
    uint32_t    version;                    /* struct version (2)            */
    uint8_t     aes_key[AES128_KEY_SIZE];   /* AES-128 key (BL inits)       */
    uint32_t    image_size;                 /* staged image size             */
    uint32_t    image_crc32;                /* staged image CRC32            */
    sFwVerArea  staged_version;             /* staged image version          */
    uint32_t    header_crc32;               /* CRC32(magic..staged_version)  */
    sBootFlags  flags;                      /* boot flags (outside CRC!)     */
} sBootStatus;

/* ==========================================================================
 * FWU action  (bootloader decision from boot flags)
 * ========================================================================== */

typedef enum {
    fwu_none = 0,           /* normal boot                              */
    fwu_install,            /* install staged image from ext flash       */
    fwu_rollback,           /* boot attempts exhausted → rollback        */
} eFwuAction;

/* ==========================================================================
 * Firmware binary layout offsets (relative to image base address)
 * ========================================================================== */

#define FW_OFFSET_APP_HEADER   0x200u       /* sAppInfo at base + 0x200     */
#define FW_OFFSET_FW_VERSION   0x204u       /* sFwVerArea at base + 0x204   */
#define FW_OFFSET_IMAGE_SIZE   0x224u       /* image_size at base + 0x224   */
#define FW_OFFSET_IMAGE_HMAC   0x228u       /* image_hmac at base + 0x228   */

#ifdef __cplusplus
}
#endif

#endif /* DFU_TYPES_H */
