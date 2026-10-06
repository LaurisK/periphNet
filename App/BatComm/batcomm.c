/*
 * batcomm.c
 *
 * The battery communication module's ONE impure file: configuration
 * persistence, the binding to a source, the bridge's frame-source callback,
 * the fallback policy and the counters (docs/design_battery_comm.md §6).
 *
 * NO LOCK AND NO CRITICAL SECTION.  The callback runs on `Tmr Svc` — priority
 * 2, a 256-word stack, shared with every Modbus mbtick — so everything it
 * touches is either its own or is read through an accessor documented as
 * legal from there:
 *
 *   - Cluster_GetOutput says so explicitly, and requires a FILE-STATIC
 *     destination rather than a local.  s_out is that static.
 *   - Pack_GetState takes a short critical section, never a mutex, so it
 *     cannot block a priority-2 caller.
 *   - CanBus_Send is the bridge's own path and is already called from ISR
 *     context by the forwarder.
 *
 * THE CONFIGURATION IS SWAPPED WITH THE CALLBACK DISARMED, which is why there
 * is no seqlock here and no staging flag: BatComm_ConfigApply takes the
 * source slot away first and gives it back last, so the callback is not
 * running across the swap and cannot observe a half-written sBatCommCfg.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/BatComm/batcomm.h"
#include "App/BatComm/batcomm_cfg.h"
#include "App/BatComm/batcomm_frame.h"
#include "App/Can/can_bridge.h"
#include "App/Can/can_bus.h"
#include "App/Cluster/cluster.h"
#include "App/Pack/pack.h"
#include "App/nv_record.h"

#include "cmsis_os.h"

#include "trice.h"

#include <stddef.h>
#include <string.h>

/* Private defines ----------------------------------------------------------*/

#define BATCOMM_CFG_MAGIC   0x42434D31u     /* "BCM1"                        */

/** CPU-only snapshots.  CCM, because main SRAM is this board's tightest
 *  region and nothing here is ever handed to DMA — unlike s_cfgRec below,
 *  which nvDb reads and writes and which must stay in main SRAM. */
#define CCMRAM_BSS  __attribute__((section(".ccmram")))

/* Private types ------------------------------------------------------------*/

/** The persisted record.  Header FIRST, as App/nv_record.h requires. */
typedef struct {
    sNvRecordHdr hdr;
    sBatCommCfg  cfg;
} sBatCommCfgRecord;

/* Private variables --------------------------------------------------------*/

/* THE MEDIUM'S VIEW.  Main SRAM: NvDb_Read/Write want a DMA-reachable
 * buffer, and neither a FreeRTOS stack (.ccmheap) nor .ccmram is one. */
static sBatCommCfgRecord s_cfgRec;

/* The configuration IN FORCE.  Written only with the callback disarmed. */
static sBatCommCfg       s_live;

static CCMRAM_BSS sClusterOutput s_out;
static CCMRAM_BSS sPackState     s_pack;
static CCMRAM_BSS sBatCommIn     s_in;

static uint32_t s_cycles, s_goodCycles, s_framesSent, s_sendFail;
static uint32_t s_withheld, s_fallbackEntries, s_lastGood_ms;
static uint16_t s_slot;
static uint8_t  s_badRun;
static uint8_t  s_cycleOk;
static uint8_t  s_tripped;          /* the wire has been handed back         */
static uint8_t  s_overrideHeld;
static uint8_t  s_armed;
static uint8_t  s_provisioned;
static uint8_t  s_inited;
static uint8_t  s_why;              /* eBatCommWhy                           */
static uint8_t  s_packIdx = BATCOMM_PACK_NONE;

/* Private function prototypes ----------------------------------------------*/

static eCanBus OtherBus(uint8_t bus);
static void    ApplyRoles(void);
static int     RefreshFromCluster(void);
static int     RefreshFromPack(void);
static int     Refresh(void);
static void    UpdateOverride(int good);
static void    SourceCb(eCanBus inverterBus, void *ctx);
static void    HoldOverride(int on);

/* Private functions --------------------------------------------------------*/

static eCanBus OtherBus(uint8_t bus)
{
    return (bus == (uint8_t)canBus_1) ? canBus_2 : canBus_1;
}

/** THE ONE PLACE A SITE FACT BECOMES A BUS ROLE.  The configured peripheral
 *  is the cell the INVERTER is on; the battery is whatever is left.  Legal
 *  only while the bridge is stopped, which is why BatComm_Init runs before
 *  CanBridge_Start and why a bus change in ConfigApply stops it first
 *  (docs/issue_can_bus_roles_not_configurable.md). */
static void ApplyRoles(void)
{
    const eCanBus inv = (s_live.inverterBus == (uint8_t)canBus_1)
                            ? canBus_1 : canBus_2;

    if (CanBridge_SetRoles(OtherBus(s_live.inverterBus), inv) != 0) {
        TRice("err:[BatComm] bus roles refused (bridge up?)\n");
    }
}

static void HoldOverride(int on)
{
    /* THE OVERRIDE IS WHAT MAKES THE TAKEOVER REAL.  With it clear the
     * battery's own frames still cross to the inverter in `bms` mode, and the
     * inverter would see two BMSs disagreeing. */
    CanBridge_SetOverrideAll(on);
    s_overrideHeld = (uint8_t)(on ? 1 : 0);
}

/** Map one settled cluster snapshot onto the neutral input. */
static int RefreshFromCluster(void)
{
    const int r = Cluster_GetOutput(&s_out);

    if (r != cluErr_ok) {
        /* POSITIVE GATE (batcomm.h contract 2): notReady, stale, busy and
         * unprovisioned all mean "emit nothing", and none of them means
         * "reuse the last frame". */
        if (r == cluErr_stale) {
            s_why = (uint8_t)batWhy_sourceStale;
        } else if (r == cluErr_notReady) {
            s_why = (uint8_t)batWhy_sourceNotReady;
        } else {
            s_why = (uint8_t)batWhy_sourceBusy;
        }
        return 0;
    }

    (void)memset(&s_in, 0, sizeof(s_in));
    s_in.voltage_mV            = s_out.voltage_mV;
    s_in.current_mA            = s_out.current_mA;
    s_in.chargeLimit_mA        = s_out.chargeLimit_mA;
    s_in.dischargeLimit_mA     = s_out.dischargeLimit_mA;
    s_in.chargeVoltLimit_mV    = s_out.chargeVoltLimit_mV;
    s_in.dischargeVoltLimit_mV = s_out.dischargeVoltLimit_mV;
    s_in.remaining_mAh         = s_out.remaining_mAh;
    s_in.capacity_mAh          = s_out.capacity_mAh;
    s_in.alarms                = s_out.alarms;
    s_in.soc_pm                = s_out.soc_pm;
    s_in.soh_pm                = s_out.soh_pm;
    s_in.tempMax_dC            = s_out.tempMax_dC;
    s_in.tempMin_dC            = s_out.tempMin_dC;
    s_in.chargeAllowed         = s_out.chargeAllowed;
    s_in.dischargeAllowed      = s_out.dischargeAllowed;
    /* 0x359's module count.  The ONLINE members, not the configured ones: a
     * pack that left the bus is not a module the inverter is talking to. */
    s_in.modules               = (s_out.onlineCnt == 0u) ? 1u : s_out.onlineCnt;

    /* eClusterField -> eBatCommField.  Spelled out rather than assumed
     * bit-identical: two enums that happen to agree today are not one enum. */
    if ((s_out.fields & (uint16_t)cluField_voltage) != 0u) {
        s_in.fields |= (uint16_t)batField_voltage;
    }
    if ((s_out.fields & (uint16_t)cluField_current) != 0u) {
        s_in.fields |= (uint16_t)batField_current;
    }
    if ((s_out.fields & (uint16_t)cluField_soc) != 0u) {
        s_in.fields |= (uint16_t)batField_soc | (uint16_t)batField_capacity;
    }
    if ((s_out.fields & (uint16_t)cluField_soh) != 0u) {
        s_in.fields |= (uint16_t)batField_soh;
    }
    if ((s_out.fields & (uint16_t)cluField_chargeLimit) != 0u) {
        s_in.fields |= (uint16_t)batField_chargeLimit;
    }
    if ((s_out.fields & (uint16_t)cluField_dischargeLimit) != 0u) {
        s_in.fields |= (uint16_t)batField_dischargeLimit;
    }
    if ((s_out.fields & (uint16_t)cluField_chargeVoltLimit) != 0u) {
        s_in.fields |= (uint16_t)batField_chargeVoltLimit;
    }
    if ((s_out.fields & (uint16_t)cluField_dischargeVoltLimit) != 0u) {
        s_in.fields |= (uint16_t)batField_dischargeVoltLimit;
    }
    if ((s_out.fields & (uint16_t)cluField_temperature) != 0u) {
        s_in.fields |= (uint16_t)batField_temperature;
    }
    if ((s_out.fields & (uint16_t)cluField_switches) != 0u) {
        s_in.fields |= (uint16_t)batField_switches;
    }

    s_why = (uint8_t)batWhy_ok;
    return 1;
}

/** Map one pack's state onto the neutral input — the TRANSLATOR case.  No
 *  derate and no margin: this is a translation, and the limits the operator
 *  set on the BMS are the limits the inverter is told about. */
static int RefreshFromPack(void)
{
    const int idx = Pack_FindByName(s_live.packName);

    if (idx < 0) {
        s_packIdx = BATCOMM_PACK_NONE;
        s_why     = (uint8_t)batWhy_packNotFound;
        return 0;
    }
    s_packIdx = (uint8_t)idx;

    if (Pack_GetState((uint8_t)idx, &s_pack) != packErr_ok) {
        s_why = (uint8_t)batWhy_sourceBusy;
        return 0;
    }
    /* packCond_stale and packCond_absent are the same answer here: a pack
     * that is not talking cannot be spoken for. */
    if (s_pack.cond != (uint8_t)packCond_online) {
        s_why = (uint8_t)batWhy_packNotOnline;
        return 0;
    }
    /* ONLINE IS NOT THE SAME AS "EVERY GROUP HAS BEEN READ": the electrical
     * group makes a pack online within a cycle or two of boot, while a JK's
     * limits arrive far later.  Booting straight into `bms` made that gap
     * visible — an all-zero 0x351, a 0 % SOC and a 0.0 C temperature are all
     * things an inverter would act on.  Emit nothing until they are real. */
    if (BatFrame_PackReady(&s_pack) == 0) {
        s_why = (uint8_t)batWhy_sourceNotReady;
        return 0;
    }

    (void)memset(&s_in, 0, sizeof(s_in));
    s_in.voltage_mV            = s_pack.voltage_mV;
    s_in.current_mA            = s_pack.current_mA;
    s_in.chargeLimit_mA        = s_pack.chargeLimit_mA;
    s_in.dischargeLimit_mA     = s_pack.dischargeLimit_mA;
    s_in.chargeVoltLimit_mV    = s_pack.chargeVoltLimit_mV;
    s_in.dischargeVoltLimit_mV = s_pack.dischargeVoltLimit_mV;
    s_in.remaining_mAh         = s_pack.remaining_mAh;
    s_in.capacity_mAh          = s_pack.capacity_mAh;
    s_in.alarms                = s_pack.alarms;
    s_in.soc_pm                = s_pack.soc_pm;
    s_in.soh_pm                = s_pack.soh_pm;
    s_in.tempMax_dC            = s_pack.tempMax_dC;
    s_in.tempMin_dC            = s_pack.tempMin_dC;
    s_in.modules               = 1u;

    /* FIELD VALIDITY IS THE CAPABILITY SET, exactly as pack.h defines it: a
     * field behind an unset capability reads zero and MEANS NOTHING. */
    s_in.fields = (uint16_t)batField_voltage | (uint16_t)batField_current;
    if ((s_pack.caps & (uint32_t)packCap_capacityAh) != 0u) {
        s_in.fields |= (uint16_t)batField_soc | (uint16_t)batField_capacity;
    }
    if ((s_pack.caps & (uint32_t)packCap_soh) != 0u) {
        s_in.fields |= (uint16_t)batField_soh;
    }
    if ((s_pack.caps & (uint32_t)packCap_currentLimits) != 0u) {
        s_in.fields |= (uint16_t)batField_chargeLimit |
                       (uint16_t)batField_dischargeLimit;
    }
    if ((s_pack.caps & (uint32_t)packCap_voltageLimits) != 0u) {
        s_in.fields |= (uint16_t)batField_chargeVoltLimit |
                       (uint16_t)batField_dischargeVoltLimit;
    }
    if ((s_pack.caps & (uint32_t)packCap_temperatures) != 0u) {
        s_in.fields |= (uint16_t)batField_temperature;
    }
    if ((s_pack.caps & (uint32_t)packCap_switchState) != 0u) {
        s_in.fields |= (uint16_t)batField_switches;
    }

    /* packSwitch_unknown is "this type cannot report it", not "open": a type
     * that cannot answer must not be read as a refusal.  An open switch, and
     * a protection that has already opened, both are. */
    s_in.chargeAllowed =
        (uint8_t)((s_pack.chargeSwitch != (uint8_t)packSwitch_open) ? 1 : 0);
    s_in.dischargeAllowed =
        (uint8_t)((s_pack.dischargeSwitch != (uint8_t)packSwitch_open) ? 1 : 0);
    if ((s_pack.alarms & (uint32_t)packAlarm_protectionOpen) != 0u) {
        s_in.chargeAllowed    = 0u;
        s_in.dischargeAllowed = 0u;
    }

    s_why = (uint8_t)batWhy_ok;
    return 1;
}

static int Refresh(void)
{
    if (s_live.source == (uint8_t)batSrc_cluster) {
        return RefreshFromCluster();
    }
    if (s_live.source == (uint8_t)batSrc_pack) {
        return RefreshFromPack();
    }
    s_why = (uint8_t)batWhy_unprovisioned;
    return 0;
}

/**
 * @brief  The fallback policy (batcomm.h contract 4).
 *
 * RECOVERY IS IMMEDIATE, ENTRY IS DEBOUNCED, and the asymmetry is deliberate:
 * one missed cycle during an OTA must not flap the inverter's BMS link, while
 * there is no safety argument for staying away once the numbers are back.
 */
static void UpdateOverride(int good)
{
    if (good != 0) {
        s_badRun = 0u;
        if (s_tripped != 0u) {
            s_tripped = 0u;
            HoldOverride(1);
            TRice("[BatComm] source back; taking the wire again\n");
        }
        return;
    }

    if (s_badRun < 0xFFu) {
        s_badRun++;
    }
    if ((s_tripped != 0u) || (s_badRun < s_live.tune.staleTrip)) {
        return;
    }

    s_tripped = 1u;
    s_fallbackEntries++;

    /* A BATTERY THAT IS AN INPUT TO US MUST NOT BE FORWARDED.  Its frames
     * describe one member of the cluster we publish, so relaying them to the
     * inverter would present a fragment of the bus as the whole of it — worse
     * than silence, because it is silence the inverter can act on wrongly. */
    if ((s_live.fallback == (uint8_t)batFallback_bridge) &&
        (s_live.batteryBusIsInput == 0u)) {
        HoldOverride(0);
        TRice("wrn:[BatComm] source unusable (%u); wire handed back to the "
              "battery\n", (unsigned)s_why);
    } else {
        TRice("wrn:[BatComm] source unusable (%u); staying silent\n",
              (unsigned)s_why);
    }
}

/**
 * @brief  The bridge's frame source.  `Tmr Svc`, once per slot, `bms` only.
 *
 * ONE FRAME PER SLOT, and the cycle is refreshed only at slot 0: every frame
 * of a cycle then describes ONE instant, which is the property a receiver
 * correlating 0x351 against 0x356 depends on and which a per-frame re-read
 * would quietly break.
 */
static void SourceCb(eCanBus inverterBus, void *ctx)
{
    sBatCommFrame f;

    (void)ctx;

    if (s_slot == 0u) {
        s_cycles++;
        s_cycleOk = (uint8_t)(Refresh() ? 1 : 0);
        if (s_cycleOk != 0u) {
            s_goodCycles++;
            s_lastGood_ms = (uint32_t)osKernelGetTickCount();
        }
        UpdateOverride((int)s_cycleOk);
    }

    if ((s_cycleOk != 0u) && (s_overrideHeld != 0u)) {
        if (BatFrame_Build((eBatCommProto)s_live.proto, &s_live.tune, &s_in,
                           (uint8_t)s_slot, &f) != 0) {
            sCanFrame out;

            (void)memset(&out, 0, sizeof(out));
            out.id  = f.id;
            out.dlc = f.dlc;
            out.bus = (uint8_t)inverterBus;
            (void)memcpy(out.data, f.data, sizeof(out.data));

            if (CanBus_Send(&out) == 0) {
                s_framesSent++;
            } else {
                s_sendFail++;
            }
        } else if (s_slot < (uint16_t)BatFrame_SlotCount(
                                (eBatCommProto)s_live.proto)) {
            /* A slot the profile owns that produced nothing: 0x351 withheld
             * for want of a voltage limit is the only way to get here. */
            s_withheld++;
        } else {
            /* the empty tail of the cycle */
        }
    }

    s_slot++;
    if (s_slot >= s_live.tune.slots) {
        s_slot = 0u;
    }
}

/* Exported functions -------------------------------------------------------*/

int BatComm_Init(void)
{
    if (s_inited != 0u) {
        return batErr_ok;                   /* idempotent                    */
    }

    BatCommCfg_Defaults(&s_live);

    if (NvRecord_Load(nvdbUser_batCommCfg, BATCOMM_CFG_MAGIC,
                      BATCOMM_CFG_VERSION, &s_cfgRec,
                      sizeof(s_cfgRec)) == 0) {
        s_live        = s_cfgRec.cfg;
        s_provisioned = 1u;
        ApplyRoles();
    }

    s_inited = 1u;
    TRice("[BatComm] up: provisioned %u, proto %u, src %u, inv CAN%u\n",
          (unsigned)s_provisioned, (unsigned)s_live.proto,
          (unsigned)s_live.source, (unsigned)s_live.inverterBus + 1u);
    return batErr_ok;
}

int BatComm_Arm(void)
{
    if (s_provisioned == 0u) {
        return batErr_unprovisioned;
    }
    if (s_live.enabled == 0u) {
        return batErr_unprovisioned;
    }
    if (s_armed != 0u) {
        return batErr_ok;
    }

    /* Registering the source ARMS NOTHING BY ITSELF: the bridge starts and
     * stops the timer with the mode, so this puts no frame on a bus the real
     * battery is still driving.  Taking over is the operator's act. */
    if (CanBridge_SetSource(SourceCb, NULL,
                            (uint32_t)s_live.tune.slot_ms) != 0) {
        TRice("err:[BatComm] could not take the bridge source slot\n");
        return batErr_transport;
    }

    s_slot    = 0u;
    s_cycleOk = 0u;
    s_badRun  = 0u;
    s_tripped = 0u;
    CanBridge_ClearOverrides();
    HoldOverride(1);
    s_armed = 1u;

    TRice("[BatComm] armed: %u ms slots, emits in bms mode on CAN%u\n",
          (unsigned)s_live.tune.slot_ms, (unsigned)s_live.inverterBus + 1u);
    return batErr_ok;
}

int BatComm_Disarm(void)
{
    if (s_armed == 0u) {
        return batErr_ok;
    }
    (void)CanBridge_SetSource(NULL, NULL, 0u);
    HoldOverride(0);
    s_armed   = 0u;
    s_cycleOk = 0u;
    TRice("[BatComm] disarmed\n");
    return batErr_ok;
}

int BatComm_IsArmed(void)
{
    return (int)s_armed;
}

int BatComm_GetStatus(sBatCommStatus *out)
{
    eBatCommState st;
    eBatCommWhy   why = (eBatCommWhy)s_why;

    if (out == NULL) {
        return batErr_badArg;
    }

    if (s_provisioned == 0u) {
        st  = batState_disabled;
        why = batWhy_unprovisioned;
    } else if (s_live.enabled == 0u) {
        st  = batState_disabled;
        why = batWhy_disabled;
    } else if (s_armed == 0u) {
        st  = batState_idle;
        why = batWhy_notArmed;
    } else if (CanBridge_GetMode() != canBrMode_bms) {
        /* ARMED BUT A SPECTATOR, and that is the normal resting state: the
         * bridge boots forwarding, so the board stays transparent until
         * somebody asks for the takeover. */
        st  = batState_idle;
        why = batWhy_notBmsMode;
    } else if (s_tripped != 0u) {
        st = batState_fallback;
    } else if (s_badRun != 0u) {
        st = batState_holdoff;
    } else {
        st = batState_emitting;
    }

    /* BUILT STRAIGHT INTO THE CALLER'S BUFFER, not through a shared scratch:
     * the HTTP task and the CLI can both be here, and a file-static staging
     * struct would be the one piece of tearable state in a module that takes
     * no lock.  120 bytes on a 4 KB task stack is the right side of that
     * trade.
     *
     * `in` IS COPIED WITHOUT SYNCHRONISATION and may be torn by a cycle that
     * lands mid-copy.  That is accepted: it is observability, the fields are
     * independent, and the alternative is a lock this module does not have. */
    (void)memset(out, 0, sizeof(*out));
    out->in                = s_in;
    out->cycles            = s_cycles;
    out->goodCycles        = s_goodCycles;
    out->framesSent        = s_framesSent;
    out->sendFailCnt       = s_sendFail;
    out->withheldCnt       = s_withheld;
    out->fallbackEntries   = s_fallbackEntries;
    out->lastGood_ms       = s_lastGood_ms;
    out->badRun            = s_badRun;
    out->proto             = s_live.proto;
    out->source            = s_live.source;
    out->fallback          = s_live.fallback;
    out->state             = (uint8_t)st;
    out->why               = (uint8_t)why;
    out->inverterBus       = s_live.inverterBus;
    out->batteryBus        = (uint8_t)OtherBus(s_live.inverterBus);
    out->packIdx           = s_packIdx;
    out->armed             = s_armed;
    out->enabled           = s_live.enabled;
    out->provisioned       = s_provisioned;
    out->batteryBusIsInput = s_live.batteryBusIsInput;
    (void)memcpy(out->packName, s_live.packName, sizeof(out->packName));

    return batErr_ok;
}

int BatComm_ConfigVerify(fBatCommByteSource src, void *srcCtx,
                         sBatCommCfgResult *res)
{
    sBatCommCfg scratch;

    /* THE PARSE TARGET IS A CALLER-STACK sBatCommCfg (36 B on http's 4 KB),
     * never the record: a verify must not be able to disturb what is stored. */
    return BatCommCfg_Parse(src, srcCtx, &scratch, res);
}

int BatComm_ConfigApply(fBatCommByteSource src, void *srcCtx,
                        sBatCommCfgResult *res)
{
    sBatCommCfg parsed;
    int         r;
    int         wasArmed;
    int         busMoved;
    eCanBrMode  mode;

    r = BatCommCfg_Parse(src, srcCtx, &parsed, res);
    if (r != batErr_ok) {
        return r;
    }

    (void)memset(&s_cfgRec, 0, sizeof(s_cfgRec));
    s_cfgRec.cfg = parsed;
    if (NvRecord_Save(nvdbUser_batCommCfg, BATCOMM_CFG_MAGIC,
                      BATCOMM_CFG_VERSION, &s_cfgRec,
                      sizeof(s_cfgRec)) != 0) {
        return batErr_transport;
    }

    /* THE CALLBACK IS TAKEN AWAY FIRST AND GIVEN BACK LAST.  Between these
     * two lines nothing reads s_live, which is what removes the need for a
     * seqlock or a staging flag on a struct this small. */
    wasArmed = (int)s_armed;
    (void)BatComm_Disarm();

    /* ASKED OF THE BRIDGE, not of the previous document: on the FIRST upload
     * there is no previous document, and the roles then in force are the
     * compiled-in ones this module exists to replace. */
    busMoved = ((uint8_t)CanBridge_InverterBus() != parsed.inverterBus);
    mode     = CanBridge_GetMode();

    s_live        = parsed;
    s_provisioned = 1u;

    if (busMoved != 0) {
        /* SetRoles is legal only while the bridge is stopped, so a cell that
         * moved costs a bounce of both buses.  It is an operator act with a
         * log line, not something the firmware does on its own. */
        if (mode != canBrMode_off) {
            TRice("wrn:[BatComm] inverter cell moved; bouncing the bridge\n");
            (void)CanBridge_Stop();
        }
        ApplyRoles();
        if (mode != canBrMode_off) {
            if (CanBridge_Start(mode, 0u) != 0) {
                TRice("err:[BatComm] the bridge did not come back up\n");
            }
        }
    }

    (void)wasArmed;                     /* Arm() refuses a disabled config   */
    (void)BatComm_Arm();
    return batErr_ok;
}

int BatComm_ConfigExport(fBatCommByteSink sink, void *ctx)
{
    if (s_provisioned == 0u) {
        return batErr_unprovisioned;
    }
    return BatCommCfg_Serialize(&s_live, sink, ctx);
}

int BatComm_ConfigErase(void)
{
    (void)BatComm_Disarm();
    (void)NvRecord_Forget(nvdbUser_batCommCfg);

    BatCommCfg_Defaults(&s_live);
    s_provisioned = 0u;
    s_packIdx     = BATCOMM_PACK_NONE;
    s_why         = (uint8_t)batWhy_unprovisioned;
    /* The bus roles in force are NOT reverted: they describe a cabinet, and a
     * cabinet does not rewire itself because a document was deleted. */
    return batErr_ok;
}
