/**
 * @file    cmd_parser.h
 * @brief   Text command parser — accepts commands from USB CDC and UART,
 *          dispatches handlers, prints results via Trice.
 *
 * Commands are newline-terminated ASCII strings. Each input source has its
 * own line buffer so partial data from USB and UART don't interfere.
 *
 * Usage:
 *   Cmd_Init();                              // call once from a task
 *   Cmd_Feed(cmdSrc_usb, data, len);        // call from USB CDC RX callback
 *   Cmd_Feed(cmdSrc_uart, &byte, 1);       // call from UART RX callback
 *
 * Available commands:
 *   peripherals   — list device peripherals (stub: "n/a")
 *   help          — list available commands
 */

#ifndef CMD_PARSER_H_
#define CMD_PARSER_H_

#include <stdint.h>
#include <stddef.h>

typedef enum {
    cmdSrc_usb,
    cmdSrc_uart,
    cmdSrc_last
} eCmdSrc;

/**
 * @brief Initialize command parser
 *
 * Starts USART1 single-byte RX interrupt for UART command input.
 * Must be called from a FreeRTOS task context.
 */
void Cmd_Init(void);

/**
 * @brief Feed raw bytes into the command parser
 *
 * Safe to call from ISR context (USB OTG IRQ, UART IRQ).
 * Accumulates bytes into a per-source line buffer. When a complete
 * line is detected ('\n' or '\r'), it is queued for processing.
 *
 * @param src   Input source identifier
 * @param data  Pointer to received bytes
 * @param len   Number of bytes
 */
void Cmd_Feed(eCmdSrc src, const uint8_t *data, size_t len);

/**
 * @brief USART1 RX complete callback — called from HAL IRQ handler
 *
 * Re-arms single-byte receive and feeds the byte into the parser.
 */
void Cmd_Uart1RxCallback(void);

#endif /* CMD_PARSER_H_ */
