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
    fwuCtlRes_last          /* sentinel */
} eFwuCtlRes;

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

/* ---- defaultTask-side jobs ---- */

bool FwuCtl_RebootPending(void);
bool FwuCtl_PromotePending(void);
void FwuCtl_RunPromotion(void);

#ifdef __cplusplus
}
#endif

#endif /* FWU_CONTROL_H */
