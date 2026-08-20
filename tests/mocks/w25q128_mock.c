/* RAM-backed mock of the W25Q driver with NOR semantics:
 * erase sets a 4 KB sector to 0xFF, programming can only clear bits
 * (byte &= data). This matches what boot_status.c relies on for
 * erase-free flag updates.
 *
 * Page programming is faithful too: the real part takes at most one page per
 * command and wraps at the page boundary instead of carrying into the next
 * one, so a caller that ignores either rule gets w25q_error here rather than
 * silently working on the host and corrupting flash on the board. */

#include "w25q128.h"
#include "w25q128_mock.h"

#include <string.h>

uint8_t  mock_flash[MOCK_FLASH_SIZE];
uint32_t mock_flash_eraseCnt;
uint32_t mock_flash_writeCnt;
uint32_t mock_flash_failAfter;
uint32_t mock_flash_glitchAfter;
uint32_t mock_flash_glitchOps;

static int s_powerGone;

void mock_flash_reset(void)
{
    memset(mock_flash, 0xFF, sizeof(mock_flash));
    mock_flash_eraseCnt  = 0;
    mock_flash_writeCnt  = 0;
    mock_flash_failAfter   = 0;
    mock_flash_glitchAfter = 0;
    mock_flash_glitchOps   = 0;
    s_powerGone            = 0;
}

void mock_flash_reset_power(void)
{
    mock_flash_failAfter   = 0;
    mock_flash_glitchAfter = 0;
    mock_flash_glitchOps   = 0;
    s_powerGone            = 0;
}

/* @retval 1 when this operation should fail cleanly and then be forgotten */
static int glitch(void)
{
    if (0u != mock_flash_glitchAfter) {
        mock_flash_glitchAfter--;
        return 0;
    }
    if (0u != mock_flash_glitchOps) {
        mock_flash_glitchOps--;
        return 1;
    }
    return 0;
}

/* @retval 1 when this operation is the one that gets cut in half */
static int power_cut(void)
{
    if (0 != s_powerGone) {
        return 1;
    }
    if (0u != mock_flash_failAfter && 0u == --mock_flash_failAfter) {
        s_powerGone = 1;
        return 1;
    }
    return 0;
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
    if (addr + len > MOCK_FLASH_SIZE || len > W25Q128_PAGE_SIZE) {
        return w25q_error;
    }
    /* A page program that crosses a page boundary wraps on the real part. */
    if (len > 0u &&
        (addr / W25Q128_PAGE_SIZE) != ((addr + len - 1u) / W25Q128_PAGE_SIZE)) {
        return w25q_error;
    }
    if (glitch()) {
        return w25q_error;
    }
    if (power_cut()) {
        for (uint32_t i = 0; i < len / 2u; i++) {
            mock_flash[addr + i] &= buffer[i];
        }
        return w25q_error;
    }
    for (uint32_t i = 0; i < len; i++) {
        mock_flash[addr + i] &= buffer[i];
    }
    mock_flash_writeCnt++;
    return w25q_ok;
}

eW25qStatus W25Q128_EraseSector(uint32_t addr)
{
    uint32_t base = addr & ~((uint32_t)W25Q128_SECTOR_SIZE - 1u);
    if (base + W25Q128_SECTOR_SIZE > MOCK_FLASH_SIZE) {
        return w25q_error;
    }
    if (glitch()) {
        return w25q_error;
    }
    if (power_cut()) {
        memset(&mock_flash[base], 0xFF, W25Q128_SECTOR_SIZE / 2u);
        return w25q_error;
    }
    memset(&mock_flash[base], 0xFF, W25Q128_SECTOR_SIZE);
    mock_flash_eraseCnt++;
    return w25q_ok;
}
