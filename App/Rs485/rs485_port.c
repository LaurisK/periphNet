/**
 * @file    rs485_port.c
 * @brief   RS485 port driver — see rs485_port.h.
 */

#include "App/Rs485/rs485_port.h"
#include "App/Modbus/modbus_port.h"

#include "usart.h"
#include "main.h"
#include "stm32f4xx_hal.h"

#include <string.h>

/* The line parameters currently programmed into USART2.  A device at another
 * rate is just the next transaction's parameters (§3.4), so the driver
 * reconfigures only when they actually change — the master owns every
 * transaction, so the line is time-multiplexed by construction. */
static uint32_t s_baud;
static uint8_t  s_format = 0xFFu;         /* 0xFF = never configured */

/* --------------------------------------------------------------------------
 * RS485 direction control (PD7)
 * -------------------------------------------------------------------------- */

static inline void rs485_tx_enable(void)
{
    HAL_GPIO_WritePin(gpio_rs485de_GPIO_Port, gpio_rs485de_Pin, GPIO_PIN_SET);
}

static inline void rs485_rx_enable(void)
{
    HAL_GPIO_WritePin(gpio_rs485de_GPIO_Port, gpio_rs485de_Pin, GPIO_PIN_RESET);
}

/* --------------------------------------------------------------------------
 * Line configuration
 *
 * ON STM32, 8E1 IS UART_WORDLENGTH_9B + UART_PARITY_EVEN.  The HAL counts the
 * parity bit inside the word length, so WORDLENGTH_8B + PARITY_EVEN gives
 * SEVEN data bits plus parity — a device configured for 8E1 then decodes some
 * bytes correctly and mangles the rest, which reads like a wiring fault rather
 * than a configuration one (docs/modbus.md §8.5).
 * -------------------------------------------------------------------------- */

static int configure_line(uint32_t baud, uint8_t format)
{
    if (baud == s_baud && format == s_format) {
        return 0;
    }

    HAL_UART_DeInit(&huart2);

    huart2.Instance          = USART2;
    huart2.Init.BaudRate     = baud;
    huart2.Init.Mode         = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    huart2.Init.StopBits     = UART_STOPBITS_1;

    switch (format) {
    case mbFmt_8E1:
        huart2.Init.WordLength = UART_WORDLENGTH_9B;
        huart2.Init.Parity     = UART_PARITY_EVEN;
        break;
    case mbFmt_8O1:
        huart2.Init.WordLength = UART_WORDLENGTH_9B;
        huart2.Init.Parity     = UART_PARITY_ODD;
        break;
    case mbFmt_8N2:
        huart2.Init.WordLength = UART_WORDLENGTH_8B;
        huart2.Init.Parity     = UART_PARITY_NONE;
        huart2.Init.StopBits   = UART_STOPBITS_2;
        break;
    default:                              /* 8N1 */
        huart2.Init.WordLength = UART_WORDLENGTH_8B;
        huart2.Init.Parity     = UART_PARITY_NONE;
        break;
    }

    if (HAL_UART_Init(&huart2) != HAL_OK) {
        s_format = 0xFFu;
        return -1;
    }

    rs485_rx_enable();
    s_baud   = baud;
    s_format = format;
    return 0;
}

/* --------------------------------------------------------------------------
 * Timing — all of it derived from baud, in the only layer that knows the line
 * (docs/modbus.md §8.5)
 *
 * max(3.5 char times, 1.75 ms), character = 11 bits.  11 is the maximum any
 * RTU format uses — 8E1, 8O1 and 8N2 are all 11, and 8N1 is the widespread
 * non-conformant variant at 10 — so the derivation is CORRECT FOR EVERY
 * FORMAT, not merely safe for one, and `format` configures the UART and
 * nothing else.
 *
 * On 8N1 every gap comes out about 10 % longer than strictly needed, which is
 * safe in both directions: the master owns the bus and there is exactly one
 * reply per request, so a longer silence can never split a frame or merge two.
 * -------------------------------------------------------------------------- */

static uint32_t frame_gap_ms(uint32_t baud)
{
    /* 3.5 chars x 11 bits = 38.5 bit times, rounded up to whole ms; the
     * 1.75 ms floor above 19200 keeps every value >= 2 ms, so millisecond
     * granularity suffices at 115200. */
    uint32_t ms = (38500u + baud - 1u) / baud;

    return (ms < 2u) ? 2u : ms;
}

/* --------------------------------------------------------------------------
 * The one call down
 * -------------------------------------------------------------------------- */

static int rtu_submit(void *ctx, const uint8_t *tx, uint16_t txLen,
                      uint8_t *rx, uint16_t rxSize,
                      const sModbusPortParams *p)
{
    (void)ctx;

    uint32_t baud    = (p->baud == 0u) ? 9600u : p->baud;
    uint32_t timeout = (p->responseTimeout_ms == 0u) ? 1000u
                                                     : p->responseTimeout_ms;
    uint32_t gap     = frame_gap_ms(baud);

    if (configure_line(baud, p->format) != 0) {
        Modbus_PortDone(mbPort_rs485, mbPortDone_txFailed, 0);
        return -1;
    }

    /* Inter-frame gap before we drive the line. */
    HAL_Delay(gap);

    rs485_tx_enable();
    HAL_StatusTypeDef st = HAL_UART_Transmit(&huart2, (uint8_t *)tx, txLen,
                                             timeout);
    /* Wait for the last byte to shift out before releasing the driver, or the
     * tail of our own frame goes out mangled. */
    uint32_t t0 = HAL_GetTick();
    while (!__HAL_UART_GET_FLAG(&huart2, UART_FLAG_TC)) {
        if ((HAL_GetTick() - t0) > 10u) {
            break;
        }
    }
    rs485_rx_enable();

    if (st != HAL_OK) {
        Modbus_PortDone(mbPort_rs485, mbPortDone_txFailed, 0);
        return -1;
    }

    /* Receive until the line goes quiet for a frame gap — THE FRAME BOUNDARY
     * IS THE DRIVER'S JOB, and it is the only thing here that knows it. */
    uint16_t rxLen        = 0;
    uint32_t lastByteTick = 0;

    t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < timeout) {
        uint8_t byte;
        HAL_StatusTypeDef r = HAL_UART_Receive(&huart2, &byte, 1, 5);

        if (r == HAL_OK) {
            if (rxLen >= rxSize) {
                /* Overflow is a LINE ERROR, not a short frame: the engine
                 * must not try to parse what it did not fully receive. */
                Modbus_PortDone(mbPort_rs485, mbPortDone_lineError, 0);
                return 0;
            }
            rx[rxLen++]  = byte;
            lastByteTick = HAL_GetTick();
            continue;
        }
        if (r == HAL_ERROR) {
            /* A UART overrun is now distinguishable from "no byte", which is
             * exactly what v1 could not say (§11.1). */
            uint32_t err = HAL_UART_GetError(&huart2);
            if ((err & (HAL_UART_ERROR_ORE | HAL_UART_ERROR_FE |
                        HAL_UART_ERROR_PE | HAL_UART_ERROR_NE)) != 0u) {
                __HAL_UART_CLEAR_OREFLAG(&huart2);
                huart2.ErrorCode = HAL_UART_ERROR_NONE;
                huart2.gState    = HAL_UART_STATE_READY;
                huart2.RxState   = HAL_UART_STATE_READY;
                Modbus_PortDone(mbPort_rs485, mbPortDone_lineError, 0);
                return 0;
            }
        }
        if (rxLen > 0u && (HAL_GetTick() - lastByteTick) >= gap) {
            break;                        /* end of frame */
        }
    }

    if (rxLen == 0u) {
        Modbus_PortDone(mbPort_rs485, mbPortDone_timeout, 0);
        return 0;
    }

    Modbus_PortDone(mbPort_rs485, mbPortDone_frame, rxLen);
    return 0;
}

int MbRtu_Register(void)
{
    static const sModbusPortDriver drv = { rtu_submit, NULL };

    return Modbus_PortRegister(mbPort_rs485, &drv);
}
