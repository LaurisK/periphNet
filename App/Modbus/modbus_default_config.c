/**
 * @file    modbus_default_config.c
 * @brief   Built-in Solis inverter config.
 *
 * Register map translated from the retired solis_registers.h / solis_poller
 * transactions (wire addresses = 1-based datasheet addresses - 1):
 *   fast (5 s):  3048 x47 (PV/grid/inverter), 3132 x20 (battery/load),
 *                3262 x2 (meter power)
 *   slow (60 s): today counters + total PV singles
 *   holding (60 s): 3009 max_charge_soc / 3010 overdischarge_soc, writable
 *                with the safety ranges the old hardcoded set-topic map
 *                enforced (design §11 — must not regress).
 *
 * Point names match the retired s_sensors[] MQTT suffixes exactly, so HA
 * unique_ids and state topics are unchanged for out-of-box devices.
 */

#include "App/Modbus/modbus_default_config.h"
#include "App/system.h"

#include "modbus_config_store.h"
#include "modbus_config_compiler.h"

#include "trice.h"
#include <string.h>

const char g_modbusDefaultConfigJson[] =
"{\"devices\":[{\"slaveAddr\":1,\"topicPrefix\":\"periphnet\","
 "\"transactions\":["

  /* PV + grid + inverter (datasheet 33049-33095) */
  "{\"startAddr\":3048,\"functionCode\":\"input\",\"readPeriodS\":5,"
   "\"points\":["
    "{\"offset\":0,\"decodeType\":\"u16\",\"scale\":0.1,\"unit\":\"V\","
     "\"name\":\"pv1_voltage\"},"
    "{\"offset\":1,\"decodeType\":\"u16\",\"scale\":0.1,\"unit\":\"A\","
     "\"name\":\"pv1_current\"},"
    "{\"offset\":2,\"decodeType\":\"u16\",\"scale\":0.1,\"unit\":\"V\","
     "\"name\":\"pv2_voltage\"},"
    "{\"offset\":3,\"decodeType\":\"u16\",\"scale\":0.1,\"unit\":\"A\","
     "\"name\":\"pv2_current\"},"
    "{\"offset\":8,\"decodeType\":\"u32_be\",\"scale\":1,\"unit\":\"W\","
     "\"name\":\"pv_power\"},"
    "{\"offset\":24,\"decodeType\":\"u16\",\"scale\":0.1,\"unit\":\"V\","
     "\"name\":\"grid_voltage\"},"
    "{\"offset\":30,\"decodeType\":\"s32_be\",\"scale\":1,\"unit\":\"W\","
     "\"name\":\"active_power\"},"
    "{\"offset\":44,\"decodeType\":\"s16\",\"scale\":0.1,\"unit\":\"C\","
     "\"name\":\"inverter_temp\"},"
    "{\"offset\":45,\"decodeType\":\"u16\",\"scale\":0.01,\"unit\":\"Hz\","
     "\"name\":\"grid_frequency\"}"
   "]},"

  /* Battery + load (datasheet 33133-33152) */
  "{\"startAddr\":3132,\"functionCode\":\"input\",\"readPeriodS\":5,"
   "\"points\":["
    "{\"offset\":0,\"decodeType\":\"u16\",\"scale\":0.1,\"unit\":\"V\","
     "\"name\":\"battery_voltage\"},"
    "{\"offset\":1,\"decodeType\":\"s16\",\"scale\":0.1,\"unit\":\"A\","
     "\"name\":\"battery_current\"},"
    "{\"offset\":6,\"decodeType\":\"u16\",\"scale\":1,\"unit\":\"%\","
     "\"name\":\"battery_soc\"},"
    "{\"offset\":7,\"decodeType\":\"u16\",\"scale\":1,\"unit\":\"%\","
     "\"name\":\"battery_soh\"},"
    "{\"offset\":14,\"decodeType\":\"u16\",\"scale\":1,\"unit\":\"W\","
     "\"name\":\"house_load_power\"},"
    "{\"offset\":15,\"decodeType\":\"u16\",\"scale\":1,\"unit\":\"W\","
     "\"name\":\"backup_load_power\"},"
    "{\"offset\":16,\"decodeType\":\"s32_be\",\"scale\":1,\"unit\":\"W\","
     "\"name\":\"battery_power\"},"
    "{\"offset\":18,\"decodeType\":\"s32_be\",\"scale\":1,\"unit\":\"W\","
     "\"name\":\"grid_port_power\"}"
   "]},"

  /* Meter total power (datasheet 33263-33264) */
  "{\"startAddr\":3262,\"functionCode\":\"input\",\"readPeriodS\":5,"
   "\"points\":["
    "{\"offset\":0,\"decodeType\":\"s32_be\",\"scale\":1,\"unit\":\"W\","
     "\"name\":\"meter_power\"}"
   "]},"

  /* Daily/total energy counters (slow) */
  "{\"startAddr\":3034,\"functionCode\":\"input\",\"readPeriodS\":60,"
   "\"points\":[{\"offset\":0,\"decodeType\":\"u16\",\"scale\":0.1,"
    "\"unit\":\"kWh\",\"name\":\"today_pv\"}]},"
  "{\"startAddr\":3162,\"functionCode\":\"input\",\"readPeriodS\":60,"
   "\"points\":[{\"offset\":0,\"decodeType\":\"u16\",\"scale\":0.1,"
    "\"unit\":\"kWh\",\"name\":\"today_bat_charge\"}]},"
  "{\"startAddr\":3166,\"functionCode\":\"input\",\"readPeriodS\":60,"
   "\"points\":[{\"offset\":0,\"decodeType\":\"u16\",\"scale\":0.1,"
    "\"unit\":\"kWh\",\"name\":\"today_bat_discharge\"}]},"
  "{\"startAddr\":3170,\"functionCode\":\"input\",\"readPeriodS\":60,"
   "\"points\":[{\"offset\":0,\"decodeType\":\"u16\",\"scale\":0.1,"
    "\"unit\":\"kWh\",\"name\":\"today_grid_import\"}]},"
  "{\"startAddr\":3174,\"functionCode\":\"input\",\"readPeriodS\":60,"
   "\"points\":[{\"offset\":0,\"decodeType\":\"u16\",\"scale\":0.1,"
    "\"unit\":\"kWh\",\"name\":\"today_grid_export\"}]},"
  "{\"startAddr\":3178,\"functionCode\":\"input\",\"readPeriodS\":60,"
   "\"points\":[{\"offset\":0,\"decodeType\":\"u16\",\"scale\":0.1,"
    "\"unit\":\"kWh\",\"name\":\"today_consumption\"}]},"
  "{\"startAddr\":3028,\"functionCode\":\"input\",\"readPeriodS\":60,"
   "\"points\":[{\"offset\":0,\"decodeType\":\"u32_be\",\"scale\":1,"
    "\"unit\":\"kWh\",\"name\":\"total_pv\"}]},"

  /* Writable SOC limits (holding; safety ranges preserved from the old
   * hardcoded set-topic map). Set topics: periphnet/<name>/set */
  "{\"startAddr\":3009,\"functionCode\":\"holding\",\"readPeriodS\":60,"
   "\"points\":["
    "{\"offset\":0,\"decodeType\":\"u16\",\"scale\":1,\"unit\":\"%\","
     "\"name\":\"max_charge_soc\",\"writable\":true,"
     "\"writeMin\":70,\"writeMax\":100},"
    "{\"offset\":1,\"decodeType\":\"u16\",\"scale\":1,\"unit\":\"%\","
     "\"name\":\"overdischarge_soc\",\"writable\":true,"
     "\"writeMin\":5,\"writeMax\":40}"
   "]}"

 "]}]}";

/* --------------------------------------------------------------------------
 * Provisioning
 * -------------------------------------------------------------------------- */

typedef struct {
    const char *data;
    uint32_t    len;
    uint32_t    pos;
} sJsonSrc;

static int json_source(void *ctx, uint8_t *buf, uint32_t maxLen)
{
    sJsonSrc *s = (sJsonSrc *)ctx;
    uint32_t  n = s->len - s->pos;

    if (n > maxLen) {
        n = maxLen;
    }
    memcpy(buf, &s->data[s->pos], n);
    s->pos += n;
    return (int)n;
}

int ModbusConfig_EnsureDefault(void)
{
    if (MbCfgStore_Init() != 0) {
        return -1;
    }
    if (MbCfgStore_RegionValid(MbCfgStore_ActiveBase())) {
        return 0;
    }

    /* Blank/corrupt active region: compile the built-in Solis config into
     * it. Both boot-time callers run before any user upload can race this;
     * the flash driver mutex serializes the individual operations. */
    sJsonSrc src = {
        g_modbusDefaultConfigJson,
        (uint32_t)strlen(g_modbusDefaultConfigJson),
        0,
    };
    sMbCompileResult res;

    if (MbCfgCompile(json_source, &src, MbCfgStore_ActiveBase(),
                     KickIwdg, &res) != 0) {
        TRiceS("Modbus: default config rejected: %s\n", res.reason);
        return -1;
    }

    TRice("Modbus: default config provisioned (%u txns %u points)\n",
          res.counts.transactions, res.counts.points);
    return 0;
}
