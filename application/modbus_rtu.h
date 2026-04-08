#ifndef MODBUS_RTU_H
#define MODBUS_RTU_H

#include <stdint.h>
#include <stdbool.h>

/* Modbus function codes */
#define MODBUS_FC_READ_HOLDING_REGS   0x03
#define MODBUS_FC_READ_INPUT_REGS     0x04

/* Modbus RTU frame limits */
#define MODBUS_RTU_MAX_ADU     256
#define MODBUS_RTU_MIN_ADU     4     /* addr + fc + crc16 */

/* Error codes */
#define MODBUS_OK              0
#define MODBUS_ERR_TIMEOUT     -1
#define MODBUS_ERR_CRC         -2
#define MODBUS_ERR_EXCEPTION   -3
#define MODBUS_ERR_SHORT       -4
#define MODBUS_ERR_BUSY        -5
#define MODBUS_ERR_DISABLED    -6

/* Port selection */
typedef enum {
    MODBUS_PORT_NONE  = 0,   /* disabled — all transactions return MODBUS_ERR_DISABLED */
    MODBUS_PORT_UART2 = 2,
    MODBUS_PORT_UART4 = 4,
} eModbusPort;

/* Modbus exception codes (from slave response) */
#define MODBUS_EX_ILLEGAL_FUNC     0x01
#define MODBUS_EX_ILLEGAL_ADDR     0x02
#define MODBUS_EX_ILLEGAL_VALUE    0x03
#define MODBUS_EX_SLAVE_FAILURE    0x04

typedef struct {
    uint8_t  slave_addr;
    uint8_t  function_code;
    uint8_t  exception_code;  /* non-zero if slave returned exception */
    uint16_t reg_count;
    uint16_t regs[125];       /* max 125 registers per read */
} sModbusResponse;

/**
 * Initialize the Modbus RTU master on the selected UART port.
 * Pass MODBUS_PORT_NONE to disable hardware (all reads return MODBUS_ERR_DISABLED).
 */
void modbus_rtu_init(eModbusPort port);

/**
 * Read holding registers (FC 0x03) from a Modbus slave.
 *
 * @param slave_addr   Slave address (1-247)
 * @param start_reg    Starting register address
 * @param count        Number of registers to read (1-125)
 * @param resp         Output: parsed response
 * @param timeout_ms   Timeout in milliseconds
 * @return MODBUS_OK on success, negative error code on failure
 */
int modbus_read_holding_regs(uint8_t slave_addr, uint16_t start_reg,
                             uint16_t count, sModbusResponse *resp,
                             uint32_t timeout_ms);

/**
 * Read input registers (FC 0x04) from a Modbus slave.
 * Same parameters as modbus_read_holding_regs.
 */
int modbus_read_input_regs(uint8_t slave_addr, uint16_t start_reg,
                           uint16_t count, sModbusResponse *resp,
                           uint32_t timeout_ms);

/**
 * Calculate CRC16 for Modbus RTU.
 */
uint16_t modbus_crc16(const uint8_t *data, uint16_t len);

#endif /* MODBUS_RTU_H */
