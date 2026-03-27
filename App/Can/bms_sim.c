/**
 * @file    bms_sim.c
 * @brief   Pylontech BMS simulator — transmits battery CAN frames on CAN1
 *
 * Simulates a 16S 48V LiFePO4 battery pack with Pylontech CAN protocol.
 * CAN1 is re-initialised to 500 kbps on start.
 */

#include "App/Can/bms_sim.h"
#include "App/Can/pylontech.h"
#include "can.h"
#include "trice.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Default battery parameters (16S LiFePO4 48V)
 * -------------------------------------------------------------------------- */

static struct {
    float    voltage;               /* pack voltage (V) */
    float    current;               /* current (A), + = discharge */
    uint16_t soc;                   /* state of charge (%) */
    uint16_t soh;                   /* state of health (%) */
    float    temperature;           /* °C */
    float    chargeVoltageLimit;    /* V */
    float    dischargeVoltageLimit; /* V */
    float    maxChargeCurrent;      /* A */
    float    maxDischargeCurrent;   /* A */
    int      running;
} s_sim = {
    .voltage               = 51.2f,
    .current               = -2.5f,   /* charging at 2.5 A */
    .soc                   = 85,
    .soh                   = 99,
    .temperature           = 25.0f,
    .chargeVoltageLimit    = 56.0f,
    .dischargeVoltageLimit = 44.8f,
    .maxChargeCurrent      = 50.0f,
    .maxDischargeCurrent   = 50.0f,
    .running               = 0,
};

/* --------------------------------------------------------------------------
 * CAN1 reconfiguration to 500 kbps
 *
 * APB1 = 42 MHz.  500k = 42M / (Prescaler * (1 + BS1 + BS2))
 * Prescaler=6, BS1=10TQ, BS2=3TQ → 6 × 14 = 84 → 500 kbps.
 * Sample point = 11/14 = 78.6 %.
 * -------------------------------------------------------------------------- */

static int can1_init_500k(void)
{
    HAL_CAN_DeInit(&hcan1);

    hcan1.Instance                  = CAN1;
    hcan1.Init.Prescaler            = 6;
    hcan1.Init.Mode                 = CAN_MODE_NORMAL;
    hcan1.Init.SyncJumpWidth        = CAN_SJW_1TQ;
    hcan1.Init.TimeSeg1             = CAN_BS1_10TQ;
    hcan1.Init.TimeSeg2             = CAN_BS2_3TQ;
    hcan1.Init.TimeTriggeredMode    = DISABLE;
    hcan1.Init.AutoBusOff           = ENABLE;
    hcan1.Init.AutoWakeUp           = DISABLE;
    hcan1.Init.AutoRetransmission   = ENABLE;
    hcan1.Init.ReceiveFifoLocked    = DISABLE;
    hcan1.Init.TransmitFifoPriority = DISABLE;

    if (HAL_CAN_Init(&hcan1) != HAL_OK) {
        return -1;
    }

    if (HAL_CAN_Start(&hcan1) != HAL_OK) {
        return -1;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Frame transmission helpers
 * -------------------------------------------------------------------------- */

static int can1_tx(uint32_t stdId, const void *data, uint8_t dlc)
{
    CAN_TxHeaderTypeDef header = {
        .StdId = stdId,
        .ExtId = 0,
        .IDE   = CAN_ID_STD,
        .RTR   = CAN_RTR_DATA,
        .DLC   = dlc,
    };

    uint32_t mailbox;
    if (HAL_CAN_AddTxMessage(&hcan1, &header, (uint8_t *)data, &mailbox) != HAL_OK) {
        return -1;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void BmsSim_Start(void)
{
    if (s_sim.running) return;

    if (can1_init_500k() != 0) {
        TRice("BmsSim: CAN1 init failed\n");
        return;
    }

    s_sim.running = 1;
    TRice("BmsSim: started on CAN1 (500 kbps)\n");
}

void BmsSim_Stop(void)
{
    if (!s_sim.running) return;

    HAL_CAN_Stop(&hcan1);
    HAL_CAN_DeInit(&hcan1);

    s_sim.running = 0;
    TRice("BmsSim: stopped\n");
}

int BmsSim_IsRunning(void)
{
    return s_sim.running;
}

void BmsSim_SendOnce(void)
{
    if (!s_sim.running) return;

    /* 0x351 — Limits */
    {
        sPylonLimits f;
        f.chargeVoltageLimit_dV    = (int16_t)(s_sim.chargeVoltageLimit * 10.0f);
        f.maxChargeCurrent_dA      = (int16_t)(s_sim.maxChargeCurrent * 10.0f);
        f.maxDischargeCurrent_dA   = (int16_t)(s_sim.maxDischargeCurrent * 10.0f);
        f.dischargeVoltageLimit_dV = (uint16_t)(s_sim.dischargeVoltageLimit * 10.0f);
        can1_tx(PYLON_CAN_ID_LIMITS, &f, 8);
    }

    /* 0x355 — SOC/SOH */
    {
        sPylonSoc f;
        f.soc_pct = s_sim.soc;
        f.soh_pct = s_sim.soh;
        can1_tx(PYLON_CAN_ID_SOC, &f, 4);
    }

    /* 0x356 — Measurements */
    {
        sPylonMeasure f;
        f.voltage_cV     = (int16_t)(s_sim.voltage * 100.0f);
        f.current_dA     = (int16_t)(s_sim.current * 10.0f);
        f.temperature_dC = (int16_t)(s_sim.temperature * 10.0f);
        can1_tx(PYLON_CAN_ID_MEASURE, &f, 6);
    }

    /* 0x359 — Alarms (all clear) */
    {
        sPylonAlarm f;
        memset(&f, 0, sizeof(f));
        f.moduleNum = 0x01;
        f.ascii_P   = 'P';
        f.ascii_N   = 'N';
        can1_tx(PYLON_CAN_ID_ALARM, &f, 7);
    }

    /* 0x35C — Charge/discharge enable */
    {
        sPylonChgCtrl f;
        f.flags    = PYLON_FLAG_CHARGE_EN | PYLON_FLAG_DISCHARGE_EN;
        f.reserved = 0;
        can1_tx(PYLON_CAN_ID_CHGCTRL, &f, 2);
    }

    /* 0x35E — Manufacturer name */
    {
        uint8_t name[8];
        memcpy(name, PYLON_MFG_NAME, 8);
        can1_tx(PYLON_CAN_ID_MFGNAME, name, 8);
    }
}

void BmsSim_SetVoltage(float volts)         { s_sim.voltage = volts; }
void BmsSim_SetCurrent(float amps)          { s_sim.current = amps; }
void BmsSim_SetSoc(uint16_t pct)            { s_sim.soc = pct; }
void BmsSim_SetTemperature(float degC)      { s_sim.temperature = degC; }
