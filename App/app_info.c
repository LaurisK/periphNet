#include "bl_app_contract.h"

#define APP_FW_DEVICE_TYPE  fwDev_periphnet
#define APP_FW_TARGET       fwTarget_local
#define APP_FW_MAJOR        1
#define APP_FW_MINOR        0
#define APP_FW_PATCH        5
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
