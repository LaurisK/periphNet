/**
 * @file    rs485_port.h
 * @brief   RS485 port driver — USART2 (PD5 TX, PD6 RX) with DE on PD7.
 *
 * A PERIPHERAL DRIVER, and nothing more (docs/modbus.md §5.1) — which is why
 * it lives OUTSIDE App/Modbus, beside the test peripheral rather than inside
 * the module it serves.  It knows
 * nothing about config, events or consumers, and the module knows nothing
 * about its transport.  All it does is register itself into a port slot and
 * move frames: send what is in tx, wait out the line's own idea of a frame
 * boundary, then say how it ended.
 *
 * Everything protocol-shaped left with the port contract: CRC, exception
 * codes, register decoding and "is this our reply" are the engine's, because
 * one place must decide what a valid reply is.  What stays here is what only
 * the line knows — DE turnaround, the inter-frame gap, end-of-frame silence,
 * and the UART itself.
 *
 * COMPLETES FROM AN ISR (§10 step 10, done).  submit sends over DMA and
 * returns; the reply is received by DMA and ended by the USART's own IDLE
 * line, so the 3.5-character silence is detected in hardware and nothing
 * above this file changed to make that true — which was the claim the port
 * contract was making all along.
 */

#ifndef RS485_PORT_H_
#define RS485_PORT_H_

#include <stdint.h>

/**
 * @brief  Register this driver into the RS485 port slot.
 *
 *         Independent of Modbus_Init and may follow it.  Until it runs, a
 *         device bound to that port is simply not polled — that is what "a
 *         port with no driver is disabled" means, and it is structural.
 *
 * @return 0, or -1 if the slot refused the registration
 */
int MbRtu_Register(void);

/**
 * @brief  The transmitted frame has fully left the wire.
 *
 *         Called from HAL_UART_TxCpltCallback, which Core/Src/usart.c owns
 *         because trice claimed it for USART3 first — one global HAL callback,
 *         so the dispatch by instance lives there and the work lives here.
 *         ISR context: it drops DE and starts the DMA receive.
 */
void MbRtu_TxCompleteFromIsr(void);

#endif /* RS485_PORT_H_ */
