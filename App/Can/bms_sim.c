/**
 * @file    bms_sim.c
 * @brief   Pylontech BMS simulator — the board answering an inverter itself
 *
 * Simulates a 16S 48V LiFePO4 pack in the Pylontech dialect.
 *
 * IT IS THE BRIDGE'S FRAME SOURCE, and that is its real job now.  In
 * canBrMode_bms the bridge stops relaying the battery and calls a registered
 * source once a second to answer the inverter instead; this file is what
 * occupies that slot today.  When the battery cluster exists it takes the same
 * slot with real numbers, and neither the bridge nor the inverter learns
 * anything new — which is the whole reason the seam is a callback and not a
 * call into this file.
 *
 * It owns NO peripheral: frames go out through App/Can/can_bus.c on whichever
 * bus the inverter is on.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Can/bms_sim.h"
#include "App/Can/can_bridge.h"
#include "App/Can/can_bus.h"
#include "App/Can/pylontech.h"

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
 * Frame transmission — through the shared bus layer, never the HAL
 * -------------------------------------------------------------------------- */

static void TxFrame(uint32_t stdId, const void *data, uint8_t dlc)
{
    sCanFrame frame;

    memset(&frame, 0, sizeof(frame));
    frame.id  = stdId;
    frame.dlc = dlc;
    frame.bus = (uint8_t)CanBridge_InverterBus();
    memcpy(frame.data, data, (dlc > 8u) ? 8u : dlc);

    (void)CanBus_Send(&frame);
}

/** The bridge's source slot.  RTOS timer context, once per period. */
static void SourceCb(eCanBus inverterBus, void *ctx)
{
    (void)inverterBus;
    (void)ctx;

    BmsSim_SendOnce();
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void BmsSim_Start(void)
{
    eCanBus bus = CanBridge_InverterBus();

    if (s_sim.running) {
        return;
    }

    /* Registering the source is enough: the bridge starts and stops the timer
     * with the mode, so `bms start` in canBrMode_bridge arms nothing and puts
     * no frame on a bus the real battery is still driving. */
    if (CanBridge_SetSource(SourceCb, NULL, PYLON_TX_INTERVAL_MS) != 0) {
        TRice("err:BmsSim: could not take the bridge source slot\n");
        return;
    }

    s_sim.running = 1;
    TRice("BmsSim: source armed for CAN%u (%u ms); emits in bms mode\n",
          (unsigned)bus + 1u, PYLON_TX_INTERVAL_MS);
}

void BmsSim_Stop(void)
{
    if (!s_sim.running) {
        return;
    }

    (void)CanBridge_SetSource(NULL, NULL, 0u);
    s_sim.running = 0;
    TRice("BmsSim: source released\n");
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
        TxFrame(PYLON_CAN_ID_LIMITS, &f, 8);
    }

    /* 0x355 — SOC/SOH */
    {
        sPylonSoc f;
        f.soc_pct = s_sim.soc;
        f.soh_pct = s_sim.soh;
        TxFrame(PYLON_CAN_ID_SOC, &f, 4);
    }

    /* 0x356 — Measurements */
    {
        sPylonMeasure f;
        f.voltage_cV     = (int16_t)(s_sim.voltage * 100.0f);
        f.current_dA     = (int16_t)(s_sim.current * 10.0f);
        f.temperature_dC = (int16_t)(s_sim.temperature * 10.0f);
        TxFrame(PYLON_CAN_ID_MEASURE, &f, 6);
    }

    /* 0x359 — Alarms (all clear) */
    {
        sPylonAlarm f;
        memset(&f, 0, sizeof(f));
        f.moduleNum = 0x01;
        f.ascii_P   = 'P';
        f.ascii_N   = 'N';
        TxFrame(PYLON_CAN_ID_ALARM, &f, 7);
    }

    /* 0x35C — Charge/discharge enable */
    {
        sPylonChgCtrl f;
        f.flags    = PYLON_FLAG_CHARGE_EN | PYLON_FLAG_DISCHARGE_EN;
        f.reserved = 0;
        TxFrame(PYLON_CAN_ID_CHGCTRL, &f, 2);
    }

    /* 0x35E — Manufacturer name */
    {
        uint8_t name[8];
        memcpy(name, PYLON_MFG_NAME, 8);
        TxFrame(PYLON_CAN_ID_MFGNAME, name, 8);
    }
}

void BmsSim_SetVoltage(float volts)         { s_sim.voltage = volts; }
void BmsSim_SetCurrent(float amps)          { s_sim.current = amps; }
void BmsSim_SetSoc(uint16_t pct)            { s_sim.soc = pct; }
void BmsSim_SetTemperature(float degC)      { s_sim.temperature = degC; }
