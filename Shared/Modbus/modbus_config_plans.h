/**
 * @file    modbus_config_plans.h
 * @brief   Runtime plan edits: validate one plan, rewrite the region with it.
 *
 * A plan is the only config object the system may change about itself while it
 * runs (docs/modbus.md §3.5).  Capabilities and devices state what is
 * physically present and move only by upload; a plan states what is being
 * watched, which is a decision, and decisions made at authoring time are not
 * the only ones worth making.
 *
 * AN EDIT IS A RECORD EDIT, AND IT PERSISTS.  There is no overlay, no second
 * source of plan truth and no new flash mechanism: creating, modifying or
 * deleting a plan rewrites the record stream into the inactive region with the
 * plan section replaced, header last, then the caller swaps — the same A/B path
 * an upload takes, run record→record instead of JSON→record.  Everything that
 * already protects an upload protects an edit: the region being written is the
 * one nobody is walking, a torn write never validates, and the swap is hot.
 *
 * The differences from an upload are that the source is records rather than
 * JSON and that the transformation cannot fail on parse — everything it emits
 * was already valid, and the new plan is validated against the same counts
 * before the rewrite begins.
 *
 * It costs one 16 KB region write per edit, which is why plan editing is an
 * operator- and init-time affordance and not something a consumer should do
 * per sample.
 */
#ifndef MODBUS_CONFIG_PLANS_H_
#define MODBUS_CONFIG_PLANS_H_

#include "modbus_records.h"
#include "nvdb.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Why a plan edit was refused.  These map onto the API's eModbusErr in
 * App/Modbus/modbus.c; they are separate because Shared/ does not know the
 * API's code set. */
typedef enum {
    mbPlan_ok            =  0,
    mbPlan_errBadArg     = -1,   /* null, or bounds exceeded                */
    mbPlan_errNoCap      = -2,   /* no such capability                      */
    mbPlan_errNoDevice   = -3,   /* a device in the set does not exist      */
    mbPlan_errDeviceCap  = -4,   /* ...or implements another capability     */
    mbPlan_errNoPoint    = -5,   /* a pointId past the capability's count   */
    mbPlan_errWriteOnly  = -6,   /* a w point cannot be watched             */
    mbPlan_errDuplicate  = -7,   /* a point listed twice in this plan       */
    mbPlan_errPeriod     = -8,   /* period < 1                              */
    mbPlan_errFlash      = -9,
    mbPlan_errStream     = -10,  /* the source region is malformed          */
    mbPlan_errFull       = -11,  /* the rewritten stream will not fit       */
} eModbusPlanErr;

/**
 * @brief  Validate a plan against the config in `base`.
 *
 *         The plan-shaped subset of the compiler's rule list (§7.4), applied
 *         against the same counts: capability exists; every device in the set
 *         exists AND implements that capability; every pointId is inside the
 *         capability and readable; no point appears in two of this plan's time
 *         tables; every period >= 1; table and entry counts within bounds.
 */
eModbusPlanErr MbCfgPlans_Validate(eNvDbUser region, const sModbusPlanSpec *spec);

/**
 * @brief  Rewrite `srcRegion` into `dstRegion` with one plan slot replaced.
 *
 *         `spec == NULL` deletes the slot.  Plans are written in ascending
 *         slot order, skipping free ones, so the section stays
 *         sentinel-terminated with no blank entries — a stored blank would
 *         collide with the name[0] == 0 sentinel and truncate the section at
 *         the first hole (§3.5).
 *
 *         Capabilities, devices and every untouched plan are copied VERBATIM.
 *         The region header is written last, so a torn rewrite never
 *         validates.
 *
 * @param  kick  called before each sector erase (IWDG); may be NULL
 */
eModbusPlanErr MbCfgPlans_Rewrite(eNvDbUser srcRegion, eNvDbUser dstRegion,
                                  uint8_t slot, const sModbusPlanSpec *spec,
                                  void (*kick)(void));

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_CONFIG_PLANS_H_ */
