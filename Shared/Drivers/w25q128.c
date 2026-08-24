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

/* --------------------------------------------------------------------------
 * Bulk transport — DMA where it is legal, polled everywhere else
 *
 * Only two transfers on this bus are big enough to matter: the payload of a
 * read and the payload of a page program.  Every other exchange is a 1..4 byte
 * command, where arming DMA costs more than clocking the bytes out.
 *
 * DMA IS NOT ALWAYS AVAILABLE, AND THE REASONS ARE NOT NEGOTIABLE:
 *
 *  - DMA CANNOT ADDRESS CCM.  Core-coupled memory at 0x10000000 is off the bus
 *    matrix the DMA controllers master, and real callers hand this driver CCM
 *    buffers -- the Modbus config compiler and the plan rewriter both keep
 *    their page staging in .ccmram, and anything pvPortMalloc'd is in .ccmheap.
 *    A DMA transfer from there does not fault, it silently moves nothing, so
 *    the address has to be checked rather than assumed.
 *  - The BOOTLOADER links this same file with no RTOS and no DMA init.
 *  - Before the scheduler runs there is nothing to block on, and in interrupt
 *    context there is nothing that may block.
 *
 * Falling back to the existing polled HAL call in all of those cases keeps one
 * code path correct everywhere instead of two that disagree.
 * -------------------------------------------------------------------------- */

/*! Under this many bytes the setup dominates; commands always land here. */
#define W25Q_DMA_MIN_BYTES  64u

/* STM32F407 memory map, for "can the DMA controller see this?" */
#define W25Q_SRAM_BASE_ADDR      0x20000000u
#define W25Q_SRAM_END_ADDR       0x20020000u   /* 128 KB main SRAM               */
#define W25Q_IFLASH_BASE_ADDR     0x08000000u
#define W25Q_IFLASH_END_ADDR      0x08080000u   /* 512 KB internal flash          */

static SemaphoreHandle_t s_dmaDone;
static volatile int      s_dmaFailed;

static bool dma_usable(const void *buf, uint32_t len)
{
    uint32_t addr = (uint32_t)buf;

    if (s_dmaDone == NULL || len < W25Q_DMA_MIN_BYTES) {
        return false;
    }
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING || __get_IPSR() != 0U) {
        return false;
    }

    /* Main SRAM, and internal flash for a const transmit source.  CCM is
     * absent from this list on purpose -- see the note above. */
    if (addr >= W25Q_SRAM_BASE_ADDR && (addr + len) <= W25Q_SRAM_END_ADDR) {
        return true;
    }
    if (addr >= W25Q_IFLASH_BASE_ADDR && (addr + len) <= W25Q_IFLASH_END_ADDR) {
        return true;
    }
    return false;
}

/*! Wait for the transfer the caller just started.  Aborts on timeout so the
 *  peripheral is never left mid-transfer for the next caller to inherit. */
static eW25qStatus dma_wait(uint32_t timeout_ms)
{
    if (xSemaphoreTake(s_dmaDone, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        (void)HAL_SPI_Abort(&W25Q128_SPI_HANDLE);
        return w25q_timeout;
    }
    return (s_dmaFailed != 0) ? w25q_error : w25q_ok;
}

/*! Discard a completion left over from a transfer that timed out. */
static void dma_arm(void)
{
    (void)xSemaphoreTake(s_dmaDone, 0);
    s_dmaFailed = 0;
}

/* HAL SPI callbacks.  SPI2 is the only SPI on this board, so the flash driver
 * owns them outright rather than dispatching on ->Instance.
 *
 * ALL THREE COMPLETION CALLBACKS ARE NEEDED, and which one fires is not
 * obvious.  HAL_SPI_Receive_DMA in 2-line master mode delegates to
 * HAL_SPI_TransmitReceive_DMA with State left at BUSY_RX, and that state makes
 * the HAL install SPI_DMAReceiveCplt -- so a read completes through
 * HAL_SPI_RxCpltCallback, NOT the TxRx one the delegation would suggest.
 * Miss it and every DMA read silently waits out its full timeout instead. */
void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    BaseType_t woken = pdFALSE;
    (void)hspi;
    xSemaphoreGiveFromISR(s_dmaDone, &woken);
    portYIELD_FROM_ISR(woken);
}

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    BaseType_t woken = pdFALSE;
    (void)hspi;
    xSemaphoreGiveFromISR(s_dmaDone, &woken);
    portYIELD_FROM_ISR(woken);
}

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    BaseType_t woken = pdFALSE;
    (void)hspi;
    xSemaphoreGiveFromISR(s_dmaDone, &woken);
    portYIELD_FROM_ISR(woken);
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    BaseType_t woken = pdFALSE;
    (void)hspi;
    s_dmaFailed = 1;
    xSemaphoreGiveFromISR(s_dmaDone, &woken);
    portYIELD_FROM_ISR(woken);
}

static void bus_lock_init(void)
{
    if (s_busLock == NULL) {
        s_busLock = xSemaphoreCreateMutex();
    }
    if (s_dmaDone == NULL) {
        s_dmaDone = xSemaphoreCreateBinary();
    }
}
#else
#define bus_lock()
#define bus_unlock()
#define bus_lock_init()
#endif

/* --------------------------------------------------------------------------
 * The two bulk transfers, DMA or polled
 * -------------------------------------------------------------------------- */

/*! Clock @p len bytes out of @p buf. */
static eW25qStatus spi_write(const uint8_t *buf, uint32_t len)
{
#ifndef BOOTLOADER_BUILD
    if (dma_usable(buf, len)) {
        dma_arm();
        if (HAL_SPI_Transmit_DMA(&W25Q128_SPI_HANDLE,
                                 (uint8_t *)buf, (uint16_t)len) != HAL_OK) {
            return w25q_error;
        }
        return dma_wait(W25Q128_TIMEOUT_MS);
    }
#endif
    return (HAL_SPI_Transmit(&W25Q128_SPI_HANDLE, (uint8_t *)buf,
                             (uint16_t)len, W25Q128_TIMEOUT_MS) == HAL_OK)
               ? w25q_ok
               : w25q_error;
}

/*! Clock @p len bytes into @p buf. */
static eW25qStatus spi_read(uint8_t *buf, uint32_t len)
{
#ifndef BOOTLOADER_BUILD
    if (dma_usable(buf, len)) {
        dma_arm();
        /* In 2LINES master mode the HAL routes Receive_DMA through
         * TransmitReceive_DMA, clocking the buffer's own content out on MOSI
         * as dummy bytes.  The flash ignores MOSI during a read data phase,
         * so that is harmless -- but it is why BOTH streams are needed, and
         * why the flash had to take DMA1 stream 3 and 4 from the Trice UART. */
        if (HAL_SPI_Receive_DMA(&W25Q128_SPI_HANDLE,
                                buf, (uint16_t)len) != HAL_OK) {
            return w25q_error;
        }
        return dma_wait(W25Q128_TIMEOUT_MS);
    }
#endif
    return (HAL_SPI_Receive(&W25Q128_SPI_HANDLE, buf,
                            (uint16_t)len, W25Q128_TIMEOUT_MS) == HAL_OK)
               ? w25q_ok
               : w25q_error;
}

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

    if (spi_read(buffer, len) != w25q_ok) {
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

    if (spi_write(buffer, len) != w25q_ok) {
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
