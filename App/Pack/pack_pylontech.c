/*
 * pack_pylontech.c
 *
 * Pack type: a Pylontech-speaking battery on CAN — the PUSH side of the type
 * contract (docs/design_battery_pack.md §11.2).  A Dyness Powerbrick speaks
 * this dialect.
 *
 * IT CANNOT BIND YET, AND THAT IS THE HONEST STATE RATHER THAN A STUB.
 * App/Can/bms_reader.c:158 defines the single weak
 * HAL_CAN_RxFifo0MsgPendingCallback, and a second definition of a weak symbol
 * is a link error — "whoever wins" is not a design.  So this type registers
 * its blueprint, parses its bind token, and refuses to bind with a stated
 * reason until App/Can grows one RX dispatcher that owns the callback and
 * fans out to bms_reader and to this file (§16 item 5).
 *
 * The core maps that refusal to packWhy_typeUnavailable, which is distinct
 * from packWhy_noType precisely for this case: the operator's configuration
 * is correct and the firmware is the limitation.
 *
 * Deliberately does NOT include pylontech.h yet.  When the dispatcher lands
 * this file will parse the PACKED FRAME STRUCTS into integers directly and
 * never touch sPylonBatteryData, whose fields are floats — no float crosses
 * this API, and none is created behind it.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Pack/pack_type.h"
#include "App/Pack/pack_types.h"
#include "App/Pack/pack_cfg.h"

#include "trice.h"

#include <stdlib.h>
#include <string.h>

/* Private types ------------------------------------------------------------*/

typedef struct {
    uint16_t nodeId;
    uint8_t  used;
} sPylonInst;

/* Private variables --------------------------------------------------------*/

static sPylonInst s_pylon[PACK_MAX];

/* Private function prototypes ----------------------------------------------*/

static int  Bind(const sPackBindInfo *info, sPackBindResult *res);
static int  Unbind(uint8_t idx);
static int  Submit(uint8_t idx, const sPackCommand *cmd, uint32_t timeout_ms);
static void Tick(uint8_t idx, uint32_t now_ms);

/**
 * The blueprint.
 *
 * capsMax is what an instance COULD offer once the dispatcher exists, and it
 * is deliberately short of the JK's: **capacity in amp-hours is absent from
 * the Pylontech frame set as implemented** — sPylonBatteryData has no capacity
 * field and 0x35F is unimplemented — so a consumer gets nameplate_mAh from
 * configuration and nothing else, which is exactly why nameplate is always
 * valid (§5, §6).
 *
 * cmdsMax is ZERO: nothing inbound is accepted in this dialect as
 * implemented.  §18 item 2 is an open question, not a guess made here.
 *
 * maxInstances is ONE: bms_reader.c's acceptance filter takes 0x350-0x35F with
 * no node discrimination at all, so two Pylontech packs on one bus are
 * indistinguishable.  A transport that cannot tell its packs apart says so
 * HERE, rather than letting a config declare three and having them overwrite
 * each other.
 */
static const sPackType s_pylontechType = {
    .name                    = "pylontech",
    .id                      = packType_pylontech,
    .capsMax                 = (uint32_t)packCap_temperatures  |
                               (uint32_t)packCap_currentLimits |
                               (uint32_t)packCap_switchState   |
                               (uint32_t)packCap_soh,
    .cmdsMax                 = 0u,   /* accepts nothing inbound              */
    .defaultStaleAfter_ms     = PACK_CFG_PYLON_STALE_MS,
    .ceiling                 = NULL,
    .ceilingCount            = 0u,
    .maxInstances            = 1u,
    .bind                    = Bind,
    .unbind                  = Unbind,
    .submit                  = Submit,
    .tick                    = Tick,
};

/* Private functions --------------------------------------------------------*/

/** "can:<nodeId>".  A CAN node id is a PROTOCOL ADDRESS, not a position in
 *  anyone's array — which is the whole reason bind tokens name physical
 *  addresses (§12). */
static int parse_bind(const char *bindKey, uint16_t *nodeId)
{
    const char *colon;
    long        v;

    if (bindKey == NULL) {
        return -1;
    }
    colon = strchr(bindKey, ':');
    if ((colon == NULL) || (strncmp(bindKey, "can", 3) != 0) ||
        ((colon - bindKey) != 3)) {
        return -1;
    }
    v = strtol(colon + 1, NULL, 0);
    if ((v < 0) || (v > 0xFFFF)) {
        return -1;
    }
    *nodeId = (uint16_t)v;
    return 0;
}

static int Bind(const sPackBindInfo *info, sPackBindResult *res)
{
    uint16_t nodeId = 0u;

    if ((info == NULL) || (res == NULL) || (info->idx >= PACK_MAX)) {
        return packErr_badArg;
    }

    /* The token is checked even though the bind cannot succeed, so a
     * malformed one is reported as malformed rather than being hidden behind
     * the missing dispatcher. */
    if (parse_bind(info->bindKey, &nodeId) != 0) {
        /* THE OPERATOR'S CONFIG is wrong here, not the firmware -- say so,
         * rather than letting this be relabelled "type unavailable". */
        res->why = (uint8_t)packWhy_noBinding;
        TRice("[Pack] pylontech: bad bind token\n");
        return packErr_badArg;
    }

    s_pylon[info->idx].nodeId = nodeId;
    s_pylon[info->idx].used   = 0u;

    /* THE PREREQUISITE, not a stub.  Until App/Can owns one RX dispatcher
     * this type has no way to receive a frame, and advertising capabilities
     * it cannot deliver is how a consumer learns to distrust the API. */
    res->why = (uint8_t)packWhy_typeUnavailable;
    TRice("[Pack] pylontech node %u: no CAN RX dispatcher, cannot bind\n",
          (unsigned)nodeId);
    return packErr_notSupported;
}

static int Unbind(uint8_t idx)
{
    if (idx >= PACK_MAX) {
        return packErr_badArg;
    }
    (void)memset(&s_pylon[idx], 0, sizeof(s_pylon[idx]));
    return packErr_ok;
}

static int Submit(uint8_t idx, const sPackCommand *cmd, uint32_t timeout_ms)
{
    (void)idx;
    (void)cmd;
    (void)timeout_ms;

    /* cmdsMax is 0, so the core refuses every command before it reaches
     * here.  This is the belt to that braces. */
    return packErr_notSupported;
}

static void Tick(uint8_t idx, uint32_t now_ms)
{
    (void)idx;
    (void)now_ms;

    /* WHEN THE DISPATCHER LANDS this is where a push type closes a partial
     * frame set that stopped arriving: publish when rxMask completes, or when
     * the tick finds an incomplete set older than ~1.2 s.  A sticky rxMask
     * that never completes because a frame is simply absent from the dialect
     * is why the timeout half has to exist at all. */
}

/* Exported functions -------------------------------------------------------*/

int PackPylontech_Register(void)
{
    return PackType_Register(&s_pylontechType);
}
