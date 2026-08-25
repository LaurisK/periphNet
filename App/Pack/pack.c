/*
 * pack.c
 *
 * The pack module's surface: instance table, subscriptions, command claim,
 * publish/commit, dispatch, persistence and statistics
 * (docs/design_battery_pack.md §10, §11, §13).
 *
 * This file owns everything every battery has in common and nothing any
 * vendor has in particular.  It never learns what a holding register or a CAN
 * id is; a TYPE does, behind pack_type.h.
 *
 * STATUS: SCAFFOLDING.  No instance table, no task wiring, no nvDb access.
 * Every entry point returns a defined failure, and nothing here is called
 * from the running firmware: Pack_Init is deliberately NOT invoked from
 * App_DefaultTaskEntry, no task is created, and nothing is registered with
 * sysmon.  The module compiles into application.elf and does nothing.
 *
 * WHAT STILL BLOCKS A REAL IMPLEMENTATION, both tracked in the design:
 *   - PERSISTENCE.  nvdbUser_packCfg and nvdbUser_packState do not exist yet.
 *     Adding them means bumping NVDB_TARGET_VER to 2 and a TWO-BOOT migration
 *     on every deployed board — the first boot after the OTA has no space and
 *     must read as unprovisioned, the second has the areas
 *     (docs/design_battery_pack.md §14).  Shared/NvDb is untouched here on
 *     purpose.
 *   - THE CAN RX DISPATCHER.  App/Can/bms_reader.c already defines the single
 *     weak HAL_CAN_RxFifo0MsgPendingCallback; pack_pylontech is blocked until
 *     App/Can grows one dispatcher that owns it (§16 item 5).
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Pack/pack.h"
#include "App/Pack/pack_cfg.h"
#include "App/Pack/pack_fsm.h"
#include "App/Pack/pack_type.h"

#include <stddef.h>
#include <string.h>

/* Private defines ----------------------------------------------------------*/

#define PACK_UNUSED(x)   ((void)(x))

/* Private variables --------------------------------------------------------*/

/* The event-id base and post function handed down by App/Func/func.c.  The
 * module never sees the queue or the task — only these two (§13). */
static uint16_t     s_evtIdBase;
static fFuncPostEvt s_post;

/* Exported functions -------------------------------------------------------*/

int Pack_Init(uint16_t evtIdBase, fFuncPostEvt post)
{
    if (NULL == post) {
        return packErr_badArg;
    }
    s_evtIdBase = evtIdBase;
    s_post      = post;

    /* STUB.  A real Pack_Init loads nvdbUser_packCfg, builds every instance
     * and binds each to its type.  It does none of that, so the board stays
     * unprovisioned — which is a valid, first-class state (§10.9) and
     * therefore the correct thing for a stub to leave behind. */
    return packErr_unprovisioned;
}

int Pack_Count(void)
{
    return 0;       /* STUB — unprovisioned */
}

int Pack_FindByName(const char *name)
{
    if (NULL == name) {
        return packErr_badArg;
    }
    return packErr_notFound;    /* STUB */
}

int Pack_GetState(uint8_t idx, sPackState *out)
{
    PACK_UNUSED(idx);

    if (NULL == out) {
        return packErr_badArg;
    }
    /* STUB.  Zeroed would claim every age is 0 — "delivered, this instant" —
     * which is the one lie PACK_AGE_NEVER exists to prevent, so the stub
     * fails rather than filling anything. */
    return packErr_notFound;
}

int Pack_GetCells(uint8_t idx, sPackCells *out)
{
    PACK_UNUSED(idx);

    if (NULL == out) {
        return packErr_badArg;
    }
    return packErr_notFound;    /* STUB */
}

int Pack_Subscribe(uint32_t evtMask, fPackSubscriber cb, void *ctx)
{
    PACK_UNUSED(evtMask);
    PACK_UNUSED(ctx);

    if (NULL == cb) {
        return packErr_badArg;
    }
    return packErr_full;        /* STUB — no table to claim a slot in */
}

int Pack_Unsubscribe(int handle)
{
    PACK_UNUSED(handle);
    return packErr_notFound;    /* STUB.  NOTE the real one owes the caller a
                                   final packEvt_released before it returns —
                                   that call is the release point after which
                                   ctx may be freed */
}

int Pack_Command(uint8_t idx, const sPackCommand *cmd, uint32_t timeout_ms,
                 fPackCmdDone done, void *ctx)
{
    PACK_UNUSED(idx);
    PACK_UNUSED(cmd);
    PACK_UNUSED(timeout_ms);
    PACK_UNUSED(done);
    PACK_UNUSED(ctx);

    /* STUB — refuse.  packErr_ok would mean ACCEPTED, and an accepted command
     * owes the caller a `done` within timeout_ms plus one tick (§10.9 rule 6)
     * that nothing here can deliver. */
    return packErr_notSupported;
}

int Pack_CommandBounds(uint8_t idx, ePackCmdId cmd, int32_t *min, int32_t *max)
{
    PACK_UNUSED(idx);
    PACK_UNUSED(cmd);

    if (NULL == min || NULL == max) {
        return packErr_badArg;
    }
    return packErr_notSupported;    /* STUB */
}

int Pack_ConfigVerify(fPackByteSource src, void *srcCtx, sPackCfgResult *res)
{
    /* Verify and Apply share ONE parser: this is already the real routing,
     * and only the commit half is missing. */
    static sPackCfg scratch;

    if (NULL == src || NULL == res) {
        return packErr_badArg;
    }
    return PackCfg_Parse(src, srcCtx, &scratch, res);
}

int Pack_ConfigApply(fPackByteSource src, void *srcCtx, sPackCfgResult *res)
{
    static sPackCfg scratch;
    int             rc;

    if (NULL == src || NULL == res) {
        return packErr_badArg;
    }
    rc = PackCfg_Parse(src, srcCtx, &scratch, res);
    if (packErr_ok != rc) {
        return rc;
    }
    /* STUB.  The commit would: write nvdbUser_packCfg through NvRecord_Save,
     * complete every outstanding command packErr_unknownOutcome (§10.9 rule
     * 8), unbind and rebind every type, and raise packEvt_config.  That user
     * does not exist yet — see the file header on the §14 two-boot
     * migration. */
    return packErr_unprovisioned;
}

int Pack_ConfigExport(fPackByteSink sink, void *ctx)
{
    PACK_UNUSED(ctx);

    if (NULL == sink) {
        return packErr_badArg;
    }
    return packErr_unprovisioned;   /* STUB — nothing active to serialise */
}

int Pack_ConfigErase(void)
{
    /* STUB.  Erase, not Reset: with no built-in default there is nothing to
     * reset TO.  There is also nothing stored, so this is a no-op that
     * reports the state it leaves behind. */
    return packErr_unprovisioned;
}

int Pack_Stats(sPackStats *out)
{
    if (NULL == out) {
        return packErr_badArg;
    }
    (void)memset(out, 0, sizeof(*out));
    out->provisioned = 0u;
    return packErr_ok;      /* The only honest success here: the counters
                               really are all zero, because nothing ran */
}

void Pack_LogStatus(void)
{
    /* STUB — no instances to log.  The real one prints one line per instance
     * (name, type, cond and `why` when not online, caps, per-group ages) plus
     * the counters, and uses Trice, so it stays off every lwIP callback. */
}

/* ==========================================================================
 * The type-facing half (§11.1).  Legal from ANY context including an ISR,
 * which is what lets one core serve a pull type publishing from the modbus
 * task and a push type publishing from its CAN RX ISR without telling them
 * apart.
 * ========================================================================== */

int PackType_Register(const sPackType *type)
{
    if (NULL == type || NULL == type->bind || NULL == type->name) {
        return packErr_badArg;
    }
    if ((uint32_t)type->id >= (uint32_t)packType_last) {
        return packErr_badArg;
    }
    return packErr_full;    /* STUB — there is no registry yet */
}

sPackRaw *PackType_Staging(uint8_t idx)
{
    PACK_UNUSED(idx);
    return NULL;            /* STUB — every instance reads as unbound */
}

sPackCells *PackType_CellStaging(uint8_t idx)
{
    PACK_UNUSED(idx);
    return NULL;            /* STUB */
}

void PackType_Publish(uint8_t idx, uint32_t groups)
{
    PACK_UNUSED(idx);
    PACK_UNUSED(groups);
    /* STUB.  The real one is the ONE place that chooses between the task- and
     * ISR-context critical section and between the two queue posts, copies
     * ~90 B under the lock, records the groups through PackFsm_NotePublish,
     * and posts packInt_stateCommitted.  It never blocks. */
}

void PackType_PublishCells(uint8_t idx)
{
    PACK_UNUSED(idx);
    /* STUB — separate from Publish because the cell group has its own clock. */
}

void PackType_CommandDone(uint8_t idx, ePackErr result)
{
    PACK_UNUSED(idx);
    PACK_UNUSED(result);
    /* STUB.  The real one routes through PackFsm_CmdComplete, which is what
     * recognises a LATE completion and discards-and-counts it rather than
     * writing it into a decision the consumer already heard (§11.1). */
}

void PackType_NoteLiveness(uint8_t idx, int answered)
{
    PACK_UNUSED(idx);
    PACK_UNUSED(answered);
    /* STUB — feeds condition and confidence WITHOUT faking a value. */
}
