/**
 ******************************************************************************
 * @file    triceUart.h
 * @brief   Trice UART hardware interface for PeriphNet
 * @note    Uses USART3 with DMA - no interrupt-driven transmission needed
 ******************************************************************************
 * This file provides empty stubs for interrupt-driven UART functions
 * since we override TriceNonBlockingWriteUartA() to use DMA instead.
 *
 * The actual DMA-based implementation is in Core/Src/usart.c:
 * - TriceNonBlockingWriteUartA() - starts DMA transfer
 * - TriceOutDepthUartA() - returns bytes remaining in DMA transfer
 ******************************************************************************
 */

#ifndef TRICE_UART_H_
#define TRICE_UART_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "trice.h"

#if TRICE_DEFERRED_UARTA == 1

/**
 * @brief  Check if UART TX data register is empty
 * @retval 0 = not empty, !0 = empty
 * @note   Not used - we use DMA, not interrupt-driven transmission
 */
TRICE_INLINE uint32_t triceTxDataRegisterEmptyUartA(void) {
    return 1;  // Always return "ready" (unused with DMA)
}

/**
 * @brief  Write byte to UART TX register
 * @param  v: Byte to transmit
 * @note   Not used - we use DMA, not interrupt-driven transmission
 */
TRICE_INLINE void triceTransmitData8UartA(uint8_t v) {
    (void)v;  // Unused with DMA
}

/**
 * @brief  Enable UART TX empty interrupt
 * @note   Not used - we use DMA, not interrupt-driven transmission
 */
TRICE_INLINE void triceEnableTxEmptyInterruptUartA(void) {
    // Unused with DMA
}

/**
 * @brief  Disable UART TX empty interrupt
 * @note   Not used - we use DMA, not interrupt-driven transmission
 */
TRICE_INLINE void triceDisableTxEmptyInterruptUartA(void) {
    // Unused with DMA
}

#endif // #if TRICE_DEFERRED_UARTA == 1

#ifdef __cplusplus
}
#endif

#endif /* TRICE_UART_H_ */
