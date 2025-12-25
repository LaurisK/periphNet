#include "bl_app_contract.h"

#define APP_VERSION_MAJOR       1
#define APP_VERSION_MINOR       0
#define APP_VERSION_PATCH       0
#define APP_BUILD_TIME          0

extern uint32_t _stext;
extern uint32_t _etext;

__attribute__((section(".app_header")))
__attribute__((used))
const sAppInfo application_info = {
    .magic = APP_INFO_MAGIC,
    .version = (APP_VERSION_MAJOR << 16) | (APP_VERSION_MINOR << 8) | APP_VERSION_PATCH,
    .build_time = APP_BUILD_TIME,
    .image_size = 0,
    .image_crc32 = 0,
    .features = APP_FEATURE_ETHERNET |
                APP_FEATURE_OTA |
                APP_FEATURE_TRACING,
    .min_bl_version = 1,
    .reset_handler_addr = APPLICATION_START_ADDR + 4,
    .vector_table_addr = APPLICATION_START_ADDR,
    .reserved = {0}
};
