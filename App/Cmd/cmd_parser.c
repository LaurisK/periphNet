/**
 * @file    cmd_parser.c
 * @brief   Text command parser — USB CDC + UART1 input, Trice output
 */

#include "App/Cmd/cmd_parser.h"
#include "App/Can/bms_sim.h"
#include "App/Can/bms_reader.h"
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

#define CMD_LINE_MAX  128

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

static const sCmdEntry s_commands[] = {
    { "peripherals", cmd_peripherals, "List device peripherals" },
    { "bms",         cmd_bms,         "BMS sim/reader (start|stop|read|set)" },
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
                line->ready = true;
                dispatch(line->buf);
                line->pos = 0;
                line->ready = false;
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
 * Init
 * -------------------------------------------------------------------------- */

void Cmd_Init(void)
{
    memset(s_lines, 0, sizeof(s_lines));

    /* Enable USART1 RX interrupt and start receiving */
    HAL_NVIC_SetPriority(USART1_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
    HAL_UART_Receive_IT(&huart1, &s_uart1_rx_byte, 1);
}
