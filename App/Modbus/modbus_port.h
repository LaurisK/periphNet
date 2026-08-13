/**
 * @file    modbus_port.h
 * @brief   The port contract — the ONE thing outside the module that talks to
 *          it (docs/modbus.md §5.1).
 *
 * The module owns a SET OF PERIPHERALS and services them.  It knows nothing
 * about any of them: not pins, not UARTs, not sockets, not whether a frame
 * leaves the board.  What it holds per port is a function pointer, its own
 * buffers, and a callback it exports.  One call down, one call up.
 *
 * A DRIVER includes this header.  A CONSUMER includes modbus.h and never sees
 * it: there is no way to select a port for a device from outside, because a
 * port is where a device lives and that is config.
 *
 * The rules that make this a contract rather than a suggestion:
 *
 *  - THE UNIT IS A FRAME, NOT A BYTE.  `submit` means *send what is in tx,
 *    wait out the line's own idea of a frame boundary, then tell me how it
 *    ended*.  End-of-frame detection — the 3.5-character silence that defines
 *    an RTU frame — lives in the driver, the only thing that knows the line.
 *    The engine never runs a character timer.
 *  - THE RESPONSE TIMEOUT IS THE DRIVER'S TOO.  It is handed down per frame
 *    and the driver arms it, so the engine waits for exactly one thing.
 *  - COMPLETION IS ASYNCHRONOUS, AND THE CALLBACK ONLY POSTS.
 *    Modbus_PortDone runs in whatever context the driver completes in — an ISR
 *    for a DMA UART, a stack thread for a socket — and does nothing but post.
 *  - A FAILED submit IS REPORTED AS mbPortDone_txFailed, NOT THROUGH THE
 *    RETURN VALUE.  A transaction is exactly one event, and the engine waits
 *    for exactly one thing; a synchronous error with no completion would break
 *    both.  The return value is advisory only.
 *  - THE DRIVER REPORTS HOW RECEPTION ENDED, NEVER WHAT THE FRAME MEANS.  A
 *    short frame and a CRC-bad frame are both mbPortDone_frame; the engine
 *    parses and rejects them, so one place decides what a valid reply is.
 *    What the driver does own is the distinction the engine cannot make:
 *    silence is timeout, and a line error is its own outcome.
 *  - THE BUFFERS ARE THE MODULE'S, one tx and one rx per port, and they live
 *    in MAIN SRAM because a DMA driver writes into them and CCM is CPU-only.
 */
#ifndef MODBUS_PORT_H_
#define MODBUS_PORT_H_

#include "modbus_records.h"      /* eModbusPortId, eModbusLineFormat */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A frame is at most 256 bytes, so two ports cost about 1 KB. */
#define MB_PORT_BUF_SIZE   256u
#define MB_PORT_COUNT      ((uint8_t)mbPort_last)

/* Only baud, format and the timeout travel.  Inter-frame gap and end-of-frame
 * silence are derived from baud INSIDE the driver; nothing about expected
 * reply length travels either, which would make the contract protocol-level
 * instead of frame-level. */
typedef struct {
    uint32_t baud;
    uint32_t responseTimeout_ms;   /* the DRIVER arms it; 0 = its default */
    uint8_t  format;               /* eModbusLineFormat                   */
} sModbusPortParams;

typedef enum {
    mbPortDone_frame = 0,   /* a frame completed; len bytes are in rx      */
    mbPortDone_timeout,     /* responseTimeout_ms expired                  */
    mbPortDone_lineError,   /* overrun / framing / parity, or rx overflow  */
    mbPortDone_txFailed,    /* the frame never went out                    */
} eModbusPortDone;

typedef struct {
    /* Send txLen bytes from tx, then receive one frame into rx.
     * Returns immediately; completion arrives via Modbus_PortDone().
     * Both buffers belong to the MODULE and stay valid until it fires.   */
    int  (*submit)(void *ctx, const uint8_t *tx, uint16_t txLen,
                   uint8_t *rx, uint16_t rxSize,
                   const sModbusPortParams *p);
    void *ctx;
} sModbusPortDriver;

/**
 * @brief  Claim a port slot.
 *
 *         A port with no driver registered is DISABLED, and that is structural
 *         rather than a mode — there is no mbPort_disabled.  A device bound to
 *         an empty slot is simply not polled.
 *
 *         Independent of Modbus_Init and may follow it: the slot is claimed by
 *         writing the driver last, the same publish-last ordering that makes
 *         Subscribe safe at any time (§4.2).
 *
 * @return 0, or -1 on a bad slot / null driver
 */
int Modbus_PortRegister(uint8_t portId, const sModbusPortDriver *drv);

/**
 * @brief  A submitted frame finished.  Called by the driver, in whatever
 *         context it completes in.  Does nothing but record and post.
 *
 * @param  len  bytes in rx; meaningful only for mbPortDone_frame
 */
void Modbus_PortDone(uint8_t portId, eModbusPortDone how, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_PORT_H_ */
