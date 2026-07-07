/**
 * @file    mqtt_bridge.c
 * @brief   MQTT bridge — energy telemetry → MQTT → Home Assistant
 *
 * Uses the lwIP built-in MQTT client (mqtt.h).  All lwIP MQTT callbacks
 * run in tcpip_thread context — no Trice calls in callbacks.
 */

#include "App/Mqtt/mqtt_bridge.h"
#include "App/Data/telemetry.h"
#include "App/Modbus/solis_poller.h"
#include "cmsis_os.h"
#include "trice.h"

#include "lwip/apps/mqtt.h"
#include "lwip/ip_addr.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */

static sMqttBridgeCfg   s_cfg;
static mqtt_client_t    *s_client;
static volatile int      s_running;
static volatile int      s_stopReq;
static volatile int      s_connected;
static osThreadId_t      s_taskHandle;

static uint32_t          s_publishCount;
static uint32_t          s_reconnectCount;

/* --------------------------------------------------------------------------
 * Topic/payload scratch buffers (used only from mqttTask)
 * -------------------------------------------------------------------------- */

#define TOPIC_MAX   80
#define PAYLOAD_MAX 512

static char s_topic[TOPIC_MAX];
static char s_payload[PAYLOAD_MAX];

/* --------------------------------------------------------------------------
 * Integration-test support state
 *
 * Trice is forbidden in tcpip_thread, so everything observed in the
 * incoming callbacks is recorded here and logged from mqttTask
 * (service_test_hooks).
 * -------------------------------------------------------------------------- */

static volatile int s_monitorEnabled;
static volatile int s_publishNow;

/* Last incoming topic (written in publish_cb, consumed in data_cb) */
static char s_inTopic[TOPIC_MAX];

/* Deferred "MQTT sub: topic = payload" monitor log */
static struct {
    char         topic[TOPIC_MAX];
    char         payload[64];
    volatile int ready;
} s_subLog;

/* Deferred "Modbus write: reg N = V" log */
static struct {
    uint16_t     reg;
    uint16_t     val;
    int          res;
    volatile int ready;
} s_writeLog;

/* Injected message; processed via tcpip_callback so the real incoming
 * callbacks run in their native tcpip_thread context */
#define INJECT_IDLE      0
#define INJECT_REQUESTED 1
#define INJECT_ISSUED    2
#define INJECT_DONE      3

static struct {
    char         topic[TOPIC_MAX];
    char         payload[192];
    uint16_t     len;
    volatile int state;
} s_inject;

/* Writable register map: topic suffix (after "prefix/") → wire register */
typedef struct {
    const char *suffix;
    uint16_t    reg;
    uint16_t    min;
    uint16_t    max;
} sSetTopicMap;

static const sSetTopicMap s_setTopics[] = {
    { "overdischarge_soc/set", 3010,  5,  40 },
    { "max_charge_soc/set",    3009, 70, 100 },
    { NULL, 0, 0, 0 }
};

/* --------------------------------------------------------------------------
 * MQTT callbacks (run in tcpip_thread — no Trice!)
 * -------------------------------------------------------------------------- */

static void mqtt_connection_cb(mqtt_client_t *client, void *arg,
                                mqtt_connection_status_t status)
{
    (void)client;
    (void)arg;
    s_connected = (status == MQTT_CONNECT_ACCEPTED) ? 1 : 0;
}

static void mqtt_incoming_publish_cb(void *arg, const char *topic,
                                      u32_t tot_len)
{
    (void)arg;
    (void)tot_len;

    strncpy(s_inTopic, topic, sizeof(s_inTopic) - 1);
    s_inTopic[sizeof(s_inTopic) - 1] = '\0';
}

static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len,
                                   u8_t flags)
{
    (void)arg;
    (void)flags;

    char buf[64];
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    memcpy(buf, data, len);
    buf[len] = '\0';

    if (s_monitorEnabled && !s_subLog.ready) {
        strcpy(s_subLog.topic, s_inTopic);
        strcpy(s_subLog.payload, buf);
        s_subLog.ready = 1;
    }

    /* Map "prefix/<suffix>/set" onto a writable Solis register */
    size_t prefixLen = strlen(s_cfg.prefix);
    if (strncmp(s_inTopic, s_cfg.prefix, prefixLen) != 0 ||
        s_inTopic[prefixLen] != '/') {
        return;
    }
    const char *suffix = s_inTopic + prefixLen + 1;

    for (const sSetTopicMap *m = s_setTopics; m->suffix != NULL; m++) {
        if (strcmp(suffix, m->suffix) == 0) {
            int val = atoi(buf);
            if (val >= (int)m->min && val <= (int)m->max &&
                !s_writeLog.ready) {
                s_writeLog.reg   = m->reg;
                s_writeLog.val   = (uint16_t)val;
                s_writeLog.res   = SolisPoller_WriteRegister(m->reg,
                                                             (uint16_t)val);
                s_writeLog.ready = 1;
            }
            return;
        }
    }
}

/* Runs in tcpip_thread — replays an injected message through the real
 * incoming callbacks */
static void mqtt_inject_cb(void *ctx)
{
    (void)ctx;
    if (s_inject.state != INJECT_ISSUED) {
        return;
    }
    mqtt_incoming_publish_cb(NULL, s_inject.topic, s_inject.len);
    mqtt_incoming_data_cb(NULL, (const u8_t *)s_inject.payload,
                          s_inject.len, MQTT_DATA_FLAG_LAST);
    s_inject.state = INJECT_DONE;
}

/* --------------------------------------------------------------------------
 * Publish helpers
 * -------------------------------------------------------------------------- */

static void publish(const char *suffix, const char *value)
{
    snprintf(s_topic, sizeof(s_topic), "%s/%s", s_cfg.prefix, suffix);

    /* Monitor logs even without a broker so the bridge output is
     * observable in broker-less (CI) testing.  Runs in mqttTask only. */
    if (s_monitorEnabled) {
        static char mon[100];
        snprintf(mon, sizeof(mon), "%s = %s", s_topic, value);
        TRiceS("MQTT pub: %s\n", mon);
    }

    if (!s_connected || s_client == NULL) return;

    mqtt_publish(s_client, s_topic, value, strlen(value),
                 0 /* QoS 0 */, 1 /* retain */, NULL, NULL);
}

static void publish_int(const char *suffix, int32_t value)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", (int)value);
    publish(suffix, buf);
}

static void publish_float1(const char *suffix, uint16_t raw_d)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u", raw_d / 10, raw_d % 10);
    publish(suffix, buf);
}

static void publish_float2(const char *suffix, uint16_t raw_c)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%02u", raw_c / 100, raw_c % 100);
    publish(suffix, buf);
}

/* --------------------------------------------------------------------------
 * Home Assistant MQTT auto-discovery
 * -------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *suffix;
    const char *deviceClass;
    const char *stateClass;
    const char *unit;
} sHaSensorDef;

static const sHaSensorDef s_sensors[] = {
    { "PV1 Voltage",       "pv1_voltage",       "voltage",     "measurement", "V"    },
    { "PV1 Current",       "pv1_current",       "current",     "measurement", "A"    },
    { "PV2 Voltage",       "pv2_voltage",       "voltage",     "measurement", "V"    },
    { "PV2 Current",       "pv2_current",       "current",     "measurement", "A"    },
    { "PV Power",          "pv_power",          "power",       "measurement", "W"    },
    { "Grid Voltage",      "grid_voltage",      "voltage",     "measurement", "V"    },
    { "Grid Frequency",    "grid_frequency",    "frequency",   "measurement", "Hz"   },
    { "Active Power",      "active_power",      "power",       "measurement", "W"    },
    { "Temperature",       "inverter_temp",     "temperature", "measurement", "\xc2\xb0""C" },
    { "Battery Voltage",   "battery_voltage",   "voltage",     "measurement", "V"    },
    { "Battery Current",   "battery_current",   "current",     "measurement", "A"    },
    { "Battery SOC",       "battery_soc",       "battery",     "measurement", "%"    },
    { "Battery SOH",       "battery_soh",       NULL,          "measurement", "%"    },
    { "Battery Power",     "battery_power",     "power",       "measurement", "W"    },
    { "House Load",        "house_load_power",  "power",       "measurement", "W"    },
    { "Backup Load",       "backup_load_power", "power",       "measurement", "W"    },
    { "Grid Port Power",   "grid_port_power",   "power",       "measurement", "W"    },
    { "Meter Power",       "meter_power",       "power",       "measurement", "W"    },
    { "Today PV",          "today_pv",          "energy",      "total_increasing", "kWh" },
    { "Today Grid Import", "today_grid_import", "energy",      "total_increasing", "kWh" },
    { "Today Grid Export", "today_grid_export", "energy",      "total_increasing", "kWh" },
    { "Today Consumption", "today_consumption", "energy",      "total_increasing", "kWh" },
    { "Today Bat Charge",  "today_bat_charge",  "energy",      "total_increasing", "kWh" },
    { "Today Bat Discharge","today_bat_discharge","energy",    "total_increasing", "kWh" },
    { "Total PV",          "total_pv",          "energy",      "total_increasing", "kWh" },
    { NULL, NULL, NULL, NULL, NULL }
};

static void publish_ha_discovery(void)
{
    for (const sHaSensorDef *s = s_sensors; s->name != NULL; s++) {
        snprintf(s_topic, sizeof(s_topic),
                 "homeassistant/sensor/%s/%s/config",
                 s_cfg.prefix, s->suffix);

        int n = snprintf(s_payload, sizeof(s_payload),
            "{"
            "\"name\":\"%s\","
            "\"state_topic\":\"%s/%s\","
            "\"unique_id\":\"%s_%s\","
            "%s%s%s"     /* device_class (optional) */
            "\"state_class\":\"%s\","
            "\"unit_of_measurement\":\"%s\","
            "\"device\":{"
              "\"name\":\"Solis Inverter\","
              "\"manufacturer\":\"Ginlong Solis\","
              "\"identifiers\":[\"%s_solis\"],"
              "\"sw_version\":\"PeriphNet\","
              "\"via_device\":\"%s\""
            "}"
            "}",
            s->name,
            s_cfg.prefix, s->suffix,
            s_cfg.prefix, s->suffix,
            s->deviceClass ? "\"device_class\":\"" : "",
            s->deviceClass ? s->deviceClass : "",
            s->deviceClass ? "\"," : "",
            s->stateClass,
            s->unit,
            s_cfg.prefix,
            s_cfg.prefix);

        if (n > 0 && n < (int)sizeof(s_payload)) {
            mqtt_publish(s_client, s_topic, s_payload, (u16_t)n,
                         0, 1 /* retain */, NULL, NULL);
            /* Small delay between discovery messages to avoid overwhelming
               the lwIP output buffer */
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

/* --------------------------------------------------------------------------
 * Publish all current values from the telemetry snapshot
 * -------------------------------------------------------------------------- */

static void publish_all_values(void)
{
    sEnergyTelemetry snapshot;
    if (Telemetry_GetEnergy(&snapshot) != 0) return;   /* no data yet */
    const sEnergyTelemetry *d = &snapshot;

    publish_float1("pv1_voltage",    d->pv1Voltage_dV);
    publish_float1("pv1_current",    d->pv1Current_dA);
    publish_float1("pv2_voltage",    d->pv2Voltage_dV);
    publish_float1("pv2_current",    d->pv2Current_dA);
    publish_int("pv_power",          (int32_t)d->pvPower_W);
    publish_float1("grid_voltage",   d->gridVoltage_dV);
    publish_float2("grid_frequency", d->gridFrequency_cHz);
    publish_int("active_power",      d->activePower_W);

    {
        /* Temperature: signed x0.1 */
        int16_t t = d->invTemperature_dC;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d.%u",
                 t / 10, (t < 0 ? -t : t) % 10);
        publish("inverter_temp", buf);
    }

    publish_float1("battery_voltage", d->batVoltage_dV);
    {
        int16_t c = d->batCurrent_dA;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d.%u",
                 c / 10, (c < 0 ? -c : c) % 10);
        publish("battery_current", buf);
    }
    publish_int("battery_soc",       (int32_t)d->batSoc);
    publish_int("battery_soh",       (int32_t)d->batSoh);
    publish_int("battery_power",     d->batPower_W);
    publish_int("house_load_power",  (int32_t)d->houseLoadPower_W);
    publish_int("backup_load_power", (int32_t)d->backupLoadPower_W);
    publish_int("grid_port_power",   d->gridPortPower_W);
    publish_int("meter_power",       d->meterPower_W);

    publish_float1("today_pv",           d->todayPv_dkWh);
    publish_float1("today_grid_import",  d->todayGridImport_dkWh);
    publish_float1("today_grid_export",  d->todayGridExport_dkWh);
    publish_float1("today_consumption",  d->todayConsumption_dkWh);
    publish_float1("today_bat_charge",   d->todayBatChg_dkWh);
    publish_float1("today_bat_discharge",d->todayBatDsg_dkWh);
    publish_int("total_pv",             (int32_t)d->totalPv_kWh);

    s_publishCount++;
}

/* --------------------------------------------------------------------------
 * Test-hook servicing — runs in mqttTask (Trice safe).  Also called inside
 * the reconnect backoff wait so injection/publish-now stay responsive with
 * no broker present (backoff can reach 60 s).
 * -------------------------------------------------------------------------- */

static void service_test_hooks(void)
{
    if (s_inject.state == INJECT_REQUESTED) {
        s_inject.state = INJECT_ISSUED;
        if (tcpip_callback(mqtt_inject_cb, NULL) != ERR_OK) {
            s_inject.state = INJECT_IDLE;
        }
    }
    if (s_inject.state == INJECT_DONE) {
        TRiceS("MQTT inject: %s\n", s_inject.topic);
        s_inject.state = INJECT_IDLE;
    }

    if (s_subLog.ready) {
        static char line[100];
        snprintf(line, sizeof(line), "%s = %s",
                 s_subLog.topic, s_subLog.payload);
        TRiceS("MQTT sub: %s\n", line);
        s_subLog.ready = 0;
    }

    if (s_writeLog.ready) {
        if (s_writeLog.res == 0) {
            TRice("Modbus write: reg %u = %u\n",
                  s_writeLog.reg, s_writeLog.val);
        } else {
            TRice("Modbus write: reg %u rejected\n", s_writeLog.reg);
        }
        s_writeLog.ready = 0;
    }

    if (s_publishNow) {
        s_publishNow = 0;
        publish_all_values();
    }
}

/* --------------------------------------------------------------------------
 * Connect to broker
 * -------------------------------------------------------------------------- */

static int mqtt_do_connect(void)
{
    if (s_client == NULL) {
        s_client = mqtt_client_new();
        if (s_client == NULL) return -1;
    }

    struct mqtt_connect_client_info_t ci;
    memset(&ci, 0, sizeof(ci));
    ci.client_id   = s_cfg.prefix;
    ci.keep_alive  = 60;

    /* Last Will: prefix/status = "offline" */
    snprintf(s_topic, sizeof(s_topic), "%s/status", s_cfg.prefix);
    ci.will_topic  = s_topic;
    ci.will_msg    = "offline";
    ci.will_qos    = 0;
    ci.will_retain = 1;

    ip_addr_t addr;
    IP4_ADDR(&addr, s_cfg.brokerIp[0], s_cfg.brokerIp[1],
             s_cfg.brokerIp[2], s_cfg.brokerIp[3]);

    mqtt_set_inpub_callback(s_client, mqtt_incoming_publish_cb,
                            mqtt_incoming_data_cb, NULL);

    err_t err = mqtt_client_connect(s_client, &addr, s_cfg.brokerPort,
                                    mqtt_connection_cb, NULL, &ci);
    return (err == ERR_OK) ? 0 : -1;
}

/* --------------------------------------------------------------------------
 * Task
 * -------------------------------------------------------------------------- */

static void mqttTask(void *arg)
{
    (void)arg;

    TRice("MQTT: bridge starting, broker %u.%u.%u.%u:%u prefix=\"%s\"\n",
          s_cfg.brokerIp[0], s_cfg.brokerIp[1],
          s_cfg.brokerIp[2], s_cfg.brokerIp[3],
          s_cfg.brokerPort, s_cfg.prefix);

    uint32_t reconnectDelay = 2000;
    uint32_t lastPublish = 0;
    int      discoveryDone = 0;

    while (!s_stopReq) {
        service_test_hooks();

        /* Connect / reconnect */
        if (!s_connected) {
            discoveryDone = 0;
            if (mqtt_do_connect() != 0) {
                TRice("MQTT: connect failed, retry in %us\n",
                      reconnectDelay / 1000);
            }
            s_reconnectCount++;

            /* Wait for connection or timeout */
            for (uint32_t w = 0; w < reconnectDelay && !s_connected && !s_stopReq;
                 w += 100) {
                vTaskDelay(pdMS_TO_TICKS(100));
                service_test_hooks();
            }

            if (!s_connected) {
                /* Exponential backoff: 2s, 4s, 8s, …, max 60s */
                if (reconnectDelay < 60000) reconnectDelay *= 2;
                continue;
            }

            reconnectDelay = 2000;
            TRice("MQTT: connected to broker\n");

            /* Publish online status */
            char statusTopic[48];
            snprintf(statusTopic, sizeof(statusTopic), "%s/status", s_cfg.prefix);
            mqtt_publish(s_client, statusTopic, "online", 6,
                         0, 1 /* retain */, NULL, NULL);

            /* Subscribe to writable-register set topics */
            snprintf(s_topic, sizeof(s_topic), "%s/+/set", s_cfg.prefix);
            mqtt_subscribe(s_client, s_topic, 0, NULL, NULL);
        }

        /* Publish HA discovery (once per connection) */
        if (s_connected && !discoveryDone) {
            TRice("MQTT: publishing HA discovery\n");
            publish_ha_discovery();
            discoveryDone = 1;
        }

        /* Periodic data publish */
        uint32_t now = HAL_GetTick();
        if (s_connected && (now - lastPublish) >= s_cfg.publishIntervalMs) {
            publish_all_values();
            lastPublish = now;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* Disconnect */
    if (s_client != NULL) {
        if (s_connected) {
            char statusTopic[48];
            snprintf(statusTopic, sizeof(statusTopic), "%s/status", s_cfg.prefix);
            mqtt_publish(s_client, statusTopic, "offline", 7,
                         0, 1, NULL, NULL);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        mqtt_disconnect(s_client);
        /* mqtt_client_free not available in lwIP 2.1 — client is static-like */
        s_client = NULL;
    }

    TRice("MQTT: stopped (published %u times, reconnects %u)\n",
          s_publishCount, s_reconnectCount);

    s_connected = 0;
    s_running = 0;
    s_taskHandle = NULL;
    vTaskDelete(NULL);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void MqttBridge_Start(const sMqttBridgeCfg *cfg)
{
    if (s_running) return;

    s_cfg = *cfg;
    if (s_cfg.prefix[0] == '\0') {
        strcpy(s_cfg.prefix, "periphnet");
    }
    if (s_cfg.brokerPort == 0) {
        s_cfg.brokerPort = 1883;
    }
    if (s_cfg.publishIntervalMs == 0) {
        s_cfg.publishIntervalMs = 5000;
    }

    s_stopReq = 0;
    s_connected = 0;
    s_publishCount = 0;
    s_reconnectCount = 0;
    s_publishNow = 0;
    s_inject.state = INJECT_IDLE;
    s_subLog.ready = 0;
    s_writeLog.ready = 0;

    static const osThreadAttr_t attr = {
        .name       = "mqtt",
        .stack_size = 512U * 4U,
        .priority   = (osPriority_t)(osPriorityNormal - 1),
    };

    s_running = 1;
    s_taskHandle = osThreadNew(mqttTask, NULL, &attr);
    if (s_taskHandle == NULL) {
        s_running = 0;
        TRice("MQTT: task create failed\n");
    }
}

void MqttBridge_Stop(void)
{
    if (!s_running) return;
    s_stopReq = 1;

    for (int i = 0; i < 30 && s_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

int MqttBridge_IsRunning(void)
{
    return s_running;
}

int MqttBridge_IsConnected(void)
{
    return s_connected;
}

void MqttBridge_SetBrokerIp(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    s_cfg.brokerIp[0] = a;
    s_cfg.brokerIp[1] = b;
    s_cfg.brokerIp[2] = c;
    s_cfg.brokerIp[3] = d;
}

void MqttBridge_LogStatus(void)
{
    char buf[100];
    snprintf(buf, sizeof(buf),
             "%s broker=%u.%u.%u.%u:%u pub=%u reconn=%u monitor=%s",
             s_connected ? "connected" : (s_running ? "connecting" : "stopped"),
             (unsigned)s_cfg.brokerIp[0], (unsigned)s_cfg.brokerIp[1],
             (unsigned)s_cfg.brokerIp[2], (unsigned)s_cfg.brokerIp[3],
             (unsigned)s_cfg.brokerPort,
             (unsigned)s_publishCount, (unsigned)s_reconnectCount,
             s_monitorEnabled ? "on" : "off");
    TRiceS("MQTT: %s\n", buf);
}

/* --------------------------------------------------------------------------
 * Integration-test support
 * -------------------------------------------------------------------------- */

void MqttBridge_SetMonitor(int enable)
{
    s_monitorEnabled = enable ? 1 : 0;
}

int MqttBridge_GetMonitor(void)
{
    return s_monitorEnabled;
}

int MqttBridge_Inject(const char *topic, const char *payload,
                      uint16_t payloadLen)
{
    if (!s_running || s_inject.state != INJECT_IDLE) {
        return -1;
    }
    if (strlen(topic) >= sizeof(s_inject.topic) ||
        payloadLen >= sizeof(s_inject.payload)) {
        return -1;
    }

    strcpy(s_inject.topic, topic);
    memcpy(s_inject.payload, payload, payloadLen);
    s_inject.payload[payloadLen] = '\0';
    s_inject.len = payloadLen;
    s_inject.state = INJECT_REQUESTED;
    return 0;
}

void MqttBridge_PublishNow(void)
{
    s_publishNow = 1;
}
