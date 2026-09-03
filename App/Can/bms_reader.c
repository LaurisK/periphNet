/**
 * @file    bms_reader.c
 * @brief   Pylontech BMS CAN reader — parses the battery frame set into a
 *          display struct
 *
 * IT NO LONGER OWNS A PERIPHERAL.  It used to re-initialise CAN2 and define
 * the one weak RX-FIFO-pending callback the HAL offers, which is what blocked
 * every other CAN consumer on this board (docs/design_battery_pack.md §16.5).
 * Now it is an ordinary subscriber of App/Can/can_bus.c: the bridge owns the
 * wire, and this file asks for 0x350-0x35F on whichever bus the battery is on.
 *
 * Its output is a float display struct and stays that way deliberately — it
 * feeds the CLI and the web UI.  Anything making a DECISION about a battery
 * goes through App/Pack, whose types parse the packed frames into integers.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Can/bms_reader.h"
#include "App/Can/can_bridge.h"
#include "App/Can/can_bus.h"

#include "trice.h"
#include "stm32f4xx_hal.h"

#include <string.h>

/* Private defines ----------------------------------------------------------*/

/** The Pylontech block: 0x350-0x35F, matched on the top seven bits. */
#define BMS_READER_ID           0x350u
#define BMS_READER_MASK         0x7F0u

/* Private variables --------------------------------------------------------*/

static sPylonBatteryData s_data;
static int               s_sub = -1;
static volatile int s_running;
static volatile uint32_t s_rxCount;

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
 * Subscription callback — ISR context, via the can_bus dispatcher
 * -------------------------------------------------------------------------- */

static void OnFrame(const sCanFrame *frame, void *ctx)
{
    (void)ctx;

    if ((frame->rtr == 0u) && (frame->ext == 0u)) {
        parse_frame(frame->id, frame->data, frame->dlc);
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void BmsReader_Start(void)
{
    eCanBus bus = CanBridge_BatteryBus();

    if (s_running) {
        return;
    }

    if (!CanBus_IsRunning(bus)) {
        TRice("err:BmsReader: CAN%u is down - start the bridge first\n",
              (unsigned)bus + 1u);
        return;
    }

    memset((void *)&s_data, 0, sizeof(s_data));
    s_rxCount = 0;

    s_sub = CanBus_Subscribe(bus, BMS_READER_ID, BMS_READER_MASK,
                             OnFrame, NULL);
    if (s_sub < 0) {
        TRice("err:BmsReader: no free subscription slot\n");
        return;
    }

    s_running = 1;
    TRice("BmsReader: listening on CAN%u for 0x350-0x35F\n",
          (unsigned)bus + 1u);
}

void BmsReader_Stop(void)
{
    if (!s_running) {
        return;
    }

    s_running = 0;
    if (s_sub >= 0) {
        (void)CanBus_Unsubscribe(s_sub);
        s_sub = -1;
    }

    TRice("BmsReader: stopped (rx=%u frames)\n", s_rxCount);
}

int BmsReader_IsRunning(void)
{
    return s_running;
}

/**
 * Nothing to poll any more: the dispatcher drains the FIFO in the RX ISR and
 * hands frames straight to OnFrame.  Kept because the CLI calls it before
 * printing, and because a caller asking "is there anything new" deserves an
 * answer that is true rather than a compile error.
 */
void BmsReader_Poll(void)
{
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
