/**
 * @file    mqtt_bridge.c
 * @brief   MQTT bridge — Modbus config points → MQTT → Home Assistant
 *
 * Uses the lwIP built-in MQTT client (mqtt.h).  All lwIP MQTT callbacks
 * run in tcpip_thread context — no Trice calls and no flash access in
 * callbacks; everything is deferred to mqttTask.  All raw lwIP MQTT calls
 * go through the tcpip core lock because MqttBridge_Publish is also called
 * from the Modbus walker task.
 */

#include "App/Mqtt/mqtt_bridge.h"
#include "App/Modbus/modbus_walker.h"
#include "cmsis_os.h"
#include "trice.h"

#include "modbus_config_store.h"
#include "modbus_decode.h"
#include "modbus_units.h"

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
#define PAYLOAD_MAX 768

/* CPU-only buffers → CCM RAM (never handed to DMA: mqtt_publish copies
 * into the client's SRAM output ring, lwIP copies that into SRAM pbufs) */
#define CCMRAM_BSS __attribute__((section(".ccmram")))

static char s_topic[TOPIC_MAX] CCMRAM_BSS;
static char s_payload[PAYLOAD_MAX] CCMRAM_BSS;

/* --------------------------------------------------------------------------
 * Deferred work state
 *
 * Trice and flash access are forbidden in tcpip_thread, so the incoming
 * callbacks only record what happened; mqttTask (service_test_hooks)
 * resolves set-topic writes against the flash config and does the logging.
 * -------------------------------------------------------------------------- */

static volatile int s_monitorEnabled;

/* Last incoming topic (written in publish_cb, consumed in data_cb) */
static char s_inTopic[TOPIC_MAX] CCMRAM_BSS;

/* Deferred "MQTT sub: topic = payload" monitor log */
static struct {
    char         topic[TOPIC_MAX];
    char         payload[64];
    volatile int ready;
} s_subLog CCMRAM_BSS;

/* Deferred inbound ".../set" message awaiting config lookup in mqttTask */
static struct {
    char         topic[TOPIC_MAX];
    char         payload[32];
    volatile int ready;
} s_pendingSet CCMRAM_BSS;

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
} s_inject CCMRAM_BSS;

/* --------------------------------------------------------------------------
 * MQTT callbacks (run in tcpip_thread — no Trice, no flash!)
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

    /* Hand ".../set" messages to mqttTask — resolving them needs the flash
     * config, which must not be touched from tcpip_thread */
    size_t tlen = strlen(s_inTopic);
    if (tlen > 4u && strcmp(&s_inTopic[tlen - 4u], "/set") == 0 &&
        !s_pendingSet.ready && strlen(buf) < sizeof(s_pendingSet.payload)) {
        strcpy(s_pendingSet.topic, s_inTopic);
        strcpy(s_pendingSet.payload, buf);
        s_pendingSet.ready = 1;
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
 * Publish core — the single path to the raw lwIP publish API.
 * Called from mqttTask and the Modbus walker task; the tcpip core lock
 * serializes both against tcpip_thread and each other.
 * -------------------------------------------------------------------------- */

static int do_publish(const char *topic, const char *payload, uint16_t len,
                      uint8_t retain, int monitorLog)
{
    if (monitorLog && s_monitorEnabled) {
        char mon[110];
        snprintf(mon, sizeof(mon), "%s = %s", topic, payload);
        TRiceS("MQTT pub: %s\n", mon);
    }

    if (!s_connected || s_client == NULL) {
        return -1;
    }

    LOCK_TCPIP_CORE();
    err_t err = mqtt_publish(s_client, topic, payload, len,
                             0 /* QoS 0 */, retain, NULL, NULL);
    UNLOCK_TCPIP_CORE();

    if (err != ERR_OK) {
        return -1;
    }
    s_publishCount++;
    return 0;
}

int MqttBridge_Publish(const char *topic, const char *payload,
                       uint16_t payloadLen, uint8_t retain)
{
    return do_publish(topic, payload, payloadLen, retain, 1);
}

void MqttBridge_PublishDeviceStatus(const char *topicPrefix, int online)
{
    char topic[48];
    snprintf(topic, sizeof(topic), "%s/availability", topicPrefix);
    do_publish(topic, online ? "online" : "offline",
               online ? 6 : 7, 1 /* retain */, 1);
}

/* --------------------------------------------------------------------------
 * Config walking helpers (mqttTask only)
 * -------------------------------------------------------------------------- */

/* Consume the remaining transactions/points of the current device. */
static int skip_device_body(sMbCfgCursor *c)
{
    sModbusTransactionRecord txn;
    sModbusPointRecord       pt;
    int rt, rp;

    while ((rt = MbCfg_NextTransaction(c, &txn)) == 1) {
        while ((rp = MbCfg_NextPoint(c, &pt)) == 1) { }
        if (rp != 0) {
            return -1;
        }
    }
    return (rt == 0) ? 0 : -1;
}

/* --------------------------------------------------------------------------
 * Set-topic resolution — "<topicPrefix>/<name>/set" against the active
 * config's writable points (design §11). Runs in mqttTask.
 * -------------------------------------------------------------------------- */

static void handle_set_message(const char *topic, const char *payload)
{
    char prefix[MB_TOPIC_PREFIX_LEN];
    char name[MB_POINT_NAME_LEN];

    /* Split "<prefix>/<name>/set" — prefix and name contain no '/' */
    const char *slash1 = strchr(topic, '/');
    if (slash1 == NULL) {
        return;
    }
    const char *slash2 = strchr(slash1 + 1, '/');
    if (slash2 == NULL || strcmp(slash2, "/set") != 0) {
        return;
    }

    size_t plen = (size_t)(slash1 - topic);
    size_t nlen = (size_t)(slash2 - slash1 - 1);
    if (plen == 0u || plen >= sizeof(prefix) ||
        nlen == 0u || nlen >= sizeof(name)) {
        return;
    }
    memcpy(prefix, topic, plen);
    prefix[plen] = '\0';
    memcpy(name, slash1 + 1, nlen);
    name[nlen] = '\0';

    sMbPointLookup lk;
    if (MbCfg_FindWritablePoint(prefix, name, &lk) != 0) {
        TRiceS("MQTT set: no writable point: %s\n", (char *)topic);
        return;
    }

    /* Parse the payload in the point's scaled-int domain (= raw register
     * value) and enforce the config's write range — this preserves the
     * safety behaviour of the old hardcoded Solis set-topic map. */
    int32_t scaled;
    if (MbParse_Scaled(payload, lk.point.scalePow10, &scaled) != 0 ||
        scaled < lk.point.writeMin || scaled > lk.point.writeMax ||
        scaled < 0 || scaled > UINT16_MAX) {
        TRice("Modbus write: reg %u rejected\n", lk.regAddr);
        return;
    }

    if (ModbusWalker_WriteRegister(lk.slaveAddr, lk.regAddr,
                                   (uint16_t)scaled) == 0) {
        TRice("Modbus write: reg %u = %u\n", lk.regAddr, (unsigned)scaled);
    } else {
        TRice("Modbus write: reg %u rejected\n", lk.regAddr);
    }
}

/* --------------------------------------------------------------------------
 * Home Assistant MQTT auto-discovery — generated from the active config
 * (design §12): sensors for every point, an additional number entity for
 * writable points. Grouped per device by topicPrefix.
 * -------------------------------------------------------------------------- */

static void ha_publish_entity(const char *devPrefix,
                              const sModbusPointRecord *pt,
                              uint16_t regAddr, int asNumber)
{
    (void)regAddr;
    const sMbUnitInfo *unit = MbUnits_FromCode(pt->unit);
    int n;

    snprintf(s_topic, sizeof(s_topic), "homeassistant/%s/%s/%s%s/config",
             asNumber ? "number" : "sensor", devPrefix, pt->name,
             asNumber ? "_set" : "");

    n = snprintf(s_payload, sizeof(s_payload),
        "{"
        "\"name\":\"%s\","
        "\"state_topic\":\"%s/%s\","
        "\"unique_id\":\"%s_%s%s\",",
        pt->name,
        devPrefix, pt->name,
        devPrefix, pt->name, asNumber ? "_set" : "");

    if (asNumber) {
        char minBuf[16], maxBuf[16], stepBuf[16];
        MbFormat_Scaled(minBuf, sizeof(minBuf), pt->writeMin, pt->scalePow10);
        MbFormat_Scaled(maxBuf, sizeof(maxBuf), pt->writeMax, pt->scalePow10);
        MbFormat_Scaled(stepBuf, sizeof(stepBuf), 1, pt->scalePow10);
        n += snprintf(s_payload + n, sizeof(s_payload) - (size_t)n,
            "\"command_topic\":\"%s/%s/set\","
            "\"min\":%s,\"max\":%s,\"step\":%s,",
            devPrefix, pt->name, minBuf, maxBuf, stepBuf);
    } else {
        if (unit != NULL && unit->haDeviceClass != NULL) {
            n += snprintf(s_payload + n, sizeof(s_payload) - (size_t)n,
                "\"device_class\":\"%s\",", unit->haDeviceClass);
        }
        if (pt->decodeType != mbDecode_ascii && unit != NULL) {
            n += snprintf(s_payload + n, sizeof(s_payload) - (size_t)n,
                "\"state_class\":\"%s\",", unit->haStateClass);
        }
    }

    if (unit != NULL && unit->haUnit != NULL) {
        n += snprintf(s_payload + n, sizeof(s_payload) - (size_t)n,
            "\"unit_of_measurement\":\"%s\",", unit->haUnit);
    }

    n += snprintf(s_payload + n, sizeof(s_payload) - (size_t)n,
        "\"availability\":[{\"topic\":\"%s/status\"},"
        "{\"topic\":\"%s/availability\"}],"
        "\"availability_mode\":\"all\","
        "\"device\":{"
          "\"name\":\"%s\","
          "\"identifiers\":[\"%s\"],"
          "\"sw_version\":\"PeriphNet\","
          "\"via_device\":\"%s\""
        "}"
        "}",
        s_cfg.prefix, devPrefix,
        devPrefix, devPrefix, s_cfg.prefix);

    if (n > 0 && n < (int)sizeof(s_payload)) {
        do_publish(s_topic, s_payload, (uint16_t)n, 1 /* retain */, 0);
        /* Small delay between discovery messages to avoid overwhelming
           the lwIP output buffer */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void publish_ha_discovery(void)
{
    sMbCfgCursor             c;
    sModbusDeviceRecord      dev;
    sModbusTransactionRecord txn;
    sModbusPointRecord       pt;

    if (MbCfg_Open(MbCfgStore_ActiveBase(), &c) != 0) {
        TRice("MQTT: no valid Modbus config for HA discovery\n");
        return;
    }

    while (MbCfg_NextDevice(&c, &dev) == 1 && !s_stopReq) {
        int rt;
        while ((rt = MbCfg_NextTransaction(&c, &txn)) == 1) {
            int rp;
            while ((rp = MbCfg_NextPoint(&c, &pt)) == 1) {
                uint16_t regAddr = (uint16_t)(txn.startAddr + pt.offset);
                ha_publish_entity(dev.topicPrefix, &pt, regAddr, 0);
                if (pt.flags & MB_POINT_FLAG_WRITABLE) {
                    ha_publish_entity(dev.topicPrefix, &pt, regAddr, 1);
                }
            }
            if (rp != 0) {
                return;
            }
        }
        if (rt != 0) {
            return;
        }
    }
}

/* --------------------------------------------------------------------------
 * Per-device set-topic subscriptions (walked from the active config)
 * -------------------------------------------------------------------------- */

static void subscribe_set_topics(void)
{
    sMbCfgCursor        c;
    sModbusDeviceRecord dev;
    int                 any = 0;

    if (MbCfg_Open(MbCfgStore_ActiveBase(), &c) == 0) {
        while (MbCfg_NextDevice(&c, &dev) == 1) {
            snprintf(s_topic, sizeof(s_topic), "%s/+/set", dev.topicPrefix);
            LOCK_TCPIP_CORE();
            mqtt_subscribe(s_client, s_topic, 0, NULL, NULL);
            UNLOCK_TCPIP_CORE();
            any = 1;
            if (skip_device_body(&c) != 0) {
                break;
            }
        }
    }

    if (!any) {
        /* No valid config — keep the old bridge-prefix subscription */
        snprintf(s_topic, sizeof(s_topic), "%s/+/set", s_cfg.prefix);
        LOCK_TCPIP_CORE();
        mqtt_subscribe(s_client, s_topic, 0, NULL, NULL);
        UNLOCK_TCPIP_CORE();
    }
}

/* --------------------------------------------------------------------------
 * Test-hook servicing — runs in mqttTask (Trice safe).  Also called inside
 * the reconnect backoff wait so injection stays responsive with no broker
 * present (backoff can reach 60 s).
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

    if (s_pendingSet.ready) {
        handle_set_message(s_pendingSet.topic, s_pendingSet.payload);
        s_pendingSet.ready = 0;
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

    /* Last Will: prefix/status = "offline" (bridge-wide; per-device
     * availability is walker-driven, see MqttBridge_PublishDeviceStatus) */
    snprintf(s_topic, sizeof(s_topic), "%s/status", s_cfg.prefix);
    ci.will_topic  = s_topic;
    ci.will_msg    = "offline";
    ci.will_qos    = 0;
    ci.will_retain = 1;

    ip_addr_t addr;
    IP4_ADDR(&addr, s_cfg.brokerIp[0], s_cfg.brokerIp[1],
             s_cfg.brokerIp[2], s_cfg.brokerIp[3]);

    LOCK_TCPIP_CORE();
    mqtt_set_inpub_callback(s_client, mqtt_incoming_publish_cb,
                            mqtt_incoming_data_cb, NULL);

    err_t err = mqtt_client_connect(s_client, &addr, s_cfg.brokerPort,
                                    mqtt_connection_cb, NULL, &ci);
    UNLOCK_TCPIP_CORE();
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
    int      discoveryDone = 0;
    uint32_t discoveryBase = 0;

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

            /* Publish online status (bridge-wide LWT counterpart) */
            char statusTopic[48];
            snprintf(statusTopic, sizeof(statusTopic), "%s/status", s_cfg.prefix);
            do_publish(statusTopic, "online", 6, 1 /* retain */, 0);

            /* Subscribe to writable-point set topics per config device */
            subscribe_set_topics();
        }

        /* Publish HA discovery once per connection — and again after a
         * config hot-swap (active region flipped) so new devices/points
         * appear without a reconnect */
        if (s_connected &&
            (!discoveryDone || discoveryBase != MbCfgStore_ActiveBase())) {
            TRice("MQTT: publishing HA discovery\n");
            discoveryBase = MbCfgStore_ActiveBase();
            publish_ha_discovery();
            if (discoveryDone) {
                subscribe_set_topics();   /* config changed: re-subscribe */
            }
            discoveryDone = 1;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* Disconnect */
    if (s_client != NULL) {
        if (s_connected) {
            char statusTopic[48];
            snprintf(statusTopic, sizeof(statusTopic), "%s/status", s_cfg.prefix);
            do_publish(statusTopic, "offline", 7, 1, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        LOCK_TCPIP_CORE();
        mqtt_disconnect(s_client);
        UNLOCK_TCPIP_CORE();
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

    s_stopReq = 0;
    s_connected = 0;
    s_publishCount = 0;
    s_reconnectCount = 0;
    s_inject.state = INJECT_IDLE;
    s_subLog.ready = 0;
    s_pendingSet.ready = 0;

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
    ModbusWalker_ForceRepublish();
}
