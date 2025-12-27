/**
  ******************************************************************************
  * @file           : w25q128.h
  * @brief          : W25Q128 16MB SPI Flash Driver Header
  ******************************************************************************
  * @attention
  *
  * W25Q128 SPI Flash Memory Driver
  * - 16MB (128Mbit) capacity
  * - SPI interface
  * - 256-byte page program
  * - 4KB sector erase, 32KB/64KB block erase
  *
  * Hardware Configuration:
  * - SPI2: PB10 (SCK), PC2 (MISO), PC3 (MOSI)
  * - CS:   PE3 (gpio_flashCs)
  *
  ******************************************************************************
  */

#ifndef W25Q128_H
#define W25Q128_H

#include "stm32f4xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

/* Hardware Configuration */
#define W25Q128_SPI_HANDLE          hspi2
#define W25Q128_CS_GPIO_PORT        GPIOE
#define W25Q128_CS_GPIO_PIN         GPIO_PIN_3

/* W25Q128 Memory Organization */
#define W25Q128_FLASH_SIZE          0x1000000  /* 16MB */
#define W25Q128_PAGE_SIZE           256        /* 256 bytes */
#define W25Q128_SECTOR_SIZE         0x1000     /* 4KB */
#define W25Q128_BLOCK_SIZE_32K      0x8000     /* 32KB */
#define W25Q128_BLOCK_SIZE_64K      0x10000    /* 64KB */

/* W25Q128 Command Set */
#define W25Q128_CMD_WRITE_ENABLE        0x06
#define W25Q128_CMD_WRITE_DISABLE       0x04
#define W25Q128_CMD_READ_STATUS_REG1    0x05
#define W25Q128_CMD_READ_STATUS_REG2    0x35
#define W25Q128_CMD_WRITE_STATUS_REG    0x01
#define W25Q128_CMD_PAGE_PROGRAM        0x02
#define W25Q128_CMD_QUAD_PAGE_PROGRAM   0x32
#define W25Q128_CMD_SECTOR_ERASE        0x20
#define W25Q128_CMD_BLOCK_ERASE_32K     0x52
#define W25Q128_CMD_BLOCK_ERASE_64K     0xD8
#define W25Q128_CMD_CHIP_ERASE          0xC7
#define W25Q128_CMD_READ_DATA           0x03
#define W25Q128_CMD_FAST_READ           0x0B
#define W25Q128_CMD_READ_JEDEC_ID       0x9F
#define W25Q128_CMD_READ_UNIQUE_ID      0x4B
#define W25Q128_CMD_POWER_DOWN          0xB9
#define W25Q128_CMD_RELEASE_POWER_DOWN  0xAB
#define W25Q128_CMD_ENABLE_RESET        0x66
#define W25Q128_CMD_RESET_DEVICE        0x99

/* Status Register Bits */
#define W25Q128_STATUS_BUSY             0x01
#define W25Q128_STATUS_WEL              0x02

/* Timeout Values */
#define W25Q128_TIMEOUT_MS              1000
#define W25Q128_ERASE_TIMEOUT_MS        5000

/* External Flash Memory Map - now defined in bl_app_contract.h */
/* Removed duplicate definitions to avoid redefinition warnings */

/* Return Codes */
typedef enum {
    W25Q128_OK = 0,
    W25Q128_ERROR = 1,
    W25Q128_BUSY = 2,
    W25Q128_TIMEOUT = 3
} W25Q128_Status_t;

/* JEDEC ID Structure */
typedef struct {
    uint8_t manufacturer_id;  /* Should be 0xEF for Winbond */
    uint8_t memory_type;      /* Should be 0x40 for W25Q series */
    uint8_t capacity;         /* Should be 0x18 for 128Mbit */
} W25Q128_ID_t;

/* External SPI Handle Declaration */
extern SPI_HandleTypeDef W25Q128_SPI_HANDLE;

/* Public API Functions */

/**
 * @brief Initialize W25Q128 flash
 * @return W25Q128_OK if successful
 */
W25Q128_Status_t W25Q128_Init(void);

/**
 * @brief Read JEDEC ID from flash
 * @param id Pointer to ID structure
 * @return W25Q128_OK if successful
 */
W25Q128_Status_t W25Q128_ReadID(W25Q128_ID_t *id);

/**
 * @brief Read data from flash
 * @param addr Address to read from (0 to 0xFFFFFF)
 * @param buffer Buffer to store read data
 * @param len Number of bytes to read
 * @return W25Q128_OK if successful
 */
W25Q128_Status_t W25Q128_Read(uint32_t addr, uint8_t *buffer, uint32_t len);

/**
 * @brief Write data to flash (page program)
 * @param addr Address to write to (0 to 0xFFFFFF)
 * @param buffer Data to write
 * @param len Number of bytes to write (max 256 per page)
 * @return W25Q128_OK if successful
 * @note Sector must be erased before writing
 * @note Address must be page-aligned for full page writes
 */
W25Q128_Status_t W25Q128_WritePage(uint32_t addr, const uint8_t *buffer, uint32_t len);

/**
 * @brief Erase 4KB sector
 * @param addr Address within sector to erase
 * @return W25Q128_OK if successful
 */
W25Q128_Status_t W25Q128_EraseSector(uint32_t addr);

/**
 * @brief Erase 32KB block
 * @param addr Address within block to erase
 * @return W25Q128_OK if successful
 */
W25Q128_Status_t W25Q128_EraseBlock32K(uint32_t addr);

/**
 * @brief Erase 64KB block
 * @param addr Address within block to erase
 * @return W25Q128_OK if successful
 */
W25Q128_Status_t W25Q128_EraseBlock64K(uint32_t addr);

/**
 * @brief Erase entire chip (16MB)
 * @return W25Q128_OK if successful
 * @warning This takes several seconds to complete!
 */
W25Q128_Status_t W25Q128_EraseChip(void);

/**
 * @brief Check if flash is busy
 * @return true if busy, false if ready
 */
bool W25Q128_IsBusy(void);

/**
 * @brief Wait until flash is ready
 * @param timeout_ms Timeout in milliseconds
 * @return W25Q128_OK if ready, W25Q128_TIMEOUT if timeout
 */
W25Q128_Status_t W25Q128_WaitReady(uint32_t timeout_ms);

/**
 * @brief Enter power-down mode
 * @return W25Q128_OK if successful
 */
W25Q128_Status_t W25Q128_PowerDown(void);

/**
 * @brief Wake from power-down mode
 * @return W25Q128_OK if successful
 */
W25Q128_Status_t W25Q128_WakeUp(void);

/**
 * @brief Software reset of flash device
 * @return W25Q128_OK if successful
 */
W25Q128_Status_t W25Q128_Reset(void);

#endif /* W25Q128_H */
