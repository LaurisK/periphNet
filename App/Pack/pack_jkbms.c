/*
 * pack_jkbms.c
 *
 * Pack type: a JK PB-series BMS reached over Modbus RTU — the PULL side of
 * the type contract (docs/design_battery_pack.md §11.2).
 *
 * It knows one protocol and one register map, and translates them into the
 * core's neutral vocabulary.  It never sees the queue, the task, the
 * subscription table, another type, or a lock — CMake fails the build if a
 * lock appears here, because a lock is a context decision and this file is
 * reached from the modbus task in two different ways.
 *
 * BINDING IS BY PHYSICAL ADDRESS AND BY POINT NAME, never by an ordinal:
 * `devOrd` is a position in another module's array, and reordering it would
 * silently rebind a pack to a different battery (§12).
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Pack/pack_type.h"
#include "App/Pack/pack_types.h"
#include "App/Pack/pack_cfg.h"
#include "App/Modbus/modbus.h"

#include "cmsis_os.h"          /* osKernelGetTickCount, for the frame clock */

#include "trice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Private defines ----------------------------------------------------------*/

#define JK_PT_NONE      0xFFFFu     /* "this point is not in the config"     */

/* Private defines ----------------------------------------------------------*/

/* How long an OPEN frame may stay open before Tick publishes it anyway.
 * A frame is normally closed by the next txn for the same device; these are
 * the fallback for when the transport simply stops, so they sit just past
 * one lap of the plan that feeds each group (5 s live, 15 s cells). */
#define JK_FRAME_CLOSE_MS   7000u
#define JK_CELL_CLOSE_MS   20000u

/* Private types ------------------------------------------------------------*/

/** The point ordinals this type needs, resolved by NAME at bind.  A renumbered
 *  point becomes a LOST binding (reported) rather than a WRONG value. */
typedef struct {
    uint16_t packVoltage;
    uint16_t packCurrent;
    uint16_t balstaSoc;
    uint16_t remaining;
    uint16_t fullCapacity;
    uint16_t sohPrecharge;
    uint16_t chgDsgState;
    uint16_t cellMinMaxNbr;
    uint16_t balanceCurrent;
    uint16_t balancePwmDsg;
    uint16_t mosTemp;
    uint16_t tempBat1;
    uint16_t tempBat2;
    uint16_t alarms;
    uint16_t chargeEnable;
    uint16_t dischargeEnable;
    uint16_t balanceEnable;
    uint16_t chargeLimit;
    uint16_t dischargeLimit;
    uint16_t cell[PACK_CELLS_MAX];
    uint16_t leadRes[PACK_CELLS_MAX];
} sJkPoints;

typedef struct {
    sJkPoints     pt;
    sPackCmdBound bounds[packCmd_last];
    uint32_t      groups;           /* accumulated since the last txn        */
    uint32_t      cellGroups;
    uint32_t      frameOpen_ms;     /* when the open frame started, for Tick  */
    uint32_t      cellOpen_ms;
    uint8_t       tempSeen;         /* a temperature landed in THIS frame     */
    uint8_t       balanceState;     /* hi byte of balsta_soc                  */
    uint8_t       unbindPending;    /* Unbind waited for a borrowed reqItem   */
    uint8_t       boundCount;
    uint8_t       devOrd;
    uint8_t       used;
    uint8_t       cmdPending;       /* a Modbus_Request is outstanding       */
    sModbusReqItem reqItem;         /* borrowed by the module until done     */
} sJkInst;

/* Private variables --------------------------------------------------------*/

static sJkInst s_jk[PACK_MAX];
static int     s_sub = -1;

/* Private function prototypes ----------------------------------------------*/

static int  Bind(const sPackBindInfo *info, sPackBindResult *res);
static int  Unbind(uint8_t idx);
static int  Submit(uint8_t idx, const sPackCommand *cmd, uint32_t timeout_ms);
/**
 * @brief  Close this instance's cell frame: derive the extremes from the
 *         WHOLE array, then publish.
 *
 * Recomputing beats folding because staging outlives a frame: a fold has no
 * reset point, and PackType_Publish is a frame CLOSE with no counterpart
 * that opens one.
 */
static void CloseCellFrame(uint8_t idx)
{
    sPackCells *cells = PackType_CellStaging(idx);
    sPackRaw   *raw   = PackType_Staging(idx);
    uint8_t     c;

    if ((cells != NULL) && (raw != NULL) && (cells->cellCount > 0u)) {
        uint16_t hi = cells->cell_mV[0];
        uint16_t lo = cells->cell_mV[0];

        for (c = 1u; c < cells->cellCount; c++) {
            if (cells->cell_mV[c] > hi) { hi = cells->cell_mV[c]; }
            if (cells->cell_mV[c] < lo) { lo = cells->cell_mV[c]; }
        }
        raw->cellMax_mV = hi;
        raw->cellMin_mV = lo;
    }
    PackType_PublishCells(idx);
    s_jk[idx].cellGroups = 0u;
}

static void Tick(uint8_t idx, uint32_t now_ms);

static void ModbusEvent(const sModbusEvent *ev, void *ctx);

/* The blueprint.  capsMax/cmdsMax are the MOST any instance could offer;
 * what an instance advertises is what Bind() confirmed against the live
 * config (§10.2). */
static const sPackType s_jkBmsType = {
    /* DESIGNATED, deliberately: sPackType is expected to grow, and with
     * positional fields `ceiling`/`ceilingCount`/`maxInstances` sit directly
     * before the four function pointers -- inserting one field silently
     * rewires bind() into maxInstances and the compiler accepts most of it. */
    .name                    = "jkbms",
    .id                      = packType_jkBms,
    .capsMax                 = (uint32_t)packCap_capacityAh    |
                               (uint32_t)packCap_soh           |
                               (uint32_t)packCap_temperatures  |
                               (uint32_t)packCap_currentLimits |
                               (uint32_t)packCap_switchState   |
                               (uint32_t)packCap_cellSummary   |
                               (uint32_t)packCap_cellDetail    |
                               (uint32_t)packCap_leadResistance |
                               (uint32_t)packCap_balancer,
    .cmdsMax                 = PACK_CMD_BIT(packCmd_chargeEnable)    |
                               PACK_CMD_BIT(packCmd_dischargeEnable) |
                               PACK_CMD_BIT(packCmd_balanceEnable)   |
                               PACK_CMD_BIT(packCmd_chargeLimit)     |
                               PACK_CMD_BIT(packCmd_dischargeLimit),
    .defaultStaleAfter_ms     = PACK_CFG_JK_STALE_MS,
    .ceiling                 = NULL,    /* ceiling lives in pack_cfg.c        */
    .ceilingCount            = 0u,
    .maxInstances            = 0u,      /* up to PACK_MAX                     */
    .bind                    = Bind,
    .unbind                  = Unbind,
    .submit                  = Submit,
    .tick                    = Tick,
};

/* Private functions --------------------------------------------------------*/

static void points_clear(sJkPoints *p)
{
    uint32_t i;
    uint16_t *w = (uint16_t *)p;

    for (i = 0u; i < (sizeof(*p) / sizeof(uint16_t)); i++) {
        w[i] = JK_PT_NONE;
    }
}

/** "<port>:<slaveAddr>" -> the ordinal Modbus currently gives that device.
 *  Resolved fresh on every bind and re-resolved on every mbEvt_config, so a
 *  reordered devices[] is a lost binding rather than a wrong battery. */
static int resolve_dev(const char *bindKey, uint8_t *devOrd,
                       uint8_t *polled)
{
    sModbusDeviceInfo dev[MB_MAX_DEVICES];
    const char       *colon;
    char              port[8];
    uint32_t          portLen;
    long              slave;
    int               n;
    int               i;
    int               found = -1;

    if (bindKey == NULL) {
        return -1;
    }
    colon = strchr(bindKey, ':');
    if (colon == NULL) {
        return -1;
    }
    portLen = (uint32_t)(colon - bindKey);
    if ((portLen == 0u) || (portLen >= sizeof(port))) {
        return -1;
    }
    (void)memcpy(port, bindKey, portLen);
    port[portLen] = '\0';
    slave = strtol(colon + 1, NULL, 10);
    if ((slave <= 0) || (slave > 247)) {
        return -1;
    }

    n = Modbus_DeviceList(dev, MB_MAX_DEVICES);
    for (i = 0; i < n; i++) {
        const char *pname = (dev[i].portId == (uint8_t)mbPort_rs485) ? "rs485"
                          : (dev[i].portId == (uint8_t)mbPort_test)  ? "test"
                          : "";

        if ((dev[i].slaveAddr == (uint8_t)slave) &&
            (strcmp(pname, port) == 0)) {
            if (found >= 0) {
                return -1;          /* ambiguous: refuse rather than guess   */
            }
            found = i;
        }
    }
    if (found < 0) {
        return -1;
    }
    *devOrd = dev[found].devOrd;
    if (polled != NULL) {
        *polled = (dev[found].polled != 0u) ? 1u : 0u;
    }
    return 0;
}

/** Walk the device's points SYNCHRONOUSLY and resolve each by name.
 *
 *  Deliberately not the catalogue burst: Bind() returns a filled
 *  sPackBindResult, so it must confirm capabilities synchronously, and the
 *  catalogue arrives later as an mbEvt_pointDesc burst on the modbus task.
 *  Using it would reproduce, in bind, exactly the acceptance/completion
 *  confusion that Submit was designed to avoid.  modbus.h:474 warns that
 *  Modbus_PointInfo is a flash walk and belongs on a control path rather than
 *  in a loop — a bind IS a control path, once per instance per config change.
 */
static void walk_points(uint8_t devOrd, sJkInst *in, uint32_t *caps,
                        uint32_t *cmds)
{
    uint16_t ptOrd;
    uint8_t  cells = 0u;
    uint8_t  leads = 0u;

    for (ptOrd = 0u; ptOrd < 512u; ptOrd++) {
        sModbusPointMeta m;
        int              idx;

        if (Modbus_PointInfo(devOrd, ptOrd, &m) != 0) {
            break;                  /* mbErr_idNotFound: end of the map      */
        }

        if (strncmp(m.name, "cell_wire_res", 13) == 0) {
            idx = atoi(&m.name[13]);
            if ((idx >= 0) && (idx < (int)PACK_CELLS_MAX)) {
                in->pt.leadRes[idx] = ptOrd;
                leads++;
            }
        } else if ((strncmp(m.name, "cell", 4) == 0) &&
                   (m.name[4] >= '0') && (m.name[4] <= '9')) {
            idx = atoi(&m.name[4]);
            if ((idx >= 0) && (idx < (int)PACK_CELLS_MAX)) {
                in->pt.cell[idx] = ptOrd;
                cells++;
            }
        }
#define JK_MAP(field, str)                                                  \
        else if (strcmp(m.name, (str)) == 0) { in->pt.field = ptOrd; }
        JK_MAP(packVoltage,     "pack_voltage")
        JK_MAP(packCurrent,     "pack_current")
        JK_MAP(balstaSoc,       "balsta_soc")
        JK_MAP(remaining,       "remaining_capacity")
        JK_MAP(fullCapacity,    "full_capacity")
        JK_MAP(sohPrecharge,    "soh_precharge")
        JK_MAP(chgDsgState,     "chg_dsg_state")
        JK_MAP(cellMinMaxNbr,   "cell_minmax_nbr")
        JK_MAP(balanceCurrent,  "balance_current")
        JK_MAP(balancePwmDsg,   "balance_pwm_dsg")
        JK_MAP(mosTemp,         "mos_temp")
        JK_MAP(tempBat1,        "temp_bat1")
        JK_MAP(tempBat2,        "temp_bat2")
        JK_MAP(alarms,          "alarms")
        JK_MAP(chargeEnable,    "charge_enable")
        JK_MAP(dischargeEnable, "discharge_enable")
        JK_MAP(balanceEnable,   "balance_enable")
        JK_MAP(chargeLimit,     "charge_current_max")
        JK_MAP(dischargeLimit,  "discharge_current_max")
#undef JK_MAP
        else {
            continue;
        }

        /* A capability is advertised only when the live config actually backs
         * it AND an accessor exists for it (§10.2). */
        if ((m.flags & MB_PT_WRITE) != 0u) {
            ePackCmdId cmd = packCmd_undefined;

            if (strcmp(m.name, "charge_enable") == 0) {
                cmd = packCmd_chargeEnable;
            } else if (strcmp(m.name, "discharge_enable") == 0) {
                cmd = packCmd_dischargeEnable;
            } else if (strcmp(m.name, "balance_enable") == 0) {
                cmd = packCmd_balanceEnable;
            } else if (strcmp(m.name, "charge_current_max") == 0) {
                cmd = packCmd_chargeLimit;
            } else if (strcmp(m.name, "discharge_current_max") == 0) {
                cmd = packCmd_dischargeLimit;
            }

            if ((cmd != packCmd_undefined) &&
                ((m.flags & MB_PT_BOUNDED) != 0u) &&
                (in->boundCount < (uint8_t)packCmd_last)) {
                in->bounds[in->boundCount].cmd        = cmd;
                in->bounds[in->boundCount].min_scaled = m.writeMin;
                in->bounds[in->boundCount].max_scaled = m.writeMax;
                in->boundCount++;
                *cmds |= PACK_CMD_BIT(cmd);
            }
        }
    }

    if (in->pt.remaining != JK_PT_NONE) {
        *caps |= (uint32_t)packCap_capacityAh;
    }
    if (in->pt.sohPrecharge != JK_PT_NONE) {
        *caps |= (uint32_t)packCap_soh;
    }
    if (in->pt.mosTemp != JK_PT_NONE) {
        *caps |= (uint32_t)packCap_temperatures;
    }
    if ((in->pt.chargeLimit != JK_PT_NONE) &&
        (in->pt.dischargeLimit != JK_PT_NONE)) {
        *caps |= (uint32_t)packCap_currentLimits;
    }
    if (in->pt.chgDsgState != JK_PT_NONE) {
        *caps |= (uint32_t)packCap_switchState;
    }
    if (cells > 0u) {
        *caps |= (uint32_t)packCap_cellSummary | (uint32_t)packCap_cellDetail;
    }
    if (leads > 0u) {
        *caps |= (uint32_t)packCap_leadResistance;
    }
    if (in->pt.balanceCurrent != JK_PT_NONE) {
        *caps |= (uint32_t)packCap_balancer;
    }
}

static int Bind(const sPackBindInfo *info, sPackBindResult *res)
{
    sJkInst *in;
    uint8_t  polled = 0u;
    uint32_t caps = 0u;
    uint32_t cmds = 0u;

    if ((info == NULL) || (res == NULL) || (info->idx >= PACK_MAX)) {
        return packErr_badArg;
    }
    in = &s_jk[info->idx];
    (void)memset(in, 0, sizeof(*in));
    points_clear(&in->pt);

    if (resolve_dev(info->bindKey, &in->devOrd, &polled) != 0) {
        /* Zero matches or several: the instance stays absent with a stated
         * reason rather than binding to whatever is there. */
        res->why = (uint8_t)packWhy_noBinding;
        return packErr_notFound;
    }

    walk_points(in->devOrd, in, &caps, &cmds);
    if (caps == 0u) {
        return packErr_notFound;    /* the device carries no point we know   */
    }

    in->used = 1u;

    /* One subscription serves every instance: planMask does no event routing
     * today and the shipped config names plans positionally, so this filters
     * by devOrd regardless (§8). */
    if (s_sub < 0) {
        s_sub = Modbus_Subscribe(MB_PLAN_ALL,
                                 (uint32_t)mbEvt_sample | (uint32_t)mbEvt_txn |
                                 (uint32_t)mbEvt_config,
                                 ModbusEvent, NULL);
    }

    res->caps       = caps;
    res->cmds       = cmds;
    res->bounds     = in->bounds;
    res->boundCount = in->boundCount;
    /* THE BIND SUCCEEDED, but if no live plan reads this device it will
     * never receive a sample -- and reporting that as packWhy_noReply sends
     * an operator to look at the battery when the fault is in the Modbus
     * plan.  §8 names this exact case; packWhy_notPolled exists for it and
     * nothing had ever produced it. */
    res->why        = (polled != 0u) ? (uint8_t)packWhy_none
                                     : (uint8_t)packWhy_notPolled;

    TRice("[Pack] jkbms bound dev=%u caps=%08x cmds=%02x\n",
          (unsigned)in->devOrd, (unsigned)caps, (unsigned)cmds);
    return packErr_ok;
}

static int Unbind(uint8_t idx)
{
    if (idx >= PACK_MAX) {
        return packErr_badArg;
    }

    /* THE ITEM ARRAY IS BORROWED BY THE MODBUS MODULE until the completion
     * fires (modbus.h: "the only moment at which the caller may free or
     * reuse the item array").  Clearing the slot with a request outstanding
     * let the engine form a frame from a zeroed item -- ptOrd 0 of that
     * device -- or write its result into memory a re-bound instance now
     * owns, clearing the NEW instance's cmdPending.
     *
     * Stand the instance down so no sample is decoded into it, but leave the
     * borrowed storage intact; ReqDone finishes it and the memset happens
     * then.  Both run on the func task, so the flag is enough. */
    s_jk[idx].used = 0u;
    if (s_jk[idx].cmdPending != 0u) {
        s_jk[idx].unbindPending = 1u;
        return packErr_ok;
    }

    (void)memset(&s_jk[idx], 0, sizeof(s_jk[idx]));
    return packErr_ok;
}

/** Completion for the one outstanding write.  Runs on the MODBUS task. */
static void ReqDone(const sModbusReqReply *rep, void *ctx)
{
    const uint8_t idx = (uint8_t)(uintptr_t)ctx;
    ePackErr      r;

    if (idx >= PACK_MAX) {
        return;
    }
    s_jk[idx].cmdPending = 0u;

    /* THE RESULT IS PER ITEM, not per reply — docs/modbus.md §11a.12 is
     * explicit that a request can succeed while its items fail. */
    if ((rep == NULL) || (rep->items == NULL) || (rep->count == 0u)) {
        r = packErr_transport;
    } else {
        const int16_t e = rep->items[0].result;

        if (e == (int16_t)mbErr_ok) {
            /* ACCEPTED ON THE WIRE.  Not "the pack changed state" — the
             * evidence for that is chargeSwitch in the next published
             * state (§10.8). */
            r = packErr_ok;
        } else if ((e <= (int16_t)mbErr_excIllegalFunction) &&
                   (e >= (int16_t)mbErr_excOther)) {
            r = packErr_refused;    /* the pack answered and said no         */
        } else if (e == (int16_t)mbErr_outOfRange) {
            r = packErr_outOfRange; /* the module's own bound check           */
        } else if (e == (int16_t)mbErr_timeout) {
            r = packErr_timeout;    /* UNDECIDED: it may well have landed    */
        } else {
            r = packErr_transport;
        }
    }

    PackType_CommandDone(idx, r);

    /* The borrow has ended: if an Unbind was waiting on it, finish the
     * teardown now. */
    if (s_jk[idx].unbindPending != 0u) {
        (void)memset(&s_jk[idx], 0, sizeof(s_jk[idx]));
    }
}

static int Submit(uint8_t idx, const sPackCommand *cmd, uint32_t timeout_ms)
{
    sJkInst *in;
    uint16_t ptOrd = JK_PT_NONE;

    if ((idx >= PACK_MAX) || (cmd == NULL)) {
        return packErr_badArg;
    }
    in = &s_jk[idx];
    if (in->used == 0u) {
        return packErr_notOnline;
    }
    if (in->cmdPending != 0u) {
        return packErr_busy;
    }

    switch (cmd->cmd) {
    case packCmd_chargeEnable:    ptOrd = in->pt.chargeEnable;    break;
    case packCmd_dischargeEnable: ptOrd = in->pt.dischargeEnable; break;
    case packCmd_balanceEnable:   ptOrd = in->pt.balanceEnable;   break;
    case packCmd_chargeLimit:     ptOrd = in->pt.chargeLimit;     break;
    case packCmd_dischargeLimit:  ptOrd = in->pt.dischargeLimit;  break;
    default:                      return packErr_notSupported;
    }
    if (ptOrd == JK_PT_NONE) {
        return packErr_notSupported;
    }

    /* The core already validated capability and bounds; a type must not
     * re-check them and must not clamp (§11). */
    in->reqItem.id    = ptOrd;
    in->reqItem.value = cmd->value;

    if (Modbus_Request(in->devOrd, &in->reqItem, 1u, timeout_ms,
                       ReqDone, (void *)(uintptr_t)idx) != 0) {
        return packErr_transport;
    }

    /* ACCEPTANCE ONLY.  The outcome arrives later through ReqDone ->
     * PackType_CommandDone, exactly once. */
    in->cmdPending = 1u;
    return packErr_ok;
}

static void Tick(uint8_t idx, uint32_t now_ms)
{
    sJkInst *in;

    if (idx >= PACK_MAX) {
        return;
    }
    in = &s_jk[idx];
    if (in->used == 0u) {
        return;
    }

    /* A PULL TYPE DOES HAVE SOMETHING TO CLOSE.  A frame is closed by the
     * NEXT txn for this device (§8: mbEvt_txn is a leading marker), so when
     * the transport goes quiet the last frame accumulated is never published
     * at all -- the most interesting one, since it is the last word from a
     * pack that is about to go stale.  Close it once the frame has been open
     * longer than a lap. */
    if ((in->groups != 0u) &&
        ((uint32_t)(now_ms - in->frameOpen_ms) >= JK_FRAME_CLOSE_MS)) {
        PackType_Publish(idx, in->groups);
        in->groups = 0u;
    }
    if ((in->cellGroups != 0u) &&
        ((uint32_t)(now_ms - in->cellOpen_ms) >= JK_CELL_CLOSE_MS)) {
        CloseCellFrame(idx);
    }
}

/**
 * The Modbus subscriber.  Runs IN THE MODBUS TASK, synchronously, and must not
 * block: every pointer is borrowed until return, so this copies and gets out.
 *
 * mbEvt_txn is a LEADING marker (§8): everything between txn N and txn N+1
 * belongs to txn N.  So a txn for this device CLOSES the previously open
 * frame and opens a new one.
 */
static void ModbusEvent(const sModbusEvent *ev, void *ctx)
{
    uint8_t i;

    (void)ctx;

    if (ev->type == mbEvt_config) {
        /* Every cached ordinal is now suspect: a config swap renumbers every
         * devOrd, so a Modbus_Request issued now would reach a DIFFERENT
         * battery.  Stand every instance down immediately...  */
        for (i = 0u; i < PACK_MAX; i++) {
            s_jk[i].used = 0u;
        }
        /* ...and ASK THE CORE TO RE-BIND.  Without this the instances stayed
         * down for good: only the core holds the configuration and the post
         * function, so a type standing itself down could never come back, and
         * one `POST /api/modbus/config/apply` darkened every JK pack on the
         * board until the next reboot. */
        PackType_RequestRebind();
        return;
    }

    if (ev->type == mbEvt_txn) {
        for (i = 0u; i < PACK_MAX; i++) {
            sJkInst *in = &s_jk[i];

            if ((in->used == 0u) || (in->devOrd != ev->u.txn.devOrd)) {
                continue;
            }
            /* An exception is an ANSWER: the slave replied, so it is alive. */
            PackType_NoteLiveness(i,
                (ev->u.txn.err == mbErr_ok) ||
                ((ev->u.txn.err <= mbErr_excIllegalFunction) &&
                 (ev->u.txn.err >= mbErr_excOther)));

            if (in->groups != 0u) {
                PackType_Publish(i, in->groups);
                in->groups = 0u;
            }
            if (in->cellGroups != 0u) {
                CloseCellFrame(i);
            }
        }
        return;
    }

    if (ev->type != mbEvt_sample) {
        return;
    }

    for (i = 0u; i < PACK_MAX; i++) {
        sJkInst   *in = &s_jk[i];
        sPackRaw  *raw;
        sPackCells *cells;
        uint16_t   pt;
        uint8_t    c;

        if ((in->used == 0u) || (in->devOrd != ev->u.sample.pt->devOrd)) {
            continue;
        }
        /* Stamp the moment a frame OPENS, so Tick can tell an open frame
         * that is still filling from one whose transport went quiet. */
        if (in->groups == 0u) {
            in->frameOpen_ms = (uint32_t)osKernelGetTickCount();
        }
        if (in->cellGroups == 0u) {
            in->cellOpen_ms = (uint32_t)osKernelGetTickCount();
        }

        raw = PackType_Staging(i);
        if (raw == NULL) {
            continue;
        }
        pt = ev->u.sample.pt->ptOrd;

        if (pt == in->pt.packVoltage) {
            raw->voltage_mV = (uint32_t)ev->u.sample.value;
            in->groups |= PACK_GRP_BIT(packGrp_electrical);
        } else if (pt == in->pt.packCurrent) {
            raw->current_mA = ev->u.sample.value;
            in->groups |= PACK_GRP_BIT(packGrp_electrical);
        } else if (pt == in->pt.balstaSoc) {
            /* Packed bytes: hi = balance state, lo = SOC %.  There is no u8
             * decode type, so the type splits them — as the bridge does. */
            const uint32_t v = (uint32_t)ev->u.sample.value;

            raw->soc_pm     = (uint16_t)((v & 0xFFu) * 10u);
            raw->socConf_pm = 500u;     /* the JK's own number, half-believed */
            in->balanceState = (uint8_t)((v >> 8) & 0xFFu);
            in->groups |= PACK_GRP_BIT(packGrp_charge);
        } else if (pt == in->pt.balanceCurrent) {
            /* THE CAPABILITY IS ADVERTISED ON THIS POINT RESOLVING, so it
             * has to be decoded: sPackCells' balance fields were left at
             * zero forever while packCap_balancer said they were live, which
             * is §16 item 10's "a capability that answers with a confident
             * lie". */
            cells = PackType_CellStaging(i);
            if (cells != NULL) {
                cells->balanceCurrent_mA = ev->u.sample.value;
                cells->balanceActive     = (in->balanceState != 0u) ? 1u : 0u;
                /* The balancer's operands ARE the extreme cells -- that is
                 * how it chooses them (docs/design_bms_cell_health_
                 * estimation.md §2.5) -- but only while it is running.
                 * Idle, there is no source and no sink, and 0 would read as
                 * "cell 0". */
                if (cells->balanceActive != 0u) {
                    cells->balanceSrcIdx  = raw->cellMaxIdx;
                    cells->balanceSinkIdx = raw->cellMinIdx;
                } else {
                    cells->balanceSrcIdx  = PACK_CELL_NONE;
                    cells->balanceSinkIdx = PACK_CELL_NONE;
                }
                in->cellGroups |= PACK_GRP_BIT(packGrp_cells);
            }
        } else if (pt == in->pt.balancePwmDsg) {
            cells = PackType_CellStaging(i);
            if (cells != NULL) {
                /* Duty is 0..255 on the wire; per-mille out. */
                cells->balanceDuty_pm =
                    (uint16_t)(((uint32_t)ev->u.sample.value * 1000u) / 255u);
                in->cellGroups |= PACK_GRP_BIT(packGrp_cells);
            }
        } else if (pt == in->pt.remaining) {
            raw->remaining_mAh = (uint32_t)ev->u.sample.value;
            in->groups |= PACK_GRP_BIT(packGrp_charge);
        } else if (pt == in->pt.fullCapacity) {
            raw->capacity_mAh = (uint32_t)ev->u.sample.value;
            in->groups |= PACK_GRP_BIT(packGrp_charge);
        } else if (pt == in->pt.sohPrecharge) {
            raw->soh_pm     = (uint16_t)((((uint32_t)ev->u.sample.value >> 8) &
                                          0xFFu) * 10u);
            /* §2 point 5: this reads 100 % forever on a solar ESS, so it is
             * reported with low confidence rather than suppressed. */
            raw->sohConf_pm = 200u;
            in->groups |= PACK_GRP_BIT(packGrp_charge);
        } else if (pt == in->pt.chgDsgState) {
            const uint32_t v = (uint32_t)ev->u.sample.value;

            raw->chargeSwitch    = (uint8_t)((((v >> 8) & 0xFFu) != 0u)
                                   ? packSwitch_closed : packSwitch_open);
            raw->dischargeSwitch = (uint8_t)(((v & 0xFFu) != 0u)
                                   ? packSwitch_closed : packSwitch_open);
            in->groups |= PACK_GRP_BIT(packGrp_switches);
        } else if (pt == in->pt.cellMinMaxNbr) {
            const uint32_t v = (uint32_t)ev->u.sample.value;

            raw->cellMaxIdx = (uint8_t)((v >> 8) & 0xFFu);
            raw->cellMinIdx = (uint8_t)(v & 0xFFu);
            /* DELIBERATELY NOT packGrp_cells.  cell_minmax_nbr lives in the
             * JK's 5 s LIVE block, while the cell VOLTAGES come from the 15 s
             * cells plan -- so stamping the cell group here refreshed it
             * every 5 s and cellStaleAfter_ms (60 s) could never expire.  A
             * BMS that stopped delivering cell voltages while still answering
             * the live block reported hours-old cells as fresh.  Only
             * PublishCells may stamp packGrp_cells. */
            in->groups |= PACK_GRP_BIT(packGrp_electrical);
        } else if (pt == in->pt.mosTemp) {
            /* THE FIRST TEMPERATURE OF THE BLOCK RESETS BOTH EXTREMES.
             * Staging persists across frames, so folding a min/max into it
             * sample by sample made cellMax/tempMax monotonically rising and
             * the mins monotonically falling -- lifetime extremes, not
             * current ones, and an imbalance signal that only ever grew.
             * mos_temp sorts first in this address-ascending block, so it is
             * the frame's reset point. */
            raw->tempMax_dC = (int16_t)ev->u.sample.value;
            raw->tempMin_dC = (int16_t)ev->u.sample.value;
            in->tempSeen    = 1u;
            in->groups |= PACK_GRP_BIT(packGrp_temperature);
        } else if ((pt == in->pt.tempBat1) || (pt == in->pt.tempBat2)) {
            const int16_t t = (int16_t)ev->u.sample.value;

            if (in->tempSeen == 0u) {
                /* mos_temp absent from this config: seed from the first
                 * sensor rather than folding against a stale 0. */
                raw->tempMax_dC = t;
                raw->tempMin_dC = t;
                in->tempSeen    = 1u;
            } else {
                if (t > raw->tempMax_dC) { raw->tempMax_dC = t; }
                if (t < raw->tempMin_dC) { raw->tempMin_dC = t; }
            }
            in->groups |= PACK_GRP_BIT(packGrp_temperature);
        } else if (pt == in->pt.alarms) {
            raw->vendorAlarms[0] = (uint32_t)ev->u.sample.value;
            /* A neutral set a consumer can reason about, and the vendor's own
             * word alongside so nothing is lost to the translation. */
            raw->alarms = 0u;
            if ((raw->vendorAlarms[0] & (1u << 4)) != 0u) {
                raw->alarms |= (uint32_t)packAlarm_cellOverVoltage;
            }
            if ((raw->vendorAlarms[0] & (1u << 11)) != 0u) {
                raw->alarms |= (uint32_t)packAlarm_cellUnderVoltage;
            }
            if ((raw->vendorAlarms[0] & (1u << 5)) != 0u) {
                raw->alarms |= (uint32_t)packAlarm_packOverVoltage;
            }
            if ((raw->vendorAlarms[0] & (1u << 12)) != 0u) {
                raw->alarms |= (uint32_t)packAlarm_packUnderVoltage;
            }
            if ((raw->vendorAlarms[0] & ((1u << 8) | (1u << 15) |
                                         (1u << 21))) != 0u) {
                raw->alarms |= (uint32_t)packAlarm_overTemperature;
            }
            if ((raw->vendorAlarms[0] & (1u << 9)) != 0u) {
                raw->alarms |= (uint32_t)packAlarm_underTemperature;
            }
            if ((raw->vendorAlarms[0] & (1u << 6)) != 0u) {
                raw->alarms |= (uint32_t)packAlarm_chargeOverCurrent;
            }
            if ((raw->vendorAlarms[0] & (1u << 13)) != 0u) {
                raw->alarms |= (uint32_t)packAlarm_dischargeOverCur;
            }
            if ((raw->vendorAlarms[0] & ((1u << 16) | (1u << 17))) != 0u) {
                raw->alarms |= (uint32_t)packAlarm_protectionOpen;
            }
            if ((raw->vendorAlarms[0] & ((1u << 3) | (1u << 10))) != 0u) {
                raw->alarms |= (uint32_t)packAlarm_internalFault;
            }
            in->groups |= PACK_GRP_BIT(packGrp_alarms);
        } else if (pt == in->pt.chargeLimit) {
            raw->chargeLimit_mA = (uint32_t)ev->u.sample.value;
            in->groups |= PACK_GRP_BIT(packGrp_limits);
        } else if (pt == in->pt.dischargeLimit) {
            raw->dischargeLimit_mA = (uint32_t)ev->u.sample.value;
            in->groups |= PACK_GRP_BIT(packGrp_limits);
        } else {
            cells = PackType_CellStaging(i);
            if (cells == NULL) {
                continue;
            }
            for (c = 0u; c < PACK_CELLS_MAX; c++) {
                if (pt == in->pt.cell[c]) {
                    cells->cell_mV[c] = (uint16_t)ev->u.sample.value;
                    if (c >= cells->cellCount) {
                        cells->cellCount = (uint8_t)(c + 1u);
                    }
                    /* The extremes are NOT folded in here -- see
                     * CloseCellFrame.  Staging persists across frames, so a
                     * running max only ever rose and a running min only ever
                     * fell, and cellMax - cellMin (the imbalance signal a
                     * consumer would actually use) grew without bound. */
                    in->cellGroups |= PACK_GRP_BIT(packGrp_cells);
                    break;
                }
                if (pt == in->pt.leadRes[c]) {
                    cells->leadRes_mOhm[c] = (uint16_t)ev->u.sample.value;
                    in->cellGroups |= PACK_GRP_BIT(packGrp_cells);
                    break;
                }
            }
        }
    }
}

/* Exported functions -------------------------------------------------------*/

int PackJkBms_Register(void)
{
    return PackType_Register(&s_jkBmsType);
}
