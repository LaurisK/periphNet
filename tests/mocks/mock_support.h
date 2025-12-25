/**
 * Mock support API for unit tests.
 * Controls mock flash and TCP behavior.
 */
#ifndef MOCK_SUPPORT_H
#define MOCK_SUPPORT_H

#include <stdint.h>
#include "lwip/tcp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reset all mocks to initial state */
void mock_reset_all(void);

/* ---- Flash mock ---- */
#define MOCK_FLASH_SIZE  (512 * 1024)  /* 512KB mock flash */
extern uint8_t mock_flash[MOCK_FLASH_SIZE];

void mock_w25q128_fail_erase_after(int n);
void mock_w25q128_fail_write_after(int n);
void mock_w25q128_fail_read_after(int n);

/* ---- TCP mock ---- */
#define MOCK_TCP_WRITE_BUF_SIZE  (512 * 1024)
extern uint8_t  mock_tcp_write_buf[MOCK_TCP_WRITE_BUF_SIZE];
extern uint32_t mock_tcp_write_pos;
extern int      mock_tcp_close_count;
extern int      mock_tcp_abort_count;
extern int      mock_tcp_recved_count;
extern uint32_t mock_tcp_recved_total;
extern int      mock_tcp_output_count;
extern err_t    mock_tcp_write_return;

#ifdef __cplusplus
}
#endif

#endif /* MOCK_SUPPORT_H */
