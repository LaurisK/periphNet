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
 * 1. Initialize minimal hardware (clocks, GPIO)
 * 2. Visual indication (blink LED 3 times)
 * 3. Check for update request (future - Phase 4)
 * 4. Jump to application at 0x08008000
 *
 ******************************************************************************
 */

#include "main.h"

/* Application start address */
#define APPLICATION_ADDRESS     0x08008000

/* LED GPIO (same as application - gpio_led1_Pin on GPIOA) */
#define BOOT_LED_PORT           GPIOA
#define BOOT_LED_PIN            GPIO_PIN_6

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
 * @brief  Bootloader main function
 * @retval int (never returns)
 */
int main(void)
{
    /* Reset of all peripherals, Initializes the Flash interface and the Systick */
    HAL_Init();

    /* Configure the system clock */
    SystemClock_Config();

    /* Initialize GPIO for LED */
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = BOOT_LED_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(BOOT_LED_PORT, &GPIO_InitStruct);

    /* Visual indication: Bootloader is running */
    /* Blink LED 3 times = "I'm the bootloader" */
    boot_blink_led(3);

    /* TODO Phase 4: Check external flash for update request */
    /*
    if (update_requested) {
        // Verify and install firmware
        // If successful, clear flag and jump to new app
        // If failed, try golden image or stay in bootloader
    }
    */

    /* No update request - jump to application */
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
