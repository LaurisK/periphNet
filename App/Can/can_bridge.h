/*
 * can_bridge.h
 *
 *  Created on: 2026-09-03
 *      Author: Lauris
 *
 * THE BRIDGE.  The board sits between a battery and an inverter with one CAN
 * segment on each side, and this file decides what crosses.
 *
 * IT IS A STORE-AND-FORWARD BRIDGE, NOT A WIRE, and the difference matters
 * before anyone trusts it:
 *
 *   - Each side is its OWN collision domain.  The board acknowledges frames on
 *     both sides, so each device sees a healthy two-node bus even when the
 *     other side is unplugged.  A device that relies on the *absence* of an
 *     acknowledge to notice its peer is gone will no longer notice.
 *   - Arbitration is per side.  Two frames that would have contended on one
 *     shared wire no longer do, and the order in which the far side sees them
 *     is the order this board queued them, not the order the identifiers would
 *     have won.
 *   - Error frames do not cross.  A device that signals an error to its peer
 *     by destroying a frame cannot reach the other side; the counters in
 *     sCanBusStats are where that shows up instead.
 *   - One frame time of latency is added, more when the egress queue is deep.
 *   - Both sides must run the SAME bitrate.  A bridge does not translate rate.
 *
 * For the Pylontech dialect — one-way, periodic, unacknowledged, 1 Hz — none of
 * those costs anything, which is what makes this the right shape here.
 *
 * FOUR MODES, ONE MECHANISM.  `monitor` records and forwards nothing;
 * `bridge` is transparent both ways; `bms` BREAKS the bridge toward the
 * inverter and lets this board answer instead.  That last one is the point of
 * the whole exercise: once the battery cluster exists, the inverter must talk
 * to US, not to one pack, and the transition is a mode change on a bus that is
 * already up rather than a rewire.
 */

#ifndef CAN_BRIDGE_H_
#define CAN_BRIDGE_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Includes -----------------------------------------------------------------*/

#include <stdint.h>

#include "App/Can/can_bus.h"

/* Exported types -----------------------------------------------------------*/

/** Identifiers the board may take over one at a time in `bms` mode. */
#define CAN_BRIDGE_OVERRIDE_MAX     12u

/** Default bus roles.  They match the pinout in use: the inverter harness is
 *  on CAN1 (PD0/PD1) and the battery on CAN2 (PB5/PB6).  Both are
 *  configurable because which connector a harness lands on is a fact about a
 *  cabinet, not about this firmware. */
#define CAN_BRIDGE_INVERTER_BUS     canBus_1
#define CAN_BRIDGE_BATTERY_BUS      canBus_2

#define CAN_BRIDGE_DEFAULT_BPS      500000u

typedef enum {
    canBrMode_off = 0,      /* both buses stopped                            */
    canBrMode_monitor,      /* both buses up, NOTHING forwarded — a tap      */
    canBrMode_bridge,       /* transparent both ways                         */
    canBrMode_bms,          /* broken toward the inverter: WE answer it      */
    canBrMode_last
} eCanBrMode;

/** Per-direction accounting.  `suppressedCnt` is a frame the POLICY refused;
 *  `droppedCnt` is one the egress bus could not take.  Conflating them would
 *  hide a congested inverter side behind an intentional override. */
typedef struct {
    uint32_t forwardedCnt;
    uint32_t suppressedCnt;
    uint32_t droppedCnt;
} sCanBrDirStats;

typedef struct {
    sCanBrDirStats toInverter;
    sCanBrDirStats toBattery;
    uint32_t       bitrate_bps;
    uint32_t       sourceEmitCnt;      /* source callbacks actually run      */
    uint32_t       sourcePeriod_ms;
    uint32_t       overrideId[CAN_BRIDGE_OVERRIDE_MAX];
    uint8_t        overrideCnt;
    uint8_t        overrideAll;        /* 1 = take over the whole direction  */
    uint8_t        mode;               /* eCanBrMode                         */
    uint8_t        inverterBus;        /* eCanBus                            */
    uint8_t        batteryBus;         /* eCanBus                            */
    uint8_t        sourceBound;        /* 1 = something can answer the inverter */
} sCanBridgeStatus;

/**
 * @brief  What emits the battery frames when the bridge is broken.
 *
 * Called on the RTOS timer task at the configured period, in `bms` mode only.
 * It builds frames and hands them to CanBus_Send with `bus` set to
 * @p inverterBus.  Today `bms_sim` registers here; the battery cluster takes
 * the same slot later without the bridge learning anything new.
 *
 * @param  inverterBus - the bus to answer on
 * @param  ctx - whatever was handed to CanBridge_SetSource
 */
typedef void (*fCanBridgeSource)(eCanBus inverterBus, void *ctx);

/* Exported functions -------------------------------------------------------*/

/**
 * @brief  Bring both buses up in a mode.
 * @param  mode - canBrMode_monitor, _bridge or _bms
 * @param  bitrate_bps - the SAME rate on both sides; 0 keeps the current one
 * @retval 0 on success, negative on a bad argument or a peripheral failure
 * @note   Task context.
 */
int CanBridge_Start(eCanBrMode mode, uint32_t bitrate_bps);

/**
 * @brief  Stop forwarding and take both buses down.
 * @retval 0 always
 */
int CanBridge_Stop(void);

/**
 * @brief  Change what crosses, without touching the wire.
 *
 * canBrMode_off stops the buses; anything else on a stopped bridge starts it
 * at the last bitrate.
 *
 * @param  mode - the new mode
 * @retval 0 on success, negative on a bad argument
 */
int CanBridge_SetMode(eCanBrMode mode);

/** @retval the mode in force. */
eCanBrMode CanBridge_GetMode(void);

/**
 * @brief  Say which side is which.  Only legal while stopped.
 * @param  batteryBus - the cell the battery is wired to
 * @param  inverterBus - the cell the inverter is wired to
 * @retval 0 on success, negative when equal, invalid, or the bridge is up
 */
int CanBridge_SetRoles(eCanBus batteryBus, eCanBus inverterBus);

/** @retval the bus the inverter is on. */
eCanBus CanBridge_InverterBus(void);

/** @retval the bus the battery is on. */
eCanBus CanBridge_BatteryBus(void);

/**
 * @brief  Register what answers the inverter in `bms` mode.
 * @param  cb - the emitter, or NULL to unbind
 * @param  ctx - passed back unchanged
 * @param  period_ms - emit period; 0 keeps the current one
 * @retval 0 on success, negative on a bad argument
 */
int CanBridge_SetSource(fCanBridgeSource cb, void *ctx, uint32_t period_ms);

/**
 * @brief  Take over ONE identifier rather than the whole direction.
 *
 * With `overrideAll` cleared, a frame from the battery crosses to the inverter
 * unless its identifier is on this list — which is how a cluster can replace
 * the limits frame while the pack's own measurements still flow.
 *
 * @param  id - the identifier to suppress
 * @retval 0 on success, negative when the list is full
 */
int CanBridge_AddOverrideId(uint32_t id);

/** @brief  Empty the override list. */
void CanBridge_ClearOverrides(void);

/**
 * @brief  Choose between taking over everything and taking over a list.
 * @param  on - non-zero to suppress the whole battery-to-inverter direction
 */
void CanBridge_SetOverrideAll(int on);

/**
 * @brief  Copy the bridge's configuration and counters.
 * @param  out - destination
 * @retval 0 on success, negative on a bad argument
 */
int CanBridge_GetStatus(sCanBridgeStatus *out);

/** @brief  Zero the bridge's own counters.  Bus counters are separate. */
void CanBridge_ResetStats(void);

/**
 * @brief  Mode name, for the CLI and JSON.
 * @param  mode - the mode
 * @retval a static string; "?" for an out-of-range value
 */
const char *CanBridge_ModeName(eCanBrMode mode);

/**
 * @brief  Parse a mode name.
 * @param  name - "off", "monitor", "bridge" or "bms"
 * @param  out - the parsed mode
 * @retval 0 on success, negative when the name matches nothing
 */
int CanBridge_ModeFromName(const char *name, eCanBrMode *out);

#ifdef __cplusplus
}
#endif

#endif /* CAN_BRIDGE_H_ */
