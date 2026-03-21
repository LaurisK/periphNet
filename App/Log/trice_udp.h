/**
 * @file    trice_udp.h
 * @brief   Trice UDP transport — sends TCOBS-encoded trice data over UDP broadcast
 *
 * Usage: call Trice_UdpInit() after lwIP is up, then assign the write function
 * to UserNonBlockingDeferredWrite8AuxiliaryFn. The trice tool receives data with:
 *   trice log -p UDP4 -args ":17001" -i ./til.json -li ./li.json
 */

#ifndef TRICE_UDP_H_
#define TRICE_UDP_H_

#include <stdint.h>
#include <stddef.h>

#define TRICE_UDP_PORT  17001

/**
 * @brief Initialize trice UDP transport
 *
 * Creates a UDP PCB and registers the auxiliary write function.
 * Must be called after tcpip_init() and from a context where
 * LOCK_TCPIP_CORE is available (e.g., a FreeRTOS task).
 */
void Trice_UdpInit(void);

/**
 * @brief Send trice data over UDP broadcast
 *
 * Registered as UserNonBlockingDeferredWrite8AuxiliaryFn.
 * Called from TriceTransfer() in the trice task.
 */
void Trice_UdpWrite(const uint8_t *data, size_t len);

#endif /* TRICE_UDP_H_ */
