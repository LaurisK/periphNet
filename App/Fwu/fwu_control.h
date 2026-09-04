#ifndef FWU_CONTROL_H
#define FWU_CONTROL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "App/Img/image_store.h"
#include "dfu_types.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * FWU process control (protocol-agnostic): install request, outside-actor
 * confirmation, golden image (rollback target) and running-image
 * verification.  The image to install comes from the image store
 * (App/Img/image_store.c) — this module consumes it through the store's
 * public API and owns no transfer logic.
 */

typedef enum {
    fwuCtlRes_ok = 0,
    fwuCtlRes_noImage,       /* no valid stored image                */
    fwuCtlRes_busy,           /* transfer / promotion in progress     */
    fwuCtlRes_flashErr,      /* boot status / ext flash write error  */
    fwuCtlRes_already,       /* confirm: nothing pending             */
    fwuCtlRes_blContract,    /* nvDb placed a blob where the BL does
                              * not look — installing would brick it  */
    fwuCtlRes_notArmed,      /* kick: nothing is counting down        */
    fwuCtlRes_last          /* sentinel */
} eFwuCtlRes;

/* ==========================================================================
 * The confirmation deadline — automatic rollback when the new image comes up
 * but cannot be reached
 * ==========================================================================
 *
 * The bootloader already rolls back after three unconfirmed boots.  The hole
 * that leaves is the one that matters most on a tunnel-only board: an image
 * that BOOTS FINE and then never reboots.  Its attempt counter is only spent
 * by a reset, so a firmware that comes up with a broken tunnel, a wedged HTTP
 * server or a bad WireGuard config sits there unconfirmed and unreachable
 * FOREVER, and the rollback the bootloader is holding never fires.  Getting
 * the board back then means a site visit and a J-Link.
 *
 * So an unconfirmed image reboots itself unless somebody keeps telling it not
 * to.  `POST /api/fwu/kick` (or `fwu kick` on the CLI) reloads the countdown;
 * `POST /api/fwu/confirm` cancels it for good.  Each expiry spends one
 * bootloader attempt, so three unattended expiries end in the golden image
 * coming back on its own — which is what makes this an automatic rollback for
 * a LOSS OF CONNECTIVITY rather than only for a failure to boot.
 *
 * A kick is deliberately the only thing that reloads it.  Anything implicit —
 * "an upload counts", "traffic counts" — would let the very subsystem under
 * test hold the deadline open forever, which is exactly the case this exists
 * to catch.  The one exception is bounded and stated at its use: a reboot is
 * deferred while the image store is receiving, because an upload cannot
 * outlive the HTTP server's own recv timeout and losing an operator's upload
 * at the last moment helps nobody.
 *
 * LOCAL-TARGET ('l') BUILDS ARE EXEMPT, on the same reasoning that exempts
 * them from attempt counting: the developer owns the board, J-Link flashing
 * stays friction-free, and a bench board must not reboot every ten minutes.
 */

/* How long a kick buys, and the bounds a requested window is clamped to.
 *
 * THE DEFAULT IS A SITE DECISION, so it is one constant.  It trades two
 * failures against each other, and both are real here:
 *
 *   too short — the WAN is down for reasons that are nobody's fault, the
 *               operator cannot reach the board to confirm a PERFECTLY GOOD
 *               image, and it rolls back.  Three reboots also drop the CAN
 *               bridge between battery and inverter for ~4 s each, which is
 *               not free on a board that sits in that path.
 *   too long  — a genuinely unreachable image sits there that much longer
 *               before the golden one comes back.
 *
 * 900 s × 3 attempts ≈ 45 minutes to a full rollback, which tolerates a WAN
 * outage of that order while still recovering a bricked tunnel unattended.
 * Autonomy is a hard requirement on this board (docs/design_remote_access_
 * and_autonomy.md), and a false rollback is the way this feature would
 * violate it — raise the default if the site's WAN is worse than its
 * firmware. */
#define FWU_CONFIRM_WINDOW_DEFAULT_SEC   900U   /* 15 min                   */
#define FWU_CONFIRM_WINDOW_MIN_SEC        30U
#define FWU_CONFIRM_WINDOW_MAX_SEC      3600U   /* 1 h                      */

typedef struct {
    uint32_t window_sec;       /* what the last kick granted             */
    uint32_t remaining_sec;    /* until the self-reboot; 0 when disarmed */
    uint32_t kickCnt;          /* kicks received this boot               */
    uint8_t  attemptsLeft;     /* boot attempts the bootloader still has */
    bool     armed;
    bool     exempt;           /* local-target build: never armed        */
} sFwuConfirmGuard;

/** Scan the golden area.  Call once at startup, after ImgStore_Init(). */
void FwuCtl_Init(void);

/** True when nvDb's placement of the boot status and the two blob areas still
 *  matches what the bootloader was built to look for.
 *
 *  The bootloader has no knowledge of nvDb by design — it cannot link it, and
 *  keeping it ignorant is what leaves the directory format free to evolve.
 *  The price is that the FWU module owns the handoff, and until that handoff
 *  is designed (docs/task_nv_db.md §6) the only honest version of it is this:
 *  check that the addresses still agree, and refuse to arm an install if they
 *  do not.  A layout change that moves a blob would otherwise be discovered
 *  by a board that no longer boots. */
bool FwuCtl_BlContractHolds(void);

/** Golden (last confirmed) image info — rollback target. */
const sBlobInfo *FwuCtl_GetGolden(void);

/** Arm the FWU flag; on success a reboot is scheduled (defaultTask polls
 *  FwuCtl_RebootPending). */
eFwuCtlRes FwuCtl_RequestInstall(void);

/** Outside-actor confirmation.  On success *promote tells whether a
 *  stored→golden promotion was queued. */
eFwuCtlRes FwuCtl_Confirm(bool *promote);

/** Authenticate the RUNNING internal image via the BL API (HMAC-SHA256).
 *  @return fwuRes_ok if authentic; FWU_ERR_* otherwise (incl. unsigned/no
 *  header/BL API unavailable mapped to eFwuRes codes). */
eFwuRes FwuCtl_VerifyRunning(void);

/**
 * @brief Reload the confirmation countdown.
 * @param  window_sec - how long to grant; 0 keeps the window in force.
 *                      Clamped to [FWU_CONFIRM_WINDOW_MIN_SEC,
 *                      FWU_CONFIRM_WINDOW_MAX_SEC] rather than refused, and
 *                      the granted value is reported back.
 * @retval fwuCtlRes_ok, or fwuCtlRes_notArmed when nothing is counting down
 *         (already confirmed, or a local-target build)
 */
eFwuCtlRes FwuCtl_KickConfirm(uint32_t window_sec);

/** Where the countdown stands.  Reads the boot status, so it costs an SPI
 *  page read — an operator-initiated call, not a poll. */
void FwuCtl_GetConfirmGuard(sFwuConfirmGuard *out);

/* ---- defaultTask-side jobs ---- */

/** @return true once an unconfirmed image has gone unkicked for its window
 *          and should reset.  Cheap enough to poll: the boot status is read
 *          only in the moment the deadline is actually reached. */
bool FwuCtl_ConfirmDeadlineDue(void);

bool FwuCtl_RebootPending(void);
bool FwuCtl_PromotePending(void);
void FwuCtl_RunPromotion(void);

#ifdef __cplusplus
}
#endif

#endif /* FWU_CONTROL_H */
