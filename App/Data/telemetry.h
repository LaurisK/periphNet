/**
 * @file    telemetry.h
 * @brief   Neutral telemetry data model between producers and consumers
 *
 * Producers (Modbus poller, CAN/BMS reader) publish snapshots; consumers
 * (MQTT bridge, HTTP status, CLI) read them. Keeps consumers independent
 * of any bus/protocol module: the MQTT bridge must not know Solis
 * registers exist.
 *
 * Values keep the fixed-point raw encodings used on the wire
 * (_dV = x0.1 V, _dA = x0.1 A, _cHz = x0.01 Hz, _dC = x0.1 °C,
 * _dkWh = x0.1 kWh).
 */

#ifndef TELEMETRY_H_
#define TELEMETRY_H_

#include <stdint.h>

typedef struct {
    /* PV */
    uint16_t pv1Voltage_dV;
    uint16_t pv1Current_dA;
    uint16_t pv2Voltage_dV;
    uint16_t pv2Current_dA;
    uint32_t pvPower_W;

    /* AC / Grid */
    uint16_t gridVoltage_dV;
    uint16_t gridFrequency_cHz;
    int32_t  activePower_W;
    int16_t  invTemperature_dC;

    /* Battery */
    uint16_t batVoltage_dV;
    int16_t  batCurrent_dA;
    uint16_t batSoc;
    uint16_t batSoh;
    int32_t  batPower_W;

    /* Load / meter */
    uint16_t houseLoadPower_W;
    uint16_t backupLoadPower_W;
    int32_t  gridPortPower_W;
    int32_t  meterPower_W;

    /* Energy counters */
    uint16_t todayPv_dkWh;
    uint16_t todayGridImport_dkWh;
    uint16_t todayGridExport_dkWh;
    uint16_t todayConsumption_dkWh;
    uint16_t todayBatChg_dkWh;
    uint16_t todayBatDsg_dkWh;
    uint32_t totalPv_kWh;
} sEnergyTelemetry;

/**
 * @brief  Publish a fresh energy snapshot (producer side).
 *         Safe to call from any task; the copy is taken atomically.
 */
void Telemetry_PublishEnergy(const sEnergyTelemetry *data);

/**
 * @brief  Get a coherent copy of the latest energy snapshot.
 * @return 0 on success, -1 if nothing has been published yet.
 */
int Telemetry_GetEnergy(sEnergyTelemetry *out);

#endif /* TELEMETRY_H_ */
