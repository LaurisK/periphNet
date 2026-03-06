#include "main.h"
#include "spi.h"
#include "gpio.h"
#include "w25q128.h"
#include "update_manager.h"
#include <stdbool.h>

#define APPLICATION_ADDRESS     0x08008000
#define BOOT_LED_PORT           GPIOA
#define BOOT_LED_PIN            GPIO_PIN_6
#define FLASH_TEST_ADDR         EXT_FLASH_FWU_STATUS_ADDR
#define FLASH_TEST_PATTERN      "BOOTLOADER_WAS_HERE_2024"
#define FLASH_TEST_SIZE         24

/**
 * @brief Jump to the application at app_address; never returns.
 * @param app_address Start address of the application vector table.
 */
static void boot_jump_to_application(uint32_t app_address)
{
    typedef void (*pFunction)(void);
    pFunction app_reset_handler;
    uint32_t app_stack_pointer;

    app_stack_pointer = *(__IO uint32_t*)app_address;
    app_reset_handler = (pFunction) (*(__IO uint32_t*)(app_address + 4));

    HAL_DeInit();
    __disable_irq();

    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;

    for (uint32_t i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFF;
        NVIC->ICPR[i] = 0xFFFFFFFF;
    }

    __set_MSP(app_stack_pointer);
    app_reset_handler();

    while (1);
}

/**
 * @brief Blink the boot LED a given number of times.
 * @param count Number of blink cycles.
 */
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

/**
 * @brief Initialize the external flash, read the JEDEC ID, and perform a write/read test.
 * @return true if all checks pass, false on any failure.
 */
static bool boot_test_external_flash(void)
{
    W25Q128_ID_t flash_id;
    const uint8_t pattern[] = FLASH_TEST_PATTERN;
    uint8_t uid[12];

    if (W25Q128_Init() != W25Q128_OK) {
        return false;
    }

    if (W25Q128_ReadID(&flash_id) != W25Q128_OK) {
        return false;
    }

    if (flash_id.manufacturer_id != 0xEF) {
        return false;
    }

    if (flash_id.capacity != 0x17 && flash_id.capacity != 0x18) {
        return false;
    }

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

    if (W25Q128_EraseSector(FLASH_TEST_ADDR) != W25Q128_OK) {
        return false;
    }

    if (W25Q128_WritePage(FLASH_TEST_ADDR, pattern, 4) != W25Q128_OK) {
        return false;
    }
    if (W25Q128_WritePage(FLASH_TEST_ADDR + 4, uid, 12) != W25Q128_OK) {
        return false;
    }

    return true;
}

/**
 * @brief Verify a pending firmware image against the update status block (stub).
 * @param status Pointer to the update status read from external flash.
 * @return true if basic field validation passes, false otherwise.
 */
static bool boot_verify_firmware(const sUpdateStatus *status)
{
    if (status->magic != UPDATE_STATUS_MAGIC) {
        return false;
    }

    if (status->image_size == 0 || status->image_size > (480 * 1024)) {
        return false;
    }

    return true;
}

/**
 * @brief Install firmware from external flash into internal flash (stub).
 * @param status Pointer to the update status describing the image location and size.
 * @return true always (installation not yet implemented).
 */
static bool boot_install_firmware(const sUpdateStatus *status)
{
    (void)status;
    return true;
}

/**
 * @brief Bootloader entry point; initializes hardware, tests flash, then jumps to application.
 * @return Never returns.
 */
int main(void)
{
    bool flash_test_ok = false;

    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_SPI2_Init();

    HAL_Delay(100);

    boot_blink_led(3);
    HAL_Delay(2000);

    flash_test_ok = boot_test_external_flash();

    if (flash_test_ok) {
        boot_blink_led(2);
    } else {
        for (uint8_t i = 0; i < 5; i++) {
            HAL_GPIO_WritePin(BOOT_LED_PORT, BOOT_LED_PIN, GPIO_PIN_SET);
            HAL_Delay(50);
            HAL_GPIO_WritePin(BOOT_LED_PORT, BOOT_LED_PIN, GPIO_PIN_RESET);
            HAL_Delay(50);
        }
        HAL_Delay(300);
    }

    boot_jump_to_application(APPLICATION_ADDRESS);

    while (1) {
        HAL_GPIO_TogglePin(BOOT_LED_PORT, BOOT_LED_PIN);
        HAL_Delay(100);
    }
}

/**
 * @brief Configure the system clock to 168 MHz from a 25 MHz HSE crystal.
 */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

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
 * @brief Error handler; disables interrupts and halts in an infinite loop.
 */
void Error_Handler(void)
{
    __disable_irq();
    while (1) {
    }
}

#ifdef  USE_FULL_ASSERT
/**
 * @brief Report the file name and line number where an assert_param failure occurred.
 * @param file Pointer to the source file name string.
 * @param line Line number of the failed assertion.
 */
void assert_failed(uint8_t *file, uint32_t line)
{
}
#endif /* USE_FULL_ASSERT */
