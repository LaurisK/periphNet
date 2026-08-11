/**
 * @file    modbus_walker.c
 * @brief   Generic Modbus poll scheduler over the flash-resident config.
 */

#include "App/Modbus/modbus_walker.h"
#include "App/Modbus/modbus_rtu.h"
#include "App/Modbus/modbus_default_config.h"
#include "App/Mqtt/mqtt_bridge.h"
#include "App/system.h"

#include "modbus_config_store.h"
#include "modbus_decode.h"
#include "image_mgmt.h"

#include "cmsis_os.h"
#include "trice.h"
#include <string.h>
#include <stdio.h>

/* --------------------------------------------------------------------------
 * Configuration / task state
 * -------------------------------------------------------------------------- */

static sModbusWalkerCfg s_cfg = { 9600, 1000 };
static volatile int     s_running;
static volatile int     s_stopReq;
static osThreadId_t     s_taskHandle;

static uint32_t         s_pollCount;      /* successful transactions */
static uint32_t         s_errorCount;     /* failed transactions     */
static uint32_t         s_lapMs;          /* duration of last lap    */

/* Pending write queue (single slot, generalized with the slave address) */
static volatile int      s_pendingWrite;
static volatile uint8_t  s_writeSlave;
static volatile uint16_t s_writeReg;
static volatile uint16_t s_writeVal;

/* --------------------------------------------------------------------------
 * Fixed per-ordinal runtime state (design §8) — indices are lap-order
 * ordinals into the active config; ALL of it is reset on config swap.
 * -------------------------------------------------------------------------- */

#define DEVICE_OFFLINE_FAILS   3u
#define OFFLINE_PROBE_MIN_S    30u   /* poll throttle for offline devices */

/* CPU-only tracking arrays live in CCM (zeroed by System_Init; never
 * touched by DMA/peripherals — modbus_rtu and the SPI flash driver both do
 * polled transfers into their own buffers) */
#define CCMRAM_BSS __attribute__((section(".ccmram")))

CCMRAM_BSS static uint32_t s_lastPollTick[MB_MAX_TXNS_TOTAL];  /* 0 = never */
CCMRAM_BSS static uint32_t s_lastPublishTick[MB_MAX_POINTS_TOTAL];
CCMRAM_BSS static int32_t  s_lastValue[MB_MAX_POINTS_TOTAL];   /* scaled/CRC */
CCMRAM_BSS static uint8_t  s_hasPublished[(MB_MAX_POINTS_TOTAL + 7u) / 8u];
CCMRAM_BSS static uint8_t  s_consecFails[MB_MAX_DEVICES];
CCMRAM_BSS static uint16_t s_regBuf[MB_MAX_REGS_PER_TXN];
static uint8_t  s_devOffline;      /* bitmask: availability = offline     */
static uint8_t  s_devAnnounced;    /* bitmask: availability ever published */

static inline int bit_get(const uint8_t *bits, uint16_t idx)
{
    return (bits[idx / 8u] >> (idx % 8u)) & 1u;
}

static inline void bit_set(uint8_t *bits, uint16_t idx, int val)
{
    if (val) {
        bits[idx / 8u] |= (uint8_t)(1u << (idx % 8u));
    } else {
        bits[idx / 8u] &= (uint8_t)~(1u << (idx % 8u));
    }
}

static void reset_runtime_state(void)
{
    memset(s_lastPollTick, 0, sizeof(s_lastPollTick));
    memset(s_lastPublishTick, 0, sizeof(s_lastPublishTick));
    memset(s_lastValue, 0, sizeof(s_lastValue));
    memset(s_hasPublished, 0, sizeof(s_hasPublished));
    memset(s_consecFails, 0, sizeof(s_consecFails));
    s_devOffline   = 0;
    s_devAnnounced = 0;
}

/* --------------------------------------------------------------------------
 * Per-device availability (design §10; topic is <prefix>/availability —
 * "<prefix>/status" would collide with the bridge-wide LWT when a device's
 * topicPrefix equals the bridge prefix, as the default Solis config's does)
 * -------------------------------------------------------------------------- */

static void device_mark_result(uint8_t devOrd, const char *prefix, int ok)
{
    if (ok) {
        s_consecFails[devOrd] = 0;
        if (bit_get(&s_devOffline, devOrd) || !bit_get(&s_devAnnounced, devOrd)) {
            bit_set(&s_devOffline, devOrd, 0);
            bit_set(&s_devAnnounced, devOrd, 1);
            MqttBridge_PublishDeviceStatus(prefix, 1);
            TRiceS("Modbus: device online: %s\n", (char *)prefix);
        }
        return;
    }

    if (s_consecFails[devOrd] < 255u) {
        s_consecFails[devOrd]++;
    }
    if (s_consecFails[devOrd] >= DEVICE_OFFLINE_FAILS &&
        !bit_get(&s_devOffline, devOrd)) {
        bit_set(&s_devOffline, devOrd, 1);
        bit_set(&s_devAnnounced, devOrd, 1);
        MqttBridge_PublishDeviceStatus(prefix, 0);
        TRiceS("Modbus: device offline: %s\n", (char *)prefix);
    }
}

/* --------------------------------------------------------------------------
 * Point publish pipeline — decode, threshold/heartbeat decision, publish.
 * `regs`/`count` is the transaction's register block; points whose
 * offset+width exceed `count` are skipped (short injected frames).
 * -------------------------------------------------------------------------- */

static void publish_point(const char *devPrefix,
                          const sModbusPointRecord *pt, uint16_t ptOrd,
                          const uint16_t *regs, uint16_t count)
{
    char     topic[64];
    char     value[52];              /* max: 48 ascii chars + NUL */
    int32_t  scaled;
    uint32_t now = HAL_GetTick();
    uint8_t  width = MbRecords_RegWidth(pt->decodeType, pt->length);

    if ((uint32_t)pt->offset + width > count) {
        return;                      /* response too short for this point */
    }

    if (pt->decodeType == mbDecode_ascii) {
        int n = MbDecode_Ascii(pt, &regs[pt->offset], value, sizeof(value));
        if (n < 0) {
            return;
        }
        /* Change detection via CRC of the string (threshold ignored) */
        scaled = (int32_t)ImgMgmt_Crc32((const uint8_t *)value, (uint32_t)n);
        if (bit_get(s_hasPublished, ptOrd) && scaled == s_lastValue[ptOrd] &&
            !(pt->publishHeartbeatS != 0u &&
              now - s_lastPublishTick[ptOrd] >=
                  (uint32_t)pt->publishHeartbeatS * 1000u)) {
            return;
        }
    } else {
        scaled = MbDecode_Scaled(pt, &regs[pt->offset]);

        int due = !bit_get(s_hasPublished, ptOrd);
        if (!due) {
            if (pt->publishThreshold == 0u) {
                due = 1;             /* 0 = publish every successful read */
            } else {
                int32_t delta = scaled - s_lastValue[ptOrd];
                if (delta < 0) {
                    delta = -delta;
                }
                due = (delta >= (int32_t)pt->publishThreshold);
            }
            if (!due && pt->publishHeartbeatS != 0u &&
                now - s_lastPublishTick[ptOrd] >=
                    (uint32_t)pt->publishHeartbeatS * 1000u) {
                due = 1;
            }
        }
        if (!due) {
            return;
        }

        if (pt->decodeType == mbDecode_bitfield) {
            snprintf(value, sizeof(value), "%u", (unsigned)(uint16_t)scaled);
        } else {
            MbFormat_Scaled(value, sizeof(value), scaled, pt->scalePow10);
        }
    }

    snprintf(topic, sizeof(topic), "%s/%s", devPrefix, pt->name);
    MqttBridge_Publish(topic, value, (uint16_t)strlen(value), 1);

    s_lastValue[ptOrd]       = scaled;
    s_lastPublishTick[ptOrd] = now;
    bit_set(s_hasPublished, ptOrd, 1);
}

/* Consume the point records of the current transaction from the cursor.
 * If `regs` is non-NULL each point runs the publish pipeline; either way
 * the cursor ends up past the point sentinel and *ptOrd is advanced by the
 * number of real points. Returns 0/-1 (malformed stream). */
static int run_points(sMbCfgCursor *c, const char *devPrefix,
                      uint16_t *ptOrd, const uint16_t *regs, uint16_t count)
{
    sModbusPointRecord pt;
    int r;

    while ((r = MbCfg_NextPoint(c, &pt)) == 1) {
        if (regs != NULL && *ptOrd < MB_MAX_POINTS_TOTAL) {
            publish_point(devPrefix, &pt, *ptOrd, regs, count);
        }
        (*ptOrd)++;
    }
    return (r == 0) ? 0 : -1;
}

/* --------------------------------------------------------------------------
 * Pending write drain — between transactions so writes stay responsive
 * -------------------------------------------------------------------------- */

static void process_pending_write(void)
{
    if (!s_pendingWrite) {
        return;
    }

    uint8_t  slave = s_writeSlave;
    uint16_t reg   = s_writeReg;
    uint16_t val   = s_writeVal;
    s_pendingWrite = 0;

    eModbusErr err = Modbus_WriteSingleRegister(slave, reg, val,
                                                s_cfg.responseTimeoutMs);
    if (err == mbErr_ok) {
        TRice("Modbus: wrote reg %u = %u\n", reg, val);
    } else {
        TRice("Modbus: write reg %u failed (%d)\n", reg, (int)err);
        s_errorCount++;
    }
}

/* --------------------------------------------------------------------------
 * One full lap through the active config
 * -------------------------------------------------------------------------- */

static void walk_lap(void)
{
    sMbCfgCursor             c;
    sModbusDeviceRecord      dev;
    sModbusTransactionRecord txn;
    uint32_t                 lapStart = HAL_GetTick();
    uint8_t                  devOrd = 0;
    uint16_t                 txnOrd = 0;
    uint16_t                 ptOrd = 0;

    if (MbCfg_Open(MbCfgStore_ActiveBase(), &c) != 0) {
        return;                      /* no valid config */
    }

    while (!s_stopReq && MbCfg_NextDevice(&c, &dev) == 1) {
        int devOffline = (devOrd < MB_MAX_DEVICES)
                             ? bit_get(&s_devOffline, devOrd) : 0;
        int rt;

        while (!s_stopReq && (rt = MbCfg_NextTransaction(&c, &txn)) == 1) {
            process_pending_write();

            uint32_t now      = HAL_GetTick();
            uint32_t periodS  = txn.readPeriodS;
            if (devOffline && periodS < OFFLINE_PROBE_MIN_S) {
                periodS = OFFLINE_PROBE_MIN_S;   /* dead-slave throttle */
            }
            int due = (txnOrd < MB_MAX_TXNS_TOTAL) &&
                      (s_lastPollTick[txnOrd] == 0u ||
                       now - s_lastPollTick[txnOrd] >= periodS * 1000u);

            if (due) {
                eModbusErr err =
                    (txn.functionCode == mbFc_holding)
                        ? Modbus_ReadHoldingRegisters(dev.slaveAddr,
                                                      txn.startAddr, txn.count,
                                                      s_regBuf,
                                                      s_cfg.responseTimeoutMs)
                        : Modbus_ReadInputRegisters(dev.slaveAddr,
                                                    txn.startAddr, txn.count,
                                                    s_regBuf,
                                                    s_cfg.responseTimeoutMs);
                s_lastPollTick[txnOrd] = HAL_GetTick();
                if (s_lastPollTick[txnOrd] == 0u) {
                    s_lastPollTick[txnOrd] = 1u;   /* keep 0 = never */
                }

                if (err == mbErr_ok) {
                    s_pollCount++;
                    device_mark_result(devOrd, dev.topicPrefix, 1);
                    devOffline = 0;
                    if (run_points(&c, dev.topicPrefix, &ptOrd,
                                   s_regBuf, txn.count) != 0) {
                        return;
                    }
                } else {
                    s_errorCount++;
                    device_mark_result(devOrd, dev.topicPrefix, 0);
                    devOffline = (devOrd < MB_MAX_DEVICES)
                                     ? bit_get(&s_devOffline, devOrd) : 0;
                    if (run_points(&c, dev.topicPrefix, &ptOrd,
                                   NULL, 0) != 0) {
                        return;
                    }
                }
            } else {
                if (run_points(&c, dev.topicPrefix, &ptOrd, NULL, 0) != 0) {
                    return;
                }
            }
            txnOrd++;
        }
        if (rt != 0) {
            return;                  /* malformed stream */
        }
        devOrd++;
    }

    process_pending_write();
    s_lapMs = HAL_GetTick() - lapStart;
}

/* --------------------------------------------------------------------------
 * Task
 * -------------------------------------------------------------------------- */

static void walkerTask(void *arg)
{
    (void)arg;

    if (Modbus_Init(s_cfg.baud) != 0) {
        TRice("Modbus: port init failed\n");
        s_running = 0;
        s_taskHandle = NULL;
        vTaskDelete(NULL);
        return;
    }

    TRice("Modbus: walker started, %u baud\n", s_cfg.baud);

    while (!s_stopReq) {
        /* Config hot-swap only at a lap boundary (design §9) */
        if (MbCfgStore_IsSwapPending() &&
            MbCfgStore_RegionValid(MbCfgStore_InactiveBase())) {
            if (MbCfgStore_CommitSwap() == 0) {
                reset_runtime_state();
                TRice("Modbus: config swapped, active region %u\n",
                      (unsigned)(MbCfgStore_ActiveBase() ==
                                 EXT_FLASH_MODBUS_LUT_B_ADDR));
            }
        }

        walk_lap();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    Modbus_DeInit();
    TRice("Modbus: stopped (polls=%u errors=%u)\n", s_pollCount, s_errorCount);

    s_running = 0;
    s_taskHandle = NULL;
    vTaskDelete(NULL);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void ModbusWalker_Start(const sModbusWalkerCfg *cfg)
{
    if (s_running) {
        return;
    }

    if (cfg != NULL) {
        if (cfg->baud != 0u) {
            s_cfg.baud = cfg->baud;
        }
        if (cfg->responseTimeoutMs != 0u) {
            s_cfg.responseTimeoutMs = cfg->responseTimeoutMs;
        }
    }

    /* Blank device: provision the built-in Solis config (idempotent) */
    if (ModbusConfig_EnsureDefault() != 0) {
        TRice("Modbus: no valid config, walker not started\n");
        return;
    }

    s_stopReq = 0;
    s_pendingWrite = 0;
    s_pollCount = 0;
    s_errorCount = 0;
    reset_runtime_state();

    static const osThreadAttr_t attr = {
        .name       = "modbus",
        .stack_size = 512U * 4U,
        .priority   = (osPriority_t)osPriorityNormal,
    };

    s_running = 1;
    s_taskHandle = osThreadNew(walkerTask, NULL, &attr);
    if (s_taskHandle == NULL) {
        s_running = 0;
        TRice("Modbus: task create failed\n");
    }
}

void ModbusWalker_Stop(void)
{
    if (!s_running) {
        return;
    }
    s_stopReq = 1;

    /* Wait for task to exit (up to 3 s; a full-timeout lap can exceed this,
     * the task still cleans itself up afterwards) */
    for (int i = 0; i < 30 && s_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

int ModbusWalker_IsRunning(void)
{
    return s_running;
}

void ModbusWalker_SetBaud(uint32_t baud)
{
    if (baud != 0u) {
        s_cfg.baud = baud;
    }
}

uint32_t ModbusWalker_GetBaud(void)
{
    return s_cfg.baud;
}

int ModbusWalker_WriteRegister(uint8_t slaveAddr, uint16_t reg, uint16_t value)
{
    if (!s_running || s_pendingWrite) {
        return -1;
    }
    s_writeSlave   = slaveAddr;
    s_writeReg     = reg;
    s_writeVal     = value;
    s_pendingWrite = 1;
    return 0;
}

int ModbusWalker_InjectResponse(uint8_t slaveAddr, uint16_t startAddr,
                                const uint16_t *regs, uint16_t count)
{
    sMbCfgCursor             c;
    sModbusDeviceRecord      dev;
    sModbusTransactionRecord txn;
    uint16_t                 ptOrd = 0;

    /* Locate the {slaveAddr, startAddr} transaction, tracking ordinals so
     * the publish pipeline uses the same state slots as live polling.
     * Races with a running walker lap are field-wise and benign — this is
     * test tooling, normally used with the RS485 port disabled. */
    if (MbCfg_Open(MbCfgStore_ActiveBase(), &c) != 0) {
        TRice("Walker inject: no valid config\n");
        return -1;
    }

    while (MbCfg_NextDevice(&c, &dev) == 1) {
        int rt;
        while ((rt = MbCfg_NextTransaction(&c, &txn)) == 1) {
            if (dev.slaveAddr == slaveAddr && txn.startAddr == startAddr) {
                TRice("Walker inject: slave %u addr %u regs %u\n",
                      slaveAddr, startAddr, count);
                if (run_points(&c, dev.topicPrefix, &ptOrd,
                               regs, count) != 0) {
                    return -1;
                }
                return 0;
            }
            if (run_points(&c, dev.topicPrefix, &ptOrd, NULL, 0) != 0) {
                return -1;
            }
        }
        if (rt != 0) {
            return -1;
        }
    }

    TRice("Walker inject: no matching transaction (slave %u addr %u)\n",
          slaveAddr, startAddr);
    return -1;
}

void ModbusWalker_ForceRepublish(void)
{
    memset(s_hasPublished, 0, sizeof(s_hasPublished));
    memset(s_lastPollTick, 0, sizeof(s_lastPollTick));
    memset(s_lastPublishTick, 0, sizeof(s_lastPublishTick));
}

void ModbusWalker_LogStatus(void)
{
    sMbCfgCounts counts;
    int          haveCounts =
        (MbCfg_Count(MbCfgStore_ActiveBase(), &counts) == 0);

    char buf[110];
    snprintf(buf, sizeof(buf),
             "%s baud=%u polls=%u errors=%u lap=%ums swap=%s",
             s_running ? "running" : "stopped",
             (unsigned)s_cfg.baud, (unsigned)s_pollCount,
             (unsigned)s_errorCount, (unsigned)s_lapMs,
             MbCfgStore_IsSwapPending() ? "pending" : "none");
    TRiceS("Modbus walker: %s\n", buf);

    if (haveCounts) {
        TRice("Modbus config: region %u, %u devices %u txns %u points\n",
              (unsigned)(MbCfgStore_ActiveBase() ==
                         EXT_FLASH_MODBUS_LUT_B_ADDR),
              counts.devices, counts.transactions, counts.points);
    } else {
        TRice("Modbus config: none valid\n");
    }

    /* Per-device failure counters */
    sMbCfgCursor        c;
    sModbusDeviceRecord dev;
    uint8_t             devOrd = 0;
    if (MbCfg_Open(MbCfgStore_ActiveBase(), &c) == 0) {
        while (MbCfg_NextDevice(&c, &dev) == 1 && devOrd < MB_MAX_DEVICES) {
            sModbusTransactionRecord txn;
            sModbusPointRecord       pt;
            int rt;
            /* skip this device's records */
            while ((rt = MbCfg_NextTransaction(&c, &txn)) == 1) {
                int rp;
                while ((rp = MbCfg_NextPoint(&c, &pt)) == 1) { }
                if (rp != 0) { return; }
            }
            if (rt != 0) { return; }

            char dbuf[64];
            snprintf(dbuf, sizeof(dbuf), "%s slave=%u fails=%u%s",
                     dev.topicPrefix, dev.slaveAddr, s_consecFails[devOrd],
                     bit_get(&s_devOffline, devOrd) ? " OFFLINE" : "");
            TRiceS(" device: %s\n", dbuf);
            devOrd++;
        }
    }
}
