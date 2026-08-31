#include "bl_app_contract.h"

#define APP_FW_DEVICE_TYPE  fwDev_periphnet
/* 'd' (not 'l'): local-target builds are exempt from boot-attempt counting,
 * which disables the 3-boot rollback to the golden image.  That is fine on the
 * bench with a J-Link attached, but for a board reached only over the network
 * it removes the only recovery path a bad image has.  Dev target restores it.
 * Consequence: every OTA now needs a version bump (the gate requires strictly
 * newer) and MUST be confirmed via POST /api/fwu/confirm within 3 boots. */
#define APP_FW_TARGET       fwTarget_dev
#define APP_FW_MAJOR        1
#define APP_FW_MINOR        1
#define APP_FW_PATCH        38
#define APP_FW_HW_ID        0

__attribute__((section(".app_header")))
__attribute__((used))
const sAppInfo application_info = {
    .magic = APP_INFO_MAGIC,
    .fw_version = {
        .ver = {
            .deviceType = APP_FW_DEVICE_TYPE,
            .target     = APP_FW_TARGET,
            .major      = APP_FW_MAJOR,
            .minor      = APP_FW_MINOR,
            .patch      = APP_FW_PATCH,
            .hwId       = APP_FW_HW_ID,
        },
        .reserved = {0},
    },
    .image_size     = 0xFFFFFFFFu,  /* patched post-build (placeholder) */
    .image_hmac     = {[0 ... 31] = 0xFFu},  /* HMAC stub (unpatched)  */
    .features       = APP_FEATURE_ETHERNET
                    | APP_FEATURE_OTA
                    | APP_FEATURE_TRACING,
    .min_bl_version = 2,
    .reserved       = {0},
};
