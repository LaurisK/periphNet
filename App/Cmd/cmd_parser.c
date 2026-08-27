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
#include "nvdb.h"
#include "nvdb_config.h"
#include "nvdb_layout.h"
#include "cmsis_os.h"
#include "trice.h"
#include "usart.h"
#include "stm32f4xx_hal.h"
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
static void cmd_modbus(const char *args);
static void cmd_mqtt(const char *args);
static void cmd_wg(const char *args);
static void cmd_nvdb(const char *args);
static void cmd_pack(const char *args);
static void cmd_sysmon(const char *args);

static const sCmdEntry s_commands[] = {
    { "peripherals", cmd_peripherals, "List device peripherals" },
    { "bms",         cmd_bms,         "BMS sim/reader (start|stop|read|set)" },
    { "modbus",      cmd_modbus,      "Modbus (read|get|set|monitor|dump|plan|inject|status)" },
    { "mqtt",        cmd_mqtt,        "MQTT bridge (start|stop|save|forget|monitor|status)"  },
    { "wg",          cmd_wg,          "WireGuard tunnel (start|stop|status|endpoint)" },
    { "nvdb",        cmd_nvdb,        "Non-volatile store (status|layout|usage|wear)" },
    { "pack",        cmd_pack,        "Battery packs (status|list|show|cells|cmd|config|erase)" },
    { "sysmon",      cmd_sysmon,      "System monitor (tasks|heap|reset)" },
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
                char tb[100];
                int  at = snprintf(tb, sizeof(tb), "%us:", 
                                   (unsigned)tables[t].period_sec);
                for (uint16_t k = 0; k < tables[t].count &&
                                     at < (int)sizeof(tb) - 8; k++) {
                    at += snprintf(tb + at, sizeof(tb) - (size_t)at, " %u",
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
        char hex[80];
        int  at = 0;
        for (uint16_t i = 0; i < n && at < (int)sizeof(hex) - 3; i++) {
            at += snprintf(hex + at, sizeof(hex) - (size_t)at, "%02x",
                           frame[i]);
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
        TRice("  balance: active=%u %d mA duty=%u src=%u sink=%u\n",
              (unsigned)cl.balanceActive, (int)cl.balanceCurrent_mA,
              (unsigned)cl.balanceDuty_pm, (unsigned)cl.balanceSrcIdx,
              (unsigned)cl.balanceSinkIdx);
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

    TRice("pack: status|list|show <n>|cells <n>|cmd <n> <name> <v>|config|erase\n");
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
