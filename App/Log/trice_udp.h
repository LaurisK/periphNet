/**
 * @file    trice_udp.h
 * @brief   Trice UDP transport — sends TCOBS-encoded trice data over UDP
 *
 * Default destination is the developer's WireGuard tunnel address, so log
 * output is visible from outside the board's LAN.  The previous default was
 * the 255.255.255.255 limited broadcast, which is never routed and therefore
 * produced nothing at all once off-LAN.
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

/* Developer laptop inside the WireGuard tunnel (peer Lauris_laptop). */
#define TRICE_UDP_DEFAULT_DEST_0  10
#define TRICE_UDP_DEFAULT_DEST_1  77
#define TRICE_UDP_DEFAULT_DEST_2  0
#define TRICE_UDP_DEFAULT_DEST_3  4

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

/**
 * @brief Retarget trice UDP output at runtime.
 *
 * Pass 255.255.255.255 to restore the old on-LAN broadcast behaviour, or any
 * unicast address (e.g. another tunnel peer) to follow the developer.
 */
void Trice_UdpSetDest(uint8_t a, uint8_t b, uint8_t c, uint8_t d);

/**
 * @brief  1 if the UDP PCB was allocated, i.e. the transport can send.
 */
int Trice_UdpIsReady(void);

/**
 * @brief  Number of datagrams handed to udp_sendto(), and how many of those
 *         it rejected (typically ERR_RTE while the tunnel is down).
 */
void Trice_UdpGetStats(uint32_t *sent, uint32_t *failed);

#endif /* TRICE_UDP_H_ */
