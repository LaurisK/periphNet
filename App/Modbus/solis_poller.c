/**
 * @file    solis_poller.c
 * @brief   Solis inverter Modbus poll scheduler (FreeRTOS task)
 */

#include "App/Modbus/solis_poller.h"
#include "App/Modbus/modbus_rtu.h"
#include "App/Data/telemetry.h"
#include "App/system.h"
#include "cmsis_os.h"
#include "trice.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */

static sSolisData       s_data;
static sSolisPollerCfg  s_cfg;
static volatile int     s_running;
static volatile int     s_stopReq;
static osThreadId_t     s_taskHandle;

/* Pending write queue (simple single-slot) */
static volatile int      s_pendingWrite;
static volatile uint16_t s_writeReg;
static volatile uint16_t s_writeVal;

/* --------------------------------------------------------------------------
 * Helpers: combine two uint16 into uint32/int32 (big-endian register order)
 * -------------------------------------------------------------------------- */

static inline uint32_t u32_from_regs(uint16_t hi, uint16_t lo)
{
    return ((uint32_t)hi << 16) | lo;
}

static inline int32_t s32_from_regs(uint16_t hi, uint16_t lo)
{
    return (int32_t)(((uint32_t)hi << 16) | lo);
}

/* --------------------------------------------------------------------------
 * Telemetry publishing — map the register cache into the neutral data model
 * -------------------------------------------------------------------------- */

static void publish_telemetry(void)
{
    sEnergyTelemetry t = {
        .pv1Voltage_dV        = s_data.pv1Voltage_dV,
        .pv1Current_dA        = s_data.pv1Current_dA,
        .pv2Voltage_dV        = s_data.pv2Voltage_dV,
        .pv2Current_dA        = s_data.pv2Current_dA,
        .pvPower_W            = s_data.pvPower_W,
        .gridVoltage_dV       = s_data.gridVoltage_dV,
        .gridFrequency_cHz    = s_data.gridFrequency_cHz,
        .activePower_W        = s_data.activePower_W,
        .invTemperature_dC    = s_data.invTemperature_dC,
        .batVoltage_dV        = s_data.batVoltage_dV,
        .batCurrent_dA        = s_data.batCurrent_dA,
        .batSoc               = s_data.batSoc,
        .batSoh               = s_data.batSoh,
        .batPower_W           = s_data.batPower_W,
        .houseLoadPower_W     = s_data.houseLoadPower_W,
        .backupLoadPower_W    = s_data.backupLoadPower_W,
        .gridPortPower_W      = s_data.gridPortPower_W,
        .meterPower_W         = s_data.meterPower_W,
        .todayPv_dkWh         = s_data.todayPv_dkWh,
        .todayGridImport_dkWh = s_data.todayGridImport_dkWh,
        .todayGridExport_dkWh = s_data.todayGridExport_dkWh,
        .todayConsumption_dkWh = s_data.todayConsumption_dkWh,
        .todayBatChg_dkWh     = s_data.todayBatChg_dkWh,
        .todayBatDsg_dkWh     = s_data.todayBatDsg_dkWh,
        .totalPv_kWh          = s_data.totalPv_kWh,
    };
    Telemetry_PublishEnergy(&t);
}

/* --------------------------------------------------------------------------
 * Poll groups
 * -------------------------------------------------------------------------- */

static void poll_fast(void)
{
    uint16_t buf[48];
    eModbusErr err;

    /* Transaction 1: PV + inverter (33049-33095 → wire 3048, 47 regs) */
    err = Modbus_ReadInputRegisters(s_cfg.slaveAddr, SOLIS_REG_PV1_VOLTAGE,
                                    47, buf, s_cfg.responseTimeoutMs);
    if (err == MODBUS_OK) {
        /* PV: offsets relative to startReg 3048 */
        s_data.pv1Voltage_dV  = buf[0];                   /* 3048 */
        s_data.pv1Current_dA  = buf[1];                   /* 3049 */
        s_data.pv2Voltage_dV  = buf[2];                   /* 3050 */
        s_data.pv2Current_dA  = buf[3];                   /* 3051 */
        s_data.pvPower_W      = u32_from_regs(buf[8], buf[9]); /* 3056-3057 */

        /* Grid: offset 3072-3048 = 24 */
        s_data.gridVoltage_dV   = buf[24];                 /* 3072 */
        s_data.gridCurrent_dA   = buf[27];                 /* 3075 */
        s_data.activePower_W    = s32_from_regs(buf[30], buf[31]); /* 3078-3079 */
        s_data.invTemperature_dC = (int16_t)buf[44];       /* 3092 */
        s_data.gridFrequency_cHz = buf[45];                /* 3093 */
        s_data.invStatus         = buf[46];                /* 3094 */
    } else {
        s_data.errorCount++;
    }

    /* Transaction 2: Battery + load (33133-33152 → wire 3132, 20 regs) */
    err = Modbus_ReadInputRegisters(s_cfg.slaveAddr, SOLIS_REG_BAT_VOLTAGE,
                                    20, buf, s_cfg.responseTimeoutMs);
    if (err == MODBUS_OK) {
        /* offsets relative to 3132 */
        s_data.batVoltage_dV    = buf[0];                  /* 3132 */
        s_data.batCurrent_dA    = (int16_t)buf[1];         /* 3133 */
        s_data.batDirection     = buf[2];                   /* 3134 */
        s_data.batSoc           = buf[6];                   /* 3138 */
        s_data.batSoh           = buf[7];                   /* 3139 */
        s_data.houseLoadPower_W = buf[14];                  /* 3146 */
        s_data.backupLoadPower_W = buf[15];                 /* 3147 */
        s_data.batPower_W       = s32_from_regs(buf[16], buf[17]); /* 3148-3149 */
        s_data.gridPortPower_W  = s32_from_regs(buf[18], buf[19]); /* 3150-3151 */
    } else {
        s_data.errorCount++;
    }

    /* Transaction 3: Meter total power (33263-33264 → wire 3262, 2 regs) */
    err = Modbus_ReadInputRegisters(s_cfg.slaveAddr, SOLIS_REG_METER_POWER_HI,
                                    2, buf, s_cfg.responseTimeoutMs);
    if (err == MODBUS_OK) {
        s_data.meterPower_W = s32_from_regs(buf[0], buf[1]);
    } else {
        s_data.errorCount++;
    }

    s_data.lastFastPollTick = HAL_GetTick();
    s_data.pollCount++;
    publish_telemetry();
}

static void poll_slow(void)
{
    uint16_t buf[8];
    eModbusErr err;

    /* Today PV generation (33035 → wire 3034, 1 reg) */
    err = Modbus_ReadInputRegisters(s_cfg.slaveAddr, SOLIS_REG_TODAY_PV,
                                    1, buf, s_cfg.responseTimeoutMs);
    if (err == MODBUS_OK) {
        s_data.todayPv_dkWh = buf[0];
    }

    /* Today battery charge (33163 → wire 3162, 1 reg) */
    err = Modbus_ReadInputRegisters(s_cfg.slaveAddr, SOLIS_REG_TODAY_BAT_CHG,
                                    1, buf, s_cfg.responseTimeoutMs);
    if (err == MODBUS_OK) {
        s_data.todayBatChg_dkWh = buf[0];
    }

    /* Today battery discharge (33167 → wire 3166, 1 reg) */
    err = Modbus_ReadInputRegisters(s_cfg.slaveAddr, SOLIS_REG_TODAY_BAT_DSG,
                                    1, buf, s_cfg.responseTimeoutMs);
    if (err == MODBUS_OK) {
        s_data.todayBatDsg_dkWh = buf[0];
    }

    /* Today grid import (33171 → wire 3170, 1 reg) */
    err = Modbus_ReadInputRegisters(s_cfg.slaveAddr, SOLIS_REG_TODAY_GRID_IMPORT,
                                    1, buf, s_cfg.responseTimeoutMs);
    if (err == MODBUS_OK) {
        s_data.todayGridImport_dkWh = buf[0];
    }

    /* Today grid export (33175 → wire 3174, 1 reg) */
    err = Modbus_ReadInputRegisters(s_cfg.slaveAddr, SOLIS_REG_TODAY_GRID_EXPORT,
                                    1, buf, s_cfg.responseTimeoutMs);
    if (err == MODBUS_OK) {
        s_data.todayGridExport_dkWh = buf[0];
    }

    /* Today consumption (33179 → wire 3178, 1 reg) */
    err = Modbus_ReadInputRegisters(s_cfg.slaveAddr, SOLIS_REG_TODAY_CONSUMPTION,
                                    1, buf, s_cfg.responseTimeoutMs);
    if (err == MODBUS_OK) {
        s_data.todayConsumption_dkWh = buf[0];
    }

    /* Total PV (33029-33030 → wire 3028, 2 regs) */
    err = Modbus_ReadInputRegisters(s_cfg.slaveAddr, SOLIS_REG_TOTAL_PV_HI,
                                    2, buf, s_cfg.responseTimeoutMs);
    if (err == MODBUS_OK) {
        s_data.totalPv_kWh = u32_from_regs(buf[0], buf[1]);
    }

    s_data.lastSlowPollTick = HAL_GetTick();
    publish_telemetry();
}

/* --------------------------------------------------------------------------
 * Pending write execution
 * -------------------------------------------------------------------------- */

static void process_pending_write(void)
{
    if (!s_pendingWrite) return;

    uint16_t reg = s_writeReg;
    uint16_t val = s_writeVal;
    s_pendingWrite = 0;

    eModbusErr err = Modbus_WriteSingleRegister(s_cfg.slaveAddr, reg,
                                                 val, s_cfg.responseTimeoutMs);
    if (err == MODBUS_OK) {
        TRice("Modbus: wrote reg %u = %u\n", reg, val);
    } else {
        TRice("Modbus: write reg %u failed (%d)\n", reg, (int)err);
        s_data.errorCount++;
    }
}

/* --------------------------------------------------------------------------
 * Task
 * -------------------------------------------------------------------------- */

static void pollerTask(void *arg)
{
    (void)arg;

    if (Modbus_Init(s_cfg.baud) != 0) {
        TRice("Modbus: USART2 init failed\n");
        s_running = 0;
        s_taskHandle = NULL;
        vTaskDelete(NULL);
        return;
    }

    TRice("Modbus: polling slave %u at %u baud\n",
          s_cfg.slaveAddr, s_cfg.baud);

    uint32_t lastFast = 0;
    uint32_t lastSlow = 0;

    while (!s_stopReq) {
        uint32_t now = HAL_GetTick();

        /* Process any pending register write first */
        process_pending_write();

        /* Fast poll */
        if ((now - lastFast) >= s_cfg.fastIntervalMs) {
            poll_fast();
            lastFast = now;
            KickIwdg();
        }

        /* Slow poll */
        if ((now - lastSlow) >= s_cfg.slowIntervalMs) {
            poll_slow();
            lastSlow = now;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    Modbus_DeInit();
    TRice("Modbus: stopped (polls=%u errors=%u)\n",
          s_data.pollCount, s_data.errorCount);

    s_running = 0;
    s_taskHandle = NULL;
    vTaskDelete(NULL);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void SolisPoller_Start(const sSolisPollerCfg *cfg)
{
    if (s_running) return;

    s_cfg = *cfg;
    s_stopReq = 0;
    s_pendingWrite = 0;
    memset(&s_data, 0, sizeof(s_data));

    static const osThreadAttr_t attr = {
        .name       = "modbus",
        .stack_size = 512U * 4U,
        .priority   = (osPriority_t)osPriorityNormal,
    };

    s_running = 1;
    s_taskHandle = osThreadNew(pollerTask, NULL, &attr);
    if (s_taskHandle == NULL) {
        s_running = 0;
        TRice("Modbus: task create failed\n");
    }
}

void SolisPoller_Stop(void)
{
    if (!s_running) return;
    s_stopReq = 1;

    /* Wait for task to exit (up to 3 s) */
    for (int i = 0; i < 30 && s_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

int SolisPoller_IsRunning(void)
{
    return s_running;
}

const sSolisData *SolisPoller_GetData(void)
{
    return &s_data;
}

const sSolisPollerCfg *SolisPoller_GetConfig(void)
{
    return &s_cfg;
}

void SolisPoller_SetBaud(uint32_t baud)
{
    s_cfg.baud = baud;
}

void SolisPoller_SetSlaveAddr(uint8_t addr)
{
    s_cfg.slaveAddr = addr;
}

int SolisPoller_WriteRegister(uint16_t reg, uint16_t value)
{
    if (!s_running || s_pendingWrite) return -1;
    s_writeReg = reg;
    s_writeVal = value;
    s_pendingWrite = 1;
    return 0;
}

void SolisPoller_LogData(void)
{
    const sSolisData *d = &s_data;

    if (d->pollCount == 0) {
        TRice("Modbus: no data yet\n");
        return;
    }

    TRice("Solis polls=%u errs=%u\n", d->pollCount, d->errorCount);
    TRice(" PV: %u.%uV %u.%uA %uW\n",
          d->pv1Voltage_dV / 10, d->pv1Voltage_dV % 10,
          d->pv1Current_dA / 10, d->pv1Current_dA % 10,
          d->pvPower_W);
    TRice(" Grid: %u.%uV %u.%02uHz %dW\n",
          d->gridVoltage_dV / 10, d->gridVoltage_dV % 10,
          d->gridFrequency_cHz / 100, d->gridFrequency_cHz % 100,
          d->activePower_W);
    TRice(" Bat: %u.%uV %d.%uA SOC=%u%% %dW\n",
          d->batVoltage_dV / 10, d->batVoltage_dV % 10,
          d->batCurrent_dA / 10,
          (d->batCurrent_dA < 0 ? -d->batCurrent_dA : d->batCurrent_dA) % 10,
          d->batSoc, d->batPower_W);
    TRice(" Load: house=%uW backup=%uW meter=%dW\n",
          d->houseLoadPower_W, d->backupLoadPower_W, d->meterPower_W);
    TRice(" Today: PV=%u.%u Grid+=%u.%u Grid-=%u.%u kWh\n",
          d->todayPv_dkWh / 10, d->todayPv_dkWh % 10,
          d->todayGridImport_dkWh / 10, d->todayGridImport_dkWh % 10,
          d->todayGridExport_dkWh / 10, d->todayGridExport_dkWh % 10);
}
