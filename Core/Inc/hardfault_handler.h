/**
 ******************************************************************************
 * @file    hardfault_handler.h
 * @brief   Comprehensive HardFault handler with TRICE debugging
 ******************************************************************************
 */

#ifndef HARDFAULT_HANDLER_H
#define HARDFAULT_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @brief  HardFault exception handler
 *
 * This handler is called when a HardFault exception occurs.
 * It captures all CPU state and fault information, then outputs
 * via TRICE for remote debugging.
 *
 * The handler provides:
 * - Complete register dump (R0-R12, LR, PC, PSR, SP)
 * - Fault status registers (CFSR, HFSR, DFSR, AFSR, BFAR, MMFAR)
 * - Decoded fault reason (MemManage, Bus, Usage fault details)
 * - Stack unwinding information for GDB
 * - Command-line snippets for post-mortem debugging
 */
void HardFault_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* HARDFAULT_HANDLER_H */
