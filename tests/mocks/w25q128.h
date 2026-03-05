/**
 * Mock w25q128.h for host-native unit tests.
 * Provides type definitions and function declarations without HAL dependency.
 */
#ifndef MOCK_W25Q128_H
#define MOCK_W25Q128_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Page/sector sizes */
#define W25Q128_PAGE_SIZE    256
#define W25Q128_SECTOR_SIZE  0x1000  /* 4KB */

/* Return codes */
typedef enum {
    W25Q128_OK = 0,
    W25Q128_ERROR = 1,
    W25Q128_BUSY = 2,
    W25Q128_TIMEOUT = 3
} W25Q128_Status_t;

/* Function declarations (implemented in mock_support.c) */
W25Q128_Status_t W25Q128_EraseSector(uint32_t addr);
W25Q128_Status_t W25Q128_WritePage(uint32_t addr, const uint8_t *buffer, uint32_t len);
W25Q128_Status_t W25Q128_Read(uint32_t addr, uint8_t *buffer, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_W25Q128_H */
