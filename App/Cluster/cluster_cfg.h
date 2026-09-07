/**
 * @file    cluster_cfg.h
 * @brief   The cluster configuration: tunables, membership, streaming JSON
 *          parse/export, and every Cluster_*Name() accessor.
 *          MODULE-INTERNAL (docs/design_battery_cluster.md §2).
 *
 * LIBC ONLY, and deliberately so: this file is compiled by tests/ as well as
 * by the firmware, and it must never learn what a flash area is.  Persistence
 * lives in cluster.c.
 *
 * NO Pack_ CALL EITHER.  A call into the live pack module is a call into a
 * lock, and would make this file non-pure and untestable at once.
 */

#ifndef CLUSTER_CFG_H_
#define CLUSTER_CFG_H_

/* Includes -----------------------------------------------------------------*/

#include "App/Cluster/cluster.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported constants -------------------------------------------------------*/

/** Bumped whenever sClusterCfg's field meanings move.  A newer document is
 *  REFUSED rather than reinterpreted through this image's meanings. */
#define CLUSTER_CFG_VERSION         1u

/* Tune defaults (§3, §3.7).  The live value is always tune.<field>; these are
 * read exactly once, by ClusterCfg_Defaults. */
#define CLUSTER_DFLT_RISE_MA_PER_S      5000u       /* 0->200 A in 40 s       */
#define CLUSTER_DFLT_LIMIT_MAX_MA       CLUSTER_LIMIT_MAX_MA
#define CLUSTER_DFLT_HOLD_MAX_AGE_MS    3600000u    /* 1 h                    */
#define CLUSTER_DFLT_VOLT_DIVERGE_MV    500u
#define CLUSTER_DFLT_ELEC_MAX_AGE_MS    5000u
/** THE GATE'S CEILING IS (loadTarget_pm / bindFrac_pm) x min(L_i/f_i), so at
 *  bindFrac == loadTarget the loop provably never publishes above the worst
 *  pack's own limit.  cluster_cfg.c REJECTS bindFrac_pm < loadTarget_pm for
 *  that reason — it is the closed form of review defect B4, in a new
 *  parameter (design §3.2.1, defect L2). */
#define CLUSTER_DFLT_BIND_FRAC_PM       900u
#define CLUSTER_DFLT_LIMIT_DEADBAND_PM  20u
#define CLUSTER_DFLT_CHARGE_DERATE_PM   800u        /* the JK's own x0.80     */
#define CLUSTER_DFLT_DISCHARGE_DERATE_PM 850u       /* the JK's own x0.85     */
#define CLUSTER_DFLT_SOC_DIVERGE_PM     200u
#define CLUSTER_DFLT_SHARE_DIVERGE_PM   700u

#define CLUSTER_DEADBAND_MAX_PM         500u
#define CLUSTER_SOH_MAX_PM              2000u

/* Exported types -----------------------------------------------------------*/

typedef struct {                                    /* 36 B                  */
    uint32_t riseRate_mA_per_s;   /* [CLUSTER_RISE_MIN/MAX_MA_PER_S]         */
    uint32_t limitMax_mA;         /* (0, CLUSTER_LIMIT_MAX_MA]               */
    uint32_t holdMaxAge_ms;       /* default 3 600 000 — needs 32 bits       */
    uint32_t voltDiverge_mV;      /* default 500                             */
    uint16_t elecMaxAge_ms;       /* default 5000                            */
    uint16_t loadTarget_pm;       /* default 900; (0, 1000)                  */
    uint16_t bindFrac_pm;         /* default 900; (0, 1000], >= loadTarget   */
    uint16_t limitDeadband_pm;    /* default 20;  [0, 500)                   */
    uint16_t convergeTol_pm;      /* default 50                              */
    uint16_t chargeDerate_pm;     /* default 800; (0, 1000]                  */
    uint16_t dischargeDerate_pm;  /* default 850; (0, 1000]                  */
    uint16_t socDiverge_pm;       /* default 200                             */
    uint16_t shareDiverge_pm;     /* default 700 — an OBSERVATION threshold  */
    uint16_t rsvd;
} sClusterTune;

typedef struct {                                    /* 180 B                 */
    char         member[CLUSTER_PACK_MAX][CLUSTER_NAME_LEN];   /* 128        */
    char         profile[CLUSTER_PROFILE_LEN];                 /*  12        */
    sClusterTune tune;                                         /*  36        */
    uint16_t     version;
    uint8_t      count;
    uint8_t      flags;
} sClusterCfg;

/* Exported functions -------------------------------------------------------*/

/** @brief Put every tunable at its documented default. */
void ClusterCfg_Defaults(sClusterTune *tune);

/**
 * @brief Parse one configuration document.
 * @retval cluErr_ok, cluErr_badArg — `res` names the member index and key
 */
int ClusterCfg_Parse(fClusterByteSource src, void *srcCtx, sClusterCfg *out,
                     sClusterCfgResult *res);

/** @brief Re-serialise a configuration.  Data-faithful, not byte-identical. */
int ClusterCfg_Serialize(const sClusterCfg *cfg, fClusterByteSink sink,
                         void *ctx);

/* The nine name accessors.  Declared in cluster.h as Cluster_*Name(); these
 * are the same functions — cluster.c does not re-export them, because unlike
 * the pack module there is no second spelling to keep in step. */

#ifdef __cplusplus
}
#endif

#endif /* CLUSTER_CFG_H_ */
