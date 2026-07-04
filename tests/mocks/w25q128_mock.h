#ifndef W25Q128_MOCK_H
#define W25Q128_MOCK_H

#include <stdint.h>

/* RAM-backed NOR flash simulation (see w25q128_mock.c).
 * Covers the boot status sector plus a little headroom. */
#define MOCK_FLASH_SIZE  0x4000u

extern uint8_t mock_flash[MOCK_FLASH_SIZE];

/* Reset the simulated flash to fully erased (0xFF). */
void mock_flash_reset(void);

#endif /* W25Q128_MOCK_H */
