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
#define CLUSTER_CFG_VERSION         2u

/* Tune defaults (§3, §3.7).  The live value is always tune.<field>; these are
 * read exactly once, by ClusterCfg_Defaults. */
#define CLUSTER_DFLT_LIMIT_MAX_MA       CLUSTER_LIMIT_MAX_MA
#define CLUSTER_DFLT_VOLT_DIVERGE_MV    500u
/** 8000, NOT 5000.  MEASURED ON BOARD 1, 2026-09-09: the JK's electrical
 *  group refreshes on a ~4.5-5.0 s sawtooth, so a 5000 ms gate has ~200 ms of
 *  margin and dropped the only member ~once every three minutes.  A staleness
 *  gate must be a MULTIPLE of the refresh it guards, not equal to it. */
#define CLUSTER_DFLT_ELEC_MAX_AGE_MS    8000u
/** BELOW THIS THE SHARE RATIO IS NOISE.  It is compared against loadMax_pm —
 *  "no pack is working harder than 10 % of its own limit" — and NOT against
 *  the published value, which is what made revision 2's gate self-referential
 *  and produced the §13.2 ramp-in trap. */
#define CLUSTER_DFLT_LOW_LOAD_FLOOR_PM  100u
/** The geometric discount of the low-load prediction, applied SMALLEST LIMIT
 *  FIRST.  Defaulted to the safety margin because there is no evidence for
 *  making them differ, not because they are the same quantity. */
#define CLUSTER_DFLT_PREDICT_DECAY_PM   900u
#define CLUSTER_DFLT_CHARGE_DERATE_PM   800u        /* the JK's own x0.80     */
#define CLUSTER_DFLT_DISCHARGE_DERATE_PM 850u       /* the JK's own x0.85     */
#define CLUSTER_DFLT_SOC_DIVERGE_PM     200u
#define CLUSTER_DFLT_SHARE_DIVERGE_PM   700u

#define CLUSTER_LOW_LOAD_FLOOR_MAX_PM   500u
#define CLUSTER_SOH_MAX_PM              2000u

/* Exported types -----------------------------------------------------------*/

typedef struct {                                    /* 24 B                  */
    uint32_t limitMax_mA;         /* (0, CLUSTER_LIMIT_MAX_MA]               */
    uint32_t voltDiverge_mV;      /* default 500                             */
    uint16_t elecMaxAge_ms;       /* default 8000                            */
    uint16_t safetyMargin_pm;     /* default 900; (0, 1000]                  */
    uint16_t lowLoadFloor_pm;     /* default 100; [0, 500)                   */
    uint16_t predictDecay_pm;     /* default 900; (0, 1000]                  */
    uint16_t chargeDerate_pm;     /* default 800; (0, 1000]                  */
    uint16_t dischargeDerate_pm;  /* default 850; (0, 1000]                  */
    uint16_t socDiverge_pm;       /* default 200                             */
    uint16_t shareDiverge_pm;     /* default 700 — an OBSERVATION threshold  */
} sClusterTune;

typedef struct {                                    /* 168 B                 */
    char         member[CLUSTER_PACK_MAX][CLUSTER_NAME_LEN];   /* 128        */
    char         profile[CLUSTER_PROFILE_LEN];                 /*  12        */
    sClusterTune tune;                                         /*  24        */
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

/* The eight name accessors.  Declared in cluster.h as Cluster_*Name(); these
 * are the same functions — cluster.c does not re-export them, because unlike
 * the pack module there is no second spelling to keep in step. */

#ifdef __cplusplus
}
#endif

#endif /* CLUSTER_CFG_H_ */
