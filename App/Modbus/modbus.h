/**
 * @file    modbus.h
 * @brief   PeriphNet Modbus module — THE public API.  Nothing outside
 *          App/Modbus may include any other header from this directory.
 *
 * The module polls Modbus RTU slaves described by an uploadable flash-resident
 * config, decodes their registers, and delivers the results to subscribers.
 * It knows nothing about MQTT, Home Assistant or CAN — those are consumers
 * that attach through Modbus_Subscribe().
 *
 * Boundary rules (docs/modbus.md §1.2):
 *   - Files in App/Modbus must not include App/Mqtt, App/Http, App/Can,
 *     App/Data or any lwIP header.
 *   - No device-specific code lives in this module.  A device is described by
 *     config, not by a driver (docs/modbus.md §1.3).
 *   - Consumers include only this header.  Shared/Modbus *types* are fair game
 *     (modbus_records.h is a layer below everyone); Shared/Modbus flash
 *     accessors (MbCfg_*, MbCfgStore_*) are not — going through them is the
 *     coupling this API exists to remove.
 *
 * THE RULE (docs/modbus.md §2.2): the module emits EVERYTHING it reads, to
 * whoever subscribed to that device, as soon as it is decoded.  It keeps no
 * copy of any value, and it carries no notion of what is worth forwarding —
 * that judgement, and the config that expresses it, belong to the consumer
 * making it.  What the module keeps is scheduling and bus state, which no
 * consumer could own.
 *
 * SCOPE: FC03/04 reads and single-register FC06 writes, which is what the
 * module does today.  Nothing here anticipates a reworked write path or
 * dialects beyond an address stride — those are limits listed in
 * docs/modbus.md §7.  Note this surface is deliberately indifferent to what is
 * BEHIND it: peripherals, per-device timers and sequences (docs/modbus.md
 * §2.5-§2.7) are all internal, which is why the engine can be rebuilt without
 * touching a consumer.
 *
 * STATUS: PROPOSED.  Not yet implemented; nothing includes this file.  Revised
 * 2026-08-10 after four review rounds (docs/modbus.md §8): baud and port are
 * config, not API; Start/Stop/SetPort/InjectResponse removed; no in-module
 * change detection and no publish policy in the config.  A later pass settled
 * the config model behind this surface — a register map is SHARED by the
 * devices using it, and a port is an enum indexing the module's static port
 * table — which changes nothing here except the meaning of ptOrd (see
 * sModbusPointDesc).  Items marked "OPEN Qn" are decisions still to make —
 * collected at the end.
 */

#ifndef MODBUS_H_
#define MODBUS_H_

#include "modbus_records.h"      /* eModbusDecodeType, MB_MAX_*, unit codes */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * Bounds
 * ========================================================================== */

#define MB_MAX_SUBS              8   /* subscription table (OPEN Q3)         */

/* ==========================================================================
 * Errors and ports
 *
 * eModbusErr moves here from modbus_rtu.h — it appears in events, so it is part
 * of the public surface.  modbus_rtu.h then includes this header instead of
 * declaring it.
 *
 * eModbusPortId does NOT live here.  Ports are internal: a device names the
 * peripheral it lives on, which is config, so nothing outside the module ever
 * selects or configures one.  docs/modbus.md §2.5.
 * ========================================================================== */

typedef enum {
    mbErr_ok             =  0,
    mbErr_timeout    = -1,  /* no response within the deadline          */
    mbErr_crc        = -2,  /* CRC mismatch in the response             */
    mbErr_exception  = -3,  /* slave returned an exception              */
    mbErr_short      = -4,  /* response too short / truncated           */
    mbErr_busy       = -5,  /* port busy, not initialised, or refused   */
    mbErr_notFound   = -6,  /* no such device / point in the config     */
    mbErr_range      = -7,  /* value outside writeMin..writeMax         */
    mbErr_full       = -8,  /* write slot or subscription table full    */
    mbErr_config     = -9,  /* no valid config / malformed record stream*/
} eModbusErr;

/* ==========================================================================
 * Point descriptor
 *
 * The identity and metadata of one configured point.  Handed out by pointer
 * in events; BORROWED — see "Contracts" below.
 * ========================================================================== */

#define MB_PT_WRITABLE    (1u << 0)  /* config declares the point writable  */

/* IDENTITY IS {devOrd, ptOrd}, NOT ptOrd ALONE.  A register map is shared by
 * every device using it (docs/modbus.md §2.6), so ptOrd is relative to that
 * MAP: four battery packs on one map produce four samples carrying the same
 * ptOrd and different devOrd.  A consumer keying on ptOrd by itself collapses
 * them.  Both ordinals are valid only within one config generation
 * (contract 6); deviceId and name are the identities that survive a swap. */
typedef struct {
    uint16_t    ptOrd;          /* ordinal within the device's map          */
    uint8_t     devOrd;
    const char *deviceId;       /* device identity (OPEN Q1)                */
    const char *name;           /* point name, unique within the device     */
    uint8_t     decodeType;     /* eModbusDecodeType                        */
    uint8_t     unit;           /* DLMS/COSEM physical-unit code            */
    int8_t      scalePow10;     /* real value = scaled * 10^scalePow10      */
    uint8_t     flags;          /* MB_PT_*                                  */
    int16_t     writeMin;       /* scaled-int domain; valid if WRITABLE     */
    int16_t     writeMax;
} sModbusPointDesc;

/* NOTE: there is no publish threshold / heartbeat here, and none in the config
 * either.  How often a reading is worth forwarding is a property of the thing
 * doing the forwarding, not of the register — the module has no way to judge
 * it and nothing to do with the answer.  A consumer that wants to throttle
 * owns that policy and the memory it costs.  docs/modbus.md §2.4. */

/* ==========================================================================
 * Events
 * ========================================================================== */

typedef enum {
    mbEvt_sample       = 1u << 0,  /* a point was read and decoded         */
    mbEvt_pointDesc   = 1u << 1,  /* catalogue entry (see Modbus_Subscribe)*/
    mbEvt_deviceState = 1u << 2,  /* device went online / offline         */
    mbEvt_writeResult = 1u << 3,  /* a submitted write finished           */
    mbEvt_config       = 1u << 4,  /* config swapped / reset               */
    mbEvt_txn          = 1u << 5,  /* transaction outcome — diagnostics    */
    mbEvt_all          = 0x3Fu,
} eModbusEventType;

#define MB_WRITE_NO_POINT  0xFFFFu   /* .ptOrd for a raw (unresolved) write */

typedef struct {
    eModbusEventType type;
    uint32_t         tick;          /* HAL_GetTick() when the event was made*/

    union {
        /* ---- mbEvt_sample ---------------------------------------------
         * Raised once per point per SUCCESSFUL READ — not per change.  The
         * module has no memory of previous values and does not deduplicate;
         * a consumer that wants change detection does it itself, on its own
         * terms.
         *
         * Carries the decoded value only — raw registers never leave the
         * module (the config already says what every register means; a
         * second, raw data path would invite consumers to re-implement
         * decoding and get word order, scaling or ASCII subtly wrong).
         *
         * For mbDecode_ascii points, `text` is the decoded string and
         * `value` is UNSPECIFIED — nothing in the module computes one.  A
         * consumer that needs change detection hashes `text`.  For every
         * other type, `value` is the scaled integer and `text` is NULL. */
        struct {
            const sModbusPointDesc *pt;
            int32_t     value;
            const char *text;
        } sample;

        /* ---- mbEvt_pointDesc -----------------------------------------
         * One per point in the subscription's scope, delivered as a burst
         * ("the catalogue").  `last` marks the final entry, including when
         * the catalogue is empty (pt == NULL in that case). */
        struct {
            const sModbusPointDesc *pt;
            uint8_t     last;
        } desc;

        /* ---- mbEvt_deviceState ---------------------------------------- */
        struct {
            uint8_t     devOrd;
            const char *deviceId;
            uint8_t     online;
        } device;

        /* ---- mbEvt_writeResult ---------------------------------------- */
        struct {
            uint32_t    id;         /* as returned by Modbus_Submit*Write   */
            uint16_t    ptOrd;      /* MB_WRITE_NO_POINT for a raw write    */
            int16_t     err;        /* eModbusErr                           */
            uint8_t     exc;        /* Modbus exception code, 0 = none      */
        } write;

        /* ---- mbEvt_config ----------------------------------------------
         * A new config went live.  Every cached ptOrd/devOrd is now invalid;
         * a fresh catalogue follows for every subscription. */
        struct {
            uint8_t     activeRegion;
            uint8_t     devices;
            uint8_t     transactions;
            uint16_t    points;
        } config;

        /* ---- mbEvt_txn -------------------------------------------------
         * Transaction outcome, for health/diagnostics consumers.  Deliberately
         * carries NO register payload; values arrive as mbEvt_sample. */
        struct {
            uint8_t     devOrd, slave;
            uint16_t    txnOrd, startAddr, count;
            int16_t     err;        /* eModbusErr                           */
            uint8_t     exc;        /* Modbus exception code, 0 = none      */
            uint16_t    elapsedMs;
        } txn;
    } u;
} sModbusEvent;

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

/**
 * @brief  Bring the module up and start polling: flash store init, A/B region
 *         recovery, provisioning of the built-in default config if none is
 *         valid, then the modbus task, the ports and the devices' timers.
 *
 *         Requires W25Q128_Init() to have run.  Idempotent.  Belongs in
 *         App_DefaultTaskEntry after the flash driver — NOT in
 *         http_server_init(), where the config store lives today.
 *
 *         Set it and forget it.  There is no Start/Stop pair, no port
 *         selection and no baud accessor: the module owns its peripherals,
 *         reads what the config tells it to, and services a write at the first
 *         opening between reads.  Nothing outside steers it.
 *
 * @return 0 on success, negative eModbusErr otherwise
 */
int  Modbus_Init(void);

/*
 * There is deliberately no baud accessor.  Baud is a property of the DEVICE,
 * carried in its config record, because one bus can serve devices at different
 * rates by time-multiplexing (Solis 9600 + JK BMS 115200 is the motivating
 * case).  Line parameters travel with each frame, and the RTU framing gaps
 * derive from the rate in force — both inside the port.  docs/modbus.md §2.6.
 */

/* ==========================================================================
 * Subscriptions — the data path out
 * ========================================================================== */

typedef void (*fModbusSubscriber)(const sModbusEvent *ev, void *ctx);

/**
 * @brief  Subscribe to one device, or to all of them.
 *
 * @param  deviceId   device identity; NULL = every device
 * @param  eventMask  bitwise-OR of eModbusEventType
 * @param  cb         callback; see "Contracts" below — it runs in the poll
 *                    task and must not block
 * @param  ctx        opaque, passed back to cb
 * @return handle >= 0, or mbErr_full
 *
 * Within that scope the subscription gets EVERY read — no thresholding, no
 * deduplication.  Device scope is routing, not filtering: it resolves to a
 * devOrd once per config generation, stores nothing, and is the same answer
 * for every consumer of that device.  A bus carries devices with unrelated
 * consumers, and a CAN-fusion path that wants one BMS should not be handed
 * an inverter's registers on every sequence.
 *
 * Naming a device that is not in the active config is legal: the subscription
 * receives nothing until a config containing it goes live.  This is what makes
 * hot config swaps survivable — consumers never re-register.
 *
 * A catalogue (a burst of mbEvt_pointDesc) is delivered for the new
 * subscription if mbEvt_pointDesc is in the mask.  It arrives from the poll
 * task, so it is NOT synchronous with this call.
 *
 * Safe to call from any task, and the module is always running by then
 * (Modbus_Init polls); see contract 8 on late registration.
 */
int  Modbus_Subscribe(const char *deviceId, uint32_t eventMask,
                      fModbusSubscriber cb, void *ctx);

int  Modbus_Unsubscribe(int handle);

/**
 * @brief  Re-deliver this subscription's catalogue.
 *
 *         Callers that need the point list at a moment unrelated to config
 *         load — MQTT publishing HA discovery on broker connect is the
 *         motivating case — ask for it here rather than walking the config.
 *         Costs one flash walk; delivered from the modbus task like everything
 *         else.
 */
int  Modbus_RequestCatalogue(int handle);

/* ==========================================================================
 * Commands
 *
 * Write submission is safe from any task and returns WITHOUT touching the bus:
 * it queues, and the engine drains it at the first opening between reads —
 * between transactions within a sequence.  That is what lets
 * the mqtt, http and cmd tasks all submit writes without knowing about each
 * other.  Modbus_Probe() blocks the caller while the engine keeps running.
 *
 * NOTE: only ONE write may be pending (the current single-slot queue).  A
 * second submission returns mbErr_full until the first completes.
 * ========================================================================== */

typedef struct {
    const char *deviceId;
    const char *pointName;
    int32_t     value;          /* scaled-int domain (= raw register domain)*/
} sModbusWriteReq;

/**
 * @brief  Resolve a point by device + name, range-check against its
 *         writeMin/writeMax, and queue an FC06 write.
 *
 *         The outcome arrives later as mbEvt_writeResult carrying *outId.
 *
 * @return 0, or mbErr_notFound / _RANGE / _FULL
 */
int  Modbus_SubmitWrite(const sModbusWriteReq *req, uint32_t *outId);

/**
 * @brief  Bring-up / CLI escape hatch (`modbus write <slave> <reg> <value>`):
 *         no config lookup, no range check, address used verbatim.
 */
int  Modbus_SubmitRawWrite(uint8_t slave, uint16_t reg, uint16_t value,
                           uint32_t *outId);

typedef struct {
    uint8_t  slave;
    uint8_t  fc;                /* 3 or 4                                   */
    uint16_t addr;              /* wire address, verbatim                   */
    uint16_t count;
    uint32_t baud;              /* MANDATORY — there is no single configured
                                   baud any more (see Modbus_Init)          */
    uint32_t timeoutMs;         /* 0 = default                              */
} sModbusProbeReq;

/**
 * @brief  Synchronous one-shot read, independent of the active config.
 *
 *         The ONLY API that hands out raw registers — deliberately, because it
 *         exists for addresses that are in no config, which is what bring-up
 *         on an unknown slave means.  Runs as an ordinary one-shot transaction
 *         whose parameters come from the caller instead of a device record;
 *         the engine keeps servicing everything else meanwhile.  BLOCKS the
 *         calling task until it completes.  Backs `modbus probe`.
 *
 *         On mbErr_exception, *outExc carries the Modbus exception code
 *         (1 Illegal Function, 2 Illegal Data Address, 3 Illegal Data Value) —
 *         telling those apart is the whole diagnosis on an unfamiliar slave.
 */
int  Modbus_Probe(const sModbusProbeReq *req, uint16_t *out, uint16_t maxOut,
                  uint16_t *outCount, uint8_t *outExc);

/* There is no injection hook.  Test frames arrive through a TEST PORT whose
 * peer is the host-side harness — it answers the request the engine actually
 * formed, rather than being fed a response below the port, so framing,
 * timeouts and the state machine are all exercised for real.  Which port a
 * device lives on is config.  docs/modbus.md §2.5. */

/**
 * @brief  Mark every transaction due so the next sequences re-read everything
 *         regardless of its period.  Backs `mqtt publish now`, which
 *         needs nothing else now that a read always produces a publish.
 */
void Modbus_ForceRefresh(void);

/* ==========================================================================
 * Configuration
 * ========================================================================== */

/** Byte source for a streaming upload: >0 = bytes read, 0 = EOF, <0 = error */
typedef int (*fModbusByteSource)(void *ctx, uint8_t *buf, uint32_t maxLen);

/** Byte sink for a streaming export: 0 = ok, <0 = error */
typedef int (*fModbusByteSink)(void *ctx, const uint8_t *buf, uint32_t len);

/**
 * Compile outcome.  Mirrors the compiler's internal result so the compiler API
 * stays private; the indices and strings are what an HTTP 422 renders.
 * (OPEN Q2)
 */
typedef struct {
    uint8_t  ok;
    int16_t  deviceIdx;         /* -1 when not applicable                   */
    int16_t  txnIdx;
    int16_t  pointIdx;
    char     field[24];
    char     reason[64];
    uint8_t  devices;
    uint8_t  transactions;
    uint16_t points;
} sModbusCompileError;

/**
 * @brief  Validate a JSON config stream WITHOUT writing anything.
 *
 *         Same compiler, same single pass, same *err — the records go to a
 *         counting sink instead of flash, so there is no second validator to
 *         keep in sync.  Use it before an upload: Modbus_ConfigCompile()
 *         consumes the inactive region whether it succeeds or fails, and that
 *         region holds the previous config.
 *
 *         Touches no flash and no live state, so it is never refused.
 */
int  Modbus_ConfigVerify(fModbusByteSource src, void *srcCtx,
                         sModbusCompileError *err);

/**
 * @brief  Compile a JSON config stream into the INACTIVE flash region.
 *
 *         Compiling IS validating — the pass that writes records is the pass
 *         that checks them.  On failure *err pinpoints the offending
 *         device/transaction/point plus field and reason.  Nothing LIVE is
 *         affected either way, but the inactive region is overwritten
 *         regardless of outcome — see Modbus_ConfigVerify().
 *
 *         Refused (mbErr_busy) while a swap is pending: the inactive
 *         region is about to go live.
 */
int  Modbus_ConfigCompile(fModbusByteSource src, void *srcCtx,
                          sModbusCompileError *err);

/** @brief  Arm the swap.  The engine stops the current devices' timers, lets
 *          anything in flight land, and constructs the new devices; a
 *          completion arriving for a torn-down device is discarded on its
 *          generation counter.  docs/modbus.md §2.7. */
int  Modbus_ConfigApply(void);

/** @brief  Stage the built-in default config and arm the swap (hot reset). */
int  Modbus_ConfigReset(void);

/** @brief  Re-serialize the ACTIVE config to JSON (data-faithful, not
 *          byte-identical).  Pure function of the region, so a caller may run
 *          it twice — once to count for Content-Length, once to send. */
int  Modbus_ConfigExport(fModbusByteSink sink, void *ctx);

typedef struct {
    uint8_t  activeRegion;      /* 0 = A, 1 = B                             */
    uint8_t  valid;
    uint8_t  swapPending;
    uint8_t  stagedValid;
    uint8_t  devices;
    uint8_t  transactions;
    uint16_t points;
    sModbusCompileError lastUpload;
} sModbusConfigStatus;

int  Modbus_ConfigStatus(sModbusConfigStatus *out);

/* ==========================================================================
 * Diagnostics
 * ========================================================================== */

typedef struct {
    uint32_t polls;             /* successful transactions                  */
    uint32_t errors;            /* failed transactions                      */
    uint32_t writesDone;
    uint32_t missed;            /* scheduled sequences dropped because the
                                   previous one was still unserviced — the
                                   capacity signal, docs/modbus.md §2.7      */
    uint8_t  monitor;
} sModbusStats;

int  Modbus_Stats(sModbusStats *out);

/** @brief  Trice dump of engine + config + per-device state, including each
 *          device's port and its missed count (`modbus status`).            */
void Modbus_LogStatus(void);

/** @brief  Raw TX/RX frame monitoring via Trice.  This is BELOW decode — for
 *          decoded values, subscribe instead. */
void Modbus_SetMonitor(int enable);
int  Modbus_GetMonitor(void);

/* ==========================================================================
 * CONTRACTS — read before writing a subscriber
 *
 * 1. Callbacks run in the MODBUS TASK, synchronously.  They extend the time
 *    the engine takes to service a sequence, directly.
 *
 * 1b. NOTHING DISPATCHES FROM ISR OR TIMER CONTEXT.  Port completion callbacks
 *    and scheduler timer callbacks post an event and return — they never
 *    decode and never call a subscriber.  That is what keeps
 *    MqttBridge_Publish (and its LOCK_TCPIP_CORE) legal in a subscriber and
 *    Trice legal in a callback.
 *
 * 2. Callbacks MUST NOT BLOCK.  No unbounded waits, no flash erase, no
 *    connect().  A consumer that needs to block copies the event into its own
 *    queue and returns.  (MqttBridge_Publish() taking LOCK_TCPIP_CORE is
 *    acceptable: bounded, and exactly what the engine already does today.)
 *    Stricter than it looks: samples now arrive per READ, not per change, so
 *    a subscriber's cost is multiplied by config size, not by how much the
 *    plant is moving.
 *
 * 3. EVERY POINTER IN AN EVENT IS BORROWED and dies when the callback
 *    returns — deviceId, name, text and the sModbusPointDesc itself point
 *    into poll-task stack/scratch.  Copy what you keep.
 *
 * 4. Callbacks must not call blocking module APIs.  Safe from a callback:
 *    Modbus_SubmitWrite, Modbus_SubmitRawWrite, Modbus_RequestCatalogue,
 *    Modbus_ForceRefresh, Modbus_Stats.  NOT safe: Modbus_ConfigVerify,
 *    Modbus_ConfigVerify, Modbus_ConfigCompile, Modbus_ConfigExport and
 *    Modbus_Probe (which blocks the calling task until its transaction
 *    completes — from a callback that is the modbus task itself).
 *
 * 5. A subscriber cannot fail a sequence.  No return value; nothing it does is
 *    checked.
 *
 * 6. ptOrd and devOrd are stable only WITHIN ONE CONFIG GENERATION.  A swap
 *    raises mbEvt_config, invalidates every cached ordinal, and is followed
 *    by a fresh catalogue.  deviceId and point name are the identities that
 *    survive a config change.
 *
 * 7. Trice is legal inside a subscriber (modbus task context).  It is NOT legal
 *    in anything a consumer defers into tcpip_thread — the existing rule.
 *
 * 8. Subscribe as early as the consumer's own init allows.  The module is
 *    already polling by then (Modbus_Init), and the table is fixed and not
 *    lock-free; late subscription works but is not hot-plug-safe under load.
 * ========================================================================== */

/* ==========================================================================
 * OPEN QUESTIONS — decide before implementing the marked item
 *
 * Q1  deviceId naming.  The config field is "topicPrefix" — the module's
 *     device identity named after one consumer's transport.  The API says
 *     deviceId, which for now simply IS that value.  Whether the JSON gains a
 *     neutral spelling is a config-schema question, deferred.
 *
 * Q2  sModbusCompileError as a mirror, or re-export sMbCompileResult?
 *     The mirror keeps modbus_config_compiler.h private at the cost of one
 *     struct copy per upload and ~30 lines of translation.  Re-exporting is
 *     free but puts MbCfgCompile() in every consumer's view.  Mirror proposed;
 *     weakest of the calls made here.
 *
 * Q3  MB_MAX_SUBS = 8.  ~16 bytes each.
 *
 * Q4  Synchronous dispatch vs an event queue.  Synchronous preserves today's
 *     behaviour exactly and costs no RAM (CCM has ~6 KB free).  The cost is
 *     that a slow subscriber lengthens sequences — sharper now that every read
 *     dispatches.  Revisit when a real consumer needs to block.
 *
 * Q5  Who owns device availability?  Kept here: the >=30 s throttle on an
 *     offline device is a bus decision only this module can make, and two
 *     consumers deriving "offline" from mbEvt_txn would disagree with each
 *     other.  The strict reading of THE RULE would push it out.
 *
 * Q6  How many built-in default configs?  Solis today; JK BMS now wants one
 *     too, which is either Modbus_ConfigReset(name) over a named set (~1-2 KB
 *     .rodata each) or a repo file to upload (free, needs a network).
 *
 * RESOLVED by the 2026-08-09 review, kept here so they are not re-opened:
 *   - Baud accessors: removed; baud is per-device config.
 *   - Start/Stop/IsRunning, SetPort/GetPort, InjectResponse: removed.  A port
 *     is where a device lives, which is config; "stopped" says nothing that
 *     "no devices configured" does not.
 *   - Writes while stopped: moot — there is no stopped state.
 *   - Catalogue before the first sequence: moot — the module is always running.
 *   - ASCII `value`: unspecified, because nothing here computes a hash.
 * ========================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_H_ */
