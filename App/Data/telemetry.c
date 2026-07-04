/**
 * @file    telemetry.c
 * @brief   Neutral telemetry data model — producer/consumer snapshot store
 */

#include "App/Data/telemetry.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

static sEnergyTelemetry s_energy;
static uint32_t         s_energyUpdates;

void Telemetry_PublishEnergy(const sEnergyTelemetry *data)
{
    if (!data) {
        return;
    }

    taskENTER_CRITICAL();
    s_energy = *data;
    s_energyUpdates++;
    taskEXIT_CRITICAL();
}

int Telemetry_GetEnergy(sEnergyTelemetry *out)
{
    if (!out) {
        return -1;
    }

    taskENTER_CRITICAL();
    uint32_t updates = s_energyUpdates;
    *out = s_energy;
    taskEXIT_CRITICAL();

    return (updates > 0) ? 0 : -1;
}
