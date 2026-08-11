/**
 * Mock implementations for flash, TCP, and pbuf APIs.
 */
#include "mock_support.h"
#include "w25q128.h"
#include "lwip/tcp.h"
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Flash mock
 * ============================================================================ */

uint8_t mock_flash[MOCK_FLASH_SIZE];

static int flash_erase_fail_after = -1;
static int flash_write_fail_after = -1;
static int flash_read_fail_after  = -1;
static int flash_erase_count = 0;
static int flash_write_count = 0;
static int flash_read_count  = 0;

void mock_w25q128_fail_erase_after(int n) { flash_erase_fail_after = n; }
void mock_w25q128_fail_write_after(int n) { flash_write_fail_after = n; }
void mock_w25q128_fail_read_after(int n)  { flash_read_fail_after = n; }

eW25qStatus W25Q128_EraseSector(uint32_t addr)
{
    if (flash_erase_fail_after >= 0 && flash_erase_count >= flash_erase_fail_after)
        return w25q_error;
    flash_erase_count++;

    /* Align to 4KB sector */
    uint32_t sector_addr = addr & ~0xFFFu;
    if (sector_addr + W25Q128_SECTOR_SIZE > MOCK_FLASH_SIZE)
        return w25q_error;

    memset(&mock_flash[sector_addr], 0xFF, W25Q128_SECTOR_SIZE);
    return w25q_ok;
}

eW25qStatus W25Q128_WritePage(uint32_t addr, const uint8_t *buffer, uint32_t len)
{
    if (flash_write_fail_after >= 0 && flash_write_count >= flash_write_fail_after)
        return w25q_error;
    flash_write_count++;

    if (len > W25Q128_PAGE_SIZE) len = W25Q128_PAGE_SIZE;
    if (addr + len > MOCK_FLASH_SIZE)
        return w25q_error;

    memcpy(&mock_flash[addr], buffer, len);
    return w25q_ok;
}

eW25qStatus W25Q128_Read(uint32_t addr, uint8_t *buffer, uint32_t len)
{
    if (flash_read_fail_after >= 0 && flash_read_count >= flash_read_fail_after)
        return w25q_error;
    flash_read_count++;

    if (addr + len > MOCK_FLASH_SIZE)
        return w25q_error;

    memcpy(buffer, &mock_flash[addr], len);
    return w25q_ok;
}

/* ============================================================================
 * TCP mock
 * ============================================================================ */

uint8_t  mock_tcp_write_buf[MOCK_TCP_WRITE_BUF_SIZE];
uint32_t mock_tcp_write_pos    = 0;
int      mock_tcp_close_count  = 0;
int      mock_tcp_abort_count  = 0;
int      mock_tcp_recved_count = 0;
uint32_t mock_tcp_recved_total = 0;
int      mock_tcp_output_count = 0;
err_t    mock_tcp_write_return = ERR_OK;

err_t tcp_write(struct tcp_pcb *pcb, const void *data, u16_t len, u8_t flags)
{
    (void)pcb;
    (void)flags;
    if (mock_tcp_write_return != ERR_OK)
        return mock_tcp_write_return;
    if (mock_tcp_write_pos + len <= MOCK_TCP_WRITE_BUF_SIZE) {
        memcpy(&mock_tcp_write_buf[mock_tcp_write_pos], data, len);
        mock_tcp_write_pos += len;
    }
    return ERR_OK;
}

void tcp_recved(struct tcp_pcb *pcb, u16_t len)
{
    (void)pcb;
    mock_tcp_recved_count++;
    mock_tcp_recved_total += len;
}

err_t tcp_close(struct tcp_pcb *pcb)
{
    (void)pcb;
    mock_tcp_close_count++;
    return ERR_OK;
}

void tcp_abort(struct tcp_pcb *pcb)
{
    (void)pcb;
    mock_tcp_abort_count++;
}

err_t tcp_output(struct tcp_pcb *pcb)
{
    (void)pcb;
    mock_tcp_output_count++;
    return ERR_OK;
}

void tcp_arg(struct tcp_pcb *pcb, void *arg)
{
    if (pcb != NULL)
        pcb->callback_arg = arg;
}

void tcp_sent(struct tcp_pcb *pcb, tcp_sent_fn sent)
{
    if (pcb != NULL)
        pcb->sent = sent;
}

void pbuf_free(struct pbuf *p)
{
    (void)p;
    /* No-op in tests — pbufs are stack/static allocated */
}

/* ============================================================================
 * Reset all mocks
 * ============================================================================ */

void mock_reset_all(void)
{
    /* Flash */
    memset(mock_flash, 0xFF, MOCK_FLASH_SIZE);
    flash_erase_fail_after = -1;
    flash_write_fail_after = -1;
    flash_read_fail_after  = -1;
    flash_erase_count = 0;
    flash_write_count = 0;
    flash_read_count  = 0;

    /* TCP */
    memset(mock_tcp_write_buf, 0, MOCK_TCP_WRITE_BUF_SIZE);
    mock_tcp_write_pos    = 0;
    mock_tcp_close_count  = 0;
    mock_tcp_abort_count  = 0;
    mock_tcp_recved_count = 0;
    mock_tcp_recved_total = 0;
    mock_tcp_output_count = 0;
    mock_tcp_write_return = ERR_OK;
}
