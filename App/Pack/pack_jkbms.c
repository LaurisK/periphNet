/*
 * pack_jkbms.c
 *
 * Pack type: JK BMS over Modbus RTU — a PULL type
 * (docs/design_battery_pack.md §11.2).
 *
 * A type knows one protocol and one map.  It never sees the queue, the task,
 * the subscription table, another type, or A LOCK — the lock ban is
 * mechanical (§9): the same code is reachable from more than one context, so
 * a type is simply not allowed to have one.
 *
 * STATUS: SCAFFOLDING.  Nothing here touches the Modbus surface yet; the
 * blueprint is filled in and every callback refuses, so a bind leaves the
 * instance packCond_absent with caps = 0 — a reportable state, not a boot
 * failure.
 *
 * WHAT bind() MUST DO WHEN IMPLEMENTED (§11.2), all of it SYNCHRONOUSLY
 * because bind() returns a filled sPackBindResult:
 *   1. Modbus_PlanList() -> Modbus_Subscribe with the plans it recognises, or
 *      MB_PLAN_ALL while configs/gen_jk_config.py still names plans
 *      positionally (§18 follow-up).
 *   2. Resolve bindKey "<port>:<slaveAddr>" -> devOrd through
 *      Modbus_DeviceList(), matching BOTH portId and slaveAddr.  Zero matches
 *      or more than one leaves the instance absent with packWhy_noBinding
 *      rather than binding to whatever is there.
 *   3. Build the name -> ptOrd map with a SYNCHRONOUS WALK: Modbus_PointInfo
 *      from ptOrd 0 until mbErr_idNotFound.  NOT from the catalogue burst —
 *      that arrives later as mbEvt_pointDesc on the modbus task and cannot
 *      satisfy a synchronous return, so using it would reproduce inside bind
 *      exactly the acceptance/completion confusion submit() was designed to
 *      avoid.  modbus.h warns the walk is flash I/O; a bind IS a control path
 *      (once per instance per config change), which is why §13 marks bind as
 *      the one call that may block briefly.
 *   4. Confirm each capability against what the walk actually found, and each
 *      command against MB_PT_WRITE plus the point's writeMin/writeMax,
 *      NARROWED by info->cmdAllow and never widened.  Fill sPackBindResult.
 *   5. Issue one Modbus_Request for device_address (4360); on read-back match
 *      set packFlag_bindVerified.  This is the one ASYNCHRONOUS step and
 *      deliberately NOT a precondition: the instance binds, comes online, and
 *      gains the flag when the answer arrives.
 *
 * THE FRAME RULE, which follows from §8: mbEvt_txn IS A LEADING MARKER, so a
 * txn for this device CLOSES the previously open frame (publish) and opens a
 * new one; tick() closes a frame no further txn followed.  There is no
 * end-of-sequence event and strict whole-device coherence is neither
 * achievable nor needed — the JK's live table is exactly 8 points in ONE
 * block, so the electrical group is already atomic.
 *
 * SOC and SOH arrive as PACKED BYTES (balsta_soc lo, soh_precharge hi)
 * because there is no u8 decode type; this type splits them, as the MQTT
 * bridge does.
 *
 * BINDING IS BY PHYSICAL ADDRESS at the device level and BY POINT NAME at the
 * point level, so a reordered devices[] or a renumbered point is a LOST
 * BINDING (reported) rather than a WRONG BATTERY (silent).
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Pack/pack_type.h"

#include <stddef.h>

/* Private defines ----------------------------------------------------------*/

#define PACK_JK_UNUSED(x)   ((void)(x))

/* Private function prototypes ----------------------------------------------*/

static int  Bind(const sPackBindInfo *info, sPackBindResult *res);
static int  Unbind(uint8_t idx);
static int  Submit(uint8_t idx, const sPackCommand *cmd, uint32_t timeout_ms);
static void Tick(uint32_t now_ms);

/* Private variables --------------------------------------------------------*/

/**
 * The blueprint.  capsMax/cmdsMax are the MOST any instance could offer; what
 * an instance actually advertises is what its bind() confirmed against the
 * live config (§16 item 10).  maxInstances is 0 — a JK's {port, slaveAddr} is
 * unique by construction, so any number up to PACK_MAX is distinguishable.
 */
static const sPackType s_jkBmsType = {
    .name                     = "jkbms",
    .id                       = packType_jkBms,
    .capsMax                  = (uint32_t)packCap_capacityAh |
                                (uint32_t)packCap_learnedCapacity |
                                (uint32_t)packCap_soh |
                                (uint32_t)packCap_temperatures |
                                (uint32_t)packCap_currentLimits |
                                (uint32_t)packCap_switchState |
                                (uint32_t)packCap_cellSummary |
                                (uint32_t)packCap_cellDetail |
                                (uint32_t)packCap_leadResistance |
                                (uint32_t)packCap_balancer,
    .cmdsMax                  = PACK_CMD_BIT(packCmd_chargeEnable) |
                                PACK_CMD_BIT(packCmd_dischargeEnable) |
                                PACK_CMD_BIT(packCmd_balanceEnable) |
                                PACK_CMD_BIT(packCmd_chargeLimit) |
                                PACK_CMD_BIT(packCmd_dischargeLimit),
    .defaultStaleAfter_ms     = 15000u,     /* three missed 5 s laps        */
    .defaultCellStaleAfter_ms = 60000u,     /* four missed 15 s cell laps   */
    .maxInstances             = 0u,
    .bind                     = Bind,
    .unbind                   = Unbind,
    .submit                   = Submit,
    .tick                     = Tick,
};

/* Private functions --------------------------------------------------------*/

/**
 * @brief  Resolve bindKey and confirm this instance's capabilities.  See the
 *         file header for the five steps this owes when implemented.
 * @retval packErr_notSupported (STUB).  A failed bind leaves the instance
 *         packCond_absent with caps = 0.
 * @note   Shared func task only.  May block briefly (a catalogue walk).
 */
static int Bind(const sPackBindInfo *info, sPackBindResult *res)
{
    PACK_JK_UNUSED(info);

    if (NULL != res) {
        res->caps       = 0u;
        res->cmds       = 0u;
        res->bounds     = NULL;
        res->boundCount = 0u;
    }
    return packErr_notSupported;
}

/**
 * @brief  Drop the Modbus subscription and the name -> ptOrd map.
 * @retval packErr_notSupported (STUB)
 * @note   Shared func task only.
 */
static int Unbind(uint8_t idx)
{
    PACK_JK_UNUSED(idx);
    return packErr_notSupported;
}

/**
 * @brief  Put one already-validated command on the bus through
 *         Modbus_Request, completing later via PackType_CommandDone from
 *         fModbusReqDone on the modbus task.
 *
 * The core already validated capability and bounds; THIS MUST NOT RE-CHECK
 * THEM AND MUST NOT CLAMP.  The return value is ACCEPTANCE ONLY.
 *
 * @retval packErr_notSupported (STUB) — a refusal, so no completion follows
 * @note   Shared func task only.  May block briefly.
 */
static int Submit(uint8_t idx, const sPackCommand *cmd, uint32_t timeout_ms)
{
    PACK_JK_UNUSED(idx);
    PACK_JK_UNUSED(cmd);
    PACK_JK_UNUSED(timeout_ms);
    return packErr_notSupported;
}

/**
 * @brief  Close a frame no further txn followed, and notice the bus went
 *         quiet.  ~4 Hz on the shared task.
 * @note   Shared func task.  MUST NOT BLOCK.
 */
static void Tick(uint32_t now_ms)
{
    PACK_JK_UNUSED(now_ms);
}

/* Exported functions -------------------------------------------------------*/

/**
 * @brief  Register the jkbms blueprint.  Called once, before Pack_Init binds
 *         anything.
 * @retval whatever PackType_Register returned
 * @note   Shared func task, at init.  Never blocks.
 */
int PackJkBms_Register(void)
{
    return PackType_Register(&s_jkBmsType);
}
