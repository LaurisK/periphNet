#include "modbus_rtu.h"
#include "main.h"
#include "usart.h"
#include "trice.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <string.h>

/* RS485 direction control */
#define RS485_TX()  HAL_GPIO_WritePin(gpio_rs485de_GPIO_Port, gpio_rs485de_Pin, GPIO_PIN_SET)
#define RS485_RX()  HAL_GPIO_WritePin(gpio_rs485de_GPIO_Port, gpio_rs485de_Pin, GPIO_PIN_RESET)

static SemaphoreHandle_t s_bus_mutex;
static eModbusPort       s_port;

/* RX buffer and state — filled by USARTx RXNE interrupt */
static volatile uint8_t  s_rx_buf[MODBUS_RTU_MAX_ADU];
static volatile uint16_t s_rx_len;
static volatile bool     s_rx_done;
static volatile uint32_t s_rx_last_byte_tick;

/* Inter-character timeout in ticks (3.5 char times at 9600 = ~4ms, use 5ms) */
#define T35_TICKS  pdMS_TO_TICKS(5)

static UART_HandleTypeDef *modbus_get_uart(void)
{
    switch (s_port) {
        case MODBUS_PORT_UART2: return &huart2;
        /* case MODBUS_PORT_UART4: return &huart4; */
        default: return NULL;
    }
}

/* ======================================================================== */

uint16_t modbus_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }
    return crc;
}

/* ======================================================================== */

void modbus_rtu_init(eModbusPort port)
{
    s_port = port;
    s_bus_mutex = xSemaphoreCreateMutex();
    s_rx_len = 0;
    s_rx_done = false;

    if (port == MODBUS_PORT_NONE) {
        TRice("MB: port disabled\n");
        return;
    }

    UART_HandleTypeDef *uart = modbus_get_uart();
    if (uart == NULL) {
        TRice("MB: invalid port %d, disabling\n", (int)port);
        s_port = MODBUS_PORT_NONE;
        return;
    }

    RS485_RX();

    /* Reconfigure for Modbus RTU: 9600 8N1 (Solis default) */
    uart->Init.BaudRate = 9600;
    HAL_UART_Init(uart);

    /* Enable RXNE interrupt */
    IRQn_Type irqn = (port == MODBUS_PORT_UART2) ? USART2_IRQn : UART4_IRQn;
    HAL_NVIC_SetPriority(irqn, 6, 0);
    HAL_NVIC_EnableIRQ(irqn);
    __HAL_UART_ENABLE_IT(uart, UART_IT_RXNE);

    TRice("MB: init done on UART%d, 9600 8N1\n", (int)port);
}

/* USART RX ISR — called from stm32f4xx_it.c for the active UART */
void USART2_RxISR(void)
{
    UART_HandleTypeDef *uart = modbus_get_uart();
    if (uart == NULL) return;

    if (__HAL_UART_GET_FLAG(uart, UART_FLAG_RXNE)) {
        uint8_t byte = (uint8_t)(uart->Instance->DR & 0xFF);
        if (s_rx_len < MODBUS_RTU_MAX_ADU && !s_rx_done) {
            s_rx_buf[s_rx_len++] = byte;
            s_rx_last_byte_tick = xTaskGetTickCountFromISR();
        }
    }
    if (__HAL_UART_GET_FLAG(uart, UART_FLAG_ORE)) {
        __HAL_UART_CLEAR_OREFLAG(uart);
    }
}

/* ======================================================================== */

static int modbus_transact(const uint8_t *tx_frame, uint16_t tx_len,
                           sModbusResponse *resp, uint32_t timeout_ms)
{
    if (s_port == MODBUS_PORT_NONE)
        return MODBUS_ERR_DISABLED;

    UART_HandleTypeDef *uart = modbus_get_uart();
    if (uart == NULL)
        return MODBUS_ERR_DISABLED;

    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return MODBUS_ERR_BUSY;

    /* Prepare RX */
    s_rx_len = 0;
    s_rx_done = false;
    RS485_RX();

    /* Transmit */
    RS485_TX();
    /* Small delay for DE settling */
    for (volatile int i = 0; i < 100; i++) {}
    HAL_UART_Transmit(uart, (uint8_t *)tx_frame, tx_len, 100);
    /* Wait for TX complete */
    while (__HAL_UART_GET_FLAG(uart, UART_FLAG_TC) == RESET) {}
    RS485_RX();

    TRice("MB TX: addr=%d fc=%d len=%d\n", tx_frame[0], tx_frame[1], tx_len);

    /* Wait for response with T3.5 silence detection */
    TickType_t start = xTaskGetTickCount();
    TickType_t deadline = start + pdMS_TO_TICKS(timeout_ms);
    s_rx_last_byte_tick = xTaskGetTickCount();

    while (xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(2));

        if (s_rx_len > 0) {
            TickType_t now = xTaskGetTickCount();
            if ((now - s_rx_last_byte_tick) >= T35_TICKS) {
                s_rx_done = true;
                break;
            }
        }
    }

    if (s_rx_len == 0) {
        TRice("MB RX: timeout (no response)\n");
        xSemaphoreGive(s_bus_mutex);
        return MODBUS_ERR_TIMEOUT;
    }

    uint16_t rx_len = s_rx_len;
    uint8_t rx_copy[MODBUS_RTU_MAX_ADU];
    memcpy(rx_copy, (const void *)s_rx_buf, rx_len);

    TRice("MB RX: len=%d [%02X %02X %02X ...]\n",
          rx_len, rx_copy[0], rx_copy[1], rx_len > 2 ? rx_copy[2] : 0);

    xSemaphoreGive(s_bus_mutex);

    /* Validate */
    if (rx_len < MODBUS_RTU_MIN_ADU)
        return MODBUS_ERR_SHORT;

    uint16_t crc_calc = modbus_crc16(rx_copy, rx_len - 2);
    uint16_t crc_recv = (uint16_t)(rx_copy[rx_len - 2]) |
                        ((uint16_t)(rx_copy[rx_len - 1]) << 8);
    if (crc_calc != crc_recv) {
        TRice("MB RX: CRC mismatch calc=0x%04X recv=0x%04X\n", crc_calc, crc_recv);
        return MODBUS_ERR_CRC;
    }

    resp->slave_addr = rx_copy[0];
    resp->function_code = rx_copy[1];
    resp->exception_code = 0;

    /* Exception response? */
    if (rx_copy[1] & 0x80) {
        resp->exception_code = rx_copy[2];
        TRice("MB RX: exception fc=0x%02X code=%d\n", rx_copy[1], rx_copy[2]);
        return MODBUS_ERR_EXCEPTION;
    }

    /* Parse register read response: [addr][fc][byte_count][data...][crc16] */
    uint8_t byte_count = rx_copy[2];
    resp->reg_count = byte_count / 2;
    for (uint16_t i = 0; i < resp->reg_count && i < 125; i++) {
        resp->regs[i] = ((uint16_t)rx_copy[3 + i * 2] << 8) |
                         (uint16_t)rx_copy[4 + i * 2];
    }

    return MODBUS_OK;
}

int modbus_read_holding_regs(uint8_t slave_addr, uint16_t start_reg,
                             uint16_t count, sModbusResponse *resp,
                             uint32_t timeout_ms)
{
    uint8_t frame[8];
    frame[0] = slave_addr;
    frame[1] = MODBUS_FC_READ_HOLDING_REGS;
    frame[2] = (uint8_t)(start_reg >> 8);
    frame[3] = (uint8_t)(start_reg & 0xFF);
    frame[4] = (uint8_t)(count >> 8);
    frame[5] = (uint8_t)(count & 0xFF);
    uint16_t crc = modbus_crc16(frame, 6);
    frame[6] = (uint8_t)(crc & 0xFF);
    frame[7] = (uint8_t)(crc >> 8);

    return modbus_transact(frame, 8, resp, timeout_ms);
}

int modbus_read_input_regs(uint8_t slave_addr, uint16_t start_reg,
                           uint16_t count, sModbusResponse *resp,
                           uint32_t timeout_ms)
{
    uint8_t frame[8];
    frame[0] = slave_addr;
    frame[1] = MODBUS_FC_READ_INPUT_REGS;
    frame[2] = (uint8_t)(start_reg >> 8);
    frame[3] = (uint8_t)(start_reg & 0xFF);
    frame[4] = (uint8_t)(count >> 8);
    frame[5] = (uint8_t)(count & 0xFF);
    uint16_t crc = modbus_crc16(frame, 6);
    frame[6] = (uint8_t)(crc & 0xFF);
    frame[7] = (uint8_t)(crc >> 8);

    return modbus_transact(frame, 8, resp, timeout_ms);
}
