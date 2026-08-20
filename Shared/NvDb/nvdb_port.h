/**
 * @file    nvdb_port.h
 * @brief   The RTOS services the nvDb core requires, and the collector entry.
 *
 * The core is RTOS-free so the host unit tests can drive it with no board.
 * Everything it needs from an RTOS arrives through these four functions —
 * the same pattern App/Net/wg_platform.c uses for WireGuard.  The
 * application implements them in App/NvDb/nvdb_platform.c; the host tests
 * implement them as no-ops and call NvDb_CollectStep() by hand, which is what
 * makes the collector's behaviour deterministic in a test.
 */
#ifndef NVDB_PORT_H_
#define NVDB_PORT_H_

#include "nvdb.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Serializes every nvDb operation.  MUST be priority-inheriting: a
 * lowest-priority collector holding this mid-erase would otherwise block a
 * high-priority writer with no way to be boosted. */
void NvDbPort_Lock(void);
void NvDbPort_Unlock(void);

/* There is deferred erase work.  Wake the collector context; must not block
 * and is called with the lock HELD. */
void NvDbPort_CollectorNotify(void);

/* Called before each erase and between units of a long copy, so a relayout of
 * a 488 KB area cannot outrun the watchdog.  No-op off-target. */
void NvDbPort_Kick(void);

/* ==========================================================================
 * The collector
 * ==========================================================================
 * Erase one erasable unit's worth of deleted space and return.  One unit per
 * call — per lock acquisition — so a waiting writer gets in between units.
 *
 * @retval true  work was done, and there may be more
 * @retval false nothing was pending; the collector context may sleep
 *
 * Callbacks for ranges that completed are fired from inside this call,
 * outside the lock.
 */
bool NvDb_CollectStep(void);

#ifdef __cplusplus
}
#endif

#endif /* NVDB_PORT_H_ */
