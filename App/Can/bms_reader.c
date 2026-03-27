/**
 * @file    bms_reader.c
 * @brief   Pylontech BMS CAN reader — receives and parses battery frames on CAN2
 *
 * CAN2 is re-initialised to 500 kbps with acceptance filters for Pylontech IDs.
 * Frames are received via RX FIFO0 interrupt (HAL_CAN_RxFifo0MsgPendingCallback).
 * BmsReader_Poll() is kept as a safety net to drain anything the ISR missed.
 */

#include "App/Can/bms_reader.h"
#include "can.h"
#include "trice.h"
#include <string.h>

static sPylonBatteryData s_data;
static volatile int s_running;
static volatile uint32_t s_rxCount;

/* --------------------------------------------------------------------------
 * CAN2 reconfiguration to 500 kbps + filters + RX interrupt
 *
 * Same timing as CAN1: Prescaler=6, BS1=10, BS2=3 → 500 kbps.
 *
 * CAN2 filter banks start at bank 14 (banks 0–13 belong to CAN1).
 * Mask filter accepts 0x350–0x35F.
 * -------------------------------------------------------------------------- */

static int can2_init_500k(void)
{
    HAL_CAN_DeInit(&hcan2);

    hcan2.Instance                  = CAN2;
    hcan2.Init.Prescaler            = 6;
    hcan2.Init.Mode                 = CAN_MODE_NORMAL;
    hcan2.Init.SyncJumpWidth        = CAN_SJW_1TQ;
    hcan2.Init.TimeSeg1             = CAN_BS1_10TQ;
    hcan2.Init.TimeSeg2             = CAN_BS2_3TQ;
    hcan2.Init.TimeTriggeredMode    = DISABLE;
    hcan2.Init.AutoBusOff           = ENABLE;
    hcan2.Init.AutoWakeUp           = DISABLE;
    hcan2.Init.AutoRetransmission   = ENABLE;
    hcan2.Init.ReceiveFifoLocked    = DISABLE;
    hcan2.Init.TransmitFifoPriority = DISABLE;

    if (HAL_CAN_Init(&hcan2) != HAL_OK) {
        return -1;
    }

    /* Accept 0x350–0x35F (mask: ignore lower 4 bits of the 11-bit ID)
     * Filter ID:   0x350 << 5 = 0x6A00
     * Filter Mask:  0x7F0 << 5 = 0xFE00  (bits 10:4 must match)
     */
    CAN_FilterTypeDef filter = {
        .FilterIdHigh         = 0x350 << 5,
        .FilterIdLow          = 0x0000,
        .FilterMaskIdHigh     = 0x7F0 << 5,
        .FilterMaskIdLow      = 0x0000,
        .FilterFIFOAssignment = CAN_FILTER_FIFO0,
        .FilterBank           = 14,
        .FilterMode           = CAN_FILTERMODE_IDMASK,
        .FilterScale          = CAN_FILTERSCALE_32BIT,
        .FilterActivation     = CAN_FILTER_ENABLE,
        .SlaveStartFilterBank = 14,
    };

    if (HAL_CAN_ConfigFilter(&hcan2, &filter) != HAL_OK) {
        return -1;
    }

    if (HAL_CAN_Start(&hcan2) != HAL_OK) {
        return -1;
    }

    /* Enable RX FIFO0 message pending interrupt */
    if (HAL_CAN_ActivateNotification(&hcan2, CAN_IT_RX_FIFO0_MSG_PENDING) != HAL_OK) {
        return -1;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Frame parsing (called from ISR context and from Poll)
 * -------------------------------------------------------------------------- */

static void parse_frame(uint32_t stdId, const uint8_t *data, uint8_t dlc)
{
    s_data.lastRxTick = HAL_GetTick();
    s_rxCount++;

    switch (stdId) {
    case PYLON_CAN_ID_LIMITS:
        if (dlc >= 8) {
            const sPylonLimits *f = (const sPylonLimits *)data;
            s_data.chargeVoltageLimit    = f->chargeVoltageLimit_dV / 10.0f;
            s_data.maxChargeCurrent      = f->maxChargeCurrent_dA / 10.0f;
            s_data.maxDischargeCurrent   = f->maxDischargeCurrent_dA / 10.0f;
            s_data.dischargeVoltageLimit = f->dischargeVoltageLimit_dV / 10.0f;
            s_data.rxMask |= PYLON_RX_GOT_LIMITS;
        }
        break;

    case PYLON_CAN_ID_SOC:
        if (dlc >= 4) {
            const sPylonSoc *f = (const sPylonSoc *)data;
            s_data.soc = f->soc_pct;
            s_data.soh = f->soh_pct;
            s_data.rxMask |= PYLON_RX_GOT_SOC;
        }
        break;

    case PYLON_CAN_ID_MEASURE:
        if (dlc >= 6) {
            const sPylonMeasure *f = (const sPylonMeasure *)data;
            s_data.voltage     = f->voltage_cV / 100.0f;
            s_data.current     = f->current_dA / 10.0f;
            s_data.temperature = f->temperature_dC / 10.0f;
            s_data.rxMask |= PYLON_RX_GOT_MEASURE;
        }
        break;

    case PYLON_CAN_ID_ALARM:
        if (dlc >= 7) {
            const sPylonAlarm *f = (const sPylonAlarm *)data;
            s_data.errors[0]   = f->errors0;
            s_data.errors[1]   = f->errors1;
            s_data.warnings[0] = f->warnings0;
            s_data.warnings[1] = f->warnings1;
            s_data.rxMask |= PYLON_RX_GOT_ALARM;
        }
        break;

    case PYLON_CAN_ID_CHGCTRL:
        if (dlc >= 1) {
            s_data.chargeEnabled    = (data[0] & PYLON_FLAG_CHARGE_EN) ? 1 : 0;
            s_data.dischargeEnabled = (data[0] & PYLON_FLAG_DISCHARGE_EN) ? 1 : 0;
            s_data.rxMask |= PYLON_RX_GOT_CHGCTRL;
        }
        break;

    case PYLON_CAN_ID_MFGNAME:
        if (dlc >= 8) {
            memcpy(s_data.manufacturer, data, 8);
            s_data.manufacturer[8] = '\0';
            s_data.rxMask |= PYLON_RX_GOT_MFGNAME;
        }
        break;

    default:
        break;
    }
}

/* --------------------------------------------------------------------------
 * HAL CAN RX callback — called from CAN2_RX0_IRQHandler via HAL_CAN_IRQHandler
 * -------------------------------------------------------------------------- */

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    if (hcan->Instance != CAN2 || !s_running) {
        return;
    }

    CAN_RxHeaderTypeDef header;
    uint8_t data[8];

    if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &header, data) == HAL_OK) {
        if (header.IDE == CAN_ID_STD) {
            parse_frame(header.StdId, data, header.DLC);
        }
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void BmsReader_Start(void)
{
    if (s_running) return;

    memset((void *)&s_data, 0, sizeof(s_data));
    s_rxCount = 0;

    if (can2_init_500k() != 0) {
        TRice("BmsReader: CAN2 init failed\n");
        return;
    }

    s_running = 1;
    TRice("BmsReader: started on CAN2 (500 kbps, IRQ)\n");
}

void BmsReader_Stop(void)
{
    if (!s_running) return;

    s_running = 0;
    HAL_CAN_DeactivateNotification(&hcan2, CAN_IT_RX_FIFO0_MSG_PENDING);
    HAL_CAN_Stop(&hcan2);
    HAL_CAN_DeInit(&hcan2);

    TRice("BmsReader: stopped (rx=%u frames)\n", s_rxCount);
}

int BmsReader_IsRunning(void)
{
    return s_running;
}

void BmsReader_Poll(void)
{
    if (!s_running) return;

    CAN_RxHeaderTypeDef header;
    uint8_t data[8];

    /* Safety net: drain anything the ISR might have missed */
    while (HAL_CAN_GetRxFifoFillLevel(&hcan2, CAN_RX_FIFO0) > 0) {
        if (HAL_CAN_GetRxMessage(&hcan2, CAN_RX_FIFO0, &header, data) == HAL_OK) {
            if (header.IDE == CAN_ID_STD) {
                parse_frame(header.StdId, data, header.DLC);
            }
        }
    }
}

const sPylonBatteryData *BmsReader_GetData(void)
{
    return (const sPylonBatteryData *)&s_data;
}

void BmsReader_LogData(void)
{
    const sPylonBatteryData *d = (const sPylonBatteryData *)&s_data;

    if (d->rxMask == 0) {
        TRice("BmsReader: no data received\n");
        return;
    }

    TRice("BMS rx=0x%02X (%u frames)", d->rxMask, s_rxCount);

    if (d->rxMask & PYLON_RX_GOT_MEASURE) {
        TRice(" V=%d.%02dV I=%d.%01dA T=%d.%01dC",
              (int)d->voltage, ((int)(d->voltage * 100)) % 100,
              (int)d->current, ((int)(d->current * 10)) % 10,
              (int)d->temperature, ((int)(d->temperature * 10)) % 10);
    }

    if (d->rxMask & PYLON_RX_GOT_SOC) {
        TRice(" SOC=%u%% SOH=%u%%", d->soc, d->soh);
    }

    if (d->rxMask & PYLON_RX_GOT_LIMITS) {
        TRice(" ChgLim=%d.%01dV DsgLim=%d.%01dV",
              (int)d->chargeVoltageLimit,
              ((int)(d->chargeVoltageLimit * 10)) % 10,
              (int)d->dischargeVoltageLimit,
              ((int)(d->dischargeVoltageLimit * 10)) % 10);
    }

    if (d->rxMask & PYLON_RX_GOT_CHGCTRL) {
        TRice(" chg=%u dsg=%u", d->chargeEnabled, d->dischargeEnabled);
    }

    if (d->rxMask & PYLON_RX_GOT_MFGNAME) {
        TRiceS(" mfg=\"%s\"", (char *)d->manufacturer);
    }

    TRice("\n");
}
