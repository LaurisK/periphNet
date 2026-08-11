#include "w25q128.h"
#include "spi.h"

#define CS_LOW()    HAL_GPIO_WritePin(W25Q128_CS_GPIO_PORT, W25Q128_CS_GPIO_PIN, GPIO_PIN_RESET)
#define CS_HIGH()   HAL_GPIO_WritePin(W25Q128_CS_GPIO_PORT, W25Q128_CS_GPIO_PIN, GPIO_PIN_SET)

static eW25qStatus W25Q128_WriteEnable(void);
static uint8_t W25Q128_ReadStatusReg1(void);

/* --------------------------------------------------------------------------
 * Bus locking — data-path operations are serialized with a mutex in the
 * application (tcpip_thread and defaultTask both access the flash).
 * The bootloader is single-threaded and fault handlers (crash dump) must
 * not block, so locking is skipped there.
 * -------------------------------------------------------------------------- */

#ifndef BOOTLOADER_BUILD
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

static SemaphoreHandle_t s_busLock;

static void bus_lock(void)
{
    if (s_busLock == NULL ||
        xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||
        __get_IPSR() != 0U) {
        return;   /* pre-scheduler init or fault-handler context */
    }
    xSemaphoreTake(s_busLock, portMAX_DELAY);
}

static void bus_unlock(void)
{
    if (s_busLock == NULL ||
        xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||
        __get_IPSR() != 0U) {
        return;
    }
    xSemaphoreGive(s_busLock);
}

static void bus_lock_init(void)
{
    if (s_busLock == NULL) {
        s_busLock = xSemaphoreCreateMutex();
    }
}
#else
#define bus_lock()
#define bus_unlock()
#define bus_lock_init()
#endif

/**
 * @brief Initialize the W25Q128 flash device.
 * @return w25q_ok if a supported Winbond W25Q64 or W25Q128 is detected.
 */
eW25qStatus W25Q128_Init(void)
{
    bus_lock_init();
    CS_HIGH();
    HAL_Delay(10);
    W25Q128_WakeUp();
    HAL_Delay(50);

    W25Q128_ID_t id;
    if (W25Q128_ReadID(&id) != w25q_ok) {
        return w25q_error;
    }

    if (id.manufacturer_id != 0xEF || id.memory_type != 0x40) {
        return w25q_error;
    }

    if (id.capacity != 0x17 && id.capacity != 0x18) {
        return w25q_error;
    }

    return w25q_ok;
}

/**
 * @brief Read the JEDEC ID from the flash device.
 * @param id Pointer to structure to receive manufacturer ID, memory type, and capacity.
 * @return w25q_ok on success, w25q_error on SPI failure.
 */
eW25qStatus W25Q128_ReadID(W25Q128_ID_t *id)
{
    uint8_t cmd = W25Q128_CMD_READ_JEDEC_ID;
    uint8_t data[3];

    CS_LOW();

    if (HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, &cmd, 1, W25Q128_TIMEOUT_MS) != HAL_OK) {
        CS_HIGH();
        return w25q_error;
    }

    if (HAL_SPI_Receive(&W25Q128_SPI_HANDLE, data, 3, W25Q128_TIMEOUT_MS) != HAL_OK) {
        CS_HIGH();
        return w25q_error;
    }

    CS_HIGH();

    id->manufacturer_id = data[0];
    id->memory_type = data[1];
    id->capacity = data[2];

    return w25q_ok;
}

/**
 * @brief Read data from flash.
 * @param addr Start address (0 to 0xFFFFFF).
 * @param buffer Buffer to receive the read data.
 * @param len Number of bytes to read.
 * @return w25q_ok on success, w25q_error or w25q_timeout on failure.
 */
static eW25qStatus read_impl(uint32_t addr, uint8_t *buffer, uint32_t len)
{
    uint8_t cmd[4];

    if (addr + len > W25Q128_FLASH_SIZE) {
        return w25q_error;
    }

    if (W25Q128_WaitReady(W25Q128_TIMEOUT_MS) != w25q_ok) {
        return w25q_timeout;
    }

    cmd[0] = W25Q128_CMD_READ_DATA;
    cmd[1] = (addr >> 16) & 0xFF;
    cmd[2] = (addr >> 8) & 0xFF;
    cmd[3] = addr & 0xFF;

    CS_LOW();

    if (HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, cmd, 4, W25Q128_TIMEOUT_MS) != HAL_OK) {
        CS_HIGH();
        return w25q_error;
    }

    if (HAL_SPI_Receive(&W25Q128_SPI_HANDLE, buffer, len, W25Q128_TIMEOUT_MS) != HAL_OK) {
        CS_HIGH();
        return w25q_error;
    }

    CS_HIGH();

    return w25q_ok;
}

/**
 * @brief Write up to one page (256 bytes) to flash.
 * @param addr Destination address; the sector must be erased before writing.
 * @param buffer Data to write.
 * @param len Number of bytes to write (max 256).
 * @return w25q_ok on success, w25q_error or w25q_timeout on failure.
 */
static eW25qStatus write_page_impl(uint32_t addr, const uint8_t *buffer, uint32_t len)
{
    uint8_t cmd[4];

    if (addr + len > W25Q128_FLASH_SIZE || len > W25Q128_PAGE_SIZE) {
        return w25q_error;
    }

    if (W25Q128_WaitReady(W25Q128_TIMEOUT_MS) != w25q_ok) {
        return w25q_timeout;
    }

    if (W25Q128_WriteEnable() != w25q_ok) {
        return w25q_error;
    }

    cmd[0] = W25Q128_CMD_PAGE_PROGRAM;
    cmd[1] = (addr >> 16) & 0xFF;
    cmd[2] = (addr >> 8) & 0xFF;
    cmd[3] = addr & 0xFF;

    CS_LOW();

    if (HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, cmd, 4, W25Q128_TIMEOUT_MS) != HAL_OK) {
        CS_HIGH();
        return w25q_error;
    }

    if (HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, (uint8_t*)buffer, len, W25Q128_TIMEOUT_MS) != HAL_OK) {
        CS_HIGH();
        return w25q_error;
    }

    CS_HIGH();

    if (W25Q128_WaitReady(W25Q128_TIMEOUT_MS) != w25q_ok) {
        return w25q_timeout;
    }

    return w25q_ok;
}

/**
 * @brief Erase the 4KB sector containing addr.
 * @param addr Any address within the target sector.
 * @return w25q_ok on success, w25q_error or w25q_timeout on failure.
 */
static eW25qStatus erase_sector_impl(uint32_t addr)
{
    uint8_t cmd[4];

    if (addr >= W25Q128_FLASH_SIZE) {
        return w25q_error;
    }

    if (W25Q128_WaitReady(W25Q128_ERASE_TIMEOUT_MS) != w25q_ok) {
        return w25q_timeout;
    }

    if (W25Q128_WriteEnable() != w25q_ok) {
        return w25q_error;
    }

    cmd[0] = W25Q128_CMD_SECTOR_ERASE;
    cmd[1] = (addr >> 16) & 0xFF;
    cmd[2] = (addr >> 8) & 0xFF;
    cmd[3] = addr & 0xFF;

    CS_LOW();

    if (HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, cmd, 4, W25Q128_TIMEOUT_MS) != HAL_OK) {
        CS_HIGH();
        return w25q_error;
    }

    CS_HIGH();

    if (W25Q128_WaitReady(W25Q128_ERASE_TIMEOUT_MS) != w25q_ok) {
        return w25q_timeout;
    }

    return w25q_ok;
}

/**
 * @brief Erase the 32KB block containing addr.
 * @param addr Any address within the target block.
 * @return w25q_ok on success, w25q_error or w25q_timeout on failure.
 */
static eW25qStatus erase_block32k_impl(uint32_t addr)
{
    uint8_t cmd[4];

    if (addr >= W25Q128_FLASH_SIZE) {
        return w25q_error;
    }

    if (W25Q128_WaitReady(W25Q128_ERASE_TIMEOUT_MS) != w25q_ok) {
        return w25q_timeout;
    }

    if (W25Q128_WriteEnable() != w25q_ok) {
        return w25q_error;
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
 * @brief Erase the 64KB block containing addr.
 * @param addr Any address within the target block.
 * @return w25q_ok on success, w25q_error or w25q_timeout on failure.
 */
static eW25qStatus erase_block64k_impl(uint32_t addr)
{
    uint8_t cmd[4];

    if (addr >= W25Q128_FLASH_SIZE) {
        return w25q_error;
    }

    if (W25Q128_WaitReady(W25Q128_ERASE_TIMEOUT_MS) != w25q_ok) {
        return w25q_timeout;
    }

    if (W25Q128_WriteEnable() != w25q_ok) {
        return w25q_error;
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
 * @brief Erase the entire flash chip.
 * @return w25q_ok on success, w25q_timeout if the operation exceeds 60 seconds.
 * @warning This operation takes several seconds to complete.
 */
static eW25qStatus erase_chip_impl(void)
{
    uint8_t cmd = W25Q128_CMD_CHIP_ERASE;

    if (W25Q128_WaitReady(W25Q128_ERASE_TIMEOUT_MS) != w25q_ok) {
        return w25q_timeout;
    }

    if (W25Q128_WriteEnable() != w25q_ok) {
        return w25q_error;
    }

    CS_LOW();
    HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, &cmd, 1, W25Q128_TIMEOUT_MS);
    CS_HIGH();

    return W25Q128_WaitReady(60000);
}

/* --------------------------------------------------------------------------
 * Locked public wrappers for the data-path operations
 * -------------------------------------------------------------------------- */

eW25qStatus W25Q128_Read(uint32_t addr, uint8_t *buffer, uint32_t len)
{
    bus_lock();
    eW25qStatus res = read_impl(addr, buffer, len);
    bus_unlock();
    return res;
}

eW25qStatus W25Q128_WritePage(uint32_t addr, const uint8_t *buffer, uint32_t len)
{
    bus_lock();
    eW25qStatus res = write_page_impl(addr, buffer, len);
    bus_unlock();
    return res;
}

eW25qStatus W25Q128_EraseSector(uint32_t addr)
{
    bus_lock();
    eW25qStatus res = erase_sector_impl(addr);
    bus_unlock();
    return res;
}

eW25qStatus W25Q128_EraseBlock32K(uint32_t addr)
{
    bus_lock();
    eW25qStatus res = erase_block32k_impl(addr);
    bus_unlock();
    return res;
}

eW25qStatus W25Q128_EraseBlock64K(uint32_t addr)
{
    bus_lock();
    eW25qStatus res = erase_block64k_impl(addr);
    bus_unlock();
    return res;
}

eW25qStatus W25Q128_EraseChip(void)
{
    bus_lock();
    eW25qStatus res = erase_chip_impl();
    bus_unlock();
    return res;
}

/**
 * @brief Check whether the flash is currently busy with an internal operation.
 * @return true if busy, false if ready.
 */
bool W25Q128_IsBusy(void)
{
    uint8_t status = W25Q128_ReadStatusReg1();
    return (status & W25Q128_STATUS_BUSY) != 0;
}

/**
 * @brief Poll until the flash is ready or the timeout expires.
 * @param timeout_ms Maximum time to wait in milliseconds.
 * @return w25q_ok if ready before timeout, w25q_timeout otherwise.
 */
eW25qStatus W25Q128_WaitReady(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();

    while (W25Q128_IsBusy()) {
        if ((HAL_GetTick() - start) > timeout_ms) {
            return w25q_timeout;
        }
    }

    return w25q_ok;
}

/**
 * @brief Put the flash into low-power power-down mode.
 * @return w25q_ok always.
 */
eW25qStatus W25Q128_PowerDown(void)
{
    uint8_t cmd = W25Q128_CMD_POWER_DOWN;

    CS_LOW();
    HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, &cmd, 1, W25Q128_TIMEOUT_MS);
    CS_HIGH();

    return w25q_ok;
}

/**
 * @brief Wake the flash from power-down mode.
 * @return w25q_ok always.
 */
eW25qStatus W25Q128_WakeUp(void)
{
    uint8_t cmd = W25Q128_CMD_RELEASE_POWER_DOWN;

    CS_LOW();
    HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, &cmd, 1, W25Q128_TIMEOUT_MS);
    CS_HIGH();

    HAL_Delay(1);

    return w25q_ok;
}

/**
 * @brief Issue a software reset to the flash device.
 * @return w25q_ok always.
 */
eW25qStatus W25Q128_Reset(void)
{
    uint8_t cmd;

    cmd = W25Q128_CMD_ENABLE_RESET;
    CS_LOW();
    HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, &cmd, 1, W25Q128_TIMEOUT_MS);
    CS_HIGH();

    cmd = W25Q128_CMD_RESET_DEVICE;
    CS_LOW();
    HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, &cmd, 1, W25Q128_TIMEOUT_MS);
    CS_HIGH();

    HAL_Delay(1);

    return w25q_ok;
}

/**
 * @brief Send the Write Enable command to the flash.
 * @return w25q_ok on success, w25q_error on SPI failure.
 */
static eW25qStatus W25Q128_WriteEnable(void)
{
    uint8_t cmd = W25Q128_CMD_WRITE_ENABLE;

    CS_LOW();
    if (HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, &cmd, 1, W25Q128_TIMEOUT_MS) != HAL_OK) {
        CS_HIGH();
        return w25q_error;
    }
    CS_HIGH();

    return w25q_ok;
}

/**
 * @brief Read Status Register 1 from the flash.
 * @return Raw byte value of Status Register 1.
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
