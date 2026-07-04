#include "main.h"
#include "spi.h"
#include "gpio.h"
#include "w25q128.h"
#include "boot_status.h"
#include "image_mgmt.h"
#include "version.h"
#include "aes_gcm.h"
#include "hmac_sha256.h"
#include "secrets.h"
#include <stdbool.h>
#include <string.h>

void SystemClock_Config(void);

#define APPLICATION_ADDRESS     0x08008000
#define BOOT_LED_PORT           GPIOA
#define BOOT_LED_PIN            GPIO_PIN_6

/* --------------------------------------------------------------------------
 * Jump to application — never returns
 * -------------------------------------------------------------------------- */

static void boot_jump_to_application(uint32_t app_address)
{
    typedef void (*pFunction)(void);

    uint32_t app_stack_pointer  = *(__IO uint32_t *)app_address;
    pFunction app_reset_handler = (pFunction)(*(__IO uint32_t *)(app_address + 4));

    HAL_DeInit();
    __disable_irq();

    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    for (uint32_t i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFF;
        NVIC->ICPR[i] = 0xFFFFFFFF;
    }

    __set_MSP(app_stack_pointer);
    app_reset_handler();

    while (1);
}

/* --------------------------------------------------------------------------
 * LED helpers
 * -------------------------------------------------------------------------- */

static void boot_blink_led(uint8_t count)
{
    for (uint8_t i = 0; i < count; i++) {
        HAL_GPIO_WritePin(BOOT_LED_PORT, BOOT_LED_PIN, GPIO_PIN_SET);
        HAL_Delay(150);
        HAL_GPIO_WritePin(BOOT_LED_PORT, BOOT_LED_PIN, GPIO_PIN_RESET);
        HAL_Delay(150);
    }
    HAL_Delay(300);
}

static void boot_blink_error(void)
{
    for (uint8_t i = 0; i < 10; i++) {
        HAL_GPIO_WritePin(BOOT_LED_PORT, BOOT_LED_PIN, GPIO_PIN_SET);
        HAL_Delay(50);
        HAL_GPIO_WritePin(BOOT_LED_PORT, BOOT_LED_PIN, GPIO_PIN_RESET);
        HAL_Delay(50);
    }
    HAL_Delay(300);
}

/* --------------------------------------------------------------------------
 * Internal application header helpers
 * -------------------------------------------------------------------------- */

static bool internal_app_header(sAppInfo *out)
{
    memcpy(out, (const void *)APP_INFO_HEADER_ADDR, sizeof(sAppInfo));
    return out->magic == APP_INFO_MAGIC;
}

/* Local-target ('l') builds are developer-owned: no version ordering and
 * no boot-attempt consumption / rollback for them. */
static bool internal_app_is_local(void)
{
    sAppInfo info;
    return internal_app_header(&info) &&
           info.fw_version.ver.target == (uint8_t)fwTarget_local;
}

/* --------------------------------------------------------------------------
 * FWU install — stream an encrypted .pnfw blob from ext flash into
 * internal flash.  The image is never present in plaintext anywhere but
 * internal flash.
 *
 *   0. Manifest sanity + whole-blob CRC32 (keyless, catches torn uploads)
 *   1. Pass 1: stream GCM decrypt (discarding plaintext) to verify the
 *      auth tag, compute the plaintext HMAC, and capture the decrypted
 *      sAppInfo header — nothing is written yet
 *   2. Version gate (skipped when the internal app header is invalid:
 *      a half-installed/blank device must accept any authentic image)
 *   3. Pass 2: erase app sectors, decrypt again, program word-by-word,
 *      verify each chunk by read-back
 * -------------------------------------------------------------------------- */

#define INSTALL_BUF_SIZE      256u
#define INSTALL_LED_INTERVAL  (64u * 1024u)  /* blink every 64 KB */

static eFwuRes blob_read_manifest(uint32_t base, uint32_t area_size,
                                  sFwuManifest *man)
{
    if (W25Q128_Read(base, (uint8_t *)man, sizeof(*man)) != W25Q128_OK) {
        return FWU_ERR_FLASH_READ;
    }

    if (man->magic != FWU_BLOB_MAGIC) {
        return FWU_ERR_NO_IMAGE;
    }
    if (man->format != FWU_BLOB_FORMAT) {
        return FWU_ERR_MANIFEST;
    }
    if (man->image_size < FW_OFFSET_APP_HEADER + sizeof(sAppInfo) ||
        man->image_size > APPLICATION_SIZE ||
        man->blob_size != man->image_size + FWU_BLOB_OVERHEAD ||
        man->blob_size > area_size) {
        return FWU_ERR_MANIFEST;
    }

    return FWU_OK;
}

static eFwuRes blob_check_crc(uint32_t base, uint32_t blob_size)
{
    uint8_t  buf[INSTALL_BUF_SIZE];
    uint32_t body_len = blob_size - FWU_BLOB_CRC_SIZE;
    uint32_t crc = ImgMgmt_Crc32Init();

    for (uint32_t off = 0; off < body_len; off += INSTALL_BUF_SIZE) {
        uint32_t n = body_len - off;
        if (n > INSTALL_BUF_SIZE) n = INSTALL_BUF_SIZE;

        if (W25Q128_Read(base + off, buf, n) != W25Q128_OK) {
            return FWU_ERR_FLASH_READ;
        }
        crc = ImgMgmt_Crc32Update(crc, buf, n);
    }

    uint32_t stored;
    if (W25Q128_Read(base + body_len, (uint8_t *)&stored,
                     sizeof(stored)) != W25Q128_OK) {
        return FWU_ERR_FLASH_READ;
    }

    return (ImgMgmt_Crc32Final(crc) == stored) ? FWU_OK : FWU_ERR_BLOB_CRC;
}

/* Copy the part of pt[off..off+n) overlapping [dst_off, dst_off+dst_len)
 * into dst, and (separately) zero the HMAC field region for HMAC input. */
static void capture_region(uint8_t *dst, uint32_t dst_off, uint32_t dst_len,
                           const uint8_t *pt, uint32_t off, uint32_t n)
{
    uint32_t start = (dst_off > off) ? dst_off : off;
    uint32_t end   = ((dst_off + dst_len) < (off + n)) ? (dst_off + dst_len)
                                                       : (off + n);
    if (start < end) {
        memcpy(dst + (start - dst_off), pt + (start - off), end - start);
    }
}

static void zero_hmac_field(uint8_t *pt, uint32_t off, uint32_t n)
{
    uint32_t hmac_start = FW_OFFSET_IMAGE_HMAC;
    uint32_t hmac_end   = FW_OFFSET_IMAGE_HMAC + DFU_HMAC_SIZE;
    uint32_t start = (hmac_start > off) ? hmac_start : off;
    uint32_t end   = (hmac_end < (off + n)) ? hmac_end : (off + n);
    if (start < end) {
        memset(pt + (start - off), 0, end - start);
    }
}

static eFwuRes blob_authenticate(uint32_t base, const sFwuManifest *man,
                                 const uint8_t nonce[FWU_GCM_NONCE_SIZE],
                                 sAppInfo *app_info)
{
    sAesGcmCtx     gcm;
    sHmacSha256Ctx hmac;
    uint8_t        ct[INSTALL_BUF_SIZE];
    uint8_t        pt[INSTALL_BUF_SIZE];

    aes_gcm_dec_init(&gcm, GLB_blKey, nonce,
                     (const uint8_t *)man, sizeof(*man));
    hmac_sha256_init(&hmac, GLB_hmacKey, sizeof(GLB_hmacKey));

    for (uint32_t off = 0; off < man->image_size; off += INSTALL_BUF_SIZE) {
        uint32_t n = man->image_size - off;
        if (n > INSTALL_BUF_SIZE) n = INSTALL_BUF_SIZE;

        if (W25Q128_Read(base + FWU_BLOB_OFF_CT + off, ct, n) != W25Q128_OK) {
            return FWU_ERR_FLASH_READ;
        }

        aes_gcm_dec_update(&gcm, ct, pt, n);

        capture_region((uint8_t *)app_info, FW_OFFSET_APP_HEADER,
                       sizeof(sAppInfo), pt, off, n);

        /* HMAC is computed over the plaintext with its HMAC field zeroed */
        zero_hmac_field(pt, off, n);
        hmac_sha256_update(&hmac, pt, n);
    }

    uint8_t tag[FWU_GCM_TAG_SIZE];
    if (W25Q128_Read(base + FWU_BLOB_OFF_CT + man->image_size,
                     tag, sizeof(tag)) != W25Q128_OK) {
        return FWU_ERR_FLASH_READ;
    }

    if (!aes_gcm_dec_final(&gcm, tag)) {
        return FWU_ERR_AUTH_TAG;
    }

    /* Plaintext is authentic from here on */
    if (app_info->magic != APP_INFO_MAGIC) {
        return FWU_ERR_WRONG_MAGIC;
    }
    if (app_info->image_size != man->image_size) {
        return FWU_ERR_MANIFEST;
    }

    uint8_t mac[HMAC_SHA256_SIZE];
    hmac_sha256_final(&hmac, mac);
    uint8_t diff = 0;
    for (uint32_t i = 0; i < DFU_HMAC_SIZE; i++) {
        diff |= mac[i] ^ app_info->image_hmac[i];
    }
    if (diff != 0) {
        return FWU_ERR_IMAGE_HMAC;
    }

    return FWU_OK;
}

static eFwuRes blob_program(uint32_t base, const sFwuManifest *man,
                            const uint8_t nonce[FWU_GCM_NONCE_SIZE])
{
    sAesGcmCtx gcm;
    uint8_t    ct[INSTALL_BUF_SIZE];
    uint8_t    pt[INSTALL_BUF_SIZE];

    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase_init;
    erase_init.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase_init.Sector       = FLASH_SECTOR_2;
    erase_init.NbSectors    = 6;
    erase_init.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    uint32_t sector_error;
    if (HAL_FLASHEx_Erase(&erase_init, &sector_error) != HAL_OK) {
        HAL_FLASH_Lock();
        return FWU_ERR_INSTALL;
    }

    boot_blink_led(1);  /* erase complete indicator */

    aes_gcm_dec_init(&gcm, GLB_blKey, nonce,
                     (const uint8_t *)man, sizeof(*man));

    uint32_t next_led = INSTALL_LED_INTERVAL;

    for (uint32_t off = 0; off < man->image_size; off += INSTALL_BUF_SIZE) {
        uint32_t n = man->image_size - off;
        if (n > INSTALL_BUF_SIZE) n = INSTALL_BUF_SIZE;

        if (W25Q128_Read(base + FWU_BLOB_OFF_CT + off, ct, n) != W25Q128_OK) {
            HAL_FLASH_Lock();
            return FWU_ERR_FLASH_READ;
        }

        aes_gcm_dec_update(&gcm, ct, pt, n);

        /* Program word-by-word (4 bytes at a time) */
        for (uint32_t i = 0; i < n; i += 4) {
            uint32_t word = 0xFFFFFFFFu;
            uint32_t bytes_left = n - i;
            memcpy(&word, &pt[i], (bytes_left >= 4) ? 4 : bytes_left);

            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                                  APPLICATION_START_ADDR + off + i,
                                  (uint64_t)word) != HAL_OK) {
                HAL_FLASH_Lock();
                return FWU_ERR_INSTALL;
            }
        }

        /* Verify chunk by read-back */
        if (memcmp(pt, (const void *)(APPLICATION_START_ADDR + off), n) != 0) {
            HAL_FLASH_Lock();
            return FWU_ERR_INSTALL;
        }

        if (off >= next_led) {
            boot_blink_led(1);
            next_led += INSTALL_LED_INTERVAL;
        }
    }

    HAL_FLASH_Lock();
    return FWU_OK;
}

/**
 * Full blob install from ext flash area at @p base.
 * @param version_gate  Apply version compatibility check against the
 *                      current internal app (false for rollback — golden
 *                      is by definition older than the failed image).
 */
static eFwuRes boot_install_blob(uint32_t base, uint32_t area_size,
                                 bool version_gate)
{
    sFwuManifest man;
    eFwuRes res = blob_read_manifest(base, area_size, &man);
    if (res != FWU_OK) {
        return res;
    }

    res = blob_check_crc(base, man.blob_size);
    if (res != FWU_OK) {
        return res;
    }

    uint8_t nonce[FWU_GCM_NONCE_SIZE];
    if (W25Q128_Read(base + FWU_BLOB_OFF_NONCE, nonce,
                     sizeof(nonce)) != W25Q128_OK) {
        return FWU_ERR_FLASH_READ;
    }

    /* Pass 1: authenticate + capture decrypted app header */
    sAppInfo staged_info;
    res = blob_authenticate(base, &man, nonce, &staged_info);
    if (res != FWU_OK) {
        return res;
    }

    if (version_gate) {
        sAppInfo current;
        if (internal_app_header(&current)) {
            res = ver_checkCompatibility(&current.fw_version.ver,
                                         &staged_info.fw_version.ver);
            if (res != FWU_OK) {
                return res;
            }
        }
        /* Invalid internal header → no gate: a blank or half-installed
         * device must accept any authentic image. */
    }

    /* Pass 2: erase + decrypt + program + verify */
    return blob_program(base, &man, nonce);
}

/* --------------------------------------------------------------------------
 * main — bootloader entry point
 *
 * Following Zhaga pattern:
 *   1. Init hardware
 *   2. Ensure boot status is valid
 *   3. Check FWU action (install / rollback / none)
 *   4. Handle unconfirmed boots (consume attempt)
 *   5. Validate internal application
 *   6. Jump to application
 * -------------------------------------------------------------------------- */

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_SPI2_Init();

    HAL_Delay(100);
    boot_blink_led(3);

    /* Initialize external flash */
    if (W25Q128_Init() != W25Q128_OK) {
        boot_blink_error();
        /* Can't access ext flash — skip FWU logic, try to boot */
        goto validate_and_jump;
    }

    /* Ensure boot status sector has a valid header */
    BootStatus_EnsureValid();

    /* Determine FWU action from boot flags */
    eFwuAction action = BootStatus_GetFwuAction();

    switch (action) {
    case fwu_install: {
        eFwuRes res = boot_install_blob(EXT_FLASH_FWU_IMG_ADDR,
                                        EXT_FLASH_FWU_IMG_SIZE, true);
        if (res == FWU_OK) {
            /* New image boots unconfirmed: the outside actor must confirm
             * within BOOT_ATTEMPTS_MAX boots or the BL rolls back.  This
             * boot counts as the first attempt (local builds exempt). */
            BootStatus_FinishFwu(FWU_OK, false);
            if (!internal_app_is_local()) {
                BootStatus_ConsumeBootAttempt();
            }
            boot_blink_led(5);
        } else {
            /* Install rejected/failed before touching internal flash (or
             * program verify failed) — keep the old app confirmed, record
             * the result so the app can report why nothing changed. */
            BootStatus_FinishFwu(res, true);
            boot_blink_error();
        }
        break;
    }

    case fwu_rollback: {
        eFwuRes res = boot_install_blob(EXT_FLASH_GOLDEN_IMG_ADDR,
                                        EXT_FLASH_GOLDEN_IMG_SIZE, false);
        if (res == FWU_OK) {
            /* Invalidate the staged blob that failed to confirm so it
             * cannot be re-installed by accident. */
            W25Q128_EraseSector(EXT_FLASH_FWU_IMG_ADDR);
            /* Golden is the last confirmed image — pre-confirmed. */
            BootStatus_FinishFwu(FWU_ROLLBACK, true);
            boot_blink_led(5);
        } else {
            /* No usable golden image — give up on rollback and keep the
             * current app running rather than looping. */
            BootStatus_FinishFwu(res, true);
            boot_blink_error();
        }
        break;
    }

    case fwu_none:
    default:
        /* Unconfirmed boot consumes one attempt (local builds exempt) */
        if (BootStatus_IsUnconfirmed() && !internal_app_is_local()) {
            BootStatus_ConsumeBootAttempt();
        }
        break;
    }

validate_and_jump:
    /* Final gate: validate application in internal flash */
    {
        uint8_t work_buf[sizeof(sAppInfo)];
        eFwuRes res = ImgMgmt_Validate(APPLICATION_START_ADDR, false,
                                       work_buf, sizeof(work_buf));
        if (res == FWU_OK) {
            boot_blink_led(2);
            boot_jump_to_application(APPLICATION_ADDRESS);
        } else {
            /* No valid application — halt with error blink */
            for (;;) {
                boot_blink_error();
                HAL_Delay(1000);
            }
        }
    }

    while (1) {
        HAL_GPIO_TogglePin(BOOT_LED_PORT, BOOT_LED_PIN);
        HAL_Delay(100);
    }
}

/* --------------------------------------------------------------------------
 * System clock — 168 MHz from 25 MHz HSE
 * -------------------------------------------------------------------------- */

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState       = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState   = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM       = 25;
    RCC_OscInitStruct.PLL.PLLN       = 336;
    RCC_OscInitStruct.PLL.PLLP       = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ       = 7;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                     | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK) {
        Error_Handler();
    }
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) {
    }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
}
#endif
