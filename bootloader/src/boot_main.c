/**
 ******************************************************************************
 * @file    boot_main.c
 * @brief   PeriphNet Bootloader - Minimal implementation
 * @author  PeriphNet Project
 * @date    2024-12-26
 ******************************************************************************
 * @attention
 *
 * Bootloader functions:
 * 1. Initialize minimal hardware (clocks, GPIO, SPI)
 * 2. Test external flash (write test pattern)
 * 3. Visual indication (blink LED)
 * 4. Check for update request (future - Phase 4)
 * 5. Jump to application at 0x08008000
 *
 ******************************************************************************
 */

#include "main.h"
#include "spi.h"
#include "gpio.h"
#include "w25q128.h"
#include <stdbool.h>

/* Application start address */
#define APPLICATION_ADDRESS     0x08008000

/* LED GPIO (same as application - gpio_led1_Pin on GPIOA) */
#define BOOT_LED_PORT           GPIOA
#define BOOT_LED_PIN            GPIO_PIN_6

/* External Flash Test */
#define FLASH_TEST_ADDR         EXT_FLASH_FWU_STATUS_ADDR
#define FLASH_TEST_PATTERN      "BOOTLOADER_WAS_HERE_2024"
#define FLASH_TEST_SIZE         24

/**
 * @brief  Jump to application
 * @param  app_address: Address of application (0x08008000)
 * @retval None (never returns)
 */
static void boot_jump_to_application(uint32_t app_address)
{
    typedef void (*pFunction)(void);
    pFunction app_reset_handler;
    uint32_t app_stack_pointer;

    /* Get application stack pointer (first entry in vector table) */
    app_stack_pointer = *(__IO uint32_t*)app_address;

    /* Get application reset handler (second entry in vector table) */
    app_reset_handler = (pFunction) (*(__IO uint32_t*)(app_address + 4));

    /* Deinitialize HAL */
    HAL_DeInit();

    /* Disable all interrupts */
    __disable_irq();

    /* Disable SysTick */
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;

    /* Clear all pending interrupts */
    for (uint32_t i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFF;
        NVIC->ICPR[i] = 0xFFFFFFFF;
    }

    /* Set application stack pointer */
    __set_MSP(app_stack_pointer);

    /* Jump to application */
    app_reset_handler();

    /* Should never reach here */
    while (1);
}

/**
 * @brief  Blink LED to indicate bootloader is running
 * @param  count: Number of blinks
 * @retval None
 */
static void boot_blink_led(uint8_t count)
{
    for (uint8_t i = 0; i < count; i++) {
        HAL_GPIO_WritePin(BOOT_LED_PORT, BOOT_LED_PIN, GPIO_PIN_SET);
        HAL_Delay(150);
        HAL_GPIO_WritePin(BOOT_LED_PORT, BOOT_LED_PIN, GPIO_PIN_RESET);
        HAL_Delay(150);
    }
    /* Pause after blinking */
    HAL_Delay(300);
}

/**
 * @brief  Test external flash (Phase 2)
 * @retval true if success, false if failure
 */
static bool boot_test_external_flash(void)
{
    W25Q128_ID_t flash_id;
    const uint8_t pattern[] = FLASH_TEST_PATTERN;
    uint8_t uid[12];  /* STM32F407 has 96-bit unique ID = 12 bytes */
    
    /* Initialize W25Q128 */
    if (W25Q128_Init() != W25Q128_OK) {
        return false;  /* Flash init failed */
    }

    /* Read flash ID (JEDEC info) */
    if (W25Q128_ReadID(&flash_id) != W25Q128_OK) {
        return false;
    }

    /* Verify it's the correct chip */
    if (flash_id.manufacturer_id != 0xEF || flash_id.capacity != 0x18) {
        return false;  /* Wrong chip or communication error */
    }

    /* Read STM32F407 Unique ID */
    uid[0] = *(uint8_t*)(0x1FFF7A10);
    uid[1] = *(uint8_t*)(0x1FFF7A10 + 1);
    uid[2] = *(uint8_t*)(0x1FFF7A10 + 2);
    uid[3] = *(uint8_t*)(0x1FFF7A10 + 3);
    uid[4] = *(uint8_t*)(0x1FFF7A10 + 4);
    uid[5] = *(uint8_t*)(0x1FFF7A10 + 5);
    uid[6] = *(uint8_t*)(0x1FFF7A10 + 6);
    uid[7] = *(uint8_t*)(0x1FFF7A10 + 7);
    uid[8] = *(uint8_t*)(0x1FFF7A10 + 8);
    uid[9] = *(uint8_t*)(0x1FFF7A10 + 9);
    uid[10] = *(uint8_t*)(0x1FFF7A10 + 10);
    uid[11] = *(uint8_t*)(0x1FFF7A10 + 11);

    /* Erase test sector */
    if (W25Q128_EraseSector(FLASH_TEST_ADDR) != W25Q128_OK) {
        return false;
    }

    /* Write UID + test pattern (4 bytes pattern + 12 bytes UID) */
    if (W25Q128_WritePage(FLASH_TEST_ADDR, pattern, 4) != W25Q128_OK) {
        return false;
    }
    if (W25Q128_WritePage(FLASH_TEST_ADDR + 4, uid, 12) != W25Q128_OK) {
        return false;
    }

    return true;
}

/**
 * @brief  Bootloader main function
 * @retval int (never returns)
 */
int main(void)
{
    bool flash_test_ok = false;

    /* Reset of all peripherals, Initializes the Flash interface and the Systick */
    HAL_Init();

    /* Configure the system clock */
    SystemClock_Config();

    /* Initialize peripherals */
    MX_GPIO_Init();
    MX_SPI2_Init();

    /* Give flash time to power up */
    HAL_Delay(100);

/* Visual indication: Bootloader is running */
    /* Blink LED 3 times = "I'm bootloader" */
    boot_blink_led(3);
    
    /* Extra delay to make it clear bootloader ran */
    HAL_Delay(2000);

    /* Phase 2: Test external flash communication */
    flash_test_ok = boot_test_external_flash();

    /* Indicate flash test result */
    if (flash_test_ok) {
        /* 2 slow blinks = Flash test OK */
        boot_blink_led(2);
    } else {
        /* 5 fast blinks = Flash test FAILED */
        for (uint8_t i = 0; i < 5; i++) {
            HAL_GPIO_WritePin(BOOT_LED_PORT, BOOT_LED_PIN, GPIO_PIN_SET);
            HAL_Delay(50);
            HAL_GPIO_WritePin(BOOT_LED_PORT, BOOT_LED_PIN, GPIO_PIN_RESET);
            HAL_Delay(50);
        }
        HAL_Delay(300);
    }

    /* TODO Phase 4: Check external flash for update request */
    /*
    if (update_requested) {
        // Verify and install firmware
        // If successful, clear flag and jump to new app
        // If failed, try golden image or stay in bootloader
    }
    */

    /* Jump to application */
    boot_jump_to_application(APPLICATION_ADDRESS);

    /* Should never reach here */
    while (1) {
        /* If we get here, application failed to start */
        HAL_GPIO_TogglePin(BOOT_LED_PORT, BOOT_LED_PIN);
        HAL_Delay(100);  /* Fast blink = error */
    }
}

/**
 * @brief  System Clock Configuration
 * @note   Same as application - 168MHz from 25MHz HSE
 * @retval None
 */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    /* Configure the main internal regulator output voltage */
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    /* Initializes the RCC Oscillators */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM = 25;
    RCC_OscInitStruct.PLL.PLLN = 336;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ = 7;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
        Error_Handler();
    }

    /* Initializes the CPU, AHB and APB buses clocks */
    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                                |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK) {
        Error_Handler();
    }
}

/**
 * @brief  This function is executed in case of error occurrence
 * @retval None
 */
void Error_Handler(void)
{
    __disable_irq();
    while (1) {
        /* Error: stay here */
    }
}

#ifdef  USE_FULL_ASSERT
/**
 * @brief  Reports the name of the source file and the line number
 *         where the assert_param error has occurred
 * @param  file: pointer to the source file name
 * @param  line: assert_param error line source number
 * @retval None
 */
void assert_failed(uint8_t *file, uint32_t line)
{
    /* User can add implementation to report the file name and line number */
}
#endif /* USE_FULL_ASSERT */
