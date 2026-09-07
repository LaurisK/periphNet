/**
 * @file    cluster_calc.h
 * @brief   THE PURE CORE: the closed-loop limit search and its gate, the
 *          restart triggers, the input sanitiser, the slew and all
 *          aggregation.  MODULE-INTERNAL (docs/design_battery_cluster.md §2).
 *
 * LIBC ONLY.  NO FreeRTOS, NO HAL, NO Trice, NO flash — AND NO Pack_.  A call
 * into the live pack module is a call into a lock, and would make this file
 * non-pure and untestable at once, so its ENTIRE input is the sClusterPackIn
 * array the caller fills.  pack_fsm.c is the precedent and the shape.
 *
 * TIME IS ALWAYS AN ARGUMENT, never read.  Every age is an unsigned
 * difference; nothing compares two absolute stamps, so 2^32 ms (~49.7 days)
 * wraps harmlessly and a host test can drive the clock.
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
typedef struct {                                    /* 112 B                 */
    uint32_t loopCharge_mA, loopDischarge_mA;    /* what the loop holds      */
    uint32_t pubCharge_mA, pubDischarge_mA;      /* after the slew           */
    uint32_t lastTick_ms;
    uint32_t bindChgTick_ms, bindDsgTick_ms;     /* last binding sample      */
    uint32_t prevLimitChg_mA[CLUSTER_PACK_MAX];  /* deadband reference,
                                                    LATCHED AT RESTART and
                                                    only at restart          */
    uint32_t prevLimitDsg_mA[CLUSTER_PACK_MAX];
    uint32_t prevPartChg, prevPartDsg;           /* participation bitmask —
                                                    join / leave detection   */
    uint16_t lastLoadMaxChg_pm, lastLoadMaxDsg_pm;
    uint8_t  chgForbidden, dsgForbidden;         /* forbidden->allowed edge  */
    uint8_t  chgBindSeen, dsgBindSeen;
    uint8_t  started;
    uint8_t  pendingRestart;                     /* eClusterRestart the
                                                    CALLER saw — adoption is
                                                    the one trigger the pure
                                                    core cannot observe       */
    uint8_t  rsvd[2];
} sClusterCalcState;

/** CALLER-OWNED WORKING MEMORY.  Without this the pure core would need file
 *  statics and "the state is the only thing carried" would be unprovable. */
typedef struct {                                    /* 96 B                  */
    uint32_t load_pm[CLUSTER_PACK_MAX];
    uint32_t absI_mA[CLUSTER_PACK_MAX];
    uint16_t share_pm[CLUSTER_PACK_MAX];
    uint8_t  partChg[CLUSTER_PACK_MAX];
    uint8_t  partDsg[CLUSTER_PACK_MAX];
} sClusterScratch;

/** PER-TICK EVENTS the published snapshot does not carry.  They exist so
 *  cluster.c can keep its counters without re-deriving them from the output,
 *  which is how two views of one tick drift apart. */
typedef enum {
    cluCalcEv_stepClampedChg = 1u << 0,
    cluCalcEv_stepClampedDsg = 1u << 1,
    cluCalcEv_slewChg        = 1u << 2,
    cluCalcEv_slewDsg        = 1u << 3,
    cluCalcEv_bindingChg     = 1u << 4,
    cluCalcEv_bindingDsg     = 1u << 5,
    cluCalcEv_restartChg     = 1u << 6,
    cluCalcEv_restartDsg     = 1u << 7,
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
 * @brief  One whole aggregation pass: sanitise, classify, run both loops,
 *         aggregate, detect divergence.
 *
 * @param  in - n pack inputs, slot-indexed
 * @param  n - configured member count, 0..CLUSTER_PACK_MAX
 * @param  tune - the live tunables
 * @param  st - carried state; zeroed by the caller means "start of life"
 * @param  scratch - caller-owned working memory, contents undefined on entry
 * @param  now_ms - monotonic milliseconds
 * @param  out - written in full; `seq` is the caller's to stamp
 * @retval cluErr_ok, cluErr_badArg
 */
int ClusterCalc_Solve(const sClusterPackIn *in, uint8_t n,
                      const sClusterTune *tune, sClusterCalcState *st,
                      sClusterScratch *scratch, uint32_t now_ms,
                      sClusterResult *out);

/**
 * @brief  Force the next Solve to restart both searches.
 *
 * The caller uses this for cluRestart_configAdopted, which it alone can see.
 * Resets the WHOLE state: every value in it is slot-indexed and a re-pointed
 * slot makes all of them meaningless.
 */
void ClusterCalc_Reset(sClusterCalcState *st, uint8_t reason);

#ifdef __cplusplus
}
#endif

#endif /* CLUSTER_CALC_H_ */
