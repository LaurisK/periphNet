/**
 * @file    cluster_calc.h
 * @brief   THE PURE CORE: the measured limit rule, its low-load prediction,
 *          the input sanitiser and all aggregation.  MODULE-INTERNAL
 *          (docs/design_battery_cluster.md §2, revision 3).
 *
 * LIBC ONLY.  NO FreeRTOS, NO HAL, NO Trice, NO flash — AND NO Pack_.  A call
 * into the live pack module is a call into a lock, and would make this file
 * non-pure and untestable at once, so its ENTIRE input is the sClusterPackIn
 * array the caller fills.  pack_fsm.c is the precedent and the shape.
 *
 * TIME IS ALWAYS AN ARGUMENT, never read — and since §14.8 it is not used for
 * arithmetic at all: `now_ms` stamps the snapshot, ages are supplied by the
 * caller inside sClusterPackIn, and no interval is measured anywhere.  The
 * 2^32 ms wrap therefore cannot reach a limit.
 *
 * ZERO FILE STATICS, exactly as pack_fsm.c has none — which is what makes
 * "the state is the only thing carried" provable rather than asserted.  Every
 * working array is caller-owned (sClusterScratch).
 */

#ifndef CLUSTER_CALC_H_
#define CLUSTER_CALC_H_

/* Includes -----------------------------------------------------------------*/

#include "App/Cluster/cluster.h"
#include "App/Cluster/cluster_cfg.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported types -----------------------------------------------------------*/

/** One pack, as the arithmetic is allowed to see it.  Filled by cluster.c
 *  from Pack_GetState alone — Pack_SocDiag is NOT called, because revision 2
 *  deleted the resistance dependency along with the estimators. */
typedef struct {                                    /* 64 B                  */
    uint32_t chargeLimit_mA, dischargeLimit_mA;
    uint32_t chargeVoltLimit_mV, dischargeVoltLimit_mV;
    uint32_t voltage_mV;
    uint32_t remaining_mAh, capacity_mAh, nameplate_mAh;
    uint32_t alarms, caps;
    uint32_t elecAge_ms;
    int32_t  current_mA;
    uint16_t soc_pm, socConf_pm, sohConf_pm;
    int16_t  tempMax_dC, tempMin_dC;
    uint8_t  cond, chargeSwitch, dischargeSwitch;
    uint8_t  packIdx;             /* the core cannot invent it               */
    uint8_t  present;             /* 0 = the name resolved to nothing        */
} sClusterPackIn;

/** Everything the loop carries from one tick to the next.  Slot-indexed
 *  throughout, which is why a configuration adoption must reset ALL of it. */
/* THERE IS NO CARRIED STATE.  `sClusterCalcState` (112 B at revision 2, cut to
 * 16 when the loop went, deleted outright when the rate limiter went — §14.8)
 * held the loop variable, the binding stamps, the deadband references, the
 * participation masks, the ramp-in flags and finally the slew.  Every one of
 * them served a mechanism that no longer exists, and with the last of them the
 * module became a PURE FUNCTION: two calls with the same packs and the same
 * tune produce the same limits, whatever happened before. */

/** CALLER-OWNED WORKING MEMORY.  Without this the pure core would need file
 *  statics and "the state is the only thing carried" would be unprovable.
 *
 *  16 B, DOWN FROM 96.  It held three more arrays — `load_pm`, `absI_mA` and
 *  `share_pm` — of which `absI_mA` was never referenced at all and the other
 *  two were WRITTEN AND NEVER READ: the per-member copies in sClusterMember
 *  are what the arithmetic and the observability surface both use.  They were
 *  the estimator hierarchy's working set and outlived it by two revisions. */
typedef struct {                                    /* 16 B                  */
    uint8_t  partChg[CLUSTER_PACK_MAX];
    uint8_t  partDsg[CLUSTER_PACK_MAX];
} sClusterScratch;

/** PER-TICK EVENTS the published snapshot does not carry.  They exist so
 *  cluster.c can keep its counters without re-deriving them from the output,
 *  which is how two views of one tick drift apart. */
typedef enum {
    cluCalcEv_predictedChg   = 1u << 0,
    cluCalcEv_predictedDsg   = 1u << 1,
} eClusterCalcEvent;

typedef struct {                                    /* 376 B                 */
    sClusterOutput pub;                             /* 112                   */
    sClusterMember member[CLUSTER_PACK_MAX];        /* 256                   */
    uint32_t       ev;                              /* eClusterCalcEvent     */
    uint8_t        sanitised;   /* packs whose values were clamped or dropped */
    uint8_t        rsvd[3];
} sClusterResult;

/* Exported functions -------------------------------------------------------*/

/**
 * @brief  One whole aggregation pass: sanitise, classify, solve both
 *         directions, aggregate, detect divergence.
 *
 * @param  in - n pack inputs, slot-indexed
 * @param  n - configured member count, 0..CLUSTER_PACK_MAX
 * @param  tune - the live tunables
 * @param  scratch - caller-owned working memory, contents undefined on entry
 * @param  now_ms - monotonic milliseconds; STAMPS THE SNAPSHOT AND NOTHING
 *                  ELSE.  No value is aged and no interval is measured, so
 *                  the 2^32 ms wrap cannot affect a limit
 * @param  out - written in full; `seq` is the caller's to stamp
 * @retval cluErr_ok, cluErr_badArg
 */
int ClusterCalc_Solve(const sClusterPackIn *in, uint8_t n,
                      const sClusterTune *tune, sClusterScratch *scratch,
                      uint32_t now_ms, sClusterResult *out);

/* THERE IS NO ClusterCalc_Reset().  A configuration adoption used to need one,
 * to drop slot-indexed state that a re-pointed member set made meaningless.
 * Nothing is indexed by slot across a tick any more, so adopting a new member
 * set is simply the next Solve. */

#ifdef __cplusplus
}
#endif

#endif /* CLUSTER_CALC_H_ */
