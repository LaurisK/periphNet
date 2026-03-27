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
