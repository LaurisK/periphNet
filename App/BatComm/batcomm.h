/**
 * @file    batcomm.h
 * @brief   PeriphNet battery communication module — THE public API.
 *
 * IT IS THE THING THAT TALKS TO THE INVERTER.  The cluster and the pack
 * module produce numbers; this module turns one of them into the frames a
 * specific inverter dialect expects and puts them on a specific CAN cell.
 * It owns no arithmetic, invents no number and commands nothing.
 *
 * THREE PARAMETERS, ALL CONFIGURATION, NONE COMPILED IN
 * (docs/design_battery_comm.md §2):
 *
 *   protocol    which dialect to speak.  `dyness_lv` today, mimicking the
 *               PowerBrick that the Solis at zaliakalnis already accepts.
 *   peripheral  which cell the INVERTER is on — can1 or can2.  This is the
 *               same site fact issue_can_bus_roles_not_configurable.md is
 *               about, and this module is where it stops being a #define.
 *   source      `cluster` (the fused bus, App/Cluster) or `pack` + a name
 *               (one pack, App/Pack — a straight translation).
 *
 * WHAT IT WILL NOT DO: aggregate; derate; decide a limit; change the bridge's
 * MODE.  Taking over the wire is an operator act (`POST /api/can/mode`), and
 * a module that quietly moved the bridge in and out of `bms` would make the
 * one state an operator has to be sure about unobservable.
 *
 * FIVE CONTRACTS
 *
 *   1. SIGN.  `current_mA` is + CHARGE, - DISCHARGE at this boundary, exactly
 *      as in sPackState and sClusterOutput.  Both devices measured in the
 *      field agree with it, so the dialect emits it unchanged; the one bit
 *      that would flip it (`invertCurrent`) is profile CONFIGURATION and
 *      defaults to off.  Nothing upstream of here ever sees the question.
 *
 *   2. TRANSMIT ONLY ON A GOOD READ, positively.  cluErr_notReady, _busy,
 *      _stale and _unprovisioned all mean the same thing here: emit nothing
 *      this cycle.  `_busy` is NOT "reuse the last frame" — a frozen snapshot
 *      is the failure the whole staleness contract exists to prevent, moved
 *      one seam downstream (docs/design_battery_cluster.md R15).
 *
 *   3. 0x351 IS WITHHELD, NOT ZEROED, when either voltage limit is invalid.
 *      A zero charge-voltage limit is not "no limit", it is "stop charging",
 *      and a pack that cannot report its limits has not said that.
 *
 *   4. GIVING THE WIRE BACK IS THE FAILURE MODE, not silence — unless the
 *      battery cell is an INPUT to us.  When the source has been unusable for
 *      `staleTrip` cycles the module drops the bridge override so the real
 *      battery's own frames reach the inverter again.  With
 *      `batteryBusIsInput` set, that battery is a member of the cluster we
 *      publish and forwarding its frames would present a fragment of the bus
 *      as the whole of it, so the module stays silent instead.
 *
 *   5. THE ENCODER IS PURE.  batcomm_frame.c is libc-only, has zero file
 *      statics and knows nothing about CAN, the RTOS or flash: it maps one
 *      sBatCommIn to one sBatCommFrame.  That is what lets the captured
 *      PowerBrick payloads be asserted byte-for-byte on a host.
 *
 * BOUNDARY: a consumer includes ONLY this header.
 *
 * STATUS: implemented, host-tested (tests/test_batcomm.c).  NOT YET RUN ON
 * HARDWARE, and it puts nothing on a wire until an operator both configures
 * it and puts the bridge in `bms` mode.
 */

#ifndef BATCOMM_H_
#define BATCOMM_H_

/* Includes -----------------------------------------------------------------*/

#include "App/Pack/pack.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported constants -------------------------------------------------------*/

#define BATCOMM_NAME_LEN        PACK_NAME_LEN   /* including NUL             */

/** Slots in one transmit cycle.  The captured PowerBrick puts twelve frames
 *  at 50 ms spacing inside a 1 s window and leaves the rest of the window
 *  empty — it is NOT a burst, and a receiver that times its own watchdog off
 *  the spacing would see a different bus if we sent one.  20 x 50 ms. */
#define BATCOMM_SLOTS_MAX       32u
#define BATCOMM_DFLT_SLOT_MS    50u
#define BATCOMM_DFLT_SLOTS      20u

/** mA per 0x351/0x356 current count, and mAh per 0x35F capacity count.
 *
 *  SETTLED BY THE NAMEPLATE, and it overturns the reasoning in
 *  reference_dyness_can_capture_2026-09-05.md §3.2: that section argued a
 *  two-module pack could not be 560 Ah and therefore read the counts as
 *  0.01 A / 0.1 Ah.  The modules are 560 Ah, so the device emits the STANDARD
 *  Pylontech scaling after all, which is also what the JK at sodas was
 *  measured emitting.  The C-rate check is unchanged by the correction —
 *  2800 counts is 280.0 A either way, 0.5 C of 560 Ah.
 *
 *  They stay TUNABLE because the argument they replace was sound and lost to
 *  a single observation; settling it the other way must not need a firmware
 *  cycle on a board reached only through a tunnel. */
#define BATCOMM_DFLT_CURRENT_SCALE_MA   100u    /* 0.1 A per count           */
#define BATCOMM_DFLT_CAPACITY_SCALE_MAH 1000u   /* 1.0 Ah per count          */

/** Consecutive unusable cycles before the wire is handed back (contract 4).
 *  Three, so one missed tick during an OTA does not flap the inverter's BMS
 *  link; recovery is immediate on the first good cycle, because there is no
 *  safety argument for staying away once the numbers are back. */
#define BATCOMM_DFLT_STALE_TRIP 3u

#define BATCOMM_PACK_NONE       0xFFu

/* Exported types -----------------------------------------------------------*/

/** Negative, like ePackErr and eModbusErr; no `_undefined = 0`, for the same
 *  reason the rest of the project omits one.  APPEND-ONLY: it reaches the
 *  HTTP surface. */
typedef enum {
    batErr_ok            =  0,
    batErr_badArg        = -1,
    batErr_notReady      = -2,
    batErr_busy          = -3,  /* a configuration is already staged         */
    batErr_unprovisioned = -4,
    batErr_transport     = -5,  /* the medium refused                        */
    batErr_notFound      = -6,
} eBatCommErr;

/** THE DIALECT.  One today.  PERSISTED — never renumbered, append only. */
typedef enum {
    batProto_none = 0,
    batProto_dynessLv,          /* the captured PowerBrick / DYNESS-L set    */
    batProto_last
} eBatCommProto;

/** Where the numbers come from.  PERSISTED — never renumbered. */
typedef enum {
    batSrc_none = 0,
    batSrc_cluster,             /* App/Cluster: N packs as one battery       */
    batSrc_pack,                /* App/Pack: one pack, translated            */
    batSrc_last
} eBatCommSource;

/** What the inverter sees when we cannot answer it.  PERSISTED. */
typedef enum {
    batFallback_bridge = 0,     /* drop the override: the real battery talks */
    batFallback_silent,         /* keep it: the inverter sees nothing        */
    batFallback_last
} eBatCommFallback;

/** Runtime only — never stored, so this one MAY be reordered. */
typedef enum {
    batState_disabled = 0,      /* unprovisioned, or "enabled": false        */
    batState_idle,              /* armed; the bridge is not in `bms` mode    */
    batState_emitting,
    batState_holdoff,           /* a bad cycle, still inside staleTrip       */
    batState_fallback,          /* the wire has been handed back             */
    batState_last
} eBatCommState;

/** WHY the module is in that state.  The values PARTITION. */
typedef enum {
    batWhy_ok = 0,
    batWhy_unprovisioned,
    batWhy_disabled,
    batWhy_notArmed,
    batWhy_notBmsMode,          /* the bridge forwards; we are a spectator   */
    batWhy_sourceNotReady,      /* nothing published yet                     */
    batWhy_sourceStale,
    batWhy_sourceBusy,
    batWhy_packNotFound,        /* the configured name resolves to no pack   */
    batWhy_packNotOnline,
    batWhy_last
} eBatCommWhy;

/** FIELD VALIDITY, this module's own vocabulary at its own seam.  A cleared
 *  bit means the field reads zero and MEANS NOTHING — it is never "no
 *  limit". */
typedef enum {
    batField_voltage            = 1u << 0,
    batField_current            = 1u << 1,
    batField_soc                = 1u << 2,
    batField_soh                = 1u << 3,
    batField_chargeLimit        = 1u << 4,
    batField_dischargeLimit     = 1u << 5,
    batField_chargeVoltLimit    = 1u << 6,
    batField_dischargeVoltLimit = 1u << 7,
    batField_temperature        = 1u << 8,
    batField_capacity           = 1u << 9,
    batField_switches           = 1u << 10,
} eBatCommField;

/* ==========================================================================
 * The neutral input — 56 bytes.
 * ONE STRUCT FOR BOTH SOURCES, so the encoder cannot grow a `if (cluster)`
 * and the host tests exercise the same bytes the board emits.  Filled by
 * batcomm.c from sClusterOutput or from sPackState; read by the pure encoder
 * and by nothing else.
 * ========================================================================== */

typedef struct {
    uint32_t voltage_mV;
    int32_t  current_mA;            /* + charge, - discharge (contract 1)    */
    uint32_t chargeLimit_mA;        /* UNSIGNED MAGNITUDE                    */
    uint32_t dischargeLimit_mA;
    uint32_t chargeVoltLimit_mV;
    uint32_t dischargeVoltLimit_mV;
    uint32_t remaining_mAh;
    uint32_t capacity_mAh;
    uint32_t alarms;                /* ePackAlarm — the PACK MODULE'S
                                       vocabulary, republished, never
                                       re-spelled here                       */
    uint16_t fields;                /* eBatCommField                         */
    uint16_t soc_pm;
    uint16_t soh_pm;
    int16_t  tempMax_dC;
    int16_t  tempMin_dC;
    uint8_t  modules;               /* 0x359 byte 4; >= 1                    */
    uint8_t  chargeAllowed;
    uint8_t  dischargeAllowed;
    uint8_t  rsvd;
} sBatCommIn;

/** One frame, before it is anything to do with CAN.  The encoder produces
 *  these; batcomm.c is the only file that turns one into an sCanFrame. */
typedef struct {
    uint32_t id;
    uint8_t  data[8];
    uint8_t  dlc;
    uint8_t  rsvd[3];
} sBatCommFrame;

/** The profile's tunables.  Every one of them is a fact about the RECEIVER or
 *  about a scale we measured, never about the battery. */
typedef struct {                                    /* 12 B                  */
    uint16_t slot_ms;               /* 50                                    */
    uint16_t slots;                 /* 20 -> a 1000 ms cycle                 */
    uint16_t currentScale_mA;       /* mA per count; 100 = 0.1 A             */
    uint16_t capacityScale_mAh;     /* mAh per count; 1000 = 1 Ah            */
    uint16_t staleTrip;             /* bad cycles before the wire goes back  */
    uint8_t  invertCurrent;         /* contract 1; UNMEASURED, default 0     */
    uint8_t  emitAlarms;            /* 0x359 alarm bytes; layout UNVERIFIED
                                       for this vendor, so it is a knob      */
} sBatCommTune;

typedef struct {                                    /* 36 B                  */
    char         packName[BATCOMM_NAME_LEN];        /* batSrc_pack only      */
    sBatCommTune tune;
    uint16_t     version;
    uint8_t      proto;             /* eBatCommProto                         */
    uint8_t      source;            /* eBatCommSource                        */
    uint8_t      inverterBus;       /* eCanBus, as a plain index             */
    uint8_t      fallback;          /* eBatCommFallback                      */
    uint8_t      enabled;
    uint8_t      batteryBusIsInput; /* contract 4                            */
} sBatCommCfg;

typedef struct {
    sBatCommIn in;                  /* the numbers the last cycle used       */
    uint32_t   cycles;
    uint32_t   goodCycles;
    uint32_t   framesSent;
    uint32_t   sendFailCnt;
    uint32_t   withheldCnt;         /* 0x351 not sent: no voltage limits     */
    uint32_t   fallbackEntries;
    uint32_t   lastGood_ms;
    uint32_t   badRun;              /* consecutive unusable cycles right now  */
    char       packName[BATCOMM_NAME_LEN];
    uint8_t    proto;               /* eBatCommProto                         */
    uint8_t    source;              /* eBatCommSource                        */
    uint8_t    fallback;            /* eBatCommFallback                      */
    uint8_t    state;               /* eBatCommState                         */
    uint8_t    why;                 /* eBatCommWhy                           */
    uint8_t    inverterBus;         /* eCanBus                               */
    uint8_t    batteryBus;          /* eCanBus — the other cell              */
    uint8_t    packIdx;             /* resolved, or BATCOMM_PACK_NONE        */
    uint8_t    armed;               /* holds the bridge's source slot        */
    uint8_t    enabled;
    uint8_t    provisioned;
    uint8_t    batteryBusIsInput;
} sBatCommStatus;

typedef fPackByteSource fBatCommByteSource;
typedef fPackByteSink   fBatCommByteSink;

typedef struct {
    int  ok;
    char field[24];
    char reason[64];
} sBatCommCfgResult;

/* Exported functions -------------------------------------------------------*/

/**
 * @brief  Load the configuration and put the bus roles in force.
 *
 * defaultTask, AFTER NvDbPlatform_Init and BEFORE CanBridge_Start: it calls
 * CanBridge_SetRoles(), which is legal only while the bridge is stopped.  An
 * unprovisioned board leaves the compiled-in roles alone and comes up exactly
 * as it does today.
 *
 * @retval batErr_ok always; not being configured is not an error
 */
int BatComm_Init(void);

/**
 * @brief  Take the bridge's frame-source slot and assert the override.
 *
 * Emission still requires the operator to put the bridge in `bms` mode; this
 * only decides WHO answers when they do.  Refuses on an unprovisioned or
 * disabled configuration, so a board that has never been told what to speak
 * cannot start speaking.
 *
 * @retval batErr_ok, batErr_unprovisioned, batErr_transport
 */
int BatComm_Arm(void);

/** @brief  Release the slot and drop the override.  Idempotent. */
int BatComm_Disarm(void);

/** @retval 1 while this module holds the bridge's source slot. */
int BatComm_IsArmed(void);

/** @brief  Copy the configuration, the counters and the last input used. */
int BatComm_GetStatus(sBatCommStatus *out);

int BatComm_ConfigVerify(fBatCommByteSource src, void *srcCtx,
                         sBatCommCfgResult *res);

/**
 * @brief  Parse, persist and APPLY a configuration.
 *
 * APPLIED, NOT STAGED, and the difference from the cluster is deliberate: a
 * bus role can only change while the bridge is stopped, so this call disarms
 * the source, restarts the bridge if the cell moved, and re-arms.  The source
 * callback therefore never observes a half-written configuration — it is not
 * running across the swap.
 *
 * Task context only (the HTTP task).  NEVER from the timer callback.
 *
 * @retval batErr_ok, batErr_badArg (res names the key), batErr_transport
 */
int BatComm_ConfigApply(fBatCommByteSource src, void *srcCtx,
                        sBatCommCfgResult *res);

/** @brief  Re-serialise the active configuration.  Data-faithful. */
int BatComm_ConfigExport(fBatCommByteSink sink, void *ctx);

/**
 * @brief  Erase it.  The module disarms and the board stops answering.
 * @note   There is no built-in default: a dialect and a cell are site facts.
 */
int BatComm_ConfigErase(void);

/* The name accessors.  Defined in batcomm_cfg.c, which is what
 * tests/test_batcomm links; -Werror=switch is scoped onto that file so a new
 * enumerator without a name fails the build rather than shipping a "?". */
const char *BatComm_ProtoName(eBatCommProto proto);
const char *BatComm_SourceName(eBatCommSource src);
const char *BatComm_FallbackName(eBatCommFallback fb);
const char *BatComm_StateName(eBatCommState st);
const char *BatComm_WhyName(eBatCommWhy why);

int BatComm_ProtoFromName(const char *name, eBatCommProto *out);

#ifdef __cplusplus
}
#endif

#endif /* BATCOMM_H_ */
