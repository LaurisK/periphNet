/**
 * @file    modbus_rtu.c
 * @brief   Modbus RTU master on USART2 (PD5 TX, PD6 RX) with RS485 DE on PD7
 */

#include "App/Modbus/modbus_rtu.h"
#include "usart.h"
#include "main.h"
#include "stm32f4xx_hal.h"
#include "trice.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Private
 * -------------------------------------------------------------------------- */

#define MODBUS_RX_BUF_SIZE  260  /* max response: 1+1+1+250+2 = 255 */

static volatile int     s_initialised;
static eModbusPort      s_port = mbPort_uart2;
static volatile int     s_monitorEnabled;
static volatile uint8_t s_lastException;   /* see Modbus_LastException() */

/* --------------------------------------------------------------------------
 * Frame monitoring — chunked hex dump; each record stays small and records
 * without '\n' are joined into one output line by the trice tool
 * -------------------------------------------------------------------------- */

static void monitor_dump_bytes(const uint8_t *buf, uint16_t len)
{
    uint16_t i = 0;
    while ((uint16_t)(len - i) >= 8U) {
        const uint8_t *c = &buf[i];
        uint8_t b0 = c[0], b1 = c[1], b2 = c[2], b3 = c[3];
        uint8_t b4 = c[4], b5 = c[5], b6 = c[6], b7 = c[7];
        TRice8("%02x %02x %02x %02x %02x %02x %02x %02x ",
               b0, b1, b2, b3, b4, b5, b6, b7);
        i += 8U;
    }
    while (i < len) {
        uint8_t b0 = buf[i];
        TRice8("%02x ", b0);
        i++;
    }
    TRice("\n");
}

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
 * @return mbErr_ok or error
 */
static eModbusErr modbus_transact(const uint8_t *txBuf, uint16_t txLen,
                                  uint8_t *rxBuf, uint16_t rxMaxLen,
                                  uint16_t *rxLen, uint32_t timeoutMs)
{
    s_lastException = 0;

    if (!s_initialised) {
        return mbErr_busy;
    }

    /* Disabled port: no physical bus.  Show the request if monitoring is on
     * and report an instant timeout (as if no slave answered). */
    if (s_port == mbPort_disabled) {
        if (s_monitorEnabled) {
            TRice("Modbus TX[%u]: ", txLen);
            monitor_dump_bytes(txBuf, txLen);
        }
        *rxLen = 0;
        return mbErr_timeout;
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
        return mbErr_busy;
    }

    if (s_monitorEnabled) {
        TRice("Modbus TX[%u]: ", txLen);
        monitor_dump_bytes(txBuf, txLen);
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
        return mbErr_timeout;
    }

    if (s_monitorEnabled) {
        TRice("Modbus RX[%u]: ", *rxLen);
        monitor_dump_bytes(rxBuf, *rxLen);
    }

    return mbErr_ok;
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
    if (err != mbErr_ok) {
        return err;
    }

    /* Minimum response: slave + fc + byteCount + 2*count + crc(2) */
    uint16_t expectedLen = 3 + count * 2 + 2;
    if (rxLen < 5) {
        return mbErr_short;
    }

    /* Check for exception response: [slave][fc|0x80][excCode][crcLo][crcHi] */
    if (rxBuf[1] & 0x80) {
        s_lastException = rxBuf[2];
        return mbErr_exception;
    }

    if (rxLen < expectedLen) {
        return mbErr_short;
    }

    /* Verify CRC */
    uint16_t rxCrc = (uint16_t)(rxBuf[rxLen - 2]) |
                     ((uint16_t)(rxBuf[rxLen - 1]) << 8);
    if (rxCrc != Modbus_CRC16(rxBuf, rxLen - 2)) {
        return mbErr_crc;
    }

    /* Extract register values (big-endian in response) */
    uint8_t byteCount = rxBuf[2];
    if (byteCount != count * 2) {
        return mbErr_short;
    }

    for (uint16_t i = 0; i < count; i++) {
        out[i] = ((uint16_t)rxBuf[3 + i * 2] << 8) |
                  (uint16_t)rxBuf[4 + i * 2];
    }

    return mbErr_ok;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int Modbus_Init(uint32_t baud)
{
    /* Disabled port: no UART to configure — transactions time out instantly */
    if (s_port == mbPort_disabled) {
        s_initialised = 1;
        return 0;
    }

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
    if (s_port == mbPort_disabled) {
        return;
    }
    rs485_rx_enable();
    HAL_UART_DeInit(&huart2);
}

/* --------------------------------------------------------------------------
 * Port selection / monitoring / injection (integration-test support)
 * -------------------------------------------------------------------------- */

int Modbus_SetPort(eModbusPort port)
{
    if (port == mbPort_uart6) {
        TRice("Modbus port: UART6 not configured\n");
        return -1;
    }
    if (port != mbPort_uart2 && port != mbPort_disabled) {
        return -1;
    }
    /* No live switching: the poller owns the UART while initialised */
    if (s_initialised) {
        TRice("Modbus port: stop poller first\n");
        return -1;
    }
    s_port = port;
    return 0;
}

eModbusPort Modbus_GetPort(void)
{
    return s_port;
}

uint8_t Modbus_LastException(void)
{
    return s_lastException;
}

void Modbus_SetMonitor(int enable)
{
    s_monitorEnabled = enable ? 1 : 0;
}

int Modbus_GetMonitor(void)
{
    return s_monitorEnabled;
}

eModbusErr Modbus_ProcessInjectedFrame(const uint8_t *frame, uint16_t len,
                                       uint16_t *regs, uint16_t maxRegs,
                                       uint16_t *regCount)
{
    *regCount = 0;

    if (len < 4) {
        TRice("Modbus inject: ERR_SHORT\n");
        return mbErr_short;
    }

    uint16_t rxCrc = (uint16_t)frame[len - 2] |
                     ((uint16_t)frame[len - 1] << 8);
    if (rxCrc != Modbus_CRC16(frame, (size_t)len - 2)) {
        TRice("Modbus inject: ERR_CRC\n");
        return mbErr_crc;
    }

    if (s_monitorEnabled) {
        TRice("Modbus RX[%u]: ", len);
        monitor_dump_bytes(frame, len);
    }

    if (frame[1] & 0x80) {
        s_lastException = frame[2];
        TRice("Modbus inject: ERR_EXCEPTION\n");
        return mbErr_exception;
    }

    /* Only register-read responses (FC 0x03/0x04) carry data to decode */
    if (frame[1] != 0x03 && frame[1] != 0x04) {
        TRice("Modbus inject: ERR_SHORT\n");
        return mbErr_short;
    }

    uint8_t byteCount = frame[2];
    if ((uint16_t)(byteCount + 5) != len || (byteCount & 1) ||
        (uint16_t)(byteCount / 2) > maxRegs) {
        TRice("Modbus inject: ERR_SHORT\n");
        return mbErr_short;
    }

    uint16_t count = byteCount / 2;
    for (uint16_t i = 0; i < count; i++) {
        regs[i] = ((uint16_t)frame[3 + i * 2] << 8) |
                   (uint16_t)frame[4 + i * 2];
    }
    *regCount = count;

    TRice("Modbus inject: %u bytes\n", len);
    return mbErr_ok;
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
    if (err != mbErr_ok) {
        return err;
    }

    if (rxLen < 5) {
        return mbErr_short;
    }

    /* Exception check */
    if (rxBuf[1] & 0x80) {
        s_lastException = rxBuf[2];
        return mbErr_exception;
    }

    /* Echo response should match request (slave + fc + reg + value + crc) */
    if (rxLen < 8) {
        return mbErr_short;
    }

    uint16_t rxCrc = (uint16_t)(rxBuf[rxLen - 2]) |
                     ((uint16_t)(rxBuf[rxLen - 1]) << 8);
    if (rxCrc != Modbus_CRC16(rxBuf, rxLen - 2)) {
        return mbErr_crc;
    }

    return mbErr_ok;
}
