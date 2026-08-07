/**
 * @file    wg_time.h
 * @brief   Reboot-surviving monotonic seconds counter for WireGuard TAI64N.
 *
 * WireGuard puts a TAI64N timestamp in every handshake initiation and the
 * remote end keeps the greatest value it has seen per peer, rejecting anything
 * that is not strictly newer.  A counter derived from sys_now() therefore
 * breaks the tunnel after every reset: our timestamps restart at zero and the
 * hub refuses to handshake until its own peer state is cleared.
 *
 * This module keeps a seconds counter that only ever moves forward, across
 * resets and power cuts, without any network time source:
 *
 *   - a base value lives in one 4 KB external-flash sector, written as an
 *     append-only ring of small slots (no erase per write, NOR-friendly);
 *   - every boot reads the last valid slot and jumps the base forward by
 *     WG_TIME_BOOT_BUMP_S before using it;
 *   - while running, WgTime_Tick() re-persists the current value every
 *     WG_TIME_PERSIST_S seconds.
 *
 * The bump is larger than the persist interval, so the worst case (power cut
 * right before a scheduled persist) still lands the next boot's base above
 * every timestamp the previous boot could have emitted.
 *
 * The value is monotonic, not wall clock.  That is all WireGuard needs.  If
 * real time ever arrives (SNTP over the tunnel), it can be handed to
 * WgTime_SetIfNewer() — a real UNIX-epoch value is far larger than this
 * free-running counter, so the sequence stays monotonic across the switch.
 */

#ifndef WG_TIME_H_
#define WG_TIME_H_

#include <stdint.h>

/* Forward jump applied to the persisted base on every boot. */
#define WG_TIME_BOOT_BUMP_S   3600u

/* How often the running value is written back to flash. Must stay well below
 * WG_TIME_BOOT_BUMP_S for the monotonicity argument above to hold. */
#define WG_TIME_PERSIST_S     900u

/* Floor for the counter, defined by CMake as the build's UNIX time (0 when the
 * build system does not supply it, e.g. the CubeIDE managed build).  Two
 * reasons for it: a board that has never had a valid flash slot still emits a
 * plausible timestamp instead of one near 1970, and reflashing always moves
 * the sequence forward even if the flash store was wiped. */
#ifndef WG_TIME_BUILD_EPOCH
#define WG_TIME_BUILD_EPOCH   0u
#endif

/**
 * @brief  Load the persisted base, bump it and write the new base back.
 *         Call once at startup AFTER W25Q128_Init() and BEFORE the WireGuard
 *         netif is created.  Safe to call twice (second call is a no-op).
 * @return 0 on success, negative if the flash store could not be read/written
 *         (the counter then runs from RAM only — monotonic within this boot).
 */
int WgTime_Init(void);

/**
 * @brief  Current monotonic seconds. Cheap, RAM-only, no flash access — safe
 *         to call from lwIP/tcpip_thread context.
 */
uint32_t WgTime_Now(void);

/**
 * @brief  Persist the current value if WG_TIME_PERSIST_S has elapsed since
 *         the last write.  Touches SPI flash, so call from a task (defaultTask
 *         at its normal cadence), never from an lwIP callback.
 */
void WgTime_Tick(void);

/**
 * @brief  Adopt an externally supplied absolute time (e.g. SNTP), but only if
 *         it is ahead of the current counter.  Persists immediately.
 * @return 1 if adopted, 0 if ignored as not newer.
 */
int WgTime_SetIfNewer(uint32_t seconds);

/**
 * @brief  Introspection for the CLI: current value, persisted base and
 *         whether flash backing is actually working.  Any pointer may be NULL.
 */
void WgTime_GetStatus(uint32_t *now, uint32_t *persisted, int *flashOk);

#endif /* WG_TIME_H_ */
