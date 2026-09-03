/*
 * can_bridge.c
 *
 *  Created on: 2026-09-03
 *      Author: Lauris
 *
 * See can_bridge.h for the shape and for what a store-and-forward bridge
 * costs.  Two implementation decisions are worth stating here:
 *
 * FORWARDING HAPPENS IN THE RX ISR, not on a task.  A task would add a
 * scheduling delay to every frame and a queue that can overflow behind the
 * one the driver already has, in exchange for nothing: the work is a struct
 * copy, a policy test and an enqueue.  The subscriber contract in can_bus.h
 * is written for exactly this.
 *
 * THE EMITTER IS A CALLBACK ON AN RTOS TIMER, not a task either.  `bms` mode
 * has to produce six frames once a second; that is a timer's job, and it
 * keeps this module out of the sysmon task budget, which is at 13 of 16.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Can/can_bridge.h"
#include "App/Can/can_monitor.h"

#include "cmsis_os.h"
#include "stm32f4xx_hal.h"
#include "trice.h"

#include <stdbool.h>
#include <string.h>

/* Private defines ----------------------------------------------------------*/

#define CAN_BRIDGE_SOURCE_PERIOD_MS 1000u

/* Private types ------------------------------------------------------------*/

typedef struct {
    fCanBridgeSource source;
    void            *sourceCtx;
    osTimerId_t      sourceTimer;
    uint32_t         sourcePeriod_ms;
    uint32_t         sourceEmitCnt;
    uint32_t         bitrate_bps;
    uint32_t         overrideId[CAN_BRIDGE_OVERRIDE_MAX];
    sCanBrDirStats   toInverter;
    sCanBrDirStats   toBattery;
    int              subBattery;
    int              subInverter;
    uint8_t          overrideCnt;
    uint8_t          overrideAll;
    uint8_t          mode;
    uint8_t          inverterBus;
    uint8_t          batteryBus;
    uint8_t          started;
} sCanBridge;

/* Private variables --------------------------------------------------------*/

static sCanBridge s_br = {
    .sourceTimer     = NULL,
    .sourcePeriod_ms = CAN_BRIDGE_SOURCE_PERIOD_MS,
    .bitrate_bps     = CAN_BRIDGE_DEFAULT_BPS,
    .subBattery      = -1,
    .subInverter     = -1,
    .overrideAll     = 1u,
    .mode            = (uint8_t)canBrMode_off,
    .inverterBus     = (uint8_t)CAN_BRIDGE_INVERTER_BUS,
    .batteryBus      = (uint8_t)CAN_BRIDGE_BATTERY_BUS,
};

static const char *const s_modeName[canBrMode_last] = {
    "off", "monitor", "bridge", "bms"
};

/* Private function prototypes ----------------------------------------------*/

static int  IsOverridden(uint32_t id);
static void Forward(const sCanFrame *frame, uint8_t dstBus,
                    sCanBrDirStats *stats);
static void OnBatteryFrame(const sCanFrame *frame, void *ctx);
static void OnInverterFrame(const sCanFrame *frame, void *ctx);
static void SourceTimerCb(void *arg);
static void ApplySourceTimer(void);
static int  BringBusesUp(uint32_t bitrate_bps);
static void Unsubscribe(void);

/* Private functions --------------------------------------------------------*/

static int IsOverridden(uint32_t id)
{
    if (s_br.overrideAll != 0u) {
        return 1;
    }
    for (uint8_t i = 0u; i < s_br.overrideCnt; i++) {
        if (s_br.overrideId[i] == id) {
            return 1;
        }
    }
    return 0;
}

/** Put one frame on the other bus, unchanged but for which bus it belongs to. */
static void Forward(const sCanFrame *frame, uint8_t dstBus,
                    sCanBrDirStats *stats)
{
    sCanFrame out = *frame;

    out.bus = dstBus;
    if (CanBus_Send(&out) == 0) {
        stats->forwardedCnt++;
    } else {
        stats->droppedCnt++;
    }
}

/** ISR context.  Battery side -> inverter side. */
static void OnBatteryFrame(const sCanFrame *frame, void *ctx)
{
    (void)ctx;

    switch ((eCanBrMode)s_br.mode) {
    case canBrMode_bridge:
        Forward(frame, s_br.inverterBus, &s_br.toInverter);
        break;

    case canBrMode_bms:
        /* THE BREAK.  What we have taken over does not cross; whatever we have
         * not taken over still does, so a partial takeover is a real option
         * and not a rewrite. */
        if (IsOverridden(frame->id)) {
            s_br.toInverter.suppressedCnt++;
        } else {
            Forward(frame, s_br.inverterBus, &s_br.toInverter);
        }
        break;

    case canBrMode_monitor:
    case canBrMode_off:
    default:
        s_br.toInverter.suppressedCnt++;
        break;
    }
}

/** ISR context.  Inverter side -> battery side. */
static void OnInverterFrame(const sCanFrame *frame, void *ctx)
{
    (void)ctx;

    if ((eCanBrMode)s_br.mode == canBrMode_bridge) {
        Forward(frame, s_br.batteryBus, &s_br.toBattery);
    } else {
        /* In `bms` mode this board is the inverter's peer, so it terminates
         * the inverter's traffic rather than relaying it to a pack that is no
         * longer the one being addressed. */
        s_br.toBattery.suppressedCnt++;
    }
}

static void SourceTimerCb(void *arg)
{
    fCanBridgeSource source = s_br.source;

    (void)arg;
    if ((source == NULL) || ((eCanBrMode)s_br.mode != canBrMode_bms)) {
        return;
    }
    s_br.sourceEmitCnt++;
    source((eCanBus)s_br.inverterBus, s_br.sourceCtx);
}

/** The timer runs when, and only when, something can answer the inverter. */
static void ApplySourceTimer(void)
{
    bool wanted = ((eCanBrMode)s_br.mode == canBrMode_bms) &&
                  (s_br.source != NULL) && (s_br.started != 0u);

    if (wanted) {
        if (s_br.sourceTimer == NULL) {
            s_br.sourceTimer = osTimerNew(SourceTimerCb, osTimerPeriodic,
                                          NULL, NULL);
        }
        if (s_br.sourceTimer != NULL) {
            (void)osTimerStart(s_br.sourceTimer,
                               pdMS_TO_TICKS(s_br.sourcePeriod_ms));
        }
    } else if (s_br.sourceTimer != NULL) {
        (void)osTimerStop(s_br.sourceTimer);
    } else {
        /* nothing to stop */
    }
}

static int BringBusesUp(uint32_t bitrate_bps)
{
    /* The monitor is attached BEFORE the wire comes up, so the very first
     * frame either side sends is already counted. */
    CanMon_Attach();

    if (CanBus_Start((eCanBus)s_br.batteryBus, bitrate_bps) != 0) {
        return -1;
    }
    if (CanBus_Start((eCanBus)s_br.inverterBus, bitrate_bps) != 0) {
        (void)CanBus_Stop((eCanBus)s_br.batteryBus);
        return -2;
    }

    Unsubscribe();
    s_br.subBattery  = CanBus_Subscribe((eCanBus)s_br.batteryBus, 0u, 0u,
                                        OnBatteryFrame, NULL);
    s_br.subInverter = CanBus_Subscribe((eCanBus)s_br.inverterBus, 0u, 0u,
                                        OnInverterFrame, NULL);
    if ((s_br.subBattery < 0) || (s_br.subInverter < 0)) {
        Unsubscribe();
        (void)CanBus_Stop((eCanBus)s_br.batteryBus);
        (void)CanBus_Stop((eCanBus)s_br.inverterBus);
        return -3;
    }
    return 0;
}

static void Unsubscribe(void)
{
    if (s_br.subBattery >= 0) {
        (void)CanBus_Unsubscribe(s_br.subBattery);
        s_br.subBattery = -1;
    }
    if (s_br.subInverter >= 0) {
        (void)CanBus_Unsubscribe(s_br.subInverter);
        s_br.subInverter = -1;
    }
}

/* Exported functions -------------------------------------------------------*/

int CanBridge_Start(eCanBrMode mode, uint32_t bitrate_bps)
{
    uint32_t rate_bps = (bitrate_bps != 0u) ? bitrate_bps : s_br.bitrate_bps;

    if ((mode == canBrMode_off) || ((uint32_t)mode >= (uint32_t)canBrMode_last)) {
        return -1;
    }
    if (s_br.started != 0u) {
        (void)CanBridge_Stop();
    }
    if (BringBusesUp(rate_bps) != 0) {
        return -2;
    }

    s_br.bitrate_bps = rate_bps;
    s_br.mode        = (uint8_t)mode;
    s_br.started     = 1u;
    ApplySourceTimer();

    TRice("CAN bridge: %s, %u bps, battery CAN%u <-> inverter CAN%u\n",
          CanBridge_ModeName(mode), rate_bps,
          (unsigned)s_br.batteryBus + 1u, (unsigned)s_br.inverterBus + 1u);
    return 0;
}

int CanBridge_Stop(void)
{
    s_br.mode    = (uint8_t)canBrMode_off;
    s_br.started = 0u;
    ApplySourceTimer();
    Unsubscribe();
    (void)CanBus_Stop((eCanBus)s_br.batteryBus);
    (void)CanBus_Stop((eCanBus)s_br.inverterBus);
    TRice("CAN bridge: stopped\n");
    return 0;
}

int CanBridge_SetMode(eCanBrMode mode)
{
    if ((uint32_t)mode >= (uint32_t)canBrMode_last) {
        return -1;
    }
    if (mode == canBrMode_off) {
        return CanBridge_Stop();
    }
    if (s_br.started == 0u) {
        return CanBridge_Start(mode, 0u);
    }

    /* Live change: the wire stays up and the very next frame obeys the new
     * policy.  That is the whole point — breaking the bridge to become the
     * BMS must not drop the inverter's link while it happens. */
    s_br.mode = (uint8_t)mode;
    ApplySourceTimer();
    TRice("CAN bridge: mode %s\n", CanBridge_ModeName(mode));
    return 0;
}

eCanBrMode CanBridge_GetMode(void)
{
    return (eCanBrMode)s_br.mode;
}

int CanBridge_SetRoles(eCanBus batteryBus, eCanBus inverterBus)
{
    if ((batteryBus == inverterBus) ||
        ((uint32_t)batteryBus >= (uint32_t)canBus_last) ||
        ((uint32_t)inverterBus >= (uint32_t)canBus_last)) {
        return -1;
    }
    if (s_br.started != 0u) {
        return -2;
    }
    s_br.batteryBus  = (uint8_t)batteryBus;
    s_br.inverterBus = (uint8_t)inverterBus;
    return 0;
}

eCanBus CanBridge_InverterBus(void)
{
    return (eCanBus)s_br.inverterBus;
}

eCanBus CanBridge_BatteryBus(void)
{
    return (eCanBus)s_br.batteryBus;
}

int CanBridge_SetSource(fCanBridgeSource cb, void *ctx, uint32_t period_ms)
{
    if ((period_ms != 0u) && (period_ms < 50u)) {
        return -1;
    }
    s_br.source    = cb;
    s_br.sourceCtx = ctx;
    if (period_ms != 0u) {
        s_br.sourcePeriod_ms = period_ms;
    }
    ApplySourceTimer();
    return 0;
}

int CanBridge_AddOverrideId(uint32_t id)
{
    for (uint8_t i = 0u; i < s_br.overrideCnt; i++) {
        if (s_br.overrideId[i] == id) {
            return 0;
        }
    }
    if (s_br.overrideCnt >= CAN_BRIDGE_OVERRIDE_MAX) {
        return -1;
    }
    s_br.overrideId[s_br.overrideCnt] = id;
    s_br.overrideCnt++;
    return 0;
}

void CanBridge_ClearOverrides(void)
{
    s_br.overrideCnt = 0u;
}

void CanBridge_SetOverrideAll(int on)
{
    s_br.overrideAll = (on != 0) ? 1u : 0u;
}

int CanBridge_GetStatus(sCanBridgeStatus *out)
{
    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->toInverter      = s_br.toInverter;
    out->toBattery       = s_br.toBattery;
    out->bitrate_bps     = s_br.bitrate_bps;
    out->sourceEmitCnt   = s_br.sourceEmitCnt;
    out->sourcePeriod_ms = s_br.sourcePeriod_ms;
    out->overrideCnt     = s_br.overrideCnt;
    out->overrideAll     = s_br.overrideAll;
    out->mode            = s_br.mode;
    out->inverterBus     = s_br.inverterBus;
    out->batteryBus      = s_br.batteryBus;
    out->sourceBound     = (s_br.source != NULL) ? 1u : 0u;
    memcpy(out->overrideId, s_br.overrideId, sizeof(out->overrideId));
    return 0;
}

void CanBridge_ResetStats(void)
{
    memset(&s_br.toInverter, 0, sizeof(s_br.toInverter));
    memset(&s_br.toBattery, 0, sizeof(s_br.toBattery));
    s_br.sourceEmitCnt = 0u;
}

const char *CanBridge_ModeName(eCanBrMode mode)
{
    if ((uint32_t)mode >= (uint32_t)canBrMode_last) {
        return "?";
    }
    return s_modeName[mode];
}

int CanBridge_ModeFromName(const char *name, eCanBrMode *out)
{
    if ((name == NULL) || (out == NULL)) {
        return -1;
    }
    for (uint8_t i = 0u; i < (uint8_t)canBrMode_last; i++) {
        size_t len = strlen(s_modeName[i]);

        if (strncmp(name, s_modeName[i], len) == 0) {
            char next = name[len];

            if ((next == '\0') || (next == ' ') || (next == '\r') ||
                (next == '\n') || (next == '&')) {
                *out = (eCanBrMode)i;
                return 0;
            }
        }
    }
    return -1;
}
