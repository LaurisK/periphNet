#include "main.h"
#include "spi.h"
#include "gpio.h"
#include "w25q128.h"
#include "boot_status.h"
#include "image_mgmt.h"
#include <stdbool.h>

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
 * FWU install (stub — copies ext flash → internal flash in the future)
 * -------------------------------------------------------------------------- */

static bool boot_install_firmware(const sBootStatus *st)
{
    (void)st;
    /*
     * Future implementation:
     *   1. Erase application sectors (2-7)
     *   2. Read from ext flash at EXT_FLASH_FWU_IMG_ADDR
     *   3. Program internal flash at APPLICATION_START_ADDR
     *   4. Verify written data
     */
    return false;   /* not implemented yet */
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
        /* Validate staged image in ext flash */
        uint8_t work_buf[sizeof(sAppInfo)];
        eFwuRes res = ImgMgmt_Validate(EXT_FLASH_FWU_IMG_ADDR, true,
                                       work_buf, sizeof(work_buf));
        if (res != FWU_OK) {
            /* Staged image invalid — clear flags, boot normally */
            boot_blink_error();
            BootStatus_ClearFlags();
            break;
        }

        /* Version compatibility check */
        sFwVerArea current_ver, staged_ver;
        if (ImgMgmt_GetVersion(APPLICATION_START_ADDR, false, &current_ver) &&
            ImgMgmt_GetVersion(EXT_FLASH_FWU_IMG_ADDR, true, &staged_ver)) {

            res = ImgMgmt_CheckVerForFwu(&current_ver, &staged_ver);
            if (res != FWU_OK) {
                boot_blink_error();
                BootStatus_ClearFlags();
                break;
            }
        }

        /* Install firmware (stub — not yet implemented) */
        sBootStatus st;
        if (BootStatus_Read(&st) == 0 && boot_install_firmware(&st)) {
            /* Success — clear flags for fresh confirmed boot */
            BootStatus_ClearFlags();
            boot_blink_led(5);
        } else {
            /* Install not implemented / failed — clear flags, boot old */
            BootStatus_ClearFlags();
            boot_blink_error();
        }
        break;
    }

    case fwu_rollback:
        /* All boot attempts exhausted — rollback (stub) */
        boot_blink_error();
        BootStatus_ClearFlags();
        break;

    case fwu_none:
    default:
        /* Check for unconfirmed boot */
        if (BootStatus_IsUnconfirmed()) {
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
