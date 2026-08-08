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
#include "App/Modbus/modbus_rtu.h"
#include "App/Modbus/modbus_walker.h"
#include "App/Mqtt/mqtt_bridge.h"
#include "App/Net/wg_link.h"
#include "App/Net/wg_platform.h"
#include "App/Net/wg_time.h"
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

static sCmdLine s_lines[CMD_SRC_COUNT];

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

static const sCmdEntry s_commands[] = {
    { "peripherals", cmd_peripherals, "List device peripherals" },
    { "bms",         cmd_bms,         "BMS sim/reader (start|stop|read|set)" },
    { "modbus",      cmd_modbus,      "Modbus RTU (start|stop|read|set|port|monitor|inject|status)" },
    { "mqtt",        cmd_mqtt,        "MQTT bridge (start|stop|monitor|inject|publish|status)"  },
    { "wg",          cmd_wg,          "WireGuard tunnel (start|stop|status|endpoint)" },
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

/**
 * Modbus command: control the generic Modbus RTU config walker.
 *
 * Usage:
 *   modbus start [baud]          — Start the walker (default 9600 baud;
 *                                  devices/registers come from the config)
 *   modbus stop                  — Stop the walker
 *   modbus read                  — Log walker/config status
 *   modbus set baud <rate>       — Change baud rate (restart to apply)
 *   modbus write <slave> <reg> <value> — Write a holding register
 *   modbus port <uart2|uart6|disabled> — Select port (stop walker first)
 *   modbus monitor <on|off>      — Trice raw TX/RX frame monitoring
 *   modbus inject <startAddr> <hexbytes> — Feed a response frame to the
 *                                  {frame slave, startAddr} config transaction
 *   modbus status                — Show walker state
 */
static void cmd_modbus(const char *args)
{
    if (strncmp(args, "start", 5) == 0) {
        if (ModbusWalker_IsRunning()) {
            TRice("Modbus already running\n");
            return;
        }
        sModbusWalkerCfg cfg = {
            .baud              = 9600,
            .responseTimeoutMs = 1000,
        };
        /* Parse optional: modbus start [baud] */
        const char *p = args + 5;
        unsigned b = 0;
        if (sscanf(p, " %u", &b) == 1 && b > 0) {
            cfg.baud = b;
        }
        ModbusWalker_Start(&cfg);
    } else if (strncmp(args, "stop", 4) == 0) {
        ModbusWalker_Stop();
    } else if (strncmp(args, "read", 4) == 0) {
        ModbusWalker_LogStatus();
    } else if (strncmp(args, "set baud ", 9) == 0) {
        unsigned b = 0;
        if (sscanf(args + 9, "%u", &b) == 1 && b > 0) {
            ModbusWalker_SetBaud(b);
            TRice("Modbus baud set to %u (restart to apply)\n", b);
        } else {
            TRice("Usage: modbus set baud <rate>\n");
        }
    } else if (strncmp(args, "write ", 6) == 0) {
        unsigned slave = 0, reg = 0, val = 0;
        if (sscanf(args + 6, "%u %u %u", &slave, &reg, &val) == 3 &&
            slave >= 1 && slave <= 247) {
            if (ModbusWalker_WriteRegister((uint8_t)slave, (uint16_t)reg,
                                           (uint16_t)val) == 0) {
                TRice("Modbus write queued: reg %u = %u\n", reg, val);
            } else {
                TRice("Modbus write failed (not running or queue full)\n");
            }
        } else {
            TRice("Usage: modbus write <slave> <register> <value>\n");
        }
    } else if (strncmp(args, "port ", 5) == 0) {
        const char *p = args + 5;
        if (strncmp(p, "uart2", 5) == 0) {
            if (Modbus_SetPort(MODBUS_PORT_UART2) == 0) {
                TRice("Modbus port: uart2\n");
            }
        } else if (strncmp(p, "uart6", 5) == 0) {
            Modbus_SetPort(MODBUS_PORT_UART6);  /* logs its own refusal */
        } else if (strncmp(p, "disabled", 8) == 0) {
            if (Modbus_SetPort(MODBUS_PORT_DISABLED) == 0) {
                TRice("Modbus port: disabled\n");
            }
        } else {
            TRice("Usage: modbus port uart2|uart6|disabled\n");
        }
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
    } else if (strncmp(args, "inject ", 7) == 0) {
        /* modbus inject <startAddr> <hexbytes> */
        unsigned startAddr = 0;
        int      consumed = 0;
        if (sscanf(args + 7, "%u %n", &startAddr, &consumed) != 1 ||
            startAddr > 65535 || consumed == 0) {
            TRice("Usage: modbus inject <startAddr> <hexbytes>\n");
            return;
        }
        uint8_t frame[128];
        int len = parse_hex_bytes(args + 7 + consumed, frame, sizeof(frame));
        if (len <= 0) {
            TRice("Usage: modbus inject <startAddr> <hexbytes>\n");
            return;
        }
        uint16_t regs[64];
        uint16_t regCount = 0;
        if (Modbus_ProcessInjectedFrame(frame, (uint16_t)len, regs,
                                        64, &regCount) == MODBUS_OK &&
            regCount > 0) {
            ModbusWalker_InjectResponse(frame[0], (uint16_t)startAddr,
                                        regs, regCount);
        }
    } else if (strncmp(args, "status", 6) == 0) {
        eModbusPort port = Modbus_GetPort();
        char buf[100];
        snprintf(buf, sizeof(buf), "%s port=%s monitor=%s baud=%u",
                 ModbusWalker_IsRunning() ? "running" : "stopped",
                 (port == MODBUS_PORT_UART2)    ? "uart2" :
                 (port == MODBUS_PORT_UART6)    ? "uart6" : "disabled",
                 Modbus_GetMonitor() ? "on" : "off",
                 (unsigned)ModbusWalker_GetBaud());
        TRiceS("Modbus %s\n", buf);
        ModbusWalker_LogStatus();
    } else {
        TRice("Usage: modbus start|stop|read|set|write|port|monitor|inject|status\n");
    }
}

/**
 * MQTT command: control MQTT bridge to Home Assistant.
 *
 * Usage:
 *   mqtt start [ip] [port]   — Start MQTT bridge (default 10.42.0.1:1883)
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
        TRice("Usage: mqtt start|stop|status|set|monitor|inject|publish\n");
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
 *   wg save                   — Persist the active config across reboot/OTA
 *   wg reset                  — Drop the persisted config, back to defaults
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
              cfg->endpointPort, cfg->keepAlive);
        TRice("WG: rng hw_seeded=%u failures=%u\n",
              (unsigned)hwSeeded, rngFailures);
        TRice("WG: time now=%us persisted=%us flash=%u\n",
              now, persisted, (unsigned)flashOk);
    } else {
        TRice("Usage: wg start|stop|status|endpoint|ip|save|reset\n");
    }
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
    if (src >= CMD_SRC_COUNT) {
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
    Cmd_Feed(CMD_SRC_UART, &s_uart1_rx_byte, 1);
    /* Re-arm single-byte receive */
    HAL_UART_Receive_IT(&huart1, &s_uart1_rx_byte, 1);
}

/* --------------------------------------------------------------------------
 * Command task — dispatches completed lines outside ISR context
 * -------------------------------------------------------------------------- */

static void cmdTask(void *arg)
{
    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));

        for (int i = 0; i < CMD_SRC_COUNT; i++) {
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
