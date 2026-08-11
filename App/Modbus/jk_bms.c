/**
 * @file    jk_bms.c
 * @brief   JK PB-series BMS presence probe — see jk_bms.h.
 */

#include "App/Modbus/jk_bms.h"
#include "App/Modbus/modbus_walker.h"

#include "trice.h"
#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * DeviceInfo block layout (byte offsets from JK_BLOCK_DEVICE_INFO).
 * Mirrors jk::info in ~/Projects/JK_BMS/src/core/JkRegisters.h.
 * -------------------------------------------------------------------------- */

#define INFO_MODEL_OFF        0x00u   /* ASCII[16] */
#define INFO_HW_VERSION_OFF   0x10u   /* ASCII[8]  */
#define INFO_SW_VERSION_OFF   0x18u   /* ASCII[8]  */
#define INFO_RUNTIME_OFF      0x20u   /* UINT32 s  */
#define INFO_PWRON_TIMES_OFF  0x24u   /* UINT32    */
#define INFO_UART1_PROTO_OFF  0xB2u   /* UINT8     */
#define INFO_CAN_PROTO_OFF    0xB3u   /* UINT8     */

/* Registers needed to cover bytes 0..0xB3.  Well under both of the BMS's read
 * ceilings (quantity < 124, and quantity + wordOffset < 147). */
#define INFO_REGS_TO_READ     90u

/* --------------------------------------------------------------------------
 * Field extraction from a big-endian-decoded register block
 * -------------------------------------------------------------------------- */

static uint8_t byte_at(const uint16_t *regs, uint16_t byteOff)
{
    uint16_t r = regs[byteOff / 2u];
    return (byteOff & 1u) ? (uint8_t)(r & 0xFFu) : (uint8_t)(r >> 8);
}

/* JK is high-word-first for 32-bit fields (firmware-confirmed, see
 * protocol-rs485-modbus.md §2.4) */
static uint32_t u32_at(const uint16_t *regs, uint16_t byteOff)
{
    uint16_t i = byteOff / 2u;
    return ((uint32_t)regs[i] << 16) | regs[i + 1u];
}

/* Copy `nBytes` of ASCII out of the register block, NUL-terminate, and trim
 * the trailing spaces/NULs the BMS pads with.  `out` must hold nBytes + 1. */
static void ascii_at(const uint16_t *regs, uint16_t byteOff, uint8_t nBytes,
                     char *out)
{
    uint8_t n = 0;

    for (uint8_t i = 0; i < nBytes; i++) {
        out[i] = (char)byte_at(regs, (uint16_t)(byteOff + i));
    }
    out[nBytes] = '\0';

    n = (uint8_t)strlen(out);
    while (n > 0u && (out[n - 1u] == ' ' || out[n - 1u] == '\xFF')) {
        out[--n] = '\0';
    }
}

/* --------------------------------------------------------------------------
 * Probe
 * -------------------------------------------------------------------------- */

eModbusErr JkBms_Probe(uint8_t slave, uint32_t baud, uint32_t timeoutMs,
                       sJkDeviceInfo *out)
{
    uint16_t   regs[INFO_REGS_TO_READ];
    eModbusErr err;

    if (out == NULL || slave < 1u || slave > 247u) {
        return mbErr_busy;
    }

    /* The walker owns USART2 while running and re-inits it at its own baud —
     * probing underneath it would corrupt an in-flight transaction. */
    if (ModbusWalker_IsRunning()) {
        return mbErr_busy;
    }

    if (Modbus_Init(baud) != 0) {
        return mbErr_busy;
    }

    err = Modbus_ReadHoldingRegisters(slave, (uint16_t)JK_BLOCK_DEVICE_INFO,
                                      INFO_REGS_TO_READ, regs, timeoutMs);

    Modbus_DeInit();

    if (err != mbErr_ok) {
        return err;
    }

    ascii_at(regs, INFO_MODEL_OFF,      16u, out->model);
    ascii_at(regs, INFO_HW_VERSION_OFF,  8u, out->hwVersion);
    ascii_at(regs, INFO_SW_VERSION_OFF,  8u, out->swVersion);
    out->runTimeS      = u32_at(regs, INFO_RUNTIME_OFF);
    out->powerOnTimes  = u32_at(regs, INFO_PWRON_TIMES_OFF);
    out->uart1Protocol = byte_at(regs, INFO_UART1_PROTO_OFF);
    out->canProtocol   = byte_at(regs, INFO_CAN_PROTO_OFF);

    return mbErr_ok;
}

/* --------------------------------------------------------------------------
 * CLI-facing report
 * -------------------------------------------------------------------------- */

static void log_failure_hint(eModbusErr err)
{
    switch (err) {
    case mbErr_timeout:
        if (Modbus_GetPort() == mbPort_disabled) {
            TRice("JK: no reply - Modbus port is DISABLED ('modbus port uart2')\n");
        } else {
            TRice("JK: no reply - check A/B polarity, baud, slave addr, RS485 mode\n");
        }
        break;
    case mbErr_exception: {
        uint8_t exc = Modbus_LastException();
        TRice("JK: slave answered, rejected request (exception %u)\n", exc);
        if (exc == 2u) {
            TRice("JK: exception 2 = Illegal Data Address - wrong block base?\n");
        } else if (exc == 1u) {
            TRice("JK: exception 1 = Illegal Function - not a JK Modbus slave?\n");
        }
        break;
    }
    case mbErr_crc:
        TRice("JK: CRC mismatch - baud mismatch or a noisy/unterminated bus\n");
        break;
    case mbErr_short:
        TRice("JK: truncated reply - RX overrun or a foreign device answered\n");
        break;
    case mbErr_busy:
        TRice("JK: bus busy - stop the walker first ('modbus stop')\n");
        break;
    default:
        break;
    }
}

eModbusErr JkBms_LogProbe(uint8_t slave, uint32_t baud)
{
    sJkDeviceInfo info;
    char          buf[110];
    eModbusErr    err;

    TRice("JK: probing slave %u at %u baud..\n", slave, baud);

    err = JkBms_Probe((uint8_t)slave, baud, 1000u, &info);
    if (err != mbErr_ok) {
        TRice("JK: probe FAILED (%d)\n", (int)err);
        log_failure_hint(err);
        return err;
    }

    snprintf(buf, sizeof(buf), "model=%s hw=%s sw=%s",
             info.model, info.hwVersion, info.swVersion);
    TRiceS("JK: FOUND %s\n", buf);

    snprintf(buf, sizeof(buf),
             "runtime=%lus powerons=%lu uart1proto=%u canproto=%u%s",
             (unsigned long)info.runTimeS, (unsigned long)info.powerOnTimes,
             info.uart1Protocol, info.canProtocol,
             (info.canProtocol == JK_CAN_PROTO_PYLONTECH) ? " (pylontech)" : "");
    TRiceS("JK: %s\n", buf);

    return mbErr_ok;
}
