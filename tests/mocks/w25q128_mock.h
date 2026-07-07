#ifndef W25Q128_MOCK_H
#define W25Q128_MOCK_H

#include <stdint.h>

/* RAM-backed NOR flash simulation (see w25q128_mock.c).
 * Covers everything up to the end of the Modbus config selector sector
 * (0x00102000) so the boot status AND the Modbus LUT regions live at their
 * real addresses. RAM is free on the host. */
#define MOCK_FLASH_SIZE  0x110000u

extern uint8_t mock_flash[MOCK_FLASH_SIZE];

/* Reset the simulated flash to fully erased (0xFF). */
void mock_flash_reset(void);

#endif /* W25Q128_MOCK_H */
