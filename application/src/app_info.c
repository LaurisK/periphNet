/**
 ******************************************************************************
 * @file    app_info.c
 * @brief   Application Info Header
 * @date    Dec 27, 2024
 ******************************************************************************
 * @attention
 *
 * Application metadata exposed to bootloader at fixed address 0x08008200
 * This is placed right after the vector table (0x08008000 + 512 bytes)
 *
 ******************************************************************************
 */

#include "bl_app_contract.h"

/* Application version */
#define APP_VERSION_MAJOR       1
#define APP_VERSION_MINOR       0
#define APP_VERSION_PATCH       0

/* Build timestamp (will be set by build system in future) */
#define APP_BUILD_TIME          0   /* TODO: Set via build script */

/* External symbols from linker */
extern uint32_t _stext;     /* Start of .text section */
extern uint32_t _etext;     /* End of .text section */

/**
 * @brief Application Info Header
 * @note Placed at fixed address 0x08008200 via linker script
 * @note This allows bootloader to validate application before jumping
 */
__attribute__((section(".app_header")))
__attribute__((used))
const sAppInfo application_info = {
    .magic = APP_INFO_MAGIC,

    /* Version encoding: (major << 16) | (minor << 8) | patch */
    .version = (APP_VERSION_MAJOR << 16) | (APP_VERSION_MINOR << 8) | APP_VERSION_PATCH,

    /* Build time (unix timestamp) */
    .build_time = APP_BUILD_TIME,

    /* Image size and CRC (STUB - will be calculated by build script) */
    .image_size = 0,            /* TODO: Set by post-build script */
    .image_crc32 = 0,           /* TODO: Calculate actual CRC */

    /* Feature flags - what this application supports */
    .features = APP_FEATURE_ETHERNET |
                APP_FEATURE_OTA |
                APP_FEATURE_TRACING,

    /* Minimum bootloader version required */
    .min_bl_version = 1,  /* Requires bootloader v1.x.x */

    /* Entry points (for debugging/info) */
    .reset_handler_addr = APPLICATION_START_ADDR + 4,  /* Reset_Handler offset */
    .vector_table_addr = APPLICATION_START_ADDR,

    /* Reserved for future use */
    .reserved = {0}
};
