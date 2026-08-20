/**
 * @file    nvdb_layout.h
 * @brief   The layout: what an operator supplies, and what the board reports.
 *
 * A layout is not anonymous — it carries a name and a version, and the
 * configuration that supplies it carries the mode it is to be applied under.
 * Nothing here is part of the user surface: an nvDb USER never asks any of
 * this.  The person who loaded a configuration and rebooted the board asks
 * it, because loading can fail even when validation passed.
 *
 * A layout is loaded, validated and applied at NvDb_Init(), and only there.
 * A new one may be SUPPLIED at any time; it is APPLIED at the next init.
 * That is why the mode travels with the configuration rather than with a
 * call, and why the outcome has to be persisted — its whole purpose is to be
 * read after the reboot that applied it.
 *
 * See docs/task_nv_db.md §2.6, §2.7, §4.4.
 */
#ifndef NVDB_LAYOUT_H_
#define NVDB_LAYOUT_H_

#include "nvdb.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVDB_LAYOUT_NAME_LEN    16u

/* ==========================================================================
 * Mode and status
 * ========================================================================== */

typedef enum {
    nvdbMode_undefined = 0,
    nvdbMode_normal,            /* refuse unless the relayout is clean       */
    nvdbMode_forced,            /* apply anyway; truncation is accepted      */
    nvdbMode_last
} eNvDbApplyMode;

typedef struct {
    uint16_t        nvdbVer;                    /* the module's own version  */
    char            layoutName[NVDB_LAYOUT_NAME_LEN];
    uint16_t        layoutVer;
    eNvDbApplyMode  lastApplyMode;
    eNvDbRes        lastApplyResult;            /* nvdbRes_refused = the
                                                 * previous layout stands    */
} sNvDbStatus;

/* ==========================================================================
 * What is supplied
 * ==========================================================================
 * The enum is the index: there is no per-entry id and no count, because the
 * array is nvdbUser_last long and a user's own value is its position.  A user
 * that is not allocated is simply 0, which is the same fact NvDb_GetSize
 * reports.  size_bytes[nvdbUser_undefined] is unused and must be 0.
 */
typedef struct {
    char            name[NVDB_LAYOUT_NAME_LEN];
    uint16_t        version;
    eNvDbApplyMode  operation;
    uint32_t        size_bytes[nvdbUser_last];
} sNvDbLayoutCfg;

/* ==========================================================================
 * What is read back
 * ==========================================================================
 * The same shape, so a config and its read-back can be diffed by eye, with
 * three differences: no `operation` (once a layout is in force the mode is
 * history — the status carries lastApplyMode), free space alongside rather
 * than inside (it is an answer, not a user), and an onboarding state.
 */
typedef enum {
    nvdbOnboard_undefined = 0,
    nvdbOnboard_none,           /* nothing is waiting                        */
    nvdbOnboard_validated,      /* one is waiting; it passed the advisory
                                 * check.  NOT a promise that it will apply —
                                 * the binding check runs at the next init    */
    nvdbOnboard_forced,         /* one is waiting; it is marked forced        */
    nvdbOnboard_last
} eNvDbOnboardState;

/* Header of a configuration that has come aboard but has not been applied:
 * name, version and operation only — no sizes and no free space of its own. */
typedef struct {
    char            name[NVDB_LAYOUT_NAME_LEN];
    uint16_t        version;
    eNvDbApplyMode  operation;
} sNvDbLayoutHdr;

typedef struct {
    char                name[NVDB_LAYOUT_NAME_LEN];
    uint16_t            version;
    uint32_t            size_bytes[nvdbUser_last];
    uint32_t            freeSpace_bytes;
    eNvDbOnboardState   onboarding;
    sNvDbLayoutHdr      received;      /* valid when onboarding != none      */
} sNvDbLayoutInfo;

/* ==========================================================================
 * Usage — occupancy and wear
 * ==========================================================================
 * Both are REPORTING ONLY.  Neither ever influences an operation (C13),
 * which is what allows both to be cheap and lossy.
 *
 * This is the one place granularity is visible, and only because the answers
 * are meaningless without it: occupancy is observed at erasable-unit
 * granularity, and wear is counted per unit.  None of it reaches nvdb.h — a
 * USER still cannot learn that an erasable unit exists.
 */

typedef struct {
    uint32_t size_bytes;            /* what the layout gave this user        */
    uint32_t occupied_bytes;        /* observed: up to the last written unit */
    uint32_t units;                 /* erasable units the area spans         */
    uint32_t eraseCntMax;           /* worst-worn unit in it                 */
    uint32_t eraseCntTotal;
} sNvDbUsage;

typedef struct {
    uint32_t medium_bytes;
    uint32_t allocated_bytes;       /* sum of every user's area              */
    uint32_t freeSpace_bytes;
    uint32_t unitSize_bytes;
    uint32_t units;
    uint32_t eraseCntMax;           /* worst-worn unit on the whole medium   */
    uint32_t eraseCntTotal;
    uint32_t eraseCntUnsaved;       /* counted but not yet written back      */
} sNvDbMediumUsage;

/* Per-user usage.  BLOCKS: observing occupancy reads the area, and for a
 * 488 KB blob that is not a quick question.  Pass scan = false to skip it and
 * get everything else immediately (occupied_bytes then reads 0). */
eNvDbRes NvDb_GetUsage(eNvDbUser user, bool scan, sNvDbUsage *out);

/* Medium-wide summary.  Does not scan; nothing in it needs to. */
eNvDbRes NvDb_GetMediumUsage(sNvDbMediumUsage *out);

/* ==========================================================================
 * Operator calls
 * ========================================================================== */

/* The status of §2.6, as persisted by the last application of a layout. */
eNvDbRes NvDb_GetStatus(sNvDbStatus *out);

/* The layout in force, plus free space and whatever is waiting to be applied. */
eNvDbRes NvDb_GetLayout(sNvDbLayoutInfo *out);

/* Take a configuration aboard.  Runs the ADVISORY structural check and, if it
 * passes, persists it to be applied at the next init.  Advisory means exactly
 * that: users keep writing between now and then, so a shrink that is safe
 * today may not be safe when it is applied, and flash can fail during the
 * copy besides.  The binding check runs at init and its outcome lands in the
 * status. */
eNvDbRes NvDb_SupplyLayout(const sNvDbLayoutCfg *cfg);

/* Discard a configuration that has come aboard but not yet been applied. */
eNvDbRes NvDb_DropSuppliedLayout(void);

#ifdef __cplusplus
}
#endif

#endif /* NVDB_LAYOUT_H_ */
