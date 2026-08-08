/**
 * @file    modbus_rtu.h
 * @brief   Modbus RTU master — USART2 with RS485 DE pin (PD7)
 *
 * Minimal Modbus RTU master supporting:
 *  - FC 0x03  Read Holding Registers
 *  - FC 0x04  Read Input Registers
 *  - FC 0x06  Write Single Register
 */

#ifndef MODBUS_RTU_H_
#define MODBUS_RTU_H_

#include <stdint.h>
#include <stddef.h>

/* --------------------------------------------------------------------------
 * Error codes
 * -------------------------------------------------------------------------- */

typedef enum {
    MODBUS_OK            =  0,
    MODBUS_ERR_TIMEOUT   = -1,   /* no response within deadline */
    MODBUS_ERR_CRC       = -2,   /* CRC mismatch in response */
    MODBUS_ERR_EXCEPTION = -3,   /* slave returned exception */
    MODBUS_ERR_SHORT     = -4,   /* response too short */
    MODBUS_ERR_BUSY      = -5,   /* UART busy / not initialised */
} eModbusErr;

/* --------------------------------------------------------------------------
 * Port selection (integration-test support)
 * -------------------------------------------------------------------------- */

typedef enum {
    MODBUS_PORT_UART2    = 0,  /* production RS485 on PD5/PD6/PD7 */
    MODBUS_PORT_UART6    = 1,  /* test port — deferred until schematic review */
    MODBUS_PORT_DISABLED = 2,  /* no physical bus; inject-only mode */
} eModbusPort;

/**
 * @brief  Select the active Modbus port.  Refused while the poller has the
 *         UART initialised (stop the poller first).  MODBUS_PORT_UART6 is
 *         not wired up yet and always returns -1.
 * @return 0 on success, -1 on failure (refusal is logged via Trice)
 */
int Modbus_SetPort(eModbusPort port);

eModbusPort Modbus_GetPort(void);

/**
 * @brief  Modbus exception code carried by the most recent response that
 *         returned MODBUS_ERR_EXCEPTION (0 = none seen yet).
 *
 *         Standard codes: 1 = Illegal Function, 2 = Illegal Data Address,
 *         3 = Illegal Data Value, 4 = Slave Device Failure.  Telling these
 *         apart is the whole diagnosis when bringing up an unfamiliar slave
 *         ("wrong function code" vs "wrong address" vs "wrong value"), so the
 *         code is kept here rather than folded into the flat eModbusErr.
 *
 *         Valid only immediately after a MODBUS_ERR_EXCEPTION return; it is
 *         reset at the start of every transaction.
 */
uint8_t Modbus_LastException(void);

/**
 * @brief  Enable/disable raw TX/RX frame monitoring — logs
 *         "Modbus TX[N]: .." / "Modbus RX[N]: ..".  Off by default.
 */
void Modbus_SetMonitor(int enable);
int  Modbus_GetMonitor(void);

/**
 * @brief  Validate and decode a raw Modbus response frame submitted via the
 *         command interface (synchronous — no bus, no poller involvement).
 *
 *         Checks length and CRC, logs the contract strings
 *         "Modbus inject: N bytes" / "Modbus inject: ERR_CRC" /
 *         "Modbus inject: ERR_SHORT", and decodes FC 0x03/0x04 register
 *         payloads.  The slave address is NOT validated here (slave
 *         validation point is still an open design decision).
 *
 * @param  frame     Complete frame including trailing CRC
 * @param  len       Frame length in bytes
 * @param  regs      Output buffer for decoded register values
 * @param  maxRegs   Capacity of regs
 * @param  regCount  Number of decoded registers (output)
 * @return MODBUS_OK or error code
 */
eModbusErr Modbus_ProcessInjectedFrame(const uint8_t *frame, uint16_t len,
                                       uint16_t *regs, uint16_t maxRegs,
                                       uint16_t *regCount);

/* --------------------------------------------------------------------------
 * Init / deinit
 * -------------------------------------------------------------------------- */

/**
 * @brief  Reconfigure USART2 for Modbus RTU at the given baud rate.
 *         Sets RS485 DE pin (PD7) LOW (receive mode).
 * @param  baud  Baud rate (typically 9600)
 * @return 0 on success, -1 on HAL error
 */
int Modbus_Init(uint32_t baud);

/**
 * @brief  Stop USART2, release resources.
 */
void Modbus_DeInit(void);

/* --------------------------------------------------------------------------
 * Register read/write
 * -------------------------------------------------------------------------- */

/**
 * @brief  Read input registers (FC 0x04).
 * @param  slave     Slave address (1-247)
 * @param  startReg  First register (wire address, 0-based)
 * @param  count     Number of registers to read (1-125)
 * @param  out       Output buffer (count × uint16_t, big-endian decoded)
 * @param  timeoutMs Response timeout in ms
 * @return MODBUS_OK or error code
 */
eModbusErr Modbus_ReadInputRegisters(uint8_t slave, uint16_t startReg,
                                     uint16_t count, uint16_t *out,
                                     uint32_t timeoutMs);

/**
 * @brief  Read holding registers (FC 0x03).
 * @param  slave     Slave address (1-247)
 * @param  startReg  First register (wire address, 0-based)
 * @param  count     Number of registers to read (1-125)
 * @param  out       Output buffer (count × uint16_t, big-endian decoded)
 * @param  timeoutMs Response timeout in ms
 * @return MODBUS_OK or error code
 */
eModbusErr Modbus_ReadHoldingRegisters(uint8_t slave, uint16_t startReg,
                                       uint16_t count, uint16_t *out,
                                       uint32_t timeoutMs);

/**
 * @brief  Write a single holding register (FC 0x06).
 * @param  slave     Slave address (1-247)
 * @param  reg       Register address (wire address, 0-based)
 * @param  value     Value to write
 * @param  timeoutMs Response timeout in ms
 * @return MODBUS_OK or error code
 */
eModbusErr Modbus_WriteSingleRegister(uint8_t slave, uint16_t reg,
                                      uint16_t value, uint32_t timeoutMs);

/**
 * @brief  CRC16/Modbus (poly 0xA001, init 0xFFFF).
 */
uint16_t Modbus_CRC16(const uint8_t *data, size_t len);

#endif /* MODBUS_RTU_H_ */
