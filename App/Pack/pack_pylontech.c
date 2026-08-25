/*
 * pack_pylontech.c
 *
 * Pack type: a Pylontech-speaking pack over CAN — a PUSH type
 * (docs/design_battery_pack.md §11.2).  A Dyness Powerbrick speaks it.
 *
 * NO CAN WIRING IS PRESENT AND NONE MAY BE ADDED HERE YET.
 * App/Can/bms_reader.c:158 already defines the single weak
 * HAL_CAN_RxFifo0MsgPendingCallback; a second definition is a link error, and
 * "whoever wins" is not a design.  THIS TYPE IS BLOCKED until App/Can grows
 * ONE RX dispatcher — Can_RxSubscribe(id, mask, cb, ctx) — that owns the
 * callback and fans out to bms_reader and to this file.  A prerequisite, not
 * a detail (§16 item 5).
 *
 * WHEN THAT EXISTS, this type accumulates into staging from the CAN RX path
 * and publishes when its rxMask completes, or when tick() finds a frame set
 * older than 1.2 s.  It reads the PACKED FRAME STRUCTS in pylontech.h and
 * NEVER touches sPylonBatteryData, whose fields are floats and which is a
 * display struct (§16 item 6).
 *
 * IT MAY NOT TAKE A LOCK (§9), and here the ban is load-bearing rather than
 * theoretical: this exact code runs in an ISR.  PackType_Publish is the one
 * function that knows there are two contexts, and it is the core's.
 *
 * CAPACITY IS ABSENT FROM THE IMPLEMENTED FRAME SET — sPylonBatteryData has
 * no capacity field and 0x35F is unimplemented — so an instance of this type
 * advertises NO packCap_capacityAh.  That is precisely why nameplate_mAh is
 * always valid in sPackState: it comes from configuration, not from the wire.
 *
 * STILL OPEN (§18 item 2, deferred by decision until the hardware arrives):
 * which Pylontech dialect the Dyness speaks, whether it accepts any inbound
 * command, and whether several packs are distinguishable on one bus.  The
 * last one decides maxInstances, which is set to 1 below as the SAFE reading:
 * a transport that cannot distinguish two of its packs says so HERE rather
 * than letting a config declare three of them and having them overwrite each
 * other.
 *
 * STATUS: SCAFFOLDING.  Every callback refuses.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Pack/pack_type.h"

#include <stddef.h>

/* Private defines ----------------------------------------------------------*/

#define PACK_PYLON_UNUSED(x)   ((void)(x))

/* Private function prototypes ----------------------------------------------*/

static int  Bind(const sPackBindInfo *info, sPackBindResult *res);
static int  Unbind(uint8_t idx);
static int  Submit(uint8_t idx, const sPackCommand *cmd, uint32_t timeout_ms);
static void Tick(uint32_t now_ms);

/* Private variables --------------------------------------------------------*/

/**
 * The blueprint.  No packCap_capacityAh, no cell detail, no learned capacity:
 * the implemented frame set carries pack-level numbers only, which is also
 * why the per-cell estimator can never run behind this type.  cmdsMax is 0
 * until §18 item 2 is answered — advertising a command whose acceptance is
 * unknown is the §16-item-10 failure.
 */
static const sPackType s_pylontechType = {
    .name                     = "pylontech",
    .id                       = packType_pylontech,
    .capsMax                  = (uint32_t)packCap_soh |
                                (uint32_t)packCap_temperatures |
                                (uint32_t)packCap_currentLimits |
                                (uint32_t)packCap_switchState |
                                (uint32_t)packCap_cellSummary,
    .cmdsMax                  = 0u,
    .defaultStaleAfter_ms     = 5000u,      /* five missed 1 Hz frames      */
    .defaultCellStaleAfter_ms = 60000u,
    .maxInstances             = 1u,
    .bind                     = Bind,
    .unbind                   = Unbind,
    .submit                   = Submit,
    .tick                     = Tick,
};

/* Private functions --------------------------------------------------------*/

/**
 * @brief  Parse bindKey "can:<nodeId>" and register the node filter with the
 *         CAN RX dispatcher.
 * @retval packErr_notSupported (STUB, and correct until the dispatcher of
 *         §16 item 5 exists).  The instance stays packCond_absent with
 *         caps = 0.
 * @note   Shared func task only.
 */
static int Bind(const sPackBindInfo *info, sPackBindResult *res)
{
    PACK_PYLON_UNUSED(info);

    if (NULL != res) {
        res->caps       = 0u;
        res->cmds       = 0u;
        res->bounds     = NULL;
        res->boundCount = 0u;
    }
    return packErr_notSupported;
}

/**
 * @brief  Drop the CAN RX subscription.
 * @retval packErr_notSupported (STUB)
 * @note   Shared func task only.
 */
static int Unbind(uint8_t idx)
{
    PACK_PYLON_UNUSED(idx);
    return packErr_notSupported;
}

/**
 * @brief  Would put a command on the CAN bus.  Refuses unconditionally: the
 *         blueprint advertises cmdsMax = 0, so the core refuses first and
 *         this is never reached — belt and braces, because a type that could
 *         be reached with an unadvertised command is one that has to re-check
 *         bounds, and re-checking is expressly forbidden.
 * @retval packErr_notSupported
 * @note   Shared func task only.
 */
static int Submit(uint8_t idx, const sPackCommand *cmd, uint32_t timeout_ms)
{
    PACK_PYLON_UNUSED(idx);
    PACK_PYLON_UNUSED(cmd);
    PACK_PYLON_UNUSED(timeout_ms);
    return packErr_notSupported;
}

/**
 * @brief  Close a frame set that stopped arriving (older than 1.2 s) and
 *         publish what did arrive, naming ONLY the groups it actually filled
 *         so a partial delivery stays visible.
 * @note   Shared func task, ~4 Hz.  MUST NOT BLOCK.
 */
static void Tick(uint32_t now_ms)
{
    PACK_PYLON_UNUSED(now_ms);
}

/* Exported functions -------------------------------------------------------*/

/**
 * @brief  Register the pylontech blueprint.
 * @retval whatever PackType_Register returned
 * @note   Shared func task, at init.  Never blocks.
 */
int PackPylontech_Register(void)
{
    return PackType_Register(&s_pylontechType);
}
