/* RAM-backed mock of the W25Q driver with NOR semantics:
 * erase sets a 4 KB sector to 0xFF, programming can only clear bits
 * (byte &= data). This matches what boot_status.c relies on for
 * erase-free flag updates. */

#include "w25q128.h"
#include "w25q128_mock.h"

#include <string.h>

uint8_t mock_flash[MOCK_FLASH_SIZE];

void mock_flash_reset(void)
{
    memset(mock_flash, 0xFF, sizeof(mock_flash));
}

eW25qStatus W25Q128_Read(uint32_t addr, uint8_t *buffer, uint32_t len)
{
    if (addr + len > MOCK_FLASH_SIZE) {
        return w25q_error;
    }
    memcpy(buffer, &mock_flash[addr], len);
    return w25q_ok;
}

eW25qStatus W25Q128_WritePage(uint32_t addr, const uint8_t *buffer, uint32_t len)
{
    if (addr + len > MOCK_FLASH_SIZE) {
        return w25q_error;
    }
    for (uint32_t i = 0; i < len; i++) {
        mock_flash[addr + i] &= buffer[i];
    }
    return w25q_ok;
}

eW25qStatus W25Q128_EraseSector(uint32_t addr)
{
    uint32_t base = addr & ~((uint32_t)W25Q128_SECTOR_SIZE - 1u);
    if (base + W25Q128_SECTOR_SIZE > MOCK_FLASH_SIZE) {
        return w25q_error;
    }
    memset(&mock_flash[base], 0xFF, W25Q128_SECTOR_SIZE);
    return w25q_ok;
}
