#ifndef W25Q128_H
#define W25Q128_H

#include "stm32f4xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

#define W25Q128_SPI_HANDLE              hspi2
#define W25Q128_CS_GPIO_PORT            GPIOE
#define W25Q128_CS_GPIO_PIN             GPIO_PIN_3

#define W25Q128_FLASH_SIZE              0x1000000
#define W25Q128_PAGE_SIZE               256
#define W25Q128_SECTOR_SIZE             0x1000
#define W25Q128_BLOCK_SIZE_32K          0x8000
#define W25Q128_BLOCK_SIZE_64K          0x10000

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

#define W25Q128_STATUS_BUSY             0x01
#define W25Q128_STATUS_WEL              0x02

#define W25Q128_TIMEOUT_MS              1000
#define W25Q128_ERASE_TIMEOUT_MS        5000

typedef enum {
    W25Q128_OK = 0,
    W25Q128_ERROR = 1,
    W25Q128_BUSY = 2,
    W25Q128_TIMEOUT = 3
} W25Q128_Status_t;

typedef struct {
    uint8_t manufacturer_id;
    uint8_t memory_type;
    uint8_t capacity;
} W25Q128_ID_t;

extern SPI_HandleTypeDef W25Q128_SPI_HANDLE;

W25Q128_Status_t W25Q128_Init(void);
W25Q128_Status_t W25Q128_ReadID(W25Q128_ID_t *id);
W25Q128_Status_t W25Q128_Read(uint32_t addr, uint8_t *buffer, uint32_t len);
W25Q128_Status_t W25Q128_WritePage(uint32_t addr, const uint8_t *buffer, uint32_t len);
W25Q128_Status_t W25Q128_EraseSector(uint32_t addr);
W25Q128_Status_t W25Q128_EraseBlock32K(uint32_t addr);
W25Q128_Status_t W25Q128_EraseBlock64K(uint32_t addr);
W25Q128_Status_t W25Q128_EraseChip(void);
bool W25Q128_IsBusy(void);
W25Q128_Status_t W25Q128_WaitReady(uint32_t timeout_ms);
W25Q128_Status_t W25Q128_PowerDown(void);
W25Q128_Status_t W25Q128_WakeUp(void);
W25Q128_Status_t W25Q128_Reset(void);

#endif /* W25Q128_H */
