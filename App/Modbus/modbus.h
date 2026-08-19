/**
 * @file    modbus.h
 * @brief   PeriphNet Modbus module — THE public API.
 *
 * The module polls Modbus RTU slaves described by an uploadable flash-resident
 * config, decodes their registers, and hands every reading to whoever
 * subscribed to it.  It knows nothing about MQTT, Home Assistant or CAN.
 *
 *     Know what to read and how to read it, translate registers to values and
 *     values back to registers, and hand the result on.
 *
 * Design: docs/modbus.md — §1 the pattern, §4 this surface.  Where this header
 * and that document disagree, the document wins.
 *
 * BOUNDARY (§2.2):
 *   - A consumer includes only this header.  modbus_rtu.h, modbus_walker.h and
 *     modbus_internal.h are module-internal; Shared/Modbus *types*
 *     (modbus_records.h) are fair game, its flash accessors (MbCfg_*,
 *     MbCfgStore_*) are not.
 *   - Files in App/Modbus must not include App/Mqtt, App/Http, App/Can,
 *     App/Data or any lwIP header.
 *   - A device is a config, not a driver (§2.3).  There is no device-specific
 *     code in this module and no way to add any.
 *
 * WHAT THIS MODULE WILL NOT DO, so nobody proposes it again (§1.2): carry
 * publish policy, remember a previous value, judge whether a slave is alive,
 * offer a raw bus write, or take a start/stop/baud/port knob.  Each of those
 * is either config or the consumer's judgement.
 *
 * SURFACE MAP — the whole of §4 lands across §10's steps; this file grows with
 * them and never shrinks:
 *
 *   step 1  (here)  eModbusErr · Modbus_Init · Modbus_Request · Config
 *                   Compile/Apply/Export/Status · diagnostics
 *   step 2          Subscribe/Unsubscribe, the event structs, sModbusPointDesc
 *   step 4          Modbus_RequestCatalogue
 *   step 7          Modbus_ConfigVerify · Modbus_PlanList/Get/Create/Modify/
 *                   Delete
 *   step 6a         the v2 record format, and with it Modbus_ConfigErase —
 *                   the version bump invalidates every region anyway, so the
 *                   built-in default died in the same move
 */

#ifndef MODBUS_H_
#define MODBUS_H_

#include "modbus_records.h"   /* eModbusDecodeType, unit codes, config data */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * Bounds (docs/modbus.md §4.6)
 * ========================================================================== */

#define MB_REQ_MAX_ITEMS       100u   /* items in one Modbus_Request         */
#define MB_REQ_TIMEOUT_MIN_MS    1u
#define MB_REQ_TIMEOUT_MAX_MS 60000u  /* longer than this is a plan, not a
                                         request                             */

/* ==========================================================================
 * Error codes (docs/modbus.md §4.1)
 *
 * mbErr_ok is 0, so `if (items[i].result)` is the idiom.  APPEND-ONLY: not
 * persisted, but it crosses into consumers and the HTTP surface.  No _last
 * sentinel — the values are negative and sparse.
 * ========================================================================== */

typedef enum {
    mbErr_ok                 =   0,
    mbErr_pending            =  -1,  /* request item, not yet decided       */
    mbErr_notAttempted       =  -2,  /* the deadline arrived first          */
    mbErr_timedOut           =  -3,  /* the request's own deadline expired  */
    mbErr_timeout            =  -4,  /* no reply within the port's timeout  */
    mbErr_crc                =  -5,
    mbErr_short              =  -6,  /* reply too short / truncated         */
    mbErr_lineError          =  -7,  /* overrun, framing, parity, overflow  */
    mbErr_txFailed           =  -8,  /* the frame never went out            */
    mbErr_badArg             =  -9,
    mbErr_full               = -10,  /* no slot: subscription, plan, FIFO   */
    mbErr_busy               = -11,  /* plan subscribed, or swap pending    */
    mbErr_idNotFound         = -12,  /* no such device or point             */
    mbErr_outOfRange         = -13,  /* outside writeMin..writeMax          */
    mbErr_config             = -14,  /* no valid config                     */
    mbErr_excIllegalFunction = -20,  /* the slave's own exception codes,    */
    mbErr_excIllegalAddress  = -21,  /*   folded in so a per-item result    */
    mbErr_excIllegalValue    = -22,  /*   says which item got which         */
    mbErr_excDeviceFailure   = -23,
    mbErr_excOther           = -24,
} eModbusErr;

/* ==========================================================================
 * Lifecycle — set it and forget it (docs/modbus.md §4.2)
 * ========================================================================== */

/**
 * @brief  Bring the module up: flash store init, A/B region recovery,
 *         erasure of anything that fails validation, and construction of
 *         whatever devices the config describes.
 *
 *         Timers are NOT started here — they come and go with subscriptions,
 *         so a board that boots with a valid config and no subscribers puts
 *         nothing on the wire.  Zero devices is a valid, first-class state
 *         (reported as *unprovisioned*, not as an error).
 *
 *         Requires W25Q128_Init() to have run.  Idempotent.  There is no
 *         Start/Stop pair, no port selection and no baud accessor: the module
 *         owns its peripherals and the config says what to do with them.
 *
 * @return 0, or a negative eModbusErr
 */
int Modbus_Init(void);

/* ==========================================================================
 * Subscriptions (docs/modbus.md §4.3)
 *
 * A consumer subscribes to a kind of thing, not to a position: `planMask` is a
 * set of plan slots, and a plan names one capability, so every device it covers
 * is the same type of hardware read at the same cadence.
 *
 * A plan nobody subscribes to is not polled at all — the config says what MAY
 * be read, a subscription says what IS read.  What follows: a consumer controls
 * the bus by subscribing and unsubscribing; values lag a reconnect by up to one
 * period; and "why is this device not polled" is a question about subscribers.
 *
 * Scoping is ROUTING, not suppression: dispatch is a bit test, it stores
 * nothing, and within its scope a subscription gets EVERY read.
 *
 * UNTIL §10 STEP 6 there are no plan records, so every subscriber passes
 * MB_PLAN_ALL and scoping is a no-op.  Nothing may hardcode a slot before
 * Modbus_PlanList exists to say which bit is which.
 * ========================================================================== */

#define MB_MAX_SUBS       8u    /* subscription table                        */
#define MB_PLAN_ALL    0xFFu    /* every plan; pins none of them (§3.5)      */
#define MB_PLAN_REQUEST 0xFFu   /* mbEvt_txn.planId: traffic a request
                                   caused, not a timer (§4.4)                */

/* MB_MAX_PLANS (8, because planMask is a uint8_t) and the MB_PT_* access
 * flags come from modbus_records.h: they are properties of the RECORD, and
 * the compiler enforces them there. */

/* The identity and metadata of one point of one device.  Handed out by pointer
 * in events and BORROWED: the engine holds the record it is servicing, so
 * handing it over costs nothing and stores nothing.  It dies when the callback
 * returns.
 *
 * IDENTITY IS {devOrd, ptOrd}, NOT ptOrd ALONE — several devices share one
 * capability, so four battery packs produce four samples with the same ptOrd
 * and different devOrd.  A consumer keying on ptOrd alone collapses them. */
typedef struct {
    const char *topicPrefix;   /* MQTT's display string for the device      */
    const char *name;          /* topic suffix; links nothing, not unique   */
    int32_t     writeMin, writeMax;   /* scaled-int; valid if MB_PT_BOUNDED */
    uint32_t    period_sec;    /* shortest live period; 0 = unwatched       */
    uint16_t    ptOrd;         /* point id within the device's capability   */
    uint8_t     devOrd;        /* device id (position in devices[])         */
    uint8_t     decodeType;    /* eModbusDecodeType                         */
    uint8_t     unit;          /* DLMS/COSEM physical-unit code             */
    int8_t      scalePow10;    /* real value = scaled * 10^scalePow10       */
    uint8_t     flags;         /* MB_PT_READ|WRITE|BOUNDED                  */
} sModbusPointDesc;

typedef enum {
    mbEvt_sample    = 1u << 0,  /* a point was read and decoded            */
    mbEvt_pointDesc = 1u << 1,  /* catalogue entry (§4.5)                  */
    mbEvt_config    = 1u << 2,  /* a new config went live                  */
    mbEvt_txn       = 1u << 3,  /* transaction outcome — diagnostics       */
    mbEvt_released  = 1u << 4,  /* Unsubscribe complete; ctx may be freed  */
    mbEvt_all       = 0x1Fu,
} eModbusEventType;

typedef struct {
    eModbusEventType type;
    uint32_t         tick;              /* HAL_GetTick() when made         */
    union {
        /* ---- mbEvt_sample ------------------------------------------------
         * Raised once per point per SUCCESSFUL READ — not per change.  The
         * module keeps no previous value and does not deduplicate; a consumer
         * wanting change detection does it on its own terms.  Only the decoded
         * value leaves: raw registers never do, because a second raw path
         * would invite consumers to re-implement decoding and get word order,
         * scaling or ASCII subtly wrong.  For ASCII points `text` is the
         * decoded string and `value` is UNSPECIFIED. */
        struct {
            const sModbusPointDesc *pt;
            int32_t     value;          /* scaled integer                  */
            const char *text;           /* ascii points only, else NULL    */
        } sample;

        /* ---- mbEvt_pointDesc ---------------------------------------------
         * The catalogue: one per point of every device in scope, `last` on
         * the final entry.  An empty catalogue is last = 1 with pt == NULL. */
        struct {
            const sModbusPointDesc *pt;
            uint8_t     last;
        } desc;

        /* ---- mbEvt_config ------------------------------------------------
         * A new config went live.  Every cached ordinal is now suspect; a
         * fresh catalogue follows. */
        struct {
            sModbusConfigCounts counts;
            uint8_t     activeRegion;
        } config;

        /* ---- mbEvt_txn ---------------------------------------------------
         * Keyed by {devOrd, planId, timeTableId} — the identity a timer, a
         * sequence and the missed counter all use.  Carries no register
         * payload; values arrive as mbEvt_sample.  A subscriber wanting
         * device availability counts failures here: the module has no basis
         * to judge it, and a device answering EXCEPTIONS is answering. */
        struct {
            uint16_t    addr;           /* first wire address of the block */
            uint16_t    regs;           /* registers requested             */
            uint16_t    elapsed_ms;
            int16_t     err;            /* eModbusErr                      */
            uint8_t     devOrd, slaveAddr;
            uint8_t     planId;         /* MB_PLAN_REQUEST if asked for    */
            uint8_t     timeTableId;
        } txn;
    } u;
} sModbusEvent;

/* ==========================================================================
 * Plans (docs/modbus.md §3.5, §4.3)
 *
 * A plan is the ONE config object the system may change about itself while it
 * runs.  Capabilities and devices state what is physically present and move
 * only by upload; a plan states what is being watched, which is a decision.
 *
 * A CONSUMER FINDS ITS PLANS RATHER THAN ASSUMING THEM: PlanList is what turns
 * a name into a mask bit, so nothing hardcodes a slot against a config it does
 * not author.  A consumer that finds nothing it recognises may CREATE the plan
 * it wants — that is the honest form of "I need this data this often", and it
 * is the same object the operator sees in `modbus plan list`.
 *
 * A PLAN WITH SUBSCRIBERS CANNOT BE MODIFIED OR DELETED (mbErr_busy).  It
 * needs no stored state: "active" is the OR of the plan masks of live
 * subscriptions EXCLUDING wildcards, recomputed on the spot.  Retuning a live
 * plan therefore means unsubscribing, or creating a second plan and moving to
 * it — which is honest, because a consumer's timers and derived blocks were
 * built from the plan it subscribed to and cannot silently change underneath
 * it.  MB_PLAN_ALL pins nothing: a subscriber that asked for every plan
 * expressed no dependency on which plans exist.
 * ========================================================================== */

/* Entries a plan edit may carry across all its time tables. */
#define MB_MAX_TT_ENTRIES_PER_PLAN  128u

typedef struct {
    char     name[MB_NAME_LEN];
    uint16_t capId;
    uint8_t  planId;          /* slot, 0..7                                */
    uint8_t  devices;         /* device set, one bit per deviceId          */
    uint8_t  timeTables;      /* how many this plan holds                  */
    uint8_t  subscribers;     /* 0 = editable; non-zero = mbErr_busy       */
} sModbusPlanInfo;

/* sModbusPlanSpec / sModbusTimeTableSpec come from modbus_records.h: an edit
 * is validated against exactly the rules the compiler applies to an uploaded
 * plan, so there is one validator reached two ways (§7.4). */

/** @brief  Every slot that holds a plan.  Synchronous: plan headers are
 *          resident (§3.5).  Returns the count, or a negative eModbusErr. */
int Modbus_PlanList(sModbusPlanInfo *out, uint8_t max);

/** @brief  One slot.  `subscribers` is counted from the subscription table on
 *          the spot, so it is a live answer rather than a stored one. */
int Modbus_PlanGet(uint8_t planId, sModbusPlanInfo *out);

/**
 * @brief  One plan's time tables, read from flash.
 *
 *         A plan's time tables are NOT in the resident header table (§4.3): a
 *         caller wanting them asks, and the module reads the records like any
 *         other.  `tables[].points` point into `idBuf`.
 *
 * @return table count, or a negative eModbusErr
 */
int Modbus_PlanTables(uint8_t planId, sModbusTimeTableSpec *tables,
                      uint8_t maxTables, uint16_t *idBuf, uint16_t maxIds);

/**
 * @brief  Create a plan in the lowest free slot.
 *
 *         The claim is synchronous, so the call keeps a real error return; the
 *         flash rewrite and the swap that follow are posted.  The plan is
 *         visible in its new form only once they have run, announced by
 *         mbEvt_config plus a fresh catalogue.
 *
 * @return 0 with *outPlanId set, or mbErr_full / mbErr_badArg / mbErr_busy /
 *         mbErr_config
 */
int Modbus_PlanCreate(const sModbusPlanSpec *spec, uint8_t *outPlanId);

/** @brief  Replace a plan.  mbErr_busy if any subscription named it. */
int Modbus_PlanModify(uint8_t planId, const sModbusPlanSpec *spec);

/** @brief  Free a slot.  mbErr_busy if any subscription named it.  Deleting
 *          moves no other plan — that is what makes a slot a slot. */
int Modbus_PlanDelete(uint8_t planId);

/**
 * @brief  Parse one plan object — the same JSON a config's plans[] holds.
 *
 *         Exists so an HTTP surface can take a plan body without reaching past
 *         this header: one schema, one validator (§8.1).  `spec` points into
 *         module storage and is valid until the next call, which is long
 *         enough to hand straight to PlanCreate/PlanModify.
 *
 *         `*outId` is the authored slot, or -1 if the body did not name one
 *         (a PUT takes it from the URL instead).
 */
int Modbus_PlanParse(fModbusByteSource src, void *srcCtx,
                     sModbusPlanSpec *spec, int *outId,
                     sModbusCompileResult *err);

typedef void (*fModbusSubscriber)(const sModbusEvent *ev, void *ctx);

/**
 * @brief  Attach a consumer.  Legal at any time, including before
 *         Modbus_Init.
 *
 *         Does NOT post, deliberately: a posted subscribe cannot return "table
 *         full", which is a real init-time error.  A brief critical section
 *         claims a slot and publishes it last, so the dispatcher sees either a
 *         complete entry or no entry.
 *
 * @param  planMask   plan slots to scope to, or MB_PLAN_ALL
 * @param  eventMask  eModbusEventType bits; mbEvt_released is always
 *                    delivered regardless
 * @return handle >= 0, or mbErr_full / mbErr_badArg
 */
int Modbus_Subscribe(uint8_t planMask, uint32_t eventMask,
                     fModbusSubscriber cb, void *ctx);

/**
 * @brief  Detach.  Posts and returns; legal from inside a callback.
 *
 *         A BORROW ENDS WHEN THE MODULE SAYS IT ENDED.  Clearing the entry
 *         from another task is unsafe for a reason no shared flag can fix — a
 *         dispatcher that has already read the entry is committed to calling
 *         it.  So release is notified: the module makes one final call with
 *         mbEvt_released, and THAT CALL IS THE RELEASE POINT.  After it
 *         returns the module never calls again and `ctx` may be freed.
 */
int Modbus_Unsubscribe(int handle);

/**
 * @brief  Ask for the catalogue again — a burst of mbEvt_pointDesc, one per
 *         point in this subscription's scope, `last` set on the final entry
 *         (pt == NULL with last = 1 when there is nothing to describe).
 *
 *         It is delivered automatically after Subscribe and after every config
 *         swap, so this call is for the case where a consumer's "I need the
 *         point list" moment does not coincide with its "I want the data"
 *         moment.  Re-subscribing to force a replay would stop polling in the
 *         gap and use teardown as a query.
 *
 *         Posted: the replay is a flash walk and it fires callbacks, so it
 *         runs on the modbus task, never inline in the caller's.
 */
int Modbus_RequestCatalogue(int handle);

/* ==========================================================================
 * Contracts — read before writing a subscriber (docs/modbus.md §4.7)
 *
 * 1. Callbacks run IN THE MODBUS TASK, synchronously.  Delivery is the call:
 *    there is no queue, no per-subscriber buffer and no drop counter.
 * 2. They MUST NOT BLOCK.  Stricter than it looks: samples arrive per read
 *    rather than per change, so a consumer's cost is multiplied by config
 *    size, not by how much the plant is moving.
 * 3. EVERY POINTER IN AN EVENT IS BORROWED and dies when the callback returns.
 * 4. A consumer cannot fail a sequence; there is no way to report back.
 * 5. Modbus_Request and Modbus_RequestCatalogue are legal from inside a
 *    callback.
 * 6. Both ordinals are authored identities and survive a config swap only for
 *    as long as the author leaves the array order alone — which is why
 *    mbEvt_config is followed by a fresh catalogue.
 * 7. Borrowing runs both ways: the request array until the completion
 *    callback, the subscriber ctx until mbEvt_released.
 * 8. Subscribe at any time, including before Modbus_Init.
 *
 * The pattern for a consumer that does real work: allocate, copy the fields it
 * needs, post the pointer to its own queue, return — and do the work on its
 * own task.  None of that is a contract; a consumer that only counts does it
 * inline and allocates nothing.
 * ========================================================================== */

/* ==========================================================================
 * Requests — one array of items, in and out (docs/modbus.md §4.6)
 *
 * It is Modbus_Request, not Modbus_Write, because THE CONFIG decides what each
 * item means:
 *
 *   access  r   reads the point; the supplied value is ignored
 *           w   writes the supplied value; no read-back is possible
 *           rw  writes, THEN reads the register back
 *
 * That is the protection: a requester cannot reach a read-only register by
 * asking differently, because asking is not how the decision is made.  The
 * module also enforces the point's write bounds — an item outside them fails
 * with mbErr_outOfRange before a frame is formed, and every other item still
 * runs.  There is no clamping.
 * ========================================================================== */

typedef struct {
    int32_t  value;    /* in: value to write · out: read or read-back        */
    uint16_t id;       /* in: pointId within the device's capability         */
    int16_t  result;   /* out: eModbusErr; mbErr_pending until decided       */
} sModbusReqItem;      /* exactly 8 bytes, no padding                        */

typedef struct {
    sModbusReqItem *items;      /* the array as submitted, now filled in     */
    uint16_t        count;
    uint8_t         devOrd;
} sModbusReqReply;

typedef void (*fModbusReqDone)(const sModbusReqReply *rep, void *ctx);

/**
 * @brief  Submit one batch of items against one device.
 *
 *         item.id is a ptOrd; item.value is in the scaled-integer domain —
 *         the same domain samples arrive in and writeMin/writeMax are authored
 *         in.  Repeated ids are legal; items run in order and each slot gets
 *         its own result.  Every item is attempted: a failure on item 3 does
 *         not stop items 4-8, and there is no rollback because the wire
 *         cannot offer one.
 *
 *         THE CONTRACT:
 *
 *           The completion callback always fires, within timeout_ms, and it is
 *           the only moment at which the caller may free or reuse the item
 *           array.
 *
 *         The deadline runs from SUBMISSION, not from first service, so a
 *         request that waits behind others spends its own timeout waiting and
 *         may complete with every item mbErr_notAttempted.  A timed-out
 *         request is abandoned, not merely reported: a reply arriving
 *         afterwards is discarded and never written into memory the caller has
 *         been told it may reclaim.  A config swap completes outstanding
 *         requests, it never drops them.
 *
 *         `cb` runs in the modbus task under the same non-blocking rule as a
 *         subscriber, and is required — without it a caller cannot know when
 *         its array is its own again.
 *
 *         A request reaches the whole capability, not just what some plan
 *         watches, and it is unaffected by whether anything is subscribed.
 *
 * @param  devOrd      device id (its position in the config's devices[])
 * @param  items       borrowed by the module until `cb` fires
 * @param  count       1..MB_REQ_MAX_ITEMS
 * @param  timeout_ms  1..MB_REQ_TIMEOUT_MAX_MS; 0 is rejected
 * @return 0 on acceptance (every item set to mbErr_pending), or a negative
 *         eModbusErr — mbErr_badArg / mbErr_full / mbErr_config.  On a
 *         negative return `cb` does NOT fire and the array was never borrowed.
 */
int Modbus_Request(uint8_t devOrd, sModbusReqItem *items, uint16_t count,
                   uint32_t timeout_ms, fModbusReqDone cb, void *ctx);

/* The catalogue's sModbusPointDesc is BORROWED and dies with the callback, so
 * it cannot answer "may I write this?" for a caller that is not a subscriber.
 * This COPIES instead — `out` is the caller's, including the name.
 *
 * It exists because access is authored, not requested (see the block comment
 * above): a would-be writer has to be able to ask what a point permits BEFORE
 * submitting, or it cannot tell "wrote it" from "read it and ignored my
 * value".  Like every other lookup here it is a flash walk, so it belongs on a
 * control path and not in a loop. */
typedef struct {
    char     name[MB_POINT_NAME_LEN];
    int32_t  writeMin, writeMax;   /* scaled-int; valid if MB_PT_BOUNDED     */
    uint8_t  decodeType;           /* eModbusDecodeType                      */
    uint8_t  unit;                 /* DLMS/COSEM physical-unit code          */
    int8_t   scalePow10;           /* real value = scaled * 10^scalePow10    */
    uint8_t  flags;                /* MB_PT_READ|WRITE|BOUNDED               */
} sModbusPointMeta;

/**
 * @brief  Metadata of one point of one device, copied out.
 * @param  devOrd  device id (position in the config's devices[])
 * @param  ptOrd   point id within that device's capability
 * @param  out     filled on success; untouched otherwise
 * @return 0, or mbErr_badArg / mbErr_idNotFound.
 */
int Modbus_PointInfo(uint8_t devOrd, uint16_t ptOrd, sModbusPointMeta *out);

/* ==========================================================================
 * Configuration (docs/modbus.md §4.9)
 *
 * The record stream in flash is the config; JSON is its outward translation,
 * so an export re-serialises whatever the records now say.
 * ========================================================================== */

/**
 * @brief  Compile a JSON config stream into the INACTIVE flash region.
 *
 *         Compiling IS validating — the pass that writes records is the pass
 *         that checks them.  On failure *err pinpoints the offending
 *         device/transaction/point plus field and reason.  Nothing live is
 *         affected either way, but the inactive region is consumed regardless
 *         of outcome, and that region holds the previous config — which is
 *         what Modbus_ConfigVerify (§10 step 7) exists for.
 *
 *         Runs on the CALLER's task: the byte source is the HTTP socket and an
 *         upload takes seconds, so posting it would stall the engine for the
 *         whole transfer.  Safe without locking because the region being
 *         written is the one nobody is walking and the header is written last.
 *
 *         Refused with mbErr_busy while an apply is pending.
 */
int Modbus_ConfigCompile(fModbusByteSource src, void *srcCtx,
                         sModbusCompileResult *err);

/**
 * @brief  Arm the config swap.  Committed by the engine at its next safe
 *         point, hot, with no reboot; outstanding requests are completed
 *         rather than dropped.  mbErr_config if nothing valid is staged.
 */
int Modbus_ConfigApply(void);

/**
 * @brief  Validate a JSON config stream WITHOUT writing anything.
 *
 *         Same compiler, same single pass, same *err — the records go to a
 *         counting sink instead of flash, so there is never a second validator
 *         to keep in sync.  Use it before an upload: Modbus_ConfigCompile
 *         consumes the inactive region whether it succeeds or fails, and that
 *         region holds the previous config.
 *
 *         Touches no flash and no live state, so it is never refused.
 */
int Modbus_ConfigVerify(fModbusByteSource src, void *srcCtx,
                        sModbusCompileResult *err);

/**
 * @brief  Erase the stored config: the board becomes UNPROVISIONED.
 *
 *         It is Erase, not Reset — with no built-in default there is nothing
 *         to reset TO, so "reset" named the wrong operation (§4.9).
 */
int Modbus_ConfigErase(void);

/**
 * @brief  Re-serialize the ACTIVE config to JSON (data-faithful, not
 *         byte-identical; recompiling a download yields the same records).
 *         A pure function of the region, so a caller may run it twice — once
 *         to count for Content-Length, once to send.
 */
int Modbus_ConfigExport(fModbusByteSink sink, void *ctx);

typedef struct {
    uint8_t  activeRegion;      /* 0 = A, 1 = B                             */
    uint8_t  valid;             /* the active region holds a usable config  */
    uint8_t  stagedValid;       /* so does the inactive one                 */
    uint8_t  swapPending;
    sModbusConfigCounts counts; /* of the active region; all-zero if !valid */
} sModbusConfigStatus;

int Modbus_ConfigStatus(sModbusConfigStatus *out);

/* Per-device state, because "why is this device not polled" is a question
 * about SUBSCRIBERS and about the port, and both `modbus status` and the
 * config status JSON have to be able to answer it (§4.3, §4.2). */
typedef struct {
    char     topicPrefix[MB_TOPIC_PREFIX_LEN];
    uint32_t baud;
    uint8_t  devOrd;
    uint8_t  slaveAddr;
    uint8_t  capId;
    uint8_t  portId;
    uint8_t  format;
    uint8_t  portUp;         /* a driver is registered on that slot        */
    uint8_t  coveringPlans;  /* plan slots naming this device              */
    uint8_t  polled;         /* covered by a LIVE plan, on a port that is up */
} sModbusDeviceInfo;

/** @return device count written, or a negative eModbusErr */
int Modbus_DeviceList(sModbusDeviceInfo *out, uint8_t max);

/* ==========================================================================
 * Diagnostics (docs/modbus.md §4.1, §5.2)
 * ========================================================================== */

typedef struct {
    uint32_t polls;      /* transactions that completed                     */
    uint32_t errors;     /* transactions that did not                       */
    uint32_t requests;   /* request batches completed                       */
    uint32_t missed;     /* scheduled sequences dropped because the previous
                            one was still unserviced — the capacity signal:
                            non-zero means the config asks for more than the
                            wire can deliver (§5.2)                          */
    uint8_t  monitor;
} sModbusStats;

int Modbus_Stats(sModbusStats *out);

/** @brief  Trice dump of engine + config + per-device state (`modbus status`). */
void Modbus_LogStatus(void);

/** @brief  Raw TX/RX frame monitoring via Trice.  This is BELOW decode — for
 *          decoded values, subscribe instead. */
void Modbus_SetMonitor(int enable);
int  Modbus_GetMonitor(void);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_H_ */
