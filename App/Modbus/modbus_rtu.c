/**
 * @file    modbus_rtu.c
 * @brief   Modbus RTU master on USART2 (PD5 TX, PD6 RX) with RS485 DE on PD7
 */

#include "App/Modbus/modbus_rtu.h"
#include "usart.h"
#include "main.h"
#include "stm32f4xx_hal.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Private
 * -------------------------------------------------------------------------- */

#define MODBUS_RX_BUF_SIZE  260  /* max response: 1+1+1+250+2 = 255 */

static volatile int s_initialised;

/* --------------------------------------------------------------------------
 * RS485 direction control (PD7)
 * -------------------------------------------------------------------------- */

static inline void rs485_tx_enable(void)
{
    HAL_GPIO_WritePin(gpio_rs485de_GPIO_Port, gpio_rs485de_Pin, GPIO_PIN_SET);
}

static inline void rs485_rx_enable(void)
{
    HAL_GPIO_WritePin(gpio_rs485de_GPIO_Port, gpio_rs485de_Pin, GPIO_PIN_RESET);
}

/* --------------------------------------------------------------------------
 * CRC16 Modbus (poly 0xA001, init 0xFFFF)
 * -------------------------------------------------------------------------- */

uint16_t Modbus_CRC16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/* --------------------------------------------------------------------------
 * Low-level TX/RX with DE pin and inter-frame gap
 * -------------------------------------------------------------------------- */

/**
 * @brief  Transmit a complete Modbus frame and receive the response.
 *
 * Handles RS485 direction switching: DE high for TX, then DE low for RX.
 * Blocks until response received or timeout.
 *
 * @param  txBuf     Frame to send (including CRC)
 * @param  txLen     Frame length
 * @param  rxBuf     Buffer for response
 * @param  rxMaxLen  Max response length
 * @param  rxLen     Actual received length (output)
 * @param  timeoutMs Response timeout
 * @return MODBUS_OK or error
 */
static eModbusErr modbus_transact(const uint8_t *txBuf, uint16_t txLen,
                                  uint8_t *rxBuf, uint16_t rxMaxLen,
                                  uint16_t *rxLen, uint32_t timeoutMs)
{
    if (!s_initialised) {
        return MODBUS_ERR_BUSY;
    }

    /* Inter-frame gap: 3.5 char times. At 9600 baud = ~4 ms */
    HAL_Delay(4);

    /* TX: enable driver, send frame, wait for completion */
    rs485_tx_enable();
    HAL_StatusTypeDef st = HAL_UART_Transmit(&huart2, (uint8_t *)txBuf,
                                              txLen, timeoutMs);
    /* Wait for last byte to shift out (TC flag) */
    uint32_t t0 = HAL_GetTick();
    while (!__HAL_UART_GET_FLAG(&huart2, UART_FLAG_TC)) {
        if ((HAL_GetTick() - t0) > 10) break;
    }
    rs485_rx_enable();

    if (st != HAL_OK) {
        return MODBUS_ERR_BUSY;
    }

    /* RX: receive response byte-by-byte with timeout */
    *rxLen = 0;
    t0 = HAL_GetTick();
    uint32_t lastByteTick = 0;

    while ((HAL_GetTick() - t0) < timeoutMs) {
        uint8_t byte;
        if (HAL_UART_Receive(&huart2, &byte, 1, 5) == HAL_OK) {
            if (*rxLen < rxMaxLen) {
                rxBuf[(*rxLen)++] = byte;
            }
            lastByteTick = HAL_GetTick();
        } else if (*rxLen > 0 && (HAL_GetTick() - lastByteTick) > 5) {
            /* 5 ms silence after receiving data = end of frame at 9600 */
            break;
        }
    }

    if (*rxLen == 0) {
        return MODBUS_ERR_TIMEOUT;
    }

    return MODBUS_OK;
}

/* --------------------------------------------------------------------------
 * Read registers (shared logic for FC 0x03 and FC 0x04)
 * -------------------------------------------------------------------------- */

static eModbusErr read_registers(uint8_t fc, uint8_t slave, uint16_t startReg,
                                 uint16_t count, uint16_t *out,
                                 uint32_t timeoutMs)
{
    /* Build request: [slave][fc][startHi][startLo][countHi][countLo][crcLo][crcHi] */
    uint8_t req[8];
    req[0] = slave;
    req[1] = fc;
    req[2] = (uint8_t)(startReg >> 8);
    req[3] = (uint8_t)(startReg & 0xFF);
    req[4] = (uint8_t)(count >> 8);
    req[5] = (uint8_t)(count & 0xFF);
    uint16_t crc = Modbus_CRC16(req, 6);
    req[6] = (uint8_t)(crc & 0xFF);
    req[7] = (uint8_t)(crc >> 8);

    uint8_t rxBuf[MODBUS_RX_BUF_SIZE];
    uint16_t rxLen = 0;
    eModbusErr err = modbus_transact(req, 8, rxBuf, sizeof(rxBuf),
                                     &rxLen, timeoutMs);
    if (err != MODBUS_OK) {
        return err;
    }

    /* Minimum response: slave + fc + byteCount + 2*count + crc(2) */
    uint16_t expectedLen = 3 + count * 2 + 2;
    if (rxLen < 5) {
        return MODBUS_ERR_SHORT;
    }

    /* Check for exception response */
    if (rxBuf[1] & 0x80) {
        return MODBUS_ERR_EXCEPTION;
    }

    if (rxLen < expectedLen) {
        return MODBUS_ERR_SHORT;
    }

    /* Verify CRC */
    uint16_t rxCrc = (uint16_t)(rxBuf[rxLen - 2]) |
                     ((uint16_t)(rxBuf[rxLen - 1]) << 8);
    if (rxCrc != Modbus_CRC16(rxBuf, rxLen - 2)) {
        return MODBUS_ERR_CRC;
    }

    /* Extract register values (big-endian in response) */
    uint8_t byteCount = rxBuf[2];
    if (byteCount != count * 2) {
        return MODBUS_ERR_SHORT;
    }

    for (uint16_t i = 0; i < count; i++) {
        out[i] = ((uint16_t)rxBuf[3 + i * 2] << 8) |
                  (uint16_t)rxBuf[4 + i * 2];
    }

    return MODBUS_OK;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int Modbus_Init(uint32_t baud)
{
    /* Reconfigure USART2 to the requested baud rate */
    HAL_UART_DeInit(&huart2);

    huart2.Instance          = USART2;
    huart2.Init.BaudRate     = baud;
    huart2.Init.WordLength   = UART_WORDLENGTH_8B;
    huart2.Init.StopBits     = UART_STOPBITS_1;
    huart2.Init.Parity       = UART_PARITY_NONE;
    huart2.Init.Mode         = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart2) != HAL_OK) {
        return -1;
    }

    rs485_rx_enable();
    s_initialised = 1;

    return 0;
}

void Modbus_DeInit(void)
{
    s_initialised = 0;
    rs485_rx_enable();
    HAL_UART_DeInit(&huart2);
}

eModbusErr Modbus_ReadInputRegisters(uint8_t slave, uint16_t startReg,
                                     uint16_t count, uint16_t *out,
                                     uint32_t timeoutMs)
{
    return read_registers(0x04, slave, startReg, count, out, timeoutMs);
}

eModbusErr Modbus_ReadHoldingRegisters(uint8_t slave, uint16_t startReg,
                                       uint16_t count, uint16_t *out,
                                       uint32_t timeoutMs)
{
    return read_registers(0x03, slave, startReg, count, out, timeoutMs);
}

eModbusErr Modbus_WriteSingleRegister(uint8_t slave, uint16_t reg,
                                      uint16_t value, uint32_t timeoutMs)
{
    /* Build request: [slave][0x06][regHi][regLo][valHi][valLo][crcLo][crcHi] */
    uint8_t req[8];
    req[0] = slave;
    req[1] = 0x06;
    req[2] = (uint8_t)(reg >> 8);
    req[3] = (uint8_t)(reg & 0xFF);
    req[4] = (uint8_t)(value >> 8);
    req[5] = (uint8_t)(value & 0xFF);
    uint16_t crc = Modbus_CRC16(req, 6);
    req[6] = (uint8_t)(crc & 0xFF);
    req[7] = (uint8_t)(crc >> 8);

    uint8_t rxBuf[16];
    uint16_t rxLen = 0;
    eModbusErr err = modbus_transact(req, 8, rxBuf, sizeof(rxBuf),
                                     &rxLen, timeoutMs);
    if (err != MODBUS_OK) {
        return err;
    }

    if (rxLen < 5) {
        return MODBUS_ERR_SHORT;
    }

    /* Exception check */
    if (rxBuf[1] & 0x80) {
        return MODBUS_ERR_EXCEPTION;
    }

    /* Echo response should match request (slave + fc + reg + value + crc) */
    if (rxLen < 8) {
        return MODBUS_ERR_SHORT;
    }

    uint16_t rxCrc = (uint16_t)(rxBuf[rxLen - 2]) |
                     ((uint16_t)(rxBuf[rxLen - 1]) << 8);
    if (rxCrc != Modbus_CRC16(rxBuf, rxLen - 2)) {
        return MODBUS_ERR_CRC;
    }

    return MODBUS_OK;
}
