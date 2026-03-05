/**
 ******************************************************************************
 * @file    triceConfig.h
 * @brief   Trice configuration for PeriphNet TCP/IP trace
 * @author  PeriphNet Project
 * @date    2024-12-28
 ******************************************************************************
 * Configuration for dual-buffer mode with TCP/IP output
 *
 * Buffer Strategy:
 * - Double buffer mode for maximum throughput
 * - Each half buffer sized to TCP MSS (536 bytes)
 * - Total buffer: 1072 bytes (2 * 536)
 * - Data offset: 64 bytes for TCOBS encoding
 *
 * Integration:
 * - TriceTransfer() called periodically from trace task
 * - Custom TriceNonBlockingDeferredWrite8() sends via TCP
 * - Custom TriceOutDepth() tracks TCP transmission state
 ******************************************************************************
 */

#ifndef TRICE_CONFIG_H_
#define TRICE_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// Define __weak attribute for ARM GCC (used by triceUart.c weak functions)
#ifndef __weak
#define __weak __attribute__((weak))
#endif

// ===========================================================================
// Buffer Configuration
// ===========================================================================

//! Use double buffer for deferred output (fastest trice execution)
#define TRICE_BUFFER TRICE_DOUBLE_BUFFER

//! Total buffer size (sum of both halves)
//! TCP_MSS = 536, so 2 half-buffers = 1072 bytes
#define TRICE_DEFERRED_BUFFER_SIZE 1072

//! Maximum single trice message size
//! Keep reasonable for embedded system
#define TRICE_SINGLE_MAX_SIZE 128

//! Data offset for in-buffer TCOBS encoding
//! 64 bytes provides safe space for encoding overhead
#define TRICE_DATA_OFFSET 64

// ===========================================================================
// Output Configuration
// ===========================================================================

//! Enable deferred output (required for double buffer)
#define TRICE_DEFERRED_OUTPUT 1

//! Disable direct output (we only use deferred TCP)
#define TRICE_DIRECT_OUTPUT 0

//! Multi-pack mode: pack several trices before delimiter
//! More efficient for TCP transmission
#define TRICE_DEFERRED_TRANSFER_MODE TRICE_MULTI_PACK_MODE

//! Use TCOBS framing for deferred output
//! Efficient framing with 0-delimiter
#define TRICE_DEFERRED_OUT_FRAMING TRICE_FRAMING_TCOBS

// ===========================================================================
// Protection and Diagnostics
// ===========================================================================

//! Enable buffer overflow protection
//! Prevents data corruption if too much data produced
#define TRICE_PROTECT 1

//! Enable diagnostics to monitor buffer usage
#define TRICE_DIAGNOSTICS 1

// ===========================================================================
// Timestamp Configuration
// ===========================================================================

//! Use FreeRTOS tick count as timestamp (32-bit)
#define TriceStamp32 xTaskGetTickCount()

// ===========================================================================
// Feature Configuration
// ===========================================================================

//! Use UART A for Trice output via DMA (USART3)
//! Functions: TriceNonBlockingWriteUartA(), TriceOutDepthUartA()
//! Implemented in Core/Src/usart.c
#define TRICE_UARTA USART3
#define TRICE_DEFERRED_UARTA 1

//! Disable TCP output (was AUXILIARY8)
#define TRICE_DEFERRED_AUXILIARY8 0

//! Disable features not needed
#define TRICE_DEFERRED_XTEA_ENCRYPT 0
#define TRICE_DIRECT_XTEA_ENCRYPT 0
#define TRICE_CGO 0
#define TRICE_SEGGER_RTT 0
#define TRICE_DEFERRED_UARTB 0

// ===========================================================================
// Critical Section (FreeRTOS integration)
// ===========================================================================

#include "FreeRTOS.h"
#include "task.h"

//! Trice uses: TRICE_ENTER_CRITICAL_SECTION { code } TRICE_LEAVE_CRITICAL_SECTION
//! So we need macros that expand to opening/closing statements
#define TRICE_ENTER_CRITICAL_SECTION taskENTER_CRITICAL();
#define TRICE_LEAVE_CRITICAL_SECTION taskEXIT_CRITICAL();

#ifdef __cplusplus
}
#endif

#endif /* TRICE_CONFIG_H_ */
