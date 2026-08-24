/**
 * @file    triceConfig.h
 * @brief   Trice configuration for PeriphNet – UDP output, optional USART3
 *
 * Double buffer mode with TCOBS framing. Critical sections use raw PRIMASK
 * (safe in any context including HardFault and NMI handlers).
 *
 * TRACING IS OVER THE NETWORK BY DEFAULT (App/Log/trice_udp.c, plus USB CDC).
 * The USART3 wire is off unless TRICE_UART_OUTPUT is set to 1 — see below.
 */

#ifndef TRICE_CONFIG_H_
#define TRICE_CONFIG_H_

#include "stm32f4xx_hal.h"   /* HAL_GetTick(), PRIMASK intrinsics via CMSIS */

#ifndef __weak
#define __weak __attribute__((weak))
#endif

/* Buffer ------------------------------------------------------------------ */
#define TRICE_BUFFER               TRICE_DOUBLE_BUFFER
#define TRICE_DEFERRED_BUFFER_SIZE 2048
#define TRICE_SINGLE_MAX_SIZE      128
#define TRICE_DATA_OFFSET          64

/* Output ------------------------------------------------------------------ */
#define TRICE_DEFERRED_OUTPUT        1
#define TRICE_DIRECT_OUTPUT          0
#define TRICE_DEFERRED_TRANSFER_MODE TRICE_MULTI_PACK_MODE
#define TRICE_DEFERRED_OUT_FRAMING   TRICE_FRAMING_TCOBS

/* USART3 @ 460800 baud — OPT-IN, AND IT COSTS THE FLASH ITS DMA -------------
 *
 * Set TRICE_UART_OUTPUT to 1 to get trice out of the USART3 wire again (a
 * bring-up aid; normal tracing is UDP + USB CDC and needs nothing here).
 *
 * It cannot go back on DMA, and that is a hardware fact, not a choice:
 * USART3_TX exists on DMA1 Stream 3 (ch4) or Stream 4 (ch7) and NOWHERE else,
 * and both of those now carry the external flash — SPI2_RX is Stream 3 only,
 * SPI2_TX is Stream 4 only, and SPI2 is APB1 so it cannot reach DMA2 at all.
 * The flash won that trade deliberately: it is the board's bulk data path and
 * had no DMA at all, while trice has two other transports.
 *
 * So the opt-in path transmits interrupt-driven (HAL_UART_Transmit_IT) — one
 * interrupt per byte at 460800 baud, which is real CPU cost and exactly why
 * this is not the default.  Core/Src/usart.c holds the transport. */
#ifndef TRICE_UART_OUTPUT
#define TRICE_UART_OUTPUT          0
#endif

#define TRICE_DEFERRED_UARTA       TRICE_UART_OUTPUT
#define TRICE_UARTA                USART3

/* Disabled features ------------------------------------------------------- */
#define TRICE_DEFERRED_XTEA_ENCRYPT 0
#define TRICE_DIRECT_XTEA_ENCRYPT   0
#define TRICE_CGO                   0
#define TRICE_SEGGER_RTT            0
#define TRICE_DEFERRED_UARTB        0
#define TRICE_DEFERRED_AUXILIARY8   1

/* Diagnostics ------------------------------------------------------------- */
#define TRICE_PROTECT    1
#define TRICE_DIAGNOSTICS 1

/* Timestamp --------------------------------------------------------------- */
/*! HAL tick (safe before FreeRTOS start and inside fault handlers). */
#define TriceStamp32 HAL_GetTick()

/* Critical section – raw PRIMASK (safe in all execution contexts) --------- */
#define TRICE_ENTER_CRITICAL_SECTION \
    { uint32_t _trice_primask = __get_PRIMASK(); __disable_irq(); {

#define TRICE_LEAVE_CRITICAL_SECTION \
    } __set_PRIMASK(_trice_primask); }

#endif /* TRICE_CONFIG_H_ */
