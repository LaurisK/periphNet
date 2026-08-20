/**
 * @file    trice_udp.h
 * @brief   Trice UDP transport — streams TCOBS-encoded trice data over UDP
 *
 * The board pushes log frames to a small list of destinations.  There is no
 * inbound socket and no subscriber protocol on the wire: this is a one-way
 * streamer, and the listener is an ordinary UDP receiver.
 *
 * Slot 0 holds the limited broadcast 255.255.255.255 by default, so anyone on
 * the board's own LAN receives the stream with no configuration whatsoever:
 *
 *   trice log -p UDP4 -args ":17001" -i ./til.json -li ./li.json
 *
 * Broadcast is never forwarded by a router, so remote listeners (across the
 * WireGuard tunnel, say) add themselves as unicast entries.  The intended way
 * is POST /api/trice/subscribe, which takes the caller's address from the HTTP
 * connection itself — the board never has to be *told* an address, and the one
 * it records is return-routable by construction, because a packet just arrived
 * from it.  POST /api/trice/dest remains for entries a connection cannot
 * express (a third-party collector, a different broadcast address).
 *
 * The list is RAM-only and reverts to broadcast-only on every reboot.  That is
 * why the default has to be something that works unconfigured: a default that
 * only a working tunnel can reach makes the log stream disappear exactly when
 * the tunnel is the thing under investigation.
 */

#ifndef TRICE_UDP_H_
#define TRICE_UDP_H_

#include <stdint.h>
#include <stddef.h>
#include "lwip/ip_addr.h"

#define TRICE_UDP_PORT      17001

/** Destination slots.  Small on purpose — this is a debug fan-out, and every
 *  entry costs one udp_sendto() per trice frame. */
#define TRICE_UDP_MAX_DEST  4

/**
 * @brief Initialize trice UDP transport
 *
 * Creates a UDP PCB, seeds the destination list with the broadcast address and
 * starts the consumer task.  Must be called after tcpip_init() and from a
 * context where LOCK_TCPIP_CORE is available (e.g. a FreeRTOS task).
 */
void Trice_UdpInit(void);

/**
 * @brief Send trice data to every configured destination
 *
 * Registered (indirectly) as UserNonBlockingDeferredWrite8AuxiliaryFn and
 * called from the consumer task.
 */
void Trice_UdpWrite(const uint8_t *data, size_t len);

/**
 * @brief  Add a destination, or refresh it if already present.
 *
 * When the list is full the least-recently-added non-sticky entry is evicted;
 * the default broadcast entry is sticky and is never evicted automatically.
 *
 * @return slot index, or -1 if the address is invalid or every slot is sticky.
 */
int Trice_UdpAddDest(const ip_addr_t *addr);

/**
 * @brief  Remove a destination.
 * @return 0 if it was removed, -1 if it was not in the list.
 */
int Trice_UdpRemoveDest(const ip_addr_t *addr);

/**
 * @brief Drop every destination and restore the default broadcast entry.
 */
void Trice_UdpResetDests(void);

/**
 * @brief Persist the current destination list.
 *
 * The list used to live only in RAM, so every reset dropped whoever was
 * listening back to the broadcast default — fine on a bench, useless on a
 * board reached through a tunnel.  Saved destinations come back at
 * Trice_UdpInit().
 *
 * @return 0 on success, -1 if the medium refused.
 */
int Trice_UdpSaveDests(void);

/**
 * @brief Discard the persisted destination list.
 * @return 0 on success, -1 if the medium refused.
 */
int Trice_UdpForgetDests(void);

/**
 * @brief  Snapshot the destination list.
 * @return number of entries written to @p out (at most @p max).
 */
uint32_t Trice_UdpGetDests(ip_addr_t *out, uint32_t max);

/**
 * @brief  1 if the UDP PCB was allocated, i.e. the transport can send.
 */
int Trice_UdpIsReady(void);

/**
 * @brief  Number of datagrams handed to udp_sendto(), and how many of those
 *         it rejected (typically ERR_RTE while a tunnel destination is down).
 *
 * Counted per destination, so one trice frame fanned out to N slots counts N.
 */
void Trice_UdpGetStats(uint32_t *sent, uint32_t *failed);

#endif /* TRICE_UDP_H_ */
