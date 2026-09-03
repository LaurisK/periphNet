/*
 * can_log.c
 *
 *  Created on: 2026-09-03
 *      Author: Lauris
 *
 * See can_log.h for the shape and the "why".  Two implementation rules carry
 * the whole file:
 *
 * NO ERASE EVER SITS ON THE HOT PATH.  A unit is only ever written into once
 * `TryOpenUnit` has confirmed — by reading it back — that it is genuinely
 * erased.  The moment a unit is opened, the NEXT one is handed to
 * `NvDb_Delete` (a single mark, returns immediately) so nvDb's collector has
 * the whole time this unit takes to fill — minutes to hours — to erase the
 * one after it. `CanLog_Service()` therefore never does anything slower than
 * a bit-clearing 16-byte page program, which is what makes it safe to run
 * every ~100 ms on `defaultTask`, the same task that kicks the IWDG.
 *
 * READS NEVER TRUST RAM BOOKKEEPING.  `CanLog_ReadRec` recomputes which
 * physical unit a record would live in and CHECKS that unit's header before
 * trusting it — an overwritten record is detected because the header's
 * `unitSeq` no longer matches what the requested `recNo` implies, not because
 * anything was watching it get overwritten.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Can/can_log.h"
#include "App/Can/can_monitor.h"

#include "nvdb.h"

#include "cmsis_os.h"
#include "stm32f4xx_hal.h"

#include <string.h>

/* Private defines ----------------------------------------------------------*/

/** One unit is one header slot plus CANLOG_RECS_PER_UNIT data slots.  4096 B
 *  is chosen to line up with the medium's usual erase granularity so a unit
 *  delete costs exactly one real erase — but that is an efficiency footnote,
 *  not a dependency: nvDb hides the real granularity, and a mismatch would
 *  only ever cost nvDb an extra partial-unit erase, never correctness. */
#define CANLOG_UNIT_BYTES       (((uint32_t)CANLOG_RECS_PER_UNIT + 1u) * \
                                (uint32_t)sizeof(sCanLogRec))

/** How many staged records one Service() tick will drain.  Bounds a tick's
 *  worst case to a handful of small page programs — comfortably under the
 *  100 ms period even at a few ms each. */
#define CANLOG_MAX_FLUSH_PER_TICK  16u

#define CANLOG_AREA_MAGIC       0x474F4C43u   /* "CLOG", little-endian bytes */
#define CANLOG_UNIT_MAGIC       0x54494E55u   /* "UNIT", little-endian bytes */
#define CANLOG_AREA_VERSION     1u

#define CANLOG_ERASED_U32       0xFFFFFFFFu

/* Private types --------------------------------------------------------------
 * Both headers are exactly one record slot (16 B) so they consume no space
 * of their own beyond the slot they occupy. */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t recSize;
    uint32_t dataUnits;     /* informational: dataUnits when last (re)built */
} sCanLogAreaHdr;

typedef struct {
    uint32_t magic;
    uint32_t unitSeq;       /* global, monotonic, never reused              */
    uint32_t bootId;        /* which boot opened this unit                  */
    uint32_t reserved;
} sCanLogUnitHdr;

_Static_assert(sizeof(sCanLogRec) == 16u, "can_log record size drifted");
_Static_assert(sizeof(sCanLogAreaHdr) == sizeof(sCanLogRec),
              "area header must occupy exactly one record slot");
_Static_assert(sizeof(sCanLogUnitHdr) == sizeof(sCanLogRec),
              "unit header must occupy exactly one record slot");

typedef struct {
    uint32_t lastLoggedStamp_ms;
    uint8_t  seen;
} sCanLogRateState;

typedef struct {
    sCanLogRec buf[CANLOG_STAGE_DEPTH];
    uint8_t    head;
    uint8_t    tail;
    uint8_t    count;
} sCanLogStage;

/* Private variables ----------------------------------------------------------
 * Plain .bss: main SRAM, not CCM.  The rate table is
 * canBus_last * CANMON_IDS_MAX entries (96 B) and the stage ring 512 B --
 * both trivial next to the reasons CCM stays off-limits elsewhere. */

static eCanLogMode   s_mode         = canLogMode_changes;
static uint32_t      s_minGap_ms    = CANLOG_MIN_GAP_MS;
static uint32_t      s_heartbeat_ms = CANLOG_HEARTBEAT_MS;
static eCanLogState  s_state        = canLogState_idle;

static uint32_t      s_areaSize_bytes;
static uint32_t      s_dataUnits;
static uint32_t      s_curUnitSeq;
static uint32_t      s_curPhysUnit;
static uint32_t      s_curSlot;      /* next free DATA slot, 0..RECS_PER_UNIT-1 */
static uint32_t      s_nextRec;
static uint32_t      s_bootId;

static uint32_t      s_droppedCnt;   /* stage ring was full (ISR side)        */
static uint32_t      s_stalledCnt;   /* Service() found the next unit not
                                        erased yet (task side)                */
static uint32_t      s_writeErrCnt;

static sCanLogStage      s_stage;
static sCanLogRateState  s_rate[(uint32_t)canBus_last * CANMON_IDS_MAX];

static const char *const s_modeName[canLogMode_last] = {
    "off", "changes", "all"
};
static const char *const s_stateName[canLogState_last] = {
    "idle", "provisioning", "running"
};

/* Private function prototypes ------------------------------------------------*/

static uint32_t EnterCritical(void);
static void     ExitCritical(uint32_t saved);
static int      TryOpenUnit(uint32_t unitSeq);
static void     Sink(const sCanFrame *frame, eCanDir dir, uint8_t rowIdx,
                     uint8_t changed, void *ctx);

/* Private functions ----------------------------------------------------------*/

static uint32_t EnterCritical(void)
{
    if (0u != __get_IPSR()) {
        return taskENTER_CRITICAL_FROM_ISR();
    }
    taskENTER_CRITICAL();
    return 0u;
}

static void ExitCritical(uint32_t saved)
{
    if (0u != __get_IPSR()) {
        taskEXIT_CRITICAL_FROM_ISR(saved);
    } else {
        (void)saved;
        taskEXIT_CRITICAL();
    }
}

/**
 * @brief  Open @p unitSeq for writing if its physical slot is erased.
 *
 * Peeks the slot rather than trusting a completion callback: the delete that
 * should have erased it was issued a whole unit-fill ago, so by the time this
 * is called it is almost always already done, and a peek costs one 16-byte
 * read either way.
 *
 * @retval 0 opened; -1 not erased yet, or the medium failed
 */
static int TryOpenUnit(uint32_t unitSeq)
{
    uint32_t       physUnit = 1u + (unitSeq % s_dataUnits);
    uint32_t       base     = physUnit * CANLOG_UNIT_BYTES;
    sCanLogUnitHdr probe;
    sCanLogUnitHdr hdr;
    uint32_t       nextPhys;

    if (NvDb_Read(nvdbUser_canLog, &probe, base, sizeof(probe)) != nvdbRes_ok) {
        return -1;
    }
    if (probe.magic != CANLOG_ERASED_U32 || probe.unitSeq != CANLOG_ERASED_U32 ||
        probe.bootId != CANLOG_ERASED_U32 || probe.reserved != CANLOG_ERASED_U32) {
        return -1;     /* the collector has not reached it yet */
    }

    hdr.magic    = CANLOG_UNIT_MAGIC;
    hdr.unitSeq  = unitSeq;
    hdr.bootId   = s_bootId;
    hdr.reserved = 0u;
    if (NvDb_Write(nvdbUser_canLog, &hdr, base, sizeof(hdr)) != nvdbRes_ok) {
        s_writeErrCnt++;
        return -1;
    }

    s_curUnitSeq  = unitSeq;
    s_curPhysUnit = physUnit;
    s_curSlot     = 0u;

    /* Delete-ahead: hand the NEXT physical unit to the collector now, so it
     * has this whole unit's fill time -- not a write's worth of time -- to
     * erase it. */
    nextPhys = 1u + ((unitSeq + 1u) % s_dataUnits);
    (void)NvDb_Delete(nvdbUser_canLog, nextPhys * CANLOG_UNIT_BYTES,
                      CANLOG_UNIT_BYTES, NULL);
    return 0;
}

/**
 * @brief  Decide whether this frame earns a record, and stage it if so.
 * @note   ALWAYS ISR CONTEXT (the monitor's sink contract).  Never blocks.
 */
static void Sink(const sCanFrame *frame, eCanDir dir, uint8_t rowIdx,
                 uint8_t changed, void *ctx)
{
    eCanLogMode       mode = s_mode;
    sCanLogRateState *rs;
    uint32_t          now_ms;
    uint32_t          saved;
    int               log = 0;

    (void)ctx;
    if ((mode == canLogMode_off) || (frame->bus >= (uint8_t)canBus_last) ||
        (rowIdx >= CANMON_IDS_MAX)) {
        return;
    }

    now_ms = HAL_GetTick();
    rs     = &s_rate[((uint32_t)frame->bus * CANMON_IDS_MAX) + rowIdx];

    if (mode == canLogMode_all) {
        log = 1;
    } else if (rs->seen == 0u) {
        log = 1;                                  /* first sighting, always */
    } else if ((changed != 0u) &&
              ((s_minGap_ms == 0u) ||
               ((now_ms - rs->lastLoggedStamp_ms) >= s_minGap_ms))) {
        log = 1;
    } else if ((s_heartbeat_ms != 0u) &&
              ((now_ms - rs->lastLoggedStamp_ms) >= s_heartbeat_ms)) {
        log = 1;                                  /* proves a constant is still there */
    }

    if (log == 0) {
        return;
    }
    rs->seen               = 1u;
    rs->lastLoggedStamp_ms = now_ms;

    saved = EnterCritical();
    if (s_stage.count >= CANLOG_STAGE_DEPTH) {
        s_droppedCnt++;
    } else {
        sCanLogRec *rec = &s_stage.buf[s_stage.head];

        rec->stamp_ms = now_ms;
        rec->id       = (uint16_t)(frame->id & 0xFFFFu);
        rec->meta     = (uint8_t)((frame->bus & 0x01u)      |
                                  ((dir == canDir_rx ? 0u : 1u) << 1) |
                                  ((frame->rtr & 0x01u) << 2) |
                                  ((frame->ext & 0x01u) << 3) |
                                  ((frame->dlc & 0x0Fu) << 4));
        rec->reserved = 0u;
        memcpy(rec->data, frame->data, 8u);

        s_stage.head = (uint8_t)((s_stage.head + 1u) % CANLOG_STAGE_DEPTH);
        s_stage.count++;
    }
    ExitCritical(saved);
}

/* Exported functions ---------------------------------------------------------*/

int CanLog_Init(void)
{
    eNvDbRes       res;
    uint32_t       size = 0u;
    sCanLogAreaHdr areaHdr;
    uint32_t       maxUnitSeq = 0u;
    uint32_t       maxBootId  = 0u;
    int            found      = 0;
    uint32_t       p;

    res = NvDb_GetSize(nvdbUser_canLog, &size);
    if ((res != nvdbRes_ok) || (size < (2u * CANLOG_UNIT_BYTES))) {
        s_state = canLogState_idle;
        return -1;
    }
    s_areaSize_bytes = size;
    s_dataUnits      = (size / CANLOG_UNIT_BYTES) - 1u;

    if (NvDb_Read(nvdbUser_canLog, &areaHdr, 0u, sizeof(areaHdr)) != nvdbRes_ok) {
        s_state = canLogState_idle;
        return -1;
    }

    if ((areaHdr.magic != CANLOG_AREA_MAGIC) ||
        (areaHdr.version != CANLOG_AREA_VERSION) ||
        (areaHdr.recSize != (uint32_t)sizeof(sCanLogRec))) {
        /* Absent, or a format this build cannot read -- rebuild rather than
         * pretend.  The header write below is the ONE place in this module
         * allowed to erase inline: it runs once, at boot, never on the
         * per-record path. */
        sCanLogAreaHdr fresh;

        fresh.magic     = CANLOG_AREA_MAGIC;
        fresh.version   = CANLOG_AREA_VERSION;
        fresh.recSize   = (uint32_t)sizeof(sCanLogRec);
        fresh.dataUnits = s_dataUnits;
        if (NvDb_Write(nvdbUser_canLog, &fresh, 0u, sizeof(fresh)) != nvdbRes_ok) {
            s_state = canLogState_idle;
            return -1;
        }
        /* The DATA region only -- unit 0 (this header) must never be inside
         * a delete mark, or the collector could erase it out from under a
         * header this module just wrote. */
        (void)NvDb_Delete(nvdbUser_canLog, CANLOG_UNIT_BYTES,
                          size - CANLOG_UNIT_BYTES, NULL);

        s_bootId     = 0u;
        s_curUnitSeq = 0u;
        s_nextRec    = 0u;
        s_state      = canLogState_provisioning;
        CanMon_SetSink(Sink, NULL);
        return 0;
    }

    /* Recovery: find the newest unit this area has ever held.  A unit whose
     * header cannot be read is treated as absent, not fatal -- one bad read
     * must not blind recovery to every other unit. */
    for (p = 1u; p <= s_dataUnits; p++) {
        sCanLogUnitHdr h;

        if (NvDb_Read(nvdbUser_canLog, &h, p * CANLOG_UNIT_BYTES,
                     sizeof(h)) != nvdbRes_ok) {
            continue;
        }
        if (h.magic != CANLOG_UNIT_MAGIC) {
            continue;
        }
        if ((found == 0) || (h.unitSeq > maxUnitSeq)) {
            maxUnitSeq = h.unitSeq;
            maxBootId  = h.bootId;
            found      = 1;
        }
    }

    s_bootId     = found ? (maxBootId + 1u) : 0u;
    s_curUnitSeq = found ? (maxUnitSeq + 1u) : 0u;
    s_nextRec    = s_curUnitSeq * (uint32_t)CANLOG_RECS_PER_UNIT;
    s_state      = canLogState_provisioning;   /* -> running once opened */
    CanMon_SetSink(Sink, NULL);
    return 0;
}

void CanLog_Service(void)
{
    uint32_t flushed = 0u;

    if (s_state == canLogState_idle) {
        return;
    }
    if (s_state != canLogState_running) {
        if (TryOpenUnit(s_curUnitSeq) == 0) {
            s_state = canLogState_running;
        } else {
            s_stalledCnt++;
            return;
        }
    }

    while (flushed < CANLOG_MAX_FLUSH_PER_TICK) {
        sCanLogRec rec;
        uint32_t   saved;
        int        have = 0;
        uint32_t   off;

        saved = EnterCritical();
        if (s_stage.count > 0u) {
            rec  = s_stage.buf[s_stage.tail];
            have = 1;
        }
        ExitCritical(saved);
        if (have == 0) {
            break;
        }

        if (s_curSlot >= (uint32_t)CANLOG_RECS_PER_UNIT) {
            if (TryOpenUnit(s_curUnitSeq + 1u) != 0) {
                s_stalledCnt++;
                break;          /* leave it staged; retry next tick */
            }
        }

        off = (s_curPhysUnit * CANLOG_UNIT_BYTES) +
              ((s_curSlot + 1u) * (uint32_t)sizeof(sCanLogRec));
        if (NvDb_Write(nvdbUser_canLog, &rec, off, sizeof(rec)) != nvdbRes_ok) {
            s_writeErrCnt++;
            break;              /* leave it staged; retry next tick */
        }

        /* Only a WRITTEN record is retired -- this is what makes a failed or
         * deferred write lose nothing. */
        saved = EnterCritical();
        s_stage.tail = (uint8_t)((s_stage.tail + 1u) % CANLOG_STAGE_DEPTH);
        s_stage.count--;
        ExitCritical(saved);

        s_curSlot++;
        s_nextRec++;
        flushed++;
    }
}

int CanLog_SetPolicy(eCanLogMode mode, uint32_t minGap_ms, uint32_t heartbeat_ms)
{
    if ((uint32_t)mode >= (uint32_t)canLogMode_last) {
        return -1;
    }
    s_mode         = mode;
    s_minGap_ms    = minGap_ms;
    s_heartbeat_ms = heartbeat_ms;

    /* A mode change invalidates "have I logged this recently" -- otherwise
     * switching from `all` to `changes` would silently go quiet until each
     * identifier's own rate window happened to elapse. */
    memset(s_rate, 0, sizeof(s_rate));
    return 0;
}

int CanLog_GetStatus(sCanLogStatus *out)
{
    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->mode          = (uint8_t)s_mode;
    out->state         = (uint8_t)s_state;
    out->minGap_ms     = s_minGap_ms;
    out->heartbeat_ms  = s_heartbeat_ms;
    out->areaSize_bytes = s_areaSize_bytes;
    out->bootId        = s_bootId;
    out->droppedCnt    = s_droppedCnt;
    out->stalledCnt    = s_stalledCnt;
    out->writeErrCnt   = s_writeErrCnt;

    if (s_dataUnits > 0u) {
        uint32_t oldestUnitSeq = ((s_curUnitSeq + 1u) >= s_dataUnits)
                                  ? (s_curUnitSeq + 1u - s_dataUnits) : 0u;

        out->capacityRecs = s_dataUnits * (uint32_t)CANLOG_RECS_PER_UNIT;
        out->nextRec      = s_nextRec;
        out->oldestRec    = oldestUnitSeq * (uint32_t)CANLOG_RECS_PER_UNIT;
        out->heldRecs     = s_nextRec - out->oldestRec;
    }
    {
        uint32_t saved = EnterCritical();

        out->staged = s_stage.count;
        ExitCritical(saved);
    }
    return 0;
}

int CanLog_ReadRec(uint32_t recNo, sCanLogRec *out)
{
    uint32_t       unitSeq;
    uint32_t       k;
    uint32_t       physUnit;
    uint32_t       base;
    sCanLogUnitHdr hdr;

    if ((out == NULL) || (s_state == canLogState_idle) || (s_dataUnits == 0u)) {
        return -1;
    }
    unitSeq  = recNo / (uint32_t)CANLOG_RECS_PER_UNIT;
    k        = recNo % (uint32_t)CANLOG_RECS_PER_UNIT;
    physUnit = 1u + (unitSeq % s_dataUnits);
    base     = physUnit * CANLOG_UNIT_BYTES;

    if (NvDb_Read(nvdbUser_canLog, &hdr, base, sizeof(hdr)) != nvdbRes_ok) {
        return -2;
    }
    if ((hdr.magic != CANLOG_UNIT_MAGIC) || (hdr.unitSeq != unitSeq)) {
        return -3;      /* overwritten by a later wrap, or never written */
    }
    if (NvDb_Read(nvdbUser_canLog, out,
                 base + ((k + 1u) * (uint32_t)sizeof(sCanLogRec)),
                 sizeof(*out)) != nvdbRes_ok) {
        return -4;
    }
    if (out->stamp_ms == CANLOG_ERASED_U32) {
        return -5;      /* this boot ended before reaching this slot */
    }
    return 0;
}

int CanLog_Wipe(void)
{
    uint32_t saved;

    if (s_state == canLogState_idle) {
        return -1;
    }
    (void)NvDb_Delete(nvdbUser_canLog, CANLOG_UNIT_BYTES,
                      s_areaSize_bytes - CANLOG_UNIT_BYTES, NULL);

    saved = EnterCritical();
    s_stage.head  = 0u;
    s_stage.tail  = 0u;
    s_stage.count = 0u;
    ExitCritical(saved);
    memset(s_rate, 0, sizeof(s_rate));

    s_curUnitSeq = 0u;
    s_nextRec    = 0u;
    s_state      = canLogState_provisioning;
    return 0;
}

const char *CanLog_ModeName(eCanLogMode mode)
{
    if ((uint32_t)mode >= (uint32_t)canLogMode_last) {
        return "?";
    }
    return s_modeName[mode];
}

int CanLog_ModeFromName(const char *name, eCanLogMode *out)
{
    if ((name == NULL) || (out == NULL)) {
        return -1;
    }
    for (uint8_t i = 0u; i < (uint8_t)canLogMode_last; i++) {
        size_t len = strlen(s_modeName[i]);

        if (strncmp(name, s_modeName[i], len) == 0) {
            char next = name[len];

            if ((next == '\0') || (next == ' ') || (next == '\r') ||
                (next == '\n') || (next == '&')) {
                *out = (eCanLogMode)i;
                return 0;
            }
        }
    }
    return -1;
}

const char *CanLog_StateName(eCanLogState state)
{
    if ((uint32_t)state >= (uint32_t)canLogState_last) {
        return "?";
    }
    return s_stateName[state];
}
