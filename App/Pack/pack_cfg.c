/*
 * pack_cfg.c
 *
 * The uploaded pack configuration — streaming parse, re-serialise, and the
 * record shape (docs/design_battery_pack.md §12).
 *
 * LIBC ONLY.  No nvDb, no RTOS, no HTTP: this file turns bytes into an
 * sPackCfg and back, and pack.c is what persists the result.  That split is
 * what lets tests/ drive the whole §12 accept/reject matrix on the host.
 *
 * STATUS: SCAFFOLDING.  Every body rejects.  A configuration parser that
 * defaulted to "accepted" would silently provision a board with whatever was
 * on the wire, so the failing direction is the safe one.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Pack/pack_cfg.h"

#include <stddef.h>
#include <string.h>

/* Private defines ----------------------------------------------------------*/

#define PACK_CFG_UNUSED(x)   ((void)(x))

/* Private functions --------------------------------------------------------*/

/**
 * @brief Fill a result struct with one failure, so every reject path reports
 *        the same shape.
 */
static void SetFailure(sPackCfgResult *res, int packIdx, const char *field,
                       const char *reason, uint8_t packs)
{
    if (NULL == res) {
        return;
    }
    res->ok      = 0;
    res->packIdx = packIdx;
    res->packs   = packs;

    res->field[0] = '\0';
    if (NULL != field) {
        (void)strncpy(res->field, field, sizeof(res->field) - 1u);
        res->field[sizeof(res->field) - 1u] = '\0';
    }
    res->reason[0] = '\0';
    if (NULL != reason) {
        (void)strncpy(res->reason, reason, sizeof(res->reason) - 1u);
        res->reason[sizeof(res->reason) - 1u] = '\0';
    }
}

/* Exported functions -------------------------------------------------------*/

int PackCfg_Parse(fPackByteSource src, void *srcCtx, sPackCfg *out,
                  sPackCfgResult *res)
{
    PACK_CFG_UNUSED(src);
    PACK_CFG_UNUSED(srcCtx);

    /* STUB.  `out` is deliberately left untouched: a parser that half-filled
     * a destination it then rejected is exactly the half-applied
     * configuration §12 exists to prevent. */
    PACK_CFG_UNUSED(out);

    SetFailure(res, -1, "json", "pack config parser not implemented", 0u);
    return packErr_badArg;
}

int PackCfg_Serialize(const sPackCfg *cfg, fPackByteSink sink, void *ctx)
{
    PACK_CFG_UNUSED(cfg);
    PACK_CFG_UNUSED(sink);
    PACK_CFG_UNUSED(ctx);
    return packErr_badArg;      /* STUB — emits nothing */
}

const sPackTypeLimits *PackCfg_TypeLimits(uint8_t typeId)
{
    PACK_CFG_UNUSED(typeId);
    return NULL;                /* STUB — no type has limits, so every
                                   `commands` block reads as unbounded */
}

int PackCfg_TypeIdFromName(const char *name, uint8_t *out)
{
    PACK_CFG_UNUSED(name);
    PACK_CFG_UNUSED(out);
    return packErr_notFound;    /* STUB */
}

const char *PackCfg_TypeName(uint8_t typeId)
{
    PACK_CFG_UNUSED(typeId);
    return NULL;                /* STUB */
}

int PackCfg_ChemFromName(const char *name, uint8_t *out)
{
    PACK_CFG_UNUSED(name);
    PACK_CFG_UNUSED(out);
    return packErr_notFound;    /* STUB */
}

const char *PackCfg_ChemName(uint8_t chem)
{
    PACK_CFG_UNUSED(chem);
    return NULL;                /* STUB */
}

int PackCfg_CmdIdFromName(const char *name, ePackCmdId *out)
{
    PACK_CFG_UNUSED(name);
    PACK_CFG_UNUSED(out);
    return packErr_notFound;    /* STUB */
}

const char *PackCfg_CmdName(ePackCmdId cmd)
{
    PACK_CFG_UNUSED(cmd);
    return NULL;                /* STUB */
}
