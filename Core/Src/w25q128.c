/**
  ******************************************************************************
  * @file           : w25q128.c
  * @brief          : W25Q128 16MB SPI Flash Driver Implementation
  ******************************************************************************
  */

#include "w25q128.h"
#include "spi.h"

/* Private Macros */
#define CS_LOW()    HAL_GPIO_WritePin(W25Q128_CS_GPIO_PORT, W25Q128_CS_GPIO_PIN, GPIO_PIN_RESET)
#define CS_HIGH()   HAL_GPIO_WritePin(W25Q128_CS_GPIO_PORT, W25Q128_CS_GPIO_PIN, GPIO_PIN_SET)

/* Private Function Prototypes */
static W25Q128_Status_t W25Q128_WriteEnable(void);
static W25Q128_Status_t W25Q128_WriteDisable(void);
static uint8_t W25Q128_ReadStatusReg1(void);

/**
 * @brief Initialize W25Q128 flash
 */
W25Q128_Status_t W25Q128_Init(void)
{
    /* CS pin should already be initialized by MX_GPIO_Init() */
    CS_HIGH();

    /* Delay after power-up */
    HAL_Delay(10);

    /* Wake up from potential power-down state */
    W25Q128_WakeUp();

    /* Additional delay after wake-up */
    HAL_Delay(50);

    /* Try to read ID to verify communication */
    W25Q128_ID_t id;
    if (W25Q128_ReadID(&id) != W25Q128_OK) {
        return W25Q128_ERROR;
    }

    /* Verify it's a Winbond W25Qxx (accept W25Q64 or W25Q128) */
    if (id.manufacturer_id != 0xEF || id.memory_type != 0x40) {
        return W25Q128_ERROR;
    }

    /* Accept both W25Q64 (0x17 = 8MB) and W25Q128 (0x18 = 16MB) */
    if (id.capacity != 0x17 && id.capacity != 0x18) {
        return W25Q128_ERROR;
    }

    return W25Q128_OK;
}

/**
 * @brief Read JEDEC ID
 */
W25Q128_Status_t W25Q128_ReadID(W25Q128_ID_t *id)
{
    uint8_t cmd = W25Q128_CMD_READ_JEDEC_ID;
    uint8_t data[3];

    CS_LOW();

    if (HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, &cmd, 1, W25Q128_TIMEOUT_MS) != HAL_OK) {
        CS_HIGH();
        return W25Q128_ERROR;
    }

    if (HAL_SPI_Receive(&W25Q128_SPI_HANDLE, data, 3, W25Q128_TIMEOUT_MS) != HAL_OK) {
        CS_HIGH();
        return W25Q128_ERROR;
    }

    CS_HIGH();

    id->manufacturer_id = data[0];
    id->memory_type = data[1];
    id->capacity = data[2];

    return W25Q128_OK;
}

/**
 * @brief Read data from flash
 */
W25Q128_Status_t W25Q128_Read(uint32_t addr, uint8_t *buffer, uint32_t len)
{
    uint8_t cmd[4];

    if (addr + len > W25Q128_FLASH_SIZE) {
        return W25Q128_ERROR;
    }

    /* Wait until flash is ready */
    if (W25Q128_WaitReady(W25Q128_TIMEOUT_MS) != W25Q128_OK) {
        return W25Q128_TIMEOUT;
    }

    /* Prepare command: READ + 24-bit address */
    cmd[0] = W25Q128_CMD_READ_DATA;
    cmd[1] = (addr >> 16) & 0xFF;
    cmd[2] = (addr >> 8) & 0xFF;
    cmd[3] = addr & 0xFF;

    CS_LOW();

    if (HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, cmd, 4, W25Q128_TIMEOUT_MS) != HAL_OK) {
        CS_HIGH();
        return W25Q128_ERROR;
    }

    if (HAL_SPI_Receive(&W25Q128_SPI_HANDLE, buffer, len, W25Q128_TIMEOUT_MS) != HAL_OK) {
        CS_HIGH();
        return W25Q128_ERROR;
    }

    CS_HIGH();

    return W25Q128_OK;
}

/**
 * @brief Write page (max 256 bytes)
 */
W25Q128_Status_t W25Q128_WritePage(uint32_t addr, const uint8_t *buffer, uint32_t len)
{
    uint8_t cmd[4];

    if (addr + len > W25Q128_FLASH_SIZE || len > W25Q128_PAGE_SIZE) {
        return W25Q128_ERROR;
    }

    /* Wait until flash is ready */
    if (W25Q128_WaitReady(W25Q128_TIMEOUT_MS) != W25Q128_OK) {
        return W25Q128_TIMEOUT;
    }

    /* Enable write */
    if (W25Q128_WriteEnable() != W25Q128_OK) {
        return W25Q128_ERROR;
    }

    /* Prepare command: PAGE_PROGRAM + 24-bit address */
    cmd[0] = W25Q128_CMD_PAGE_PROGRAM;
    cmd[1] = (addr >> 16) & 0xFF;
    cmd[2] = (addr >> 8) & 0xFF;
    cmd[3] = addr & 0xFF;

    CS_LOW();

    if (HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, cmd, 4, W25Q128_TIMEOUT_MS) != HAL_OK) {
        CS_HIGH();
        return W25Q128_ERROR;
    }

    if (HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, (uint8_t*)buffer, len, W25Q128_TIMEOUT_MS) != HAL_OK) {
        CS_HIGH();
        return W25Q128_ERROR;
    }

    CS_HIGH();

    /* Wait for write to complete */
    if (W25Q128_WaitReady(W25Q128_TIMEOUT_MS) != W25Q128_OK) {
        return W25Q128_TIMEOUT;
    }

    return W25Q128_OK;
}

/**
 * @brief Erase 4KB sector
 */
W25Q128_Status_t W25Q128_EraseSector(uint32_t addr)
{
    uint8_t cmd[4];

    if (addr >= W25Q128_FLASH_SIZE) {
        return W25Q128_ERROR;
    }

    /* Wait until flash is ready */
    if (W25Q128_WaitReady(W25Q128_ERASE_TIMEOUT_MS) != W25Q128_OK) {
        return W25Q128_TIMEOUT;
    }

    /* Enable write */
    if (W25Q128_WriteEnable() != W25Q128_OK) {
        return W25Q128_ERROR;
    }

    /* Prepare command: SECTOR_ERASE + 24-bit address */
    cmd[0] = W25Q128_CMD_SECTOR_ERASE;
    cmd[1] = (addr >> 16) & 0xFF;
    cmd[2] = (addr >> 8) & 0xFF;
    cmd[3] = addr & 0xFF;

    CS_LOW();

    if (HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, cmd, 4, W25Q128_TIMEOUT_MS) != HAL_OK) {
        CS_HIGH();
        return W25Q128_ERROR;
    }

    CS_HIGH();

    /* Wait for erase to complete (can take up to 400ms) */
    if (W25Q128_WaitReady(W25Q128_ERASE_TIMEOUT_MS) != W25Q128_OK) {
        return W25Q128_TIMEOUT;
    }

    return W25Q128_OK;
}

/**
 * @brief Erase 32KB block
 */
W25Q128_Status_t W25Q128_EraseBlock32K(uint32_t addr)
{
    uint8_t cmd[4];

    if (addr >= W25Q128_FLASH_SIZE) {
        return W25Q128_ERROR;
    }

    if (W25Q128_WaitReady(W25Q128_ERASE_TIMEOUT_MS) != W25Q128_OK) {
        return W25Q128_TIMEOUT;
    }

    if (W25Q128_WriteEnable() != W25Q128_OK) {
        return W25Q128_ERROR;
    }

    cmd[0] = W25Q128_CMD_BLOCK_ERASE_32K;
    cmd[1] = (addr >> 16) & 0xFF;
    cmd[2] = (addr >> 8) & 0xFF;
    cmd[3] = addr & 0xFF;

    CS_LOW();
    HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, cmd, 4, W25Q128_TIMEOUT_MS);
    CS_HIGH();

    return W25Q128_WaitReady(W25Q128_ERASE_TIMEOUT_MS);
}

/**
 * @brief Erase 64KB block
 */
W25Q128_Status_t W25Q128_EraseBlock64K(uint32_t addr)
{
    uint8_t cmd[4];

    if (addr >= W25Q128_FLASH_SIZE) {
        return W25Q128_ERROR;
    }

    if (W25Q128_WaitReady(W25Q128_ERASE_TIMEOUT_MS) != W25Q128_OK) {
        return W25Q128_TIMEOUT;
    }

    if (W25Q128_WriteEnable() != W25Q128_OK) {
        return W25Q128_ERROR;
    }

    cmd[0] = W25Q128_CMD_BLOCK_ERASE_64K;
    cmd[1] = (addr >> 16) & 0xFF;
    cmd[2] = (addr >> 8) & 0xFF;
    cmd[3] = addr & 0xFF;

    CS_LOW();
    HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, cmd, 4, W25Q128_TIMEOUT_MS);
    CS_HIGH();

    return W25Q128_WaitReady(W25Q128_ERASE_TIMEOUT_MS);
}

/**
 * @brief Erase entire chip
 */
W25Q128_Status_t W25Q128_EraseChip(void)
{
    uint8_t cmd = W25Q128_CMD_CHIP_ERASE;

    if (W25Q128_WaitReady(W25Q128_ERASE_TIMEOUT_MS) != W25Q128_OK) {
        return W25Q128_TIMEOUT;
    }

    if (W25Q128_WriteEnable() != W25Q128_OK) {
        return W25Q128_ERROR;
    }

    CS_LOW();
    HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, &cmd, 1, W25Q128_TIMEOUT_MS);
    CS_HIGH();

    /* Chip erase can take many seconds - use long timeout */
    return W25Q128_WaitReady(60000);
}

/**
 * @brief Check if flash is busy
 */
bool W25Q128_IsBusy(void)
{
    uint8_t status = W25Q128_ReadStatusReg1();
    return (status & W25Q128_STATUS_BUSY) != 0;
}

/**
 * @brief Wait until flash is ready
 */
W25Q128_Status_t W25Q128_WaitReady(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();

    while (W25Q128_IsBusy()) {
        if ((HAL_GetTick() - start) > timeout_ms) {
            return W25Q128_TIMEOUT;
        }
    }

    return W25Q128_OK;
}

/**
 * @brief Power down
 */
W25Q128_Status_t W25Q128_PowerDown(void)
{
    uint8_t cmd = W25Q128_CMD_POWER_DOWN;

    CS_LOW();
    HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, &cmd, 1, W25Q128_TIMEOUT_MS);
    CS_HIGH();

    return W25Q128_OK;
}

/**
 * @brief Wake up from power down
 */
W25Q128_Status_t W25Q128_WakeUp(void)
{
    uint8_t cmd = W25Q128_CMD_RELEASE_POWER_DOWN;

    CS_LOW();
    HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, &cmd, 1, W25Q128_TIMEOUT_MS);
    CS_HIGH();

    /* tRES1 = 3us typical */
    HAL_Delay(1);

    return W25Q128_OK;
}

/**
 * @brief Software reset
 */
W25Q128_Status_t W25Q128_Reset(void)
{
    uint8_t cmd;

    /* Enable reset */
    cmd = W25Q128_CMD_ENABLE_RESET;
    CS_LOW();
    HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, &cmd, 1, W25Q128_TIMEOUT_MS);
    CS_HIGH();

    /* Reset device */
    cmd = W25Q128_CMD_RESET_DEVICE;
    CS_LOW();
    HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, &cmd, 1, W25Q128_TIMEOUT_MS);
    CS_HIGH();

    /* tRST = 30us typical */
    HAL_Delay(1);

    return W25Q128_OK;
}

/* Private Functions */

/**
 * @brief Enable write operations
 */
static W25Q128_Status_t W25Q128_WriteEnable(void)
{
    uint8_t cmd = W25Q128_CMD_WRITE_ENABLE;

    CS_LOW();
    if (HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, &cmd, 1, W25Q128_TIMEOUT_MS) != HAL_OK) {
        CS_HIGH();
        return W25Q128_ERROR;
    }
    CS_HIGH();

    return W25Q128_OK;
}

/**
 * @brief Disable write operations
 */
static W25Q128_Status_t W25Q128_WriteDisable(void)
{
    uint8_t cmd = W25Q128_CMD_WRITE_DISABLE;

    CS_LOW();
    HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, &cmd, 1, W25Q128_TIMEOUT_MS);
    CS_HIGH();

    return W25Q128_OK;
}

/**
 * @brief Read status register 1
 */
static uint8_t W25Q128_ReadStatusReg1(void)
{
    uint8_t cmd = W25Q128_CMD_READ_STATUS_REG1;
    uint8_t status = 0;

    CS_LOW();
    HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, &cmd, 1, W25Q128_TIMEOUT_MS);
    HAL_SPI_Receive(&W25Q128_SPI_HANDLE, &status, 1, W25Q128_TIMEOUT_MS);
    CS_HIGH();

    return status;
}
