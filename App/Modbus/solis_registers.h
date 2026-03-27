/**
 * @file    solis_registers.h
 * @brief   Solis hybrid inverter Modbus register map
 *
 * Register addresses are "wire addresses" (0-based), derived from the
 * Solis documentation addresses:
 *   Input register:   wire = doc_addr - 30001
 *   Holding register: wire = doc_addr - 40001
 *
 * Based on Solis RS485_MODBUS RTU Hybrid Inverter Protocol Ver3.2 and
 * the Pho3niX90/solis_modbus HACS integration register definitions.
 */

#ifndef SOLIS_REGISTERS_H_
#define SOLIS_REGISTERS_H_

#include <stdint.h>

/* --------------------------------------------------------------------------
 * Input registers (FC 0x04, read-only)
 * -------------------------------------------------------------------------- */

/* PV / DC input */
#define SOLIS_REG_PV1_VOLTAGE        3048   /* U16, x0.1 V */
#define SOLIS_REG_PV1_CURRENT        3049   /* U16, x0.1 A */
#define SOLIS_REG_PV2_VOLTAGE        3050   /* U16, x0.1 V */
#define SOLIS_REG_PV2_CURRENT        3051   /* U16, x0.1 A */
#define SOLIS_REG_PV_POWER_HI        3056   /* U32 (hi), x1 W */
#define SOLIS_REG_PV_POWER_LO        3057   /* U32 (lo) */

/* AC / Inverter output */
#define SOLIS_REG_GRID_VOLTAGE       3072   /* U16, x0.1 V */
#define SOLIS_REG_GRID_CURRENT       3075   /* U16, x0.1 A */
#define SOLIS_REG_ACTIVE_POWER_HI    3078   /* S32 (hi), x1 W */
#define SOLIS_REG_ACTIVE_POWER_LO    3079   /* S32 (lo) */
#define SOLIS_REG_INV_TEMPERATURE    3092   /* S16, x0.1 C */
#define SOLIS_REG_GRID_FREQUENCY     3093   /* U16, x0.01 Hz */
#define SOLIS_REG_INV_STATUS         3094   /* U16, status code */

/* Battery */
#define SOLIS_REG_BAT_VOLTAGE        3132   /* U16, x0.1 V */
#define SOLIS_REG_BAT_CURRENT        3133   /* S16, x0.1 A */
#define SOLIS_REG_BAT_DIRECTION      3134   /* U16, 0=charge 1=discharge */
#define SOLIS_REG_BAT_SOC            3138   /* U16, x1 % */
#define SOLIS_REG_BAT_SOH            3139   /* U16, x1 % */
#define SOLIS_REG_HOUSE_LOAD_POWER   3146   /* U16, x1 W */
#define SOLIS_REG_BACKUP_LOAD_POWER  3147   /* U16, x1 W */
#define SOLIS_REG_BAT_POWER_HI       3148   /* S32 (hi), x1 W */
#define SOLIS_REG_BAT_POWER_LO       3149   /* S32 (lo) */
#define SOLIS_REG_GRID_PORT_POWER_HI 3150   /* S32 (hi), x1 W */
#define SOLIS_REG_GRID_PORT_POWER_LO 3151   /* S32 (lo) */

/* Meter */
#define SOLIS_REG_METER_POWER_HI     3262   /* S32 (hi), x1 W */
#define SOLIS_REG_METER_POWER_LO     3263   /* S32 (lo) */

/* Energy totals (daily) */
#define SOLIS_REG_TODAY_PV           3034   /* U16, x0.1 kWh */
#define SOLIS_REG_TODAY_BAT_CHG      3162   /* U16, x0.1 kWh */
#define SOLIS_REG_TODAY_BAT_DSG      3166   /* U16, x0.1 kWh */
#define SOLIS_REG_TODAY_GRID_IMPORT  3170   /* U16, x0.1 kWh */
#define SOLIS_REG_TODAY_GRID_EXPORT  3174   /* U16, x0.1 kWh */
#define SOLIS_REG_TODAY_CONSUMPTION  3178   /* U16, x0.1 kWh */

/* Energy totals (lifetime) */
#define SOLIS_REG_TOTAL_PV_HI        3028   /* U32 (hi), x1 kWh */
#define SOLIS_REG_TOTAL_PV_LO        3029   /* U32 (lo) */

/* Device info (read once) */
#define SOLIS_REG_MODEL              2999   /* U16 */
#define SOLIS_REG_SERIAL_START       3003   /* 16 x U16 ASCII */
#define SOLIS_REG_SERIAL_COUNT       16

/* --------------------------------------------------------------------------
 * Holding registers (FC 0x03 read / FC 0x06 write)
 * -------------------------------------------------------------------------- */

#define SOLIS_HREG_STORAGE_MODE      3109   /* U16, bitfield */
#define SOLIS_HREG_CHARGE_I_LIMIT    3116   /* U16, x0.1 A */
#define SOLIS_HREG_DISCHARGE_I_LIMIT 3117   /* U16, x0.1 A */
#define SOLIS_HREG_MAX_CHARGE_SOC    3009   /* U16, x1 % (70-100) */
#define SOLIS_HREG_OVERDISCHARGE_SOC 3010   /* U16, x1 % (5-40) */

/* --------------------------------------------------------------------------
 * Register cache — populated by poller, read by MQTT publisher
 * -------------------------------------------------------------------------- */

typedef struct {
    /* PV */
    uint16_t pv1Voltage_dV;        /* x0.1 V */
    uint16_t pv1Current_dA;        /* x0.1 A */
    uint16_t pv2Voltage_dV;        /* x0.1 V */
    uint16_t pv2Current_dA;        /* x0.1 A */
    uint32_t pvPower_W;            /* W */

    /* AC / Grid */
    uint16_t gridVoltage_dV;       /* x0.1 V */
    uint16_t gridCurrent_dA;       /* x0.1 A */
    int32_t  activePower_W;        /* W (signed) */
    uint16_t gridFrequency_cHz;    /* x0.01 Hz */
    int16_t  invTemperature_dC;    /* x0.1 C */
    uint16_t invStatus;

    /* Battery */
    uint16_t batVoltage_dV;        /* x0.1 V */
    int16_t  batCurrent_dA;        /* x0.1 A (signed) */
    uint16_t batDirection;         /* 0=charge 1=discharge */
    uint16_t batSoc;               /* % */
    uint16_t batSoh;               /* % */
    int32_t  batPower_W;           /* W (signed) */
    uint16_t houseLoadPower_W;     /* W */
    uint16_t backupLoadPower_W;    /* W */
    int32_t  gridPortPower_W;      /* W (signed) */

    /* Meter */
    int32_t  meterPower_W;         /* W (signed) */

    /* Energy (daily, x0.1 kWh stored as raw uint16) */
    uint16_t todayPv_dkWh;
    uint16_t todayBatChg_dkWh;
    uint16_t todayBatDsg_dkWh;
    uint16_t todayGridImport_dkWh;
    uint16_t todayGridExport_dkWh;
    uint16_t todayConsumption_dkWh;

    /* Energy (total) */
    uint32_t totalPv_kWh;

    /* Timestamps */
    uint32_t lastFastPollTick;     /* HAL_GetTick() of last fast poll */
    uint32_t lastSlowPollTick;     /* HAL_GetTick() of last slow poll */
    uint32_t pollCount;            /* total successful poll cycles */
    uint32_t errorCount;           /* total Modbus errors */
} sSolisData;

#endif /* SOLIS_REGISTERS_H_ */
