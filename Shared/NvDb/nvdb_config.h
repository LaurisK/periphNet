/**
 * @file    nvdb_config.h
 * @brief   The layout as JSON: what a person writes, and what a board reports.
 *
 * Configuration arrives as JSON and is stored as a structure — the same shape
 * as the Modbus register config, and for the same reason: a human writes the
 * source, the device holds the compiled form.  Parsing is a supply-time
 * operation with a supply-time error (a bad field points at the field);
 * structural validation of the resulting layout is separate and follows it,
 * inside NvDb_SupplyLayout().
 *
 * nvDb serializes ITS OWN configuration and nothing else.  A user's bytes
 * remain opaque.
 *
 *   {
 *     "name": "periphnet",
 *     "version": 3,
 *     "operation": "normal",
 *     "users": { "crashLog": 4096, "fwuStored": 499712 }
 *   }
 *
 * Keyed by user name because a human writes it; a user left out is size 0,
 * which is the same fact NvDb_GetSize reports.
 */
#ifndef NVDB_CONFIG_H_
#define NVDB_CONFIG_H_

#include "nvdb_layout.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVDB_CFG_ERR_LEN    48u

/* Where a parse gave up, in the operator's own vocabulary. */
typedef struct {
    char        field[NVDB_CFG_ERR_LEN];    /* the key that offended         */
    char        reason[NVDB_CFG_ERR_LEN];
    uint32_t    offset_bytes;               /* how far into the document     */
} sNvDbCfgError;

/* Parse a whole document.  `len_bytes` may be 0 for a NUL-terminated string.
 * Returns 0 on success; -1 with `err` filled otherwise.  `err` may be NULL. */
int NvDbCfg_Parse(const char *json, uint32_t len_bytes, sNvDbLayoutCfg *out,
                  sNvDbCfgError *err);

/* Render the read-back form (§2.7).  Returns the number of bytes written,
 * excluding the NUL, or 0 if it did not fit. */
uint32_t NvDbCfg_Render(const sNvDbLayoutInfo *info, const sNvDbStatus *status,
                        char *out, uint32_t cap_bytes);

/* Render the usage report: the medium as a whole, then one object per user
 * that holds anything.  `scan` observes occupancy, which reads every area and
 * is therefore not something to do on a whim.  Returns bytes written
 * excluding the NUL, or 0 if it did not fit. */
uint32_t NvDbCfg_RenderUsage(bool scan, char *out, uint32_t cap_bytes);

/* The two spellings of eNvDbApplyMode, for a renderer and for a parser. */
const char *NvDbCfg_ModeName(eNvDbApplyMode mode);

#ifdef __cplusplus
}
#endif

#endif /* NVDB_CONFIG_H_ */
