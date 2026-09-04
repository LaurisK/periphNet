/**
 * @file    cmd_parser.c
 * @brief   Text command parser — USB CDC + UART1 input, Trice output
 *
 * Input bytes are buffered from ISR context (Cmd_Feed); completed lines
 * are dispatched by the dedicated "cmd" task so command handlers may
 * block and use RTOS/lwIP APIs freely.
 */

#include "App/Cmd/cmd_parser.h"
#include "App/Can/bms_sim.h"
#include "App/Can/bms_reader.h"
#include "App/Can/can_bridge.h"
#include "App/Can/can_bus.h"
#include "App/Can/can_log.h"
#include "App/Can/can_monitor.h"
#include "App/Mon/sysmon.h"
#include "App/system.h"
#include "App/Test/modbus_test_port.h"
#include "App/Modbus/modbus_trice_sink.h"
#include "App/Modbus/modbus.h"
#include "App/Mqtt/mqtt_bridge.h"
#include "App/Net/wg_link.h"
#include "App/Net/wg_platform.h"
#include "App/Net/wg_time.h"
#include "App/Pack/pack.h"
#include "App/Func/func.h"
#include "App/Fwu/fwu_control.h"
#include "boot_status.h"
#include "json.h"
#include "nvdb.h"
#include "nvdb_config.h"
#include "nvdb_layout.h"
#include "cmsis_os.h"
#include "trice.h"
#include "usart.h"
#include "stm32f4xx_hal.h"
#include <ctype.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */

#define CMD_LINE_MAX  256

/* --------------------------------------------------------------------------
 * Line buffer per input source
 * -------------------------------------------------------------------------- */

typedef struct {
    char     buf[CMD_LINE_MAX];
    uint16_t pos;
    volatile bool ready;
    bool     synced;     /* true after first newline seen (discards initial junk) */
} sCmdLine;

static sCmdLine s_lines[cmdSrc_last];

/* USART1 single-byte RX buffer */
static uint8_t s_uart1_rx_byte;

/* --------------------------------------------------------------------------
 * Command table
 * -------------------------------------------------------------------------- */

typedef void (*fCmdHandler)(const char *args);

typedef struct {
    const char    *name;
    fCmdHandler    handler;
    const char    *description;
} sCmdEntry;

static void cmd_peripherals(const char *args);
static void cmd_help(const char *args);
static void cmd_reboot(const char *args);
static void cmd_dfu(const char *args);
static void cmd_bms(const char *args);
static void cmd_can(const char *args);
static int  parse_hex_bytes(const char *p, uint8_t *out, size_t maxLen);
static void cmd_modbus(const char *args);
static void cmd_mqtt(const char *args);
static void cmd_wg(const char *args);
static void cmd_nvdb(const char *args);
static void cmd_pack(const char *args);
static void cmd_sysmon(const char *args);
static void cmd_fwu(const char *args);

static const sCmdEntry s_commands[] = {
    { "peripherals", cmd_peripherals, "List device peripherals" },
    { "bms",         cmd_bms,         "BMS sim/reader (start|stop|read|set)" },
    { "can",         cmd_can,         "CAN bridge + flash trace (start|stop|mode|status|ids|trace|log|send)" },
    { "modbus",      cmd_modbus,      "Modbus (read|get|set|monitor|dump|plan|inject|status)" },
    { "mqtt",        cmd_mqtt,        "MQTT bridge (start|stop|save|forget|monitor|status)"  },
    { "wg",          cmd_wg,          "WireGuard tunnel (start|stop|status|endpoint)" },
    { "nvdb",        cmd_nvdb,        "Non-volatile store (status|layout|usage|wear)" },
    { "pack",        cmd_pack,        "Battery packs (status|list|show|cells|stats|balance|cmd|config|erase)" },
    { "sysmon",      cmd_sysmon,      "System monitor (tasks|heap|reset)" },
    { "fwu",         cmd_fwu,         "Firmware update (status|kick [sec]|confirm)" },
    { "reboot",      cmd_reboot,      "Reboot the board"        },
    { "dfu",         cmd_dfu,         "Enter USB DFU bootloader"},
    { "help",        cmd_help,        "List available commands"  },
    { NULL,          NULL,            NULL                       }
};

/* --------------------------------------------------------------------------
 * Command handlers
 * -------------------------------------------------------------------------- */

static void cmd_peripherals(const char *args)
{
    (void)args;
    TRice("n/a\n");
}

/**
 * BMS command: control Pylontech BMS simulator (CAN1 TX) and reader (CAN2 RX).
 *
 * Usage:
 *   bms start       — Start both simulator and reader
 *   bms stop        — Stop both
 *   bms read        — Poll CAN2 RX and log parsed battery data
 *   bms send        — Transmit one round of BMS frames on CAN1
 *   bms set V I SOC T — Set simulator voltage/current/SOC/temperature
 *   bms status      — Show running state
 */
static void cmd_bms(const char *args)
{
    if (strncmp(args, "start", 5) == 0) {
        BmsSim_Start();
        BmsReader_Start();
    } else if (strncmp(args, "stop", 4) == 0) {
        BmsReader_Stop();
        BmsSim_Stop();
    } else if (strncmp(args, "send", 4) == 0) {
        if (!BmsSim_IsRunning()) {
            TRice("BMS sim not running\n");
            return;
        }
        BmsSim_SendOnce();
        TRice("BMS frames sent\n");
    } else if (strncmp(args, "read", 4) == 0) {
        if (!BmsReader_IsRunning()) {
            TRice("BMS reader not running\n");
            return;
        }
        BmsReader_Poll();
        BmsReader_LogData();
    } else if (strncmp(args, "set ", 4) == 0) {
        float v, i, t;
        int soc;
        if (sscanf(args + 4, "%f %f %d %f", &v, &i, &soc, &t) == 4) {
            BmsSim_SetVoltage(v);
            BmsSim_SetCurrent(i);
            BmsSim_SetSoc((uint16_t)soc);
            BmsSim_SetTemperature(t);
            TRice("BMS sim set: V=%d.%01d I=%d.%01d SOC=%d T=%d.%01d\n",
                  (int)v, ((int)(v * 10)) % 10,
                  (int)i, ((int)(i * 10)) % 10,
                  soc,
                  (int)t, ((int)(t * 10)) % 10);
        } else {
            TRice("Usage: bms set <voltage> <current> <soc> <temperature>\n");
        }
    } else if (strncmp(args, "status", 6) == 0) {
        TRice("BMS sim=%s reader=%s\n",
              BmsSim_IsRunning() ? "running" : "stopped",
              BmsReader_IsRunning() ? "running" : "stopped");
    } else {
        TRice("Usage: bms start|stop|send|read|set|status\n");
    }
}

/* --------------------------------------------------------------------------
 * CAN bridge
 * -------------------------------------------------------------------------- */

/** Print one bus's counters. */
static void CanPrintBus(eCanBus bus)
{
    sCanBusStats st;

    if (CanBus_GetStats(bus, &st) != 0) {
        return;
    }
    TRice("  CAN%u %s %u bps rx=%u tx=%u/%u drop=%u\n",
          (unsigned)bus + 1u, st.running ? "up" : "down", st.bitrate_bps,
          st.rxCnt, st.txDoneCnt, st.txAcceptedCnt, st.txDroppedCnt);
    TRice("       ovr=%u err=%u busoff=%u(%u) esr rec=%u tec=%u q=%u/%u\n",
          st.rxOverrunCnt, st.errorCnt, st.busOffCnt, st.busOff,
          st.rxErrorCnt, st.txErrorCnt, st.txQueueDepth, st.txQueuePeak);
}

/** Print the identifier register of one bus, one row at a time. */
static void CanPrintIds(eCanBus bus)
{
    uint32_t now_ms = HAL_GetTick();
    uint8_t  n = CanMon_IdCount(bus);

    TRice("CAN%u: %u identifiers\n", (unsigned)bus + 1u, n);
    for (uint8_t i = 0u; i < n; i++) {
        sCanMonId r;

        if (CanMon_GetIdAt(bus, i, &r) != 0) {
            break;
        }
        TRice("  %03X%s rx=%u tx=%u chg=%u age=%ums gap=%u..%ums\n",
              r.id, r.ext ? "x" : "", r.rxCnt, r.txCnt, r.changeCnt,
              now_ms - r.lastStamp_ms, r.minGap_ms, r.maxGap_ms);
        TRice("      dlc=%u %02X%02X%02X%02X%02X%02X%02X%02X\n",
              r.dlc, r.data[0], r.data[1], r.data[2], r.data[3],
              r.data[4], r.data[5], r.data[6], r.data[7]);
    }
}

/** Dump the trace ring, oldest first. */
static void CanPrintTrace(uint16_t max)
{
    uint16_t n = CanMon_TraceCount();
    uint16_t first = 0u;

    if ((max != 0u) && (n > max)) {
        first = (uint16_t)(n - max);
    }

    TRice("CAN trace: %u of %u frames\n", n - first, n);
    for (uint16_t i = first; i < n; i++) {
        sCanMonTrace t;

        if (CanMon_GetTraceAt(i, &t) != 0) {
            break;
        }
        TRice("  %ums CAN%u %c %03X [%u]", t.stamp_ms,
              (unsigned)t.bus + 1u,
              (t.dir == (uint8_t)canDir_rx) ? 'R' : 'T', t.id, t.dlc);
        TRice(" %02X%02X%02X%02X%02X%02X%02X%02X\n",
              t.data[0], t.data[1], t.data[2], t.data[3],
              t.data[4], t.data[5], t.data[6], t.data[7]);
    }
}

/** Print the flash trace's own state: mode, capacity, how much it holds. */
static void CanPrintLogStatus(void)
{
    sCanLogStatus st;

    if (CanLog_GetStatus(&st) != 0) {
        return;
    }
    TRiceS("CAN log: mode=%s", (char *)CanLog_ModeName((eCanLogMode)st.mode));
    TRiceS(" state=%s", (char *)CanLog_StateName((eCanLogState)st.state));
    TRice(" gap>=%ums heartbeat<=%ums\n", st.minGap_ms, st.heartbeat_ms);
    TRice("  held=%u/%u recs (next=%u oldest=%u) staged=%u/%u area=%uB boot=%u\n",
          st.heldRecs, st.capacityRecs, st.nextRec, st.oldestRec,
          st.staged, CANLOG_STAGE_DEPTH, st.areaSize_bytes, st.bootId);
    TRice("  dropped=%u stalled=%u writeErr=%u\n",
          st.droppedCnt, st.stalledCnt, st.writeErrCnt);
}

static void CanPrintStatus(void)
{
    sCanBridgeStatus br;
    sCanMonStats     mon;

    if (CanBridge_GetStatus(&br) != 0) {
        return;
    }
    CanMon_GetStats(&mon);

    TRiceS("CAN bridge: mode=%s", (char *)CanBridge_ModeName((eCanBrMode)br.mode));
    TRice(" battery=CAN%u inverter=CAN%u %u bps\n",
          (unsigned)br.batteryBus + 1u, (unsigned)br.inverterBus + 1u,
          br.bitrate_bps);
    TRice("  bat->inv fwd=%u supp=%u drop=%u\n",
          br.toInverter.forwardedCnt, br.toInverter.suppressedCnt,
          br.toInverter.droppedCnt);
    TRice("  inv->bat fwd=%u supp=%u drop=%u\n",
          br.toBattery.forwardedCnt, br.toBattery.suppressedCnt,
          br.toBattery.droppedCnt);
    TRiceS("  source %s", (char *)(br.sourceBound ? "bound" : "NONE"));
    TRice(" emits=%u every %ums, override ",
          br.sourceEmitCnt, br.sourcePeriod_ms);
    TRiceS("%s", (char *)(br.overrideAll ? "all" : "list"));
    TRice(" (%u ids)\n", br.overrideCnt);
    TRice("  monitor recorded=%u idOverflow=%u trace=%s dropped=%u\n",
          mon.recordedCnt, mon.idOverflowCnt, mon.tracing ? "on" : "off",
          mon.traceDroppedCnt);
    CanPrintBus(canBus_1);
    CanPrintBus(canBus_2);
    CanPrintLogStatus();
}

/**
 * CAN command: the CAN1/CAN2 bridge, its traffic register, and its flash
 * trace.
 *
 * Usage:
 *   can start [monitor|bridge|bms] [bitrate]  — bring both buses up
 *   can stop                                  — take both buses down
 *   can mode <off|monitor|bridge|bms>         — change policy, wire stays up
 *   can status                                — modes, counters, bus health
 *   can ids [1|2]                             — the identifier register
 *   can trace <on|off|show [n]>               — the RAM frame ring (seconds)
 *   can log status                            — the flash trace's own state
 *   can log mode <off|changes|all> [gap] [hb] — capture policy (ms, ms)
 *   can log read <recNo>                      — one record by global number
 *   can log wipe                              — erase the flash trace
 *   can send <bus> <id> <hexbytes>            — put one frame on a bus
 *   can roles <batteryBus> <inverterBus>      — which cell is which side
 *   can override <all|none|id ...>            — what bms mode takes over
 *   can reset                                 — zero every counter
 */
static void cmd_can(const char *args)
{
    if (strncmp(args, "start", 5) == 0) {
        eCanBrMode mode = canBrMode_bridge;
        uint32_t   bitrate_bps = 0u;
        const char *p = args + 5;

        while (*p == ' ') {
            p++;
        }
        if ((*p != '\0') && ((*p < '0') || (*p > '9'))) {
            if (CanBridge_ModeFromName(p, &mode) != 0) {
                TRice("Usage: can start [monitor|bridge|bms] [bitrate]\n");
                return;
            }
            while ((*p != '\0') && (*p != ' ')) {
                p++;
            }
            while (*p == ' ') {
                p++;
            }
        }
        if ((*p >= '0') && (*p <= '9')) {
            bitrate_bps = (uint32_t)strtoul(p, NULL, 0);
        }
        if (mode == canBrMode_off) {
            TRice("can start needs monitor, bridge or bms\n");
            return;
        }
        if (CanBridge_Start(mode, bitrate_bps) != 0) {
            TRice("CAN bridge start FAILED\n");
            return;
        }
        CanPrintStatus();
    } else if (strncmp(args, "stop", 4) == 0) {
        (void)CanBridge_Stop();
    } else if (strncmp(args, "mode ", 5) == 0) {
        eCanBrMode mode;

        if (CanBridge_ModeFromName(args + 5, &mode) != 0) {
            TRice("Usage: can mode <off|monitor|bridge|bms>\n");
            return;
        }
        if (CanBridge_SetMode(mode) != 0) {
            TRice("CAN mode change FAILED\n");
            return;
        }
        CanPrintStatus();
    } else if (strncmp(args, "status", 6) == 0) {
        CanPrintStatus();
    } else if (strncmp(args, "ids", 3) == 0) {
        int which = atoi(args + 3);

        if (which == 1) {
            CanPrintIds(canBus_1);
        } else if (which == 2) {
            CanPrintIds(canBus_2);
        } else {
            CanPrintIds(canBus_1);
            CanPrintIds(canBus_2);
        }
    } else if (strncmp(args, "trace on", 8) == 0) {
        CanMon_TraceEnable(1);
        TRice("CAN trace armed (%u frames)\n", CANMON_TRACE_MAX);
    } else if (strncmp(args, "trace off", 9) == 0) {
        CanMon_TraceEnable(0);
        TRice("CAN trace off\n");
    } else if (strncmp(args, "trace", 5) == 0) {
        CanPrintTrace((uint16_t)atoi(args + 5));
    } else if (strncmp(args, "send ", 5) == 0) {
        unsigned  bus = 0u;
        unsigned  id = 0u;
        char      hex[32];
        sCanFrame frame;
        int       len;

        hex[0] = '\0';
        if (sscanf(args + 5, "%u %x %31s", &bus, &id, hex) < 2) {
            TRice("Usage: can send <1|2> <id-hex> [databytes-hex]\n");
            return;
        }
        memset(&frame, 0, sizeof(frame));
        len = parse_hex_bytes(hex, frame.data, sizeof(frame.data));
        if (len < 0) {
            TRice("can send: malformed hex payload\n");
            return;
        }
        if ((bus < 1u) || (bus > (unsigned)canBus_last)) {
            TRice("can send: bus must be 1 or 2\n");
            return;
        }
        frame.bus = (uint8_t)(bus - 1u);
        frame.id  = id;
        frame.ext = (id > 0x7FFu) ? 1u : 0u;
        frame.dlc = (uint8_t)len;
        if (CanBus_Send(&frame) != 0) {
            TRice("can send: refused (bus down or queue full)\n");
            return;
        }
        TRice("can send: CAN%u %03X [%u]\n", bus, id, frame.dlc);
    } else if (strncmp(args, "roles ", 6) == 0) {
        unsigned bat = 0u;
        unsigned inv = 0u;

        if (sscanf(args + 6, "%u %u", &bat, &inv) != 2) {
            TRice("Usage: can roles <batteryBus> <inverterBus>\n");
            return;
        }
        if (CanBridge_SetRoles((eCanBus)(bat - 1u), (eCanBus)(inv - 1u)) != 0) {
            TRice("can roles: refused (stop the bridge first, and the two "
                  "sides must differ)\n");
            return;
        }
        TRice("can roles: battery=CAN%u inverter=CAN%u\n", bat, inv);
    } else if (strncmp(args, "override ", 9) == 0) {
        const char *p = args + 9;

        if (strncmp(p, "all", 3) == 0) {
            CanBridge_SetOverrideAll(1);
            TRice("can override: the whole battery->inverter direction\n");
        } else if (strncmp(p, "none", 4) == 0) {
            CanBridge_ClearOverrides();
            CanBridge_SetOverrideAll(0);
            TRice("can override: nothing (bms mode relays everything)\n");
        } else {
            CanBridge_SetOverrideAll(0);
            while (isxdigit((int)(unsigned char)*p) != 0) {
                uint32_t id = (uint32_t)strtoul(p, NULL, 16);

                if (CanBridge_AddOverrideId(id) != 0) {
                    TRice("can override: list full\n");
                    break;
                }
                TRice("can override: +%03X\n", id);
                while ((*p != '\0') && (*p != ' ')) {
                    p++;
                }
                while (*p == ' ') {
                    p++;
                }
            }
        }
    } else if (strncmp(args, "log status", 10) == 0) {
        CanPrintLogStatus();
    } else if (strncmp(args, "log mode ", 9) == 0) {
        const char *p = args + 9;
        eCanLogMode mode;
        uint32_t    minGap_ms = CANLOG_MIN_GAP_MS;
        uint32_t    heartbeat_ms = CANLOG_HEARTBEAT_MS;
        unsigned    g = 0u;
        unsigned    h = 0u;
        int         got;

        if (CanLog_ModeFromName(p, &mode) != 0) {
            TRice("Usage: can log mode <off|changes|all> [gap_ms] [hb_ms]\n");
            return;
        }
        while ((*p != '\0') && (*p != ' ')) {
            p++;
        }
        while (*p == ' ') {
            p++;
        }
        got = sscanf(p, "%u %u", &g, &h);
        if (got >= 1) {
            minGap_ms = g;
        }
        if (got >= 2) {
            heartbeat_ms = h;
        }
        if (CanLog_SetPolicy(mode, minGap_ms, heartbeat_ms) != 0) {
            TRice("can log mode: refused\n");
            return;
        }
        CanPrintLogStatus();
    } else if (strncmp(args, "log wipe", 8) == 0) {
        if (CanLog_Wipe() != 0) {
            TRice("can log wipe: no usable flash area\n");
            return;
        }
        TRice("CAN log: wipe requested\n");
    } else if (strncmp(args, "log read ", 9) == 0) {
        uint32_t   recNo = (uint32_t)strtoul(args + 9, NULL, 0);
        sCanLogRec rec;
        int        rc = CanLog_ReadRec(recNo, &rec);

        if (rc != 0) {
            TRice("can log read %u: unavailable (%d)\n", recNo, rc);
            return;
        }
        TRice("rec %u: t=%ums CAN%u %c %03X [%u]", recNo, rec.stamp_ms,
              (unsigned)CANLOG_META_BUS(rec.meta) + 1u,
              CANLOG_META_DIR(rec.meta) ? 'T' : 'R', rec.id,
              CANLOG_META_DLC(rec.meta));
        TRice(" %02X%02X%02X%02X%02X%02X%02X%02X\n",
              rec.data[0], rec.data[1], rec.data[2], rec.data[3],
              rec.data[4], rec.data[5], rec.data[6], rec.data[7]);
    } else if (strncmp(args, "log", 3) == 0) {
        TRice("Usage: can log status|mode|read|wipe\n");
    } else if (strncmp(args, "reset", 5) == 0) {
        CanBus_ResetStats();
        CanBridge_ResetStats();
        CanMon_Reset();
        TRice("CAN counters cleared\n");
    } else {
        TRice("Usage: can start|stop|mode|status|ids|trace|log|send|roles|"
              "override|reset\n");
    }
}

/**
 * Parse a no-space hex byte string ("0104280200ff...") into a byte buffer.
 * Returns byte count, or -1 on malformed input (odd digits / non-hex char).
 */
static int parse_hex_bytes(const char *p, uint8_t *out, size_t maxLen)
{
    size_t len = 0;

    while (*p != '\0' && *p != ' ' && *p != '\t') {
        int hi, lo;
        hi = (*p >= '0' && *p <= '9') ? *p - '0' :
             (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10 :
             (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10 : -1;
        p++;
        lo = (*p >= '0' && *p <= '9') ? *p - '0' :
             (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10 :
             (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10 : -1;
        if (hi < 0 || lo < 0 || len >= maxLen) {
            return -1;
        }
        out[len++] = (uint8_t)((hi << 4) | lo);
        p++;
    }

    return (int)len;
}

/* One-item Modbus_Request from the CLI.  The array is BORROWED by the module
 * until the callback fires, so it is static and guarded — the contract is the
 * same one every requester lives under (docs/modbus.md §4.6). */
static sModbusReqItem s_cliItem;
static volatile int   s_cliBusy;
static uint8_t        s_cliDev;

static void cli_req_done(const sModbusReqReply *rep, void *ctx)
{
    (void)ctx;

    if (rep->count > 0) {
        TRice("Modbus req: dev %u pt %u = %d (%d)\n", s_cliDev,
              rep->items[0].id, (int)rep->items[0].value,
              (int)rep->items[0].result);
    }
    s_cliBusy = 0;
}

static void cli_request(const char *args, int isWrite)
{
    unsigned dev = 0, pt = 0;
    int      val = 0;

    if (isWrite) {
        if (sscanf(args, "%u %u %d", &dev, &pt, &val) != 3) {
            TRice("Usage: modbus set <devOrd> <ptOrd> <value>\n");
            return;
        }
    } else if (sscanf(args, "%u %u", &dev, &pt) != 2) {
        TRice("Usage: modbus get <devOrd> <ptOrd>\n");
        return;
    }

    if (s_cliBusy) {
        TRice("Modbus req: busy\n");
        return;
    }

    s_cliDev         = (uint8_t)dev;
    s_cliItem.id     = (uint16_t)pt;
    s_cliItem.value  = val;
    s_cliItem.result = mbErr_pending;
    s_cliBusy        = 1;

    int r = Modbus_Request((uint8_t)dev, &s_cliItem, 1, 3000u,
                           cli_req_done, NULL);
    if (r != 0) {
        s_cliBusy = 0;
        TRice("Modbus req: rejected (%d)\n", r);
    }
}

/**
 * Modbus command: drive the module.
 *
 * There is no start/stop (Modbus_Init is the entire lifecycle and what gets
 * polled is decided by SUBSCRIPTIONS), no `set baud` (a device parameter), no
 * `port` (a device lives on a port; that is config), no raw `write` (there is
 * no raw write path) and no `probe` (no direct bus access at all) — each was a
 * knob on something that is now either config or nobody's business outside the
 * module (docs/modbus.md §8.2).
 *
 * Usage:
 *   modbus read / status         — Log engine/config status
 *   modbus get <devOrd> <ptOrd>  — Read one point via Modbus_Request
 *   modbus set <devOrd> <ptOrd> <value> — Write one point
 *   modbus monitor <on|off>      — Trice raw TX/RX frame monitoring
 *   modbus dump <on|off>         — Trice subscriber: every decoded reading
 *   modbus plan list             — every slot: name, capability, devices,
 *                                  time tables, subscriber count
 *   modbus plan show <id>        — one plan's time tables and point ids
 *   modbus plan del <id>         — free a slot (refused while subscribed)
 *   modbus inject <hexbytes>     — Stage the reply the next frame gets
 *   modbus silence               — Stage silence for the next frame
 *   modbus lastreq               — The last frame the engine formed
 */
static void cmd_modbus(const char *args)
{
    if (strncmp(args, "get ", 4) == 0) {
        /* What the config declares can be read on demand, whether or not any
         * plan watches it (§4.6). */
        cli_request(args + 4, 0);
    } else if (strncmp(args, "set ", 4) == 0) {
        cli_request(args + 4, 1);
    } else if (strncmp(args, "read", 4) == 0) {
        Modbus_LogStatus();
    } else if (strncmp(args, "monitor ", 8) == 0) {
        if (strncmp(args + 8, "on", 2) == 0) {
            Modbus_SetMonitor(1);
            TRice("Modbus monitor: on\n");
        } else if (strncmp(args + 8, "off", 3) == 0) {
            Modbus_SetMonitor(0);
            TRice("Modbus monitor: off\n");
        } else {
            TRice("Usage: modbus monitor on|off\n");
        }
    } else if (strncmp(args, "plan", 4) == 0) {
        const char *p = args + 4;
        while (*p == ' ') p++;

        if (strncmp(p, "list", 4) == 0) {
            sModbusPlanInfo plans[MB_MAX_PLANS];
            int n = Modbus_PlanList(plans, MB_MAX_PLANS);

            if (n <= 0) {
                TRice("Modbus plans: none\n");
                return;
            }
            for (int i = 0; i < n; i++) {
                char buf[100];
                snprintf(buf, sizeof(buf),
                         "%u %s cap=%u devices=0x%02x tables=%u subs=%u",
                         plans[i].planId, plans[i].name, plans[i].capId,
                         plans[i].devices, plans[i].timeTables,
                         plans[i].subscribers);
                TRiceS("Modbus plan: %s\n", buf);
            }
        } else if (strncmp(p, "show ", 5) == 0) {
            unsigned id = 0;
            sModbusPlanInfo info;

            if (sscanf(p + 5, "%u", &id) != 1 || id >= MB_MAX_PLANS) {
                TRice("Usage: modbus plan show <id>\n");
                return;
            }
            if (Modbus_PlanGet((uint8_t)id, &info) != 0) {
                TRice("Modbus plan: %u is free\n", id);
                return;
            }

            char buf[100];
            snprintf(buf, sizeof(buf),
                     "%u %s cap=%u devices=0x%02x tables=%u subs=%u",
                     info.planId, info.name, info.capId, info.devices,
                     info.timeTables, info.subscribers);
            TRiceS("Modbus plan: %s\n", buf);

            /* A plan's time tables are NOT in the resident header table, so
             * this reads them from flash like any other record (§4.3). */
            sModbusTimeTableSpec tables[MB_MAX_TIME_TABLES_PER_PLAN];
            uint16_t             ids[MB_MAX_TT_ENTRIES_PER_PLAN];
            int n = Modbus_PlanTables((uint8_t)id, tables,
                                      MB_MAX_TIME_TABLES_PER_PLAN,
                                      ids, MB_MAX_TT_ENTRIES_PER_PLAN);
            for (int t = 0; t < n; t++) {
                char   tb[100];
                size_t at = Json_Cat(tb, sizeof(tb), 0u, "%us:",
                                     (unsigned)tables[t].period_sec);
                for (uint16_t k = 0; k < tables[t].count; k++) {
                    at = Json_Cat(tb, sizeof(tb), at, " %u",
                                  tables[t].points[k]);
                }
                TRiceS("  table: %s\n", tb);
            }
        } else if (strncmp(p, "del ", 4) == 0) {
            unsigned id = 0;
            if (sscanf(p + 4, "%u", &id) != 1 || id >= MB_MAX_PLANS) {
                TRice("Usage: modbus plan del <id>\n");
                return;
            }
            int r = Modbus_PlanDelete((uint8_t)id);
            if (r == 0) {
                TRice("Modbus plan: %u delete armed\n", id);
            } else if (r == mbErr_busy) {
                TRice("Modbus plan: %u refused, it has a subscriber\n", id);
            } else {
                TRice("Modbus plan: %u delete failed (%d)\n", id, r);
            }
        } else {
            /* `plan add` and `plan set` take a plan object, and a plan object
             * is JSON — which the CLI has no business carrying.  They live on
             * POST/PUT /api/modbus/plans (docs/modbus.md §8.1). */
            TRice("Usage: modbus plan list|show <id>|del <id>  (add/set: HTTP)\n");
        }
    } else if (strncmp(args, "dump ", 5) == 0) {
        if (strncmp(args + 5, "on", 2) == 0) {
            ModbusTriceSink_Set(1);
        } else if (strncmp(args + 5, "off", 3) == 0) {
            ModbusTriceSink_Set(0);
        } else {
            TRice("Usage: modbus dump on|off\n");
        }
    } else if (strncmp(args, "inject ", 7) == 0) {
        /* modbus inject <hexbytes> — stage the reply the next frame gets.
         * NOT a hole beneath the port: it feeds an App-layer driver the module
         * cannot distinguish from a UART (docs/modbus.md §8.2). */
        uint8_t frame[MB_TEST_FRAME_MAX];
        int len = parse_hex_bytes(args + 7, frame, sizeof(frame));

        if (len <= 0) {
            TRice("Usage: modbus inject <hexbytes>\n");
            return;
        }
        if (ModbusTestPort_StageReply(frame, (uint16_t)len) == 0) {
            TRice("Modbus inject: %d bytes staged\n", len);
        } else {
            TRice("Modbus inject: ERR_SHORT\n");
        }
    } else if (strncmp(args, "lastreq", 7) == 0) {
        /* What the engine FORMED, for a harness that wants to assert on the
         * request rather than only on what comes up out of the module.  A
         * bonus rather than the point: integration testing is upward-only
         * (docs/modbus.md §9). */
        uint8_t frame[MB_TEST_FRAME_MAX];
        uint16_t n = ModbusTestPort_LastRequest(frame, sizeof(frame));

        if (n == 0u) {
            TRice("Modbus lastreq: none\n");
            return;
        }
        char   hex[80];
        size_t at = 0u;
        for (uint16_t i = 0; i < n; i++) {
            at = Json_Cat(hex, sizeof(hex), at, "%02x", frame[i]);
        }
        TRiceS("Modbus lastreq: %s\n", hex);
    } else if (strncmp(args, "silence", 7) == 0) {
        ModbusTestPort_StageSilence();
        TRice("Modbus inject: silence staged\n");
    } else if (strncmp(args, "status", 6) == 0) {
        sModbusStats st;
        char         buf[100];

        (void)Modbus_Stats(&st);
        snprintf(buf, sizeof(buf),
                 "monitor=%s polls=%lu errors=%lu missed=%lu reqs=%lu test=%lu",
                 st.monitor ? "on" : "off", (unsigned long)st.polls,
                 (unsigned long)st.errors, (unsigned long)st.missed,
                 (unsigned long)st.requests,
                 (unsigned long)ModbusTestPort_RequestCount());
        TRiceS("Modbus %s\n", buf);
        Modbus_LogStatus();
    } else {
        TRice("Usage: modbus read|get|set|monitor|dump|plan|inject|silence|lastreq|status\n");
    }
}

/**
 * MQTT command: control MQTT bridge to Home Assistant.
 *
 * Usage:
 *   mqtt start [ip] [port]   — Start MQTT bridge.  With no address it uses
 *                              the saved one, or 10.42.0.1:1883 if none
 *   mqtt save                — Persist the running config across resets
 *   mqtt forget              — Discard the persisted config
 *   mqtt stop                — Stop bridge
 *   mqtt status              — Show connection state
 *   mqtt set ip <a.b.c.d>   — Change broker IP
 *   mqtt monitor <on|off>    — Trice pub/sub message monitoring
 *   mqtt inject <topic> <payload> — Process a message as if from broker
 *   mqtt publish now         — Publish all values immediately
 */
static void cmd_mqtt(const char *args)
{
    if (strncmp(args, "start", 5) == 0) {
        if (MqttBridge_IsRunning()) {
            TRice("MQTT already running\n");
            return;
        }
        sMqttBridgeCfg cfg = {
            .brokerIp = {10, 42, 0, 1},
            .brokerPort = 1883,
            .prefix = "periphnet",
            .publishIntervalMs = 5000,
        };
        /* A saved broker beats the compiled-in one, which is a bench
         * address and wrong everywhere else. */
        if (MqttBridge_LoadCfg(&cfg) == 0) {
            TRice("MQTT: using saved broker\n");
        }
        /* Parse optional: mqtt start [a.b.c.d] [port] */
        const char *p = args + 5;
        unsigned a, b, c, d, port = 0;
        if (sscanf(p, " %u.%u.%u.%u %u", &a, &b, &c, &d, &port) >= 4) {
            cfg.brokerIp[0] = a;
            cfg.brokerIp[1] = b;
            cfg.brokerIp[2] = c;
            cfg.brokerIp[3] = d;
            if (port > 0) cfg.brokerPort = (uint16_t)port;
        }
        MqttBridge_Start(&cfg);
    } else if (strncmp(args, "save", 4) == 0) {
        if (MqttBridge_SaveCfg() == 0) {
            TRice("MQTT config saved\n");
        } else {
            TRice("MQTT config save FAILED\n");
        }
    } else if (strncmp(args, "forget", 6) == 0) {
        if (MqttBridge_ForgetCfg() == 0) {
            TRice("MQTT config forgotten\n");
        } else {
            TRice("MQTT config forget FAILED\n");
        }
    } else if (strncmp(args, "stop", 4) == 0) {
        MqttBridge_Stop();
    } else if (strncmp(args, "set ip ", 7) == 0) {
        unsigned a, b, c, d;
        if (sscanf(args + 7, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            MqttBridge_SetBrokerIp((uint8_t)a, (uint8_t)b,
                                    (uint8_t)c, (uint8_t)d);
            TRice("MQTT broker IP set to %u.%u.%u.%u\n", a, b, c, d);
        } else {
            TRice("Usage: mqtt set ip <a.b.c.d>\n");
        }
    } else if (strncmp(args, "monitor ", 8) == 0) {
        if (strncmp(args + 8, "on", 2) == 0) {
            MqttBridge_SetMonitor(1);
            TRice("MQTT monitor: on\n");
        } else if (strncmp(args + 8, "off", 3) == 0) {
            MqttBridge_SetMonitor(0);
            TRice("MQTT monitor: off\n");
        } else {
            TRice("Usage: mqtt monitor on|off\n");
        }
    } else if (strncmp(args, "inject ", 7) == 0) {
        /* mqtt inject <topic> <payload — rest of line> */
        const char *p = args + 7;
        while (*p == ' ') p++;
        const char *topicStart = p;
        while (*p != '\0' && *p != ' ') p++;
        size_t topicLen = (size_t)(p - topicStart);
        while (*p == ' ') p++;

        char topic[80];
        if (topicLen == 0 || topicLen >= sizeof(topic) || *p == '\0') {
            TRice("Usage: mqtt inject <topic> <payload>\n");
            return;
        }
        memcpy(topic, topicStart, topicLen);
        topic[topicLen] = '\0';

        if (MqttBridge_Inject(topic, p, (uint16_t)strlen(p)) != 0) {
            TRice("MQTT inject failed (not running or busy)\n");
        }
    } else if (strncmp(args, "publish now", 11) == 0) {
        if (!MqttBridge_IsRunning()) {
            TRice("MQTT not running\n");
            return;
        }
        MqttBridge_PublishNow();
    } else if (strncmp(args, "status", 6) == 0) {
        MqttBridge_LogStatus();
    } else {
        TRice("Usage: mqtt start|stop|save|forget|status|set|monitor|inject|publish\n");
    }
}

/**
 * WG command: control the WireGuard tunnel netif.
 *
 * Usage:
 *   wg start                  — Bring the tunnel up with the active config
 *   wg stop                   — Remove the netif (tunnel down)
 *   wg status                 — Config, session state, RNG health, time base
 *   wg endpoint <a.b.c.d> [port] — Point at a different hub (live if running)
 *   wg ip <a.b.c.d> [mask]    — Set our address inside the tunnel (restarts it)
 *   wg genkey                 — Mint a new identity key on-device + print pub
 *   wg save                   — Persist the active config across reboot/OTA
 *   wg reset                  — Erase the stored config (and its private key)
 *
 * "ip" and "endpoint" change the running config only; follow with "wg save"
 * to make them survive a reset.
 */
static void cmd_wg(const char *args)
{
    if (strncmp(args, "start", 5) == 0) {
        int rc = WgLink_Start(NULL);
        if (rc == 0) {
            TRice("WG started\n");
        } else if (rc == -1) {
            TRice("WG already running\n");
        } else {
            TRice("WG start failed (%d)\n", rc);
        }
    } else if (strncmp(args, "stop", 4) == 0) {
        WgLink_Stop();
    } else if (strncmp(args, "endpoint ", 9) == 0) {
        unsigned a, b, c, d, port = 51820u;
        if (sscanf(args + 9, "%u.%u.%u.%u %u", &a, &b, &c, &d, &port) >= 4) {
            uint8_t ip[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
            if (WgLink_SetEndpoint(ip, (uint16_t)port) == 0) {
                TRice("WG endpoint set to %u.%u.%u.%u:%u\n", a, b, c, d, port);
            } else {
                TRice("WG endpoint update failed\n");
            }
        } else {
            TRice("Usage: wg endpoint <a.b.c.d> [port]\n");
        }
    } else if (strncmp(args, "ip ", 3) == 0) {
        unsigned a, b, c, d;
        unsigned m0 = 0, m1 = 0, m2 = 0, m3 = 0;
        int      n = sscanf(args + 3, "%u.%u.%u.%u %u.%u.%u.%u",
                            &a, &b, &c, &d, &m0, &m1, &m2, &m3);
        if (n == 4 || n == 8) {
            uint8_t ip[4]   = { (uint8_t)a,  (uint8_t)b,  (uint8_t)c,  (uint8_t)d  };
            uint8_t mask[4] = { (uint8_t)m0, (uint8_t)m1, (uint8_t)m2, (uint8_t)m3 };
            if (WgLink_SetTunnelIp(ip, (n == 8) ? mask : NULL) == 0) {
                TRice("WG tunnel ip set to %u.%u.%u.%u (use 'wg save' to keep)\n",
                      a, b, c, d);
            } else {
                TRice("WG tunnel ip update failed\n");
            }
        } else {
            TRice("Usage: wg ip <a.b.c.d> [mask]\n");
        }
    } else if (strncmp(args, "genkey", 6) == 0) {
        /* Generating on-device means the private key exists nowhere else —
         * register the printed public key with the hub. */
        if (WgLink_GenerateKey(1) >= 0) {
            char pub[WG_KEY_B64_SIZE];
            if (WgLink_GetPublicKeyB64(pub, sizeof(pub)) == 0) {
                TRiceS("WG new public key: %s\n", pub);
            }
        } else {
            TRice("WG genkey failed\n");
        }
    } else if (strncmp(args, "save", 4) == 0) {
        if (WgLink_SaveCfg() == 0) {
            TRice("WG config saved\n");
        } else {
            TRice("WG config save failed\n");
        }
    } else if (strncmp(args, "reset", 5) == 0) {
        if (WgLink_ResetCfg() == 0) {
            TRice("WG config reset to defaults\n");
        } else {
            TRice("WG config reset failed\n");
        }
    } else if (strncmp(args, "status", 6) == 0) {
        const sWgLinkCfg *cfg = WgLink_ActiveCfg();
        uint32_t now = 0, persisted = 0, rngFailures = 0;
        int flashOk = 0, hwSeeded = 0;

        WgTime_GetStatus(&now, &persisted, &flashOk);
        WgPlatform_GetRngStatus(&hwSeeded, &rngFailures);

        TRice("WG: running=%u session=%u cfg=%s\n",
              (unsigned)WgLink_IsRunning(), (unsigned)WgLink_IsUp(),
              WgLink_CfgIsStored() ? "stored" : "built-in");
        {
            /* session= above is a threshold on this age, not the port's own
             * peer-is-up flag — which never goes false once a session has
             * existed.  A keypair that stays valid while the age climbs is
             * the stuck peer; recoveries counts the rebuilds that fixed it. */
            sWgPeerStats st;
            if (WgLink_GetPeerStats(&st) == 0) {
                TRice("WG: alive_age=%dms keypair=%u/%u age=%ums recoveries=%u\n",
                      (st.aliveAge_ms == WG_LINK_AGE_NEVER)
                          ? -1 : (int)st.aliveAge_ms,
                      (unsigned)st.sessionValid, (unsigned)st.prevValid,
                      (unsigned)st.keypairAge_ms,
                      (unsigned)WgLink_RecoveryCount());
            }
        }
        TRice("WG: tunnel ip %d.%d.%d.%d/%d.%d.%d.%d\n",
              cfg->tunnelIp[0], cfg->tunnelIp[1],
              cfg->tunnelIp[2], cfg->tunnelIp[3],
              cfg->tunnelMask[0], cfg->tunnelMask[1],
              cfg->tunnelMask[2], cfg->tunnelMask[3]);
        TRice("WG: endpoint %d.%d.%d.%d:%d keepalive=%us\n",
              cfg->endpointIp[0], cfg->endpointIp[1],
              cfg->endpointIp[2], cfg->endpointIp[3],
              cfg->endpointPort, cfg->keepAlive_sec);
        {
            char pub[WG_KEY_B64_SIZE];
            uint8_t i;
            if (WgLink_GetPublicKeyB64(pub, sizeof(pub)) == 0) {
                TRiceS("WG: public key %s\n", pub);
            } else {
                TRice("WG: NO IDENTITY — upload a .conf to provision\n");
            }
            for (i = 0u; i < cfg->allowedCount; i++) {
                TRice("WG: allowed %d.%d.%d.%d/%d.%d.%d.%d\n",
                      cfg->allowed[i].ip[0], cfg->allowed[i].ip[1],
                      cfg->allowed[i].ip[2], cfg->allowed[i].ip[3],
                      cfg->allowed[i].mask[0], cfg->allowed[i].mask[1],
                      cfg->allowed[i].mask[2], cfg->allowed[i].mask[3]);
            }
        }
        TRice("WG: rng hw_seeded=%u failures=%u\n",
              (unsigned)hwSeeded, rngFailures);
        TRice("WG: time now=%us persisted=%us flash=%u\n",
              now, persisted, (unsigned)flashOk);
    } else {
        TRice("Usage: wg start|stop|status|endpoint|ip|genkey|save|reset\n");
    }
}

/**
 * The non-volatile store: what it did with the medium, for a person.
 *
 * Usage:
 *   nvdb status      — module and layout version, and how the last apply went
 *   nvdb layout      — the layout in force, free space, anything waiting
 *   nvdb usage       — per-user occupancy and wear (SLOW: reads every area)
 *   nvdb wear        — erase counts only, no scan
 *   nvdb drop        — discard a layout that came aboard but has not applied
 *
 * Read-only apart from `drop`.  An nvDb USER never asks any of this, and the
 * operator asking it has no business erasing somebody else's area from a
 * console — a layout is supplied over HTTP (POST /api/nvdb/layout), where a
 * parse error can point at the offending field.
 */

/* --- battery packs ------------------------------------------------------- */

/** Stream the exported config out as Trice lines.  Chunked deliberately: the
 *  document is ~1 KB and a Trice payload is not. */
static int pack_cli_sink(void *ctx, const char *data, uint32_t len)
{
    static char line[80];
    uint32_t    off = 0u;

    (void)ctx;
    while (off < len) {
        uint32_t n = len - off;

        if (n > (sizeof(line) - 1u)) {
            n = sizeof(line) - 1u;
        }
        (void)memcpy(line, &data[off], n);
        line[n] = '\0';
        TRiceS("%s", line);
        off += n;
    }
    return (int)len;
}

static const char *pack_cond_name(uint8_t c)
{
    switch ((ePackCondition)c) {
    case packCond_absent: return "absent";
    case packCond_stale:  return "stale";
    case packCond_online: return "online";
    default:              return "?";
    }
}

static const char *pack_why_name(uint8_t w)
{
    switch ((ePackAbsentReason)w) {
    case packWhy_none:            return "-";
    case packWhy_noType:          return "no such type in this firmware";
    case packWhy_typeUnavailable: return "type present, transport missing";
    case packWhy_noBinding:       return "bind token did not resolve";
    case packWhy_notPolled:       return "resolved, but no live plan reads it";
    case packWhy_noReply:         return "never answered";
    default:                      return "?";
    }
}

static const char *pack_sw_name(uint8_t sw)
{
    switch ((ePackSwitch)sw) {
    case packSwitch_unknown: return "?";
    case packSwitch_open:    return "open";
    case packSwitch_closed:  return "closed";
    default:                 return "?";
    }
}

/** One line per pack: what an operator wants first. */
static void pack_print_line(uint8_t i)
{
    sPackState st;

    if (Pack_GetState(i, &st) != packErr_ok) {
        return;
    }
    TRiceS("  %s", st.name);
    TRice(" [%u] %u.%03u V %d mA soc=%u.%u%% cond=",
          (unsigned)i,
          (unsigned)(st.voltage_mV / 1000u), (unsigned)(st.voltage_mV % 1000u),
          (int)st.current_mA,
          (unsigned)(st.soc_pm / 10u), (unsigned)(st.soc_pm % 10u));
    TRiceS("%s\n", pack_cond_name(st.cond));
}

static void cmd_pack(const char *args)
{
    uint8_t idx = 0u;

    if ((*args == '\0') || (strncmp(args, "status", 6) == 0)) {
        sPackStats stats;
        sFuncStats fs;

        if (Pack_Stats(&stats) != packErr_ok) {
            TRice("pack: not initialised\n");
            return;
        }
        TRice("packs=%d provisioned=%u\n", Pack_Count(),
              (unsigned)stats.provisioned);
        TRice("  updates=%u stale=%u bindFail=%u\n",
              (unsigned)stats.updates, (unsigned)stats.staleEvents,
              (unsigned)stats.bindFailures);
        TRice("  cmd acc=%u ok=%u fail=%u unknown=%u refused=%u late=%u\n",
              (unsigned)stats.cmdAccepted, (unsigned)stats.cmdOk,
              (unsigned)stats.cmdFailed, (unsigned)stats.cmdUnknown,
              (unsigned)stats.cmdRefused, (unsigned)stats.lateCompletes);
        if (Func_Stats(&fs) == 0) {
            /* A DROPPED EVENT IS A MISSED STATE CHANGE, which is why func.c
             * counts them per client rather than only logging. */
            TRice("  func: posted=%u handled=%u ticks=%u dropped=%u/%u\n",
                  (unsigned)fs.posted, (unsigned)fs.handled,
                  (unsigned)fs.ticks, (unsigned)fs.dropped[0],
                  (unsigned)fs.droppedUnknown);
        }
        return;
    }

    if (strncmp(args, "list", 4) == 0) {
        uint8_t i;

        if (Pack_Count() == 0) {
            TRice("pack: unprovisioned - upload a config first\n");
            return;
        }
        for (i = 0u; i < PACK_MAX; i++) {
            pack_print_line(i);
        }
        return;
    }

    if (strncmp(args, "show", 4) == 0) {
        sPackState st;
        uint32_t   g;

        idx = (uint8_t)atoi(args + 4);
        if (Pack_GetState(idx, &st) != packErr_ok) {
            TRice("pack %u: no such pack\n", (unsigned)idx);
            return;
        }
        TRiceS("pack '%s'", st.name);
        TRiceS(" type=%s", Pack_TypeName(st.typeId));
        TRiceS(" cond=%s", pack_cond_name(st.cond));
        TRiceS(" why=%s\n", pack_why_name(st.why));
        /* socDrift is the estimator's disagreement with its own coulomb
         * count at the last anchor -- the bound on how wrong SOC can be. */
        TRice("  soc drift=%d per-mille  estimated=%u\n",
              (int)st.socDrift_pm,
              (unsigned)((st.flags & (uint32_t)packFlag_socEstimated) ? 1u : 0u));
        TRice("  caps=%08x cmds=%02x flags=%02x conf soc=%u soh=%u (per-mille)\n",
              (unsigned)st.caps, (unsigned)st.cmds, (unsigned)st.flags,
              (unsigned)st.socConf_pm, (unsigned)st.sohConf_pm);
        TRice("  %u.%03u V  %d mA   soc=%u.%u%% soh=%u.%u%%\n",
              (unsigned)(st.voltage_mV / 1000u), (unsigned)(st.voltage_mV % 1000u),
              (int)st.current_mA,
              (unsigned)(st.soc_pm / 10u), (unsigned)(st.soc_pm % 10u),
              (unsigned)(st.soh_pm / 10u), (unsigned)(st.soh_pm % 10u));
        TRice("  remaining=%u mAh capacity=%u mAh nameplate=%u mAh\n",
              (unsigned)st.remaining_mAh, (unsigned)st.capacity_mAh,
              (unsigned)st.nameplate_mAh);
        TRice("  limits: chg=%u mA dsg=%u mA chgV=%u mV dsgV=%u mV\n",
              (unsigned)st.chargeLimit_mA, (unsigned)st.dischargeLimit_mA,
              (unsigned)st.chargeVoltLimit_mV,
              (unsigned)st.dischargeVoltLimit_mV);
        TRiceS("  charge switch=%s", pack_sw_name(st.chargeSwitch));
        TRiceS(" discharge switch=%s\n", pack_sw_name(st.dischargeSwitch));
        TRice("  temp %d..%d dC  cells %u..%u mV (idx %u/%u)\n",
              (int)st.tempMin_dC, (int)st.tempMax_dC,
              (unsigned)st.cellMin_mV, (unsigned)st.cellMax_mV,
              (unsigned)st.cellMinIdx, (unsigned)st.cellMaxIdx);
        TRice("  alarms=%08x vendor=%08x/%08x\n",
              (unsigned)st.alarms, (unsigned)st.vendorAlarms[0],
              (unsigned)st.vendorAlarms[1]);
        /* PER-GROUP AGES, because a pack is not one clock: on a JK the cell
         * group runs 4-5 s behind the electrical one by construction. */
        for (g = 0u; g < (uint32_t)packGrp_last; g++) {
            if (st.age_ms[g] != PACK_AGE_NEVER) {
                TRice("  age[%u]=%u ms\n", (unsigned)g,
                      (unsigned)st.age_ms[g]);
            } else {
                TRice("  age[%u]=never\n", (unsigned)g);
            }
        }
        return;
    }

    if (strncmp(args, "cells", 5) == 0) {
        sPackCells cl;
        uint8_t    c;
        int        r;

        idx = (uint8_t)atoi(args + 5);
        r   = Pack_GetCells(idx, &cl);
        if (r == packErr_notSupported) {
            TRice("pack %u: no cell detail from this type\n", (unsigned)idx);
            return;
        }
        if (r != packErr_ok) {
            TRice("pack %u: no such pack\n", (unsigned)idx);
            return;
        }
        TRice("pack %u: %u cells, age=%u ms\n", (unsigned)idx,
              (unsigned)cl.cellCount, (unsigned)cl.age_ms);
        for (c = 0u; c < cl.cellCount; c++) {
            TRice("  cell%02u %u mV  lead=%u mOhm\n", (unsigned)c,
                  (unsigned)cl.cell_mV[c], (unsigned)cl.leadRes_mOhm[c]);
        }
        {
            sPackCellEstimate est;

            if (Pack_GetCellEstimate(idx, &est) == packErr_ok) {
                uint8_t k;

                TRice("  per-cell estimate: %u of %u measured, weakest=%d\n",
                      (unsigned)est.measuredCount, (unsigned)est.cellCount,
                      (int)est.weakestIdx);
                for (k = 0u; k < est.cellCount; k++) {
                    TRice("   cell%02u soc=%d per-mille cap=%d mAh conf=%u\n",
                          (unsigned)k, (int)est.soc_pm[k],
                          (int)est.capacity_mAh[k],
                          (unsigned)est.capConf_pm[k]);
                }
            }
        }
        TRice("  balance: active=%u %d mA duty=%u src=%u sink=%u\n",
              (unsigned)cl.balanceActive, (int)cl.balanceCurrent_mA,
              (unsigned)cl.balanceDuty_pm, (unsigned)cl.balanceSrcIdx,
              (unsigned)cl.balanceSinkIdx);
        return;
    }

    if (strncmp(args, "stats", 5) == 0) {
        int n;
        int i;

        idx = (uint8_t)atoi(args + 5);
        n   = Pack_StatCount(idx);
        if (n < 0) {
            TRice("pack %u: no such pack\n", (unsigned)idx);
            return;
        }
        TRice("pack %u: %d statistics\n", (unsigned)idx, n);
        for (i = 0; i < n; i++) {
            sPackStat st;

            if (Pack_StatGet(idx, (uint8_t)i, &st) != packErr_ok) {
                continue;
            }
            TRiceS("  %s", st.name);
            TRice(" = %d e%d unit=%u flags=%02x\n", (int)st.value,
                  (int)st.scale_pow10, (unsigned)st.unit, (unsigned)st.flags);
        }
        return;
    }

    if (strncmp(args, "balance", 7) == 0) {
        sPackBalanceStats b;
        uint8_t           c;
        int               r;

        if (strstr(args, "reset") != NULL) {
            idx = (uint8_t)atoi(args + 7);
            TRice("pack %u: balance reset -> %d\n", (unsigned)idx,
                  Pack_BalanceReset(idx));
            return;
        }
        idx = (uint8_t)atoi(args + 7);
        r   = Pack_BalanceStats(idx, &b);
        if (r == packErr_notSupported) {
            TRice("pack %u: this type has no balancer\n", (unsigned)idx);
            return;
        }
        if (r != packErr_ok) {
            TRice("pack %u: no such pack\n", (unsigned)idx);
            return;
        }
        TRice("pack %u: window %u s, %u active samples, %u cells\n",
              (unsigned)idx, (unsigned)b.window_sec,
              (unsigned)b.activeSamples, (unsigned)b.cellCount);
        /* capacityDelta is RELATIVE TO THE PACK MEDIAN and negative means
         * the balancer keeps having to charge this cell -- i.e. it holds
         * less than the pack (§23.3). */
        for (c = 0u; c < b.cellCount; c++) {
            TRice("  cell%02u in=%d mAs out=%d mAs  dCap=%d mAh\n",
                  (unsigned)c, (int)b.in_mAs[c], (int)b.out_mAs[c],
                  (int)b.capacityDelta_mAh[c]);
        }
        return;
    }

    if (strncmp(args, "cmd", 3) == 0) {
        /* `pack cmd <idx> <name> <value>` -- values are SCALED INTEGERS in
         * the writeMin/writeMax domain, never floats, exactly as
         * /api/modbus/write takes them. */
        char        name[24];
        int         v = 0;
        ePackCmdId  id;
        sPackCommand c;
        int          n = 0;

        if (sscanf(args + 3, "%hhu %23s %d%n", &idx, name, &v, &n) < 3) {
            TRice("usage: pack cmd <idx> <chargeEnable|dischargeEnable|"
                  "balanceEnable|chargeLimit|dischargeLimit> <value>\n");
            return;
        }
        if (Pack_CmdIdFromName(name, &id) != 0) {
            TRiceS("pack: unknown command '%s'\n", name);
            return;
        }
        (void)memset(&c, 0, sizeof(c));
        c.cmd    = id;
        c.value  = v;
        c.origin = "cli";
        /* Fire and forget from the CLI: the outcome arrives as a Trice line
         * from FinishCommand, on the func task. */
        TRice("pack %u: cmd -> %d\n", (unsigned)idx,
              Pack_Command(idx, &c, 3000u, NULL, NULL));
        return;
    }

    if (strncmp(args, "config", 6) == 0) {
        if (Pack_ConfigExport(pack_cli_sink, NULL) != packErr_ok) {
            TRice("pack: export failed\n");
        }
        TRice("\n");
        return;
    }

    if (strncmp(args, "erase", 5) == 0) {
        TRice("pack: erase -> %d (board is now unprovisioned)\n",
              Pack_ConfigErase());
        return;
    }

    TRice("pack: status|list|show <n>|cells <n>|stats <n>|balance <n> [reset]|"
          "cmd <n> <name> <v>|config|erase\n");
}

static void cmd_nvdb(const char *args)
{
    if (strncmp(args, "status", 6) == 0) {
        sNvDbStatus st;

        if (NvDb_GetStatus(&st) != nvdbRes_ok) {
            TRice("nvDb: not initialised\n");
            return;
        }
        TRice("nvDb v%u, layout '%s' v%u\n", st.nvdbVer, st.layoutName,
              st.layoutVer);
        TRice("  last apply: mode %s, result %u\n",
              NvDbCfg_ModeName(st.lastApplyMode), st.lastApplyResult);
    } else if (strncmp(args, "layout", 6) == 0) {
        sNvDbLayoutInfo info;
        uint32_t        i = 0u;

        if (NvDb_GetLayout(&info) != nvdbRes_ok) {
            TRice("nvDb: not initialised\n");
            return;
        }
        TRice("layout '%s' v%u, free %u B\n", info.name, info.version,
              info.freeSpace_bytes);
        for (i = 1u; i < (uint32_t)nvdbUser_last; i++) {
            const char *name = NvDb_UserName((eNvDbUser)i);

            if (name == NULL) {
                continue;
            }
            TRice("  %s %u B\n", name, info.size_bytes[i]);
        }
        if (info.onboarding != nvdbOnboard_none) {
            TRice("  waiting: '%s' v%u (%s)\n", info.received.name,
                  info.received.version,
                  NvDbCfg_ModeName(info.received.operation));
        }
    } else if (strncmp(args, "usage", 5) == 0 ||
               strncmp(args, "wear", 4) == 0) {
        /* `usage` observes occupancy, which reads every area — nearly a
         * megabyte of SPI with two blob areas in the layout.  `wear` answers
         * from counters already in RAM. */
        bool             scan = (strncmp(args, "usage", 5) == 0);
        sNvDbMediumUsage med;
        uint32_t         i = 0u;

        if (NvDb_GetMediumUsage(&med) != nvdbRes_ok) {
            TRice("nvDb: not initialised\n");
            return;
        }
        TRice("medium %u B, allocated %u B, free %u B\n",
              med.medium_bytes, med.allocated_bytes, med.freeSpace_bytes);
        TRice("  erases: max %u, total %u, %u not yet saved\n",
              med.eraseCntMax, med.eraseCntTotal, med.eraseCntUnsaved);

        for (i = 1u; i < (uint32_t)nvdbUser_last; i++) {
            const char *name = NvDb_UserName((eNvDbUser)i);
            sNvDbUsage  u;

            if (name == NULL ||
                NvDb_GetUsage((eNvDbUser)i, scan, &u) != nvdbRes_ok ||
                u.size_bytes == 0u) {
                continue;
            }
            if (scan) {
                TRice("  %s %u/%u B used, erases max %u\n", name,
                      u.occupied_bytes, u.size_bytes, u.eraseCntMax);
            } else {
                TRice("  %s %u B, erases max %u total %u\n", name,
                      u.size_bytes, u.eraseCntMax, u.eraseCntTotal);
            }
        }
    } else if (strncmp(args, "drop", 4) == 0) {
        if (NvDb_DropSuppliedLayout() == nvdbRes_ok) {
            TRice("nvDb: staged layout dropped\n");
        } else {
            TRice("nvDb: nothing to drop, or the medium refused\n");
        }
    } else {
        TRice("Usage: nvdb status|layout|usage|wear|drop\n");
    }
}

/**
 * System monitor: task liveness, stacks, heap and CPU share.
 *
 * Usage:
 *   sysmon          — full report (summary + one line per task)
 *   sysmon tasks    — same as above
 *   sysmon heap     — heap and watchdog margin only
 *   sysmon reset    — clear peak CPU, the IWDG gap maximum and stale counts
 */
/**
 * FWU command: the confirmation deadline, from a console.
 *
 *   fwu status        — running version, confirm state, countdown
 *   fwu kick [sec]    — reload the countdown (no argument keeps the window)
 *   fwu confirm       — confirm the running image and stop the countdown
 *
 * The countdown is why this is worth having on the console at all: a board
 * whose network is the broken thing cannot be kicked over the network.
 */
static void cmd_fwu(const char *args)
{
    sFwuConfirmGuard g;
    char             buf[112];

    if (strncmp(args, "kick", 4) == 0) {
        unsigned sec = 0U;

        (void)sscanf(args + 4, "%u", &sec);
        if (FwuCtl_KickConfirm(sec) != fwuCtlRes_ok) {
            TRice("FWU: nothing is counting down\n");
            return;
        }
        FwuCtl_GetConfirmGuard(&g);
        TRice("FWU: kicked, %u s (kick %u)\n",
              (unsigned)g.window_sec, (unsigned)g.kickCnt);
        return;
    }
    if (strncmp(args, "confirm", 7) == 0) {
        bool promote = false;

        switch (FwuCtl_Confirm(&promote)) {
        case fwuCtlRes_ok:
            TRice("FWU: confirmed (promote=%u)\n", (unsigned)promote);
            break;
        case fwuCtlRes_already:
            TRice("FWU: already confirmed\n");
            break;
        default:
            TRice("FWU: could not write the boot status\n");
            break;
        }
        return;
    }

    FwuCtl_GetConfirmGuard(&g);
    (void)snprintf(buf, sizeof(buf),
                   "%s attempts=%u guard=%s%s window=%us left=%us kicks=%u",
                   BootStatus_IsUnconfirmed() ? "UNCONFIRMED" : "confirmed",
                   (unsigned)g.attemptsLeft,
                   g.armed ? "armed" : "off",
                   g.exempt ? " (local build, exempt)" : "",
                   (unsigned)g.window_sec, (unsigned)g.remaining_sec,
                   (unsigned)g.kickCnt);
    TRiceS("FWU: %s\n", buf);
}

static void cmd_sysmon(const char *args)
{
    if (strncmp(args, "reset", 5) == 0) {
        SysMon_ResetPeaks();
        TRice("SysMon: peaks cleared\n");
        return;
    }

    if (strncmp(args, "heap", 4) == 0) {
        sSysMonSummary sum;
        char           line[112];

        SysMon_GetSummary(&sum);
        snprintf(line, sizeof(line),
                 "heap %u/%u B free (min %u, used %u) | iwdg gap max %u ms of "
                 "16400", (unsigned)sum.heapFree_bytes,
                 (unsigned)sum.heapSize_bytes,
                 (unsigned)sum.heapFreeMin_bytes,
                 (unsigned)(sum.heapSize_bytes - sum.heapFree_bytes),
                 (unsigned)sum.iwdgGapMax_ms);
        TRiceS("SysMon: %s\n", line);
        return;
    }

    SysMon_Report();
}

static void cmd_reboot(const char *args)
{
    (void)args;
    TRice("Rebooting...\n");
    NVIC_SystemReset();
}

/**
 * Jump to the STM32 built-in system bootloader (USB DFU mode).
 * System memory on STM32F407 is at 0x1FFF0000.
 */
static void cmd_dfu(const char *args)
{
    (void)args;
    TRice("Entering DFU mode...\n");

    /* Disable all interrupts */
    __disable_irq();

    /* Disable SysTick */
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    /* Disable all NVIC interrupts and clear pending */
    for (uint32_t i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFF;
        NVIC->ICPR[i] = 0xFFFFFFFF;
    }

    /* Force USB OTG FS disconnect + peripheral reset */
    USB_OTG_FS->GCCFG = 0;   /* Disable VBUS sensing / power */
    __HAL_RCC_USB_OTG_FS_FORCE_RESET();
    for (volatile int i = 0; i < 10000; i++) {}
    __HAL_RCC_USB_OTG_FS_RELEASE_RESET();
    __HAL_RCC_USB_OTG_FS_CLK_DISABLE();

    /* Deinit HAL (resets clocks to HSI default) */
    HAL_RCC_DeInit();
    HAL_DeInit();

    /* Remap system flash to 0x00000000 */
    __HAL_SYSCFG_REMAPMEMORY_SYSTEMFLASH();

    /* Set MSP to system bootloader's stack pointer */
    __set_MSP(*(volatile uint32_t *)0x1FFF0000);

    /* Jump to system bootloader reset handler */
    ((void (*)(void))(*(volatile uint32_t *)0x1FFF0004))();

    /* Should never reach here */
    for (;;) {}
}

static void cmd_help(const char *args)
{
    (void)args;
    TRice("Available commands:\n");
    for (const sCmdEntry *e = s_commands; e->name != NULL; e++) {
        TRice("  ");
        TRiceS("%-16s", (char *)e->name);
        TRiceS(" %s\n", (char *)e->description);
    }
}

/* --------------------------------------------------------------------------
 * Command dispatch
 * -------------------------------------------------------------------------- */

static void dispatch(const char *line)
{
    /* Skip leading whitespace */
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (*line == '\0') {
        return;
    }

    /* Extract command name (first token) */
    const char *cmd_start = line;
    const char *p = line;
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        p++;
    }
    size_t cmd_len = (size_t)(p - cmd_start);

    /* Skip whitespace between command and arguments */
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    const char *args = p;

    /* Look up command */
    for (const sCmdEntry *e = s_commands; e->name != NULL; e++) {
        if (strlen(e->name) == cmd_len &&
            strncmp(e->name, cmd_start, cmd_len) == 0) {
            e->handler(args);
            return;
        }
    }

    TRiceS("Unknown command: %s\n", (char *)line);
}

/* --------------------------------------------------------------------------
 * Feed & process
 * -------------------------------------------------------------------------- */

void Cmd_Feed(eCmdSrc src, const uint8_t *data, size_t len)
{
    if (src >= cmdSrc_last) {
        return;
    }

    sCmdLine *line = &s_lines[src];

    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];

        if (c == '\n' || c == '\r') {
            if (!line->synced) {
                /* First newline: discard any junk from port enumeration */
                line->synced = true;
                line->pos = 0;
                continue;
            }
            if (line->pos > 0 && !line->ready) {
                line->buf[line->pos] = '\0';
                /* Hand off to cmdTask — command handlers block (vTaskDelay,
                 * osThreadNew, tcpip_callback) and Cmd_Feed runs in ISR
                 * context (USB CDC RX / UART1 RX).  The buffer stays
                 * untouched until cmdTask clears `ready`. */
                line->ready = true;
            }
        } else if (c >= ' ' && c <= '~' && line->synced &&
                   line->pos < CMD_LINE_MAX - 1 && !line->ready) {
            line->buf[line->pos++] = c;
        }
    }
}

/* --------------------------------------------------------------------------
 * UART1 RX callback
 * -------------------------------------------------------------------------- */

void Cmd_Uart1RxCallback(void)
{
    Cmd_Feed(cmdSrc_uart, &s_uart1_rx_byte, 1);
    /* Re-arm single-byte receive */
    HAL_UART_Receive_IT(&huart1, &s_uart1_rx_byte, 1);
}

/* --------------------------------------------------------------------------
 * Command task — dispatches completed lines outside ISR context
 * -------------------------------------------------------------------------- */

static void cmdTask(void *arg)
{
    (void)arg;

    /* A handler may block for a long time (flash erase, `wg` restart), so the
     * deadline is generous — this catches a wedged parser, not a slow one. */
    int8_t monId = SysMon_TaskRegister(1024U, 5000U);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        SysMon_TaskCheckin(monId);

        for (int i = 0; i < cmdSrc_last; i++) {
            sCmdLine *line = &s_lines[i];
            if (line->ready) {
                dispatch(line->buf);
                line->pos = 0;
                line->ready = false;
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Init
 * -------------------------------------------------------------------------- */

void Cmd_Init(void)
{
    memset(s_lines, 0, sizeof(s_lines));

    static const osThreadAttr_t attr = {
        .name       = "cmd",
        .stack_size = 1024U * 4U,
        .priority   = (osPriority_t)osPriorityNormal,
    };
    osThreadNew(cmdTask, NULL, &attr);

    /* Enable USART1 RX interrupt and start receiving */
    HAL_NVIC_SetPriority(USART1_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
    HAL_UART_Receive_IT(&huart1, &s_uart1_rx_byte, 1);
}
