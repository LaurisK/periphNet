/**
 * @file    batcomm_cfg.h
 * @brief   The battery-communication configuration: defaults, streaming JSON
 *          parse/export, and every BatComm_*Name() accessor.
 *          MODULE-INTERNAL (docs/design_battery_comm.md §5).
 *
 * LIBC ONLY: this file is compiled by tests/ as well as by the firmware and
 * must never learn what a flash area or a CAN cell is.  Persistence lives in
 * batcomm.c.
 */

#ifndef BATCOMM_CFG_H_
#define BATCOMM_CFG_H_

/* Includes -----------------------------------------------------------------*/

#include "App/BatComm/batcomm.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported constants -------------------------------------------------------*/

/** Bumped whenever sBatCommCfg's field meanings move.  A newer document is
 *  REFUSED rather than reinterpreted through this image's meanings. */
#define BATCOMM_CFG_VERSION     1u

/* Exported functions -------------------------------------------------------*/

/** @brief Put every tunable at its documented default. */
void BatCommCfg_Defaults(sBatCommCfg *cfg);

/**
 * @brief Parse one configuration document.
 * @retval batErr_ok, batErr_badArg — `res` names the offending key
 */
int BatCommCfg_Parse(fBatCommByteSource src, void *srcCtx, sBatCommCfg *out,
                     sBatCommCfgResult *res);

/** @brief Re-serialise a configuration.  Data-faithful, not byte-identical. */
int BatCommCfg_Serialize(const sBatCommCfg *cfg, fBatCommByteSink sink,
                         void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* BATCOMM_CFG_H_ */
