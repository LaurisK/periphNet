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
    FWU_CTL_OK = 0,
    FWU_CTL_NO_IMAGE,       /* no valid stored image                */
    FWU_CTL_BUSY,           /* transfer / promotion in progress     */
    FWU_CTL_FLASH_ERR,      /* boot status / ext flash write error  */
    FWU_CTL_ALREADY,        /* confirm: nothing pending             */
} eFwuCtlRes;

/** Scan the golden area.  Call once at startup, after ImgStore_Init(). */
void FwuCtl_Init(void);

/** Golden (last confirmed) image info — rollback target. */
const sBlobInfo *FwuCtl_GetGolden(void);

/** Arm the FWU flag; on success a reboot is scheduled (defaultTask polls
 *  FwuCtl_RebootPending). */
eFwuCtlRes FwuCtl_RequestInstall(void);

/** Outside-actor confirmation.  On success *promote tells whether a
 *  stored→golden promotion was queued. */
eFwuCtlRes FwuCtl_Confirm(bool *promote);

/** Authenticate the RUNNING internal image via the BL API (HMAC-SHA256).
 *  @return FWU_OK if authentic; FWU_ERR_* otherwise (incl. unsigned/no
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
