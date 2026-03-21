/**
 * @file    triceConfig.h
 * @brief   Trice configuration for PeriphNet – UART DMA output (USART3)
 *
 * Double buffer mode with TCOBS framing. Output via DMA1_Stream3 (USART3 TX)
 * at 460800 baud. Critical sections use raw PRIMASK (safe in any context
 * including HardFault and NMI handlers).
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

/* USART3 @ 460800 baud, DMA1_Stream3 Ch4 ---------------------------------- */
#define TRICE_DEFERRED_UARTA       1
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
