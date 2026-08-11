/**
 * @file    jk_bms.h
 * @brief   JK PB-series BMS presence probe (RS485 / Modbus RTU).
 *
 * Scope is deliberately narrow: a one-shot DeviceInfo read that answers
 * "is a JK BMS actually on this bus, at this address, at this baud?".
 * Realtime data is not read here — that belongs in the uploadable register
 * config the walker already drives (docs/modbus.md §4).
 *
 * @note This file is slated for deletion: everything it does is expressible as
 *       a device type in the config, and the generic Modbus_Probe replaces the
 *       bring-up read.  docs/modbus.md §1.3, §2.16 step 12.
 *
 * Protocol source: ~/Projects/JK_BMS — `src/core/JkRegisters.h` for the map,
 * `FW/decompiled/protocol-rs485-modbus.md` for the addressing model, which was
 * read out of the BMS firmware itself.
 *
 * @note JK register addresses are BYTE-addressed: the wire address is
 *       `blockBase + byteOffset`, NOT `blockBase + byteOffset/2`.  The BMS
 *       halves the address delta itself to get a word offset.  This module
 *       reads from the block base, so its own offsets are plain word indices.
 */

#ifndef JK_BMS_H_
#define JK_BMS_H_

#include "App/Modbus/modbus_rtu.h"

#include <stdint.h>

/* --------------------------------------------------------------------------
 * Wire constants
 * -------------------------------------------------------------------------- */

#define JK_BLOCK_DEVICE_INFO   0x1400u  /* R  model / versions / protocol sel */
#define JK_DEFAULT_SLAVE       1u
#define JK_DEFAULT_BAUD        115200u

/* --------------------------------------------------------------------------
 * Probe result
 * -------------------------------------------------------------------------- */

typedef struct {
    char     model[17];      /* ManufacturerDeviceID, ASCII[16] + NUL   */
    char     hwVersion[9];   /* ASCII[8] + NUL                          */
    char     swVersion[9];   /* ASCII[8] + NUL                          */
    uint32_t runTimeS;       /* cumulative run time, seconds            */
    uint32_t powerOnTimes;   /* power-on cycle count                    */
    uint8_t  uart1Protocol;  /* RS485_1 protocol slot (0-21)            */
    uint8_t  canProtocol;    /* CAN protocol slot (0-11); 4 = Pylontech */
} sJkDeviceInfo;

/** CAN protocol slot the inverter-facing Pylontech dialect lives in. */
#define JK_CAN_PROTO_PYLONTECH   4u

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/**
 * @brief  One-shot DeviceInfo read — proves the BMS is present and talking.
 *
 *         Takes over USART2 for the duration (init at `baud`, transact,
 *         deinit), so it is refused with MODBUS_ERR_BUSY while the walker is
 *         running — stop the walker first.  Blocks for at most ~2x timeoutMs.
 *
 * @param  slave      Modbus slave address (JK default 1)
 * @param  baud       Bus baud rate (JK PB default 115200)
 * @param  timeoutMs  Response timeout
 * @param  out        Filled in on MODBUS_OK; untouched otherwise
 * @return MODBUS_OK, or an eModbusErr.  On MODBUS_ERR_EXCEPTION the slave
 *         answered but rejected the request — Modbus_LastException() says why.
 *         MODBUS_ERR_BUSY also covers a bad argument (NULL out, slave out of
 *         range); eModbusErr has no parameter-error code.
 */
eModbusErr JkBms_Probe(uint8_t slave, uint32_t baud, uint32_t timeoutMs,
                       sJkDeviceInfo *out);

/**
 * @brief  Run JkBms_Probe and report the outcome via Trice, including a
 *         diagnosis hint on failure.  For CLI/manual use.
 * @return Same as JkBms_Probe.
 */
eModbusErr JkBms_LogProbe(uint8_t slave, uint32_t baud);

#endif /* JK_BMS_H_ */
