/**
 * @file    rs485_port.c
 * @brief   RS485 port driver — see rs485_port.h.
 *
 * DMA IN, DMA OUT, AND THE FRAME BOUNDARY IN HARDWARE (docs/modbus.md §10
 * step 10).  submit returns immediately and every completion arrives from an
 * ISR, which is what the port contract was written for all along.
 *
 * WHY THE POLLED VERSION HAD TO GO.  It received one byte at a time with
 * HAL_UART_Receive from the modbus task, and USART2 has a ONE-BYTE receive
 * register and no FIFO.  A task at priority 24 gets preempted -- by trice at
 * 25, by EthIf at 48, by tcpip_thread running the WireGuard crypto -- and at
 * 115200 a byte lands every 87 us, so any preemption longer than that loses
 * data.  Two ways, both silent:
 *
 *   1. HAL_UART_Receive never reports an overrun mid-frame.  Its ORE test
 *      lives inside the wait-for-RXNE loop, so when a byte is already waiting
 *      the loop body never runs and the call returns HAL_OK.  Bytes vanished
 *      with no error anywhere.
 *   2. End of frame was "no byte for one gap", measured with HAL_GetTick.
 *      After a 2 ms preemption that test is ALREADY TRUE on resume, so the
 *      driver called end-of-frame while the slave was still transmitting and
 *      handed the truncated buffer up as mbPortDone_frame.
 *
 * Measured on a JK PB-series pack at 115200: 47 % of polls failed CRC with
 * frame logging on, 5 % with it off -- i.e. the failure rate tracked CPU load,
 * which is the signature of a scheduling bug rather than a wiring one.
 *
 * None of that can happen here.  The DMA controller drains the receive
 * register regardless of what the CPU is doing, and the 3.5-character silence
 * that ends an RTU frame is detected by the USART's own IDLE line condition,
 * so end-of-frame is a hardware fact rather than a wall-clock guess.
 */

#include "App/Rs485/rs485_port.h"
#include "App/Modbus/modbus_port.h"

#include "usart.h"
#include "main.h"
#include "stm32f4xx_hal.h"

#include "FreeRTOS.h"
#include "timers.h"
#include "cmsis_os.h"

#include <string.h>

/* The line parameters currently programmed into USART2.  A device at another
 * rate is just the next transaction's parameters (§3.4), so the driver
 * reconfigures only when they actually change — the master owns every
 * transaction, so the line is time-multiplexed by construction. */
static uint32_t s_baud;
static uint8_t  s_format = 0xFFu;         /* 0xFF = never configured */

/* USART2 on this part: RX is DMA1 stream 5, TX is DMA1 stream 6, both on
 * channel 4 (RM0090 table 42).  Stream 3 is the trice USART3 TX and is the
 * only other DMA1 user, so neither of these collides with anything. */
static DMA_HandleTypeDef s_dmaRx;
static DMA_HandleTypeDef s_dmaTx;

/* The response timeout is the DRIVER'S to arm (the port contract), so it is a
 * timer here rather than a wait upstairs. */
static TimerHandle_t     s_respTimer;

/* One transaction at a time: the engine submits, then waits for exactly one
 * completion.  s_busy is the interlock that makes a second completion — a
 * timeout racing a reply — discard itself instead of reporting twice. */
static volatile uint8_t  s_busy;
static uint8_t          *s_rxBuf;
static uint16_t          s_rxSize;

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
 * Timing — all of it derived from baud, in the only layer that knows the line
 * (docs/modbus.md §8.5)
 *
 * Only the INTER-FRAME GAP is still computed here.  End-of-frame silence used
 * to be too; the USART's IDLE detection now does that in hardware, which is
 * the whole point of this rewrite.
 *
 * max(3.5 char times, 1.75 ms), character = 11 bits.  11 is the maximum any
 * RTU format uses — 8E1, 8O1 and 8N2 are all 11, and 8N1 is the widespread
 * non-conformant variant at 10 — so the derivation is CORRECT FOR EVERY
 * FORMAT, not merely safe for one, and `format` configures the UART and
 * nothing else.
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
 * DMA and interrupt wiring
 *
 * Done here rather than in Core/: CubeMX owns that directory and has never
 * been told this UART does DMA, and the driver is the thing that knows it
 * needs it.  configure_line tears the UART down and back up, which runs
 * MspDeInit/MspInit and unlinks the streams, so this is re-run every time
 * rather than once at registration.
 * -------------------------------------------------------------------------- */

static int dma_bind(void)
{
    s_dmaRx.Instance                 = DMA1_Stream5;
    s_dmaRx.Init.Channel             = DMA_CHANNEL_4;
    s_dmaRx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    s_dmaRx.Init.PeriphInc           = DMA_PINC_DISABLE;
    s_dmaRx.Init.MemInc              = DMA_MINC_ENABLE;
    s_dmaRx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    s_dmaRx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    s_dmaRx.Init.Mode                = DMA_NORMAL;
    s_dmaRx.Init.Priority            = DMA_PRIORITY_HIGH;
    s_dmaRx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;

    (void)HAL_DMA_DeInit(&s_dmaRx);
    if (HAL_DMA_Init(&s_dmaRx) != HAL_OK) {
        return -1;
    }
    __HAL_LINKDMA(&huart2, hdmarx, s_dmaRx);

    s_dmaTx.Instance                 = DMA1_Stream6;
    s_dmaTx.Init.Channel             = DMA_CHANNEL_4;
    s_dmaTx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    s_dmaTx.Init.PeriphInc           = DMA_PINC_DISABLE;
    s_dmaTx.Init.MemInc              = DMA_MINC_ENABLE;
    s_dmaTx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    s_dmaTx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    s_dmaTx.Init.Mode                = DMA_NORMAL;
    s_dmaTx.Init.Priority            = DMA_PRIORITY_LOW;
    s_dmaTx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;

    (void)HAL_DMA_DeInit(&s_dmaTx);
    if (HAL_DMA_Init(&s_dmaTx) != HAL_OK) {
        return -1;
    }
    __HAL_LINKDMA(&huart2, hdmatx, s_dmaTx);

    /* Priority 5 is configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY, the most
     * urgent level still allowed to call the FreeRTOS FromISR API — which
     * these must, because completion posts a semaphore.  Same level the trice
     * DMA stream already runs at. */
    HAL_NVIC_SetPriority(DMA1_Stream5_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);
    HAL_NVIC_SetPriority(DMA1_Stream6_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream6_IRQn);
    HAL_NVIC_SetPriority(USART2_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);

    return 0;
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
    if (dma_bind() != 0) {
        s_format = 0xFFu;
        return -1;
    }

    rs485_rx_enable();
    s_baud   = baud;
    s_format = format;
    return 0;
}

/* Anything the line left behind belongs to a transaction that is over.
 *
 * This matters more than it looks: a reply that was cut short leaves its tail
 * in flight, and without this the NEXT transaction reads those bytes as its
 * own header and fails too.  That is a self-sustaining desync, and it is
 * exactly what was seen on the bench -- a reply arriving as "01 00 00 ..."
 * with the function code and byte count missing, because they belonged to the
 * frame before it. */
static void rx_flush(void)
{
    __HAL_UART_CLEAR_OREFLAG(&huart2);
    __HAL_UART_CLEAR_IDLEFLAG(&huart2);
    (void)huart2.Instance->SR;
    (void)huart2.Instance->DR;
    huart2.ErrorCode = HAL_UART_ERROR_NONE;
}

/* --------------------------------------------------------------------------
 * Completion paths — all of them ISR context except the timeout
 * -------------------------------------------------------------------------- */

/* Claim the right to complete.  Whoever wins reports; everyone else returns.
 * A reply landing in the same microsecond its timeout expires is the case
 * this exists for. */
static int claim_completion(void)
{
    UBaseType_t saved;
    int         won = 0;

    /* Called from ISRs AND from tasks (the timeout, and submit's own error
     * path), so the mask has to be saved and put back rather than forced to
     * zero — restoring zero from task context would enable interrupts that
     * were deliberately masked further up. */
    saved = taskENTER_CRITICAL_FROM_ISR();
    if (s_busy) {
        s_busy = 0u;
        won    = 1;
    }
    taskEXIT_CRITICAL_FROM_ISR(saved);
    return won;
}

/* The response timeout expired: no reply, or not a whole one.  Runs on the
 * timer service task, so the blocking abort is legal here. */
static void resp_timeout_cb(TimerHandle_t timer)
{
    (void)timer;

    if (!s_busy) {
        return;                       /* completed while this was queued */
    }
    if (!claim_completion()) {
        return;
    }
    (void)HAL_UART_Abort(&huart2);
    rs485_rx_enable();
    Modbus_PortDone(mbPort_rs485, mbPortDone_timeout, 0);
}

static void stop_timer_from_isr(void)
{
    BaseType_t woken = pdFALSE;

    (void)xTimerStopFromISR(s_respTimer, &woken);
    portYIELD_FROM_ISR(woken);
}

/* The USART's IDLE line fired: the slave stopped talking, so the frame is
 * whatever DMA has already written.  THIS IS THE 3.5-CHARACTER SILENCE, done
 * by the hardware — no character timer, no wall clock, and immune to whatever
 * else the CPU was doing. */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
    if (huart->Instance != USART2) {
        return;
    }
    if (!claim_completion()) {
        return;
    }
    stop_timer_from_isr();

    if (size == 0u || size >= s_rxSize) {
        /* Filling the buffer is an overflow, not a frame: the engine must not
         * try to parse what did not fit. */
        Modbus_PortDone(mbPort_rs485, mbPortDone_lineError, 0);
        return;
    }
    Modbus_PortDone(mbPort_rs485, mbPortDone_frame, size);
}

/* Overrun, framing, parity or noise.  Now actually reachable — the polled
 * version could not see any of these (see the file header). */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART2) {
        return;
    }
    if (!claim_completion()) {
        return;
    }
    stop_timer_from_isr();
    rs485_rx_enable();
    Modbus_PortDone(mbPort_rs485, mbPortDone_lineError, 0);
}

/* Dispatched from HAL_UART_TxCpltCallback in Core/Src/usart.c, which trice
 * already owns for USART3.
 *
 * The HAL calls that back only after the TRANSMISSION COMPLETE flag, not when
 * the DMA finishes filling the shift register — so this is the exact moment
 * the last stop bit has left the wire and DE may drop.  Releasing it any
 * earlier truncates the tail of our own frame. */
void MbRtu_TxCompleteFromIsr(void)
{
    rs485_rx_enable();

    if (!s_busy) {
        return;
    }
    if (HAL_UARTEx_ReceiveToIdle_DMA(&huart2, s_rxBuf, s_rxSize) != HAL_OK) {
        if (claim_completion()) {
            stop_timer_from_isr();
            Modbus_PortDone(mbPort_rs485, mbPortDone_lineError, 0);
        }
        return;
    }
    /* Half-transfer would report a "frame" at 128 bytes of a 256-byte buffer.
     * Only IDLE and the full buffer are frame boundaries here. */
    __HAL_DMA_DISABLE_IT(&s_dmaRx, DMA_IT_HT);
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

    /* Runs on the modbus task, which is blocked on this transaction anyway —
     * so yield the gap rather than spinning it away as HAL_Delay did. */
    osDelay(pdMS_TO_TICKS(gap));

    s_rxBuf  = rx;
    s_rxSize = rxSize;
    rx_flush();

    s_busy = 1u;
    rs485_tx_enable();

    if (HAL_UART_Transmit_DMA(&huart2, (uint8_t *)tx, txLen) != HAL_OK) {
        s_busy = 0u;
        rs485_rx_enable();
        Modbus_PortDone(mbPort_rs485, mbPortDone_txFailed, 0);
        return -1;
    }

    /* Armed across the whole transaction, gap included, because the reply is
     * what it bounds and the reply cannot start before we stop talking. */
    if (xTimerChangePeriod(s_respTimer,
                           pdMS_TO_TICKS(timeout + gap), 0) != pdPASS) {
        /* Without a timeout a lost reply would hang the port until the
         * engine's own backstop; refuse the transaction instead. */
        if (claim_completion()) {
            (void)HAL_UART_Abort(&huart2);
            rs485_rx_enable();
            Modbus_PortDone(mbPort_rs485, mbPortDone_txFailed, 0);
        }
        return -1;
    }
    return 0;                          /* completion arrives from an ISR */
}

int MbRtu_Register(void)
{
    static const sModbusPortDriver drv = { rtu_submit, NULL };

    if (s_respTimer == NULL) {
        s_respTimer = xTimerCreate("mbrtu", pdMS_TO_TICKS(1000), pdFALSE,
                                   NULL, resp_timeout_cb);
        if (s_respTimer == NULL) {
            return -1;
        }
    }

    return Modbus_PortRegister(mbPort_rs485, &drv);
}

/* --------------------------------------------------------------------------
 * Vectors
 *
 * Defined here, not in Core/Src/stm32f4xx_it.c: they exist only because this
 * driver asked for DMA, and the startup file declares them weak so claiming
 * them costs CubeMX nothing.
 * -------------------------------------------------------------------------- */

void DMA1_Stream5_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_dmaRx);
}

void DMA1_Stream6_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_dmaTx);
}

void USART2_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart2);
}
