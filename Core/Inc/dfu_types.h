#ifndef DFU_TYPES_H
#define DFU_TYPES_H

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
    FWU_ERR_MANIFEST        = 14,   /* blob manifest inconsistent           */
    FWU_ERR_BLOB_CRC        = 15,   /* blob CRC32 mismatch (torn transfer)  */
    FWU_ERR_AUTH_TAG        = 16,   /* AES-GCM authentication tag mismatch  */

    FWU_NO_RESULT           = 0xFF, /* last_fwu_result: no FWU attempted    */
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
        uint32_t confirmed      : 1;        /* 0 = actor confirmed healthy  */
        uint32_t boot_attempt_0 : 1;        /* 0 = 1st unconfirmed boot     */
        uint32_t boot_attempt_1 : 1;        /* 0 = 2nd                      */
        uint32_t boot_attempt_2 : 1;        /* 0 = 3rd (rollback trigger)   */
        uint32_t _reserved      : 27;
    } bits;
} sBootFlags;

#define BOOT_ATTEMPTS_MAX    3u
#define BOOT_STATUS_VERSION  3u

/*
 * Boot status is deliberately minimal: staged and golden image metadata
 * live in the cleartext manifests of the blobs themselves (self-describing
 * flash areas), so this header only carries control state.
 */
typedef struct {
    uint32_t    magic;                      /* BOOT_STATUS_MAGIC             */
    uint32_t    version;                    /* BOOT_STATUS_VERSION           */
    uint32_t    last_fwu_result;            /* eFwuRes of last BL install /
                                               rollback, FWU_NO_RESULT once
                                               none was attempted            */
    uint32_t    header_crc32;               /* CRC32(magic..last_fwu_result) */
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
 * FWU blob (.pnfw) — distribution / staged / golden storage format
 *
 * The firmware image never exists in plaintext outside the build directory
 * and internal flash.  Layout:
 *
 *   [0x00]              sFwuManifest   (64 B, cleartext, GCM AAD)
 *   [0x40]              GCM nonce      (12 B)
 *   [0x4C]              ciphertext     (image_size bytes, AES-128-GCM
 *                                       over the signed plaintext binary)
 *   [0x4C+image_size]   GCM tag        (16 B)
 *   [blob_size-4]       CRC32          (over blob[0 .. blob_size-5],
 *                                       keyless transfer-integrity check)
 *
 *   blob_size = image_size + FWU_BLOB_OVERHEAD
 * ========================================================================== */

#define FWU_BLOB_MAGIC        0x57464E50u   /* "PNFW" (little-endian)       */
#define FWU_BLOB_FORMAT       1u
#define FWU_MANIFEST_SIZE     64u
#define FWU_GCM_NONCE_SIZE    12u
#define FWU_GCM_TAG_SIZE      16u
#define FWU_BLOB_CRC_SIZE     4u
#define FWU_BLOB_OVERHEAD     (FWU_MANIFEST_SIZE + FWU_GCM_NONCE_SIZE + \
                               FWU_GCM_TAG_SIZE + FWU_BLOB_CRC_SIZE)   /* 96 */

#define FWU_BLOB_OFF_NONCE    FWU_MANIFEST_SIZE                        /* 0x40 */
#define FWU_BLOB_OFF_CT       (FWU_MANIFEST_SIZE + FWU_GCM_NONCE_SIZE) /* 0x4C */

typedef struct {
    uint32_t    magic;                      /* FWU_BLOB_MAGIC                */
    uint32_t    format;                     /* FWU_BLOB_FORMAT               */
    sFwVerArea  fw_version;                 /* cleartext copy for UI/gating;
                                               authenticated as GCM AAD      */
    uint32_t    image_size;                 /* plaintext image size (= ct len)*/
    uint32_t    blob_size;                  /* total blob size incl. CRC     */
    uint8_t     reserved[16];
} __attribute__((packed)) sFwuManifest;

_Static_assert(sizeof(sFwuManifest) == FWU_MANIFEST_SIZE,
               "sFwuManifest size mismatch");

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
