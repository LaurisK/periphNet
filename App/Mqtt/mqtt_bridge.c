/**
 * @file    mqtt_bridge.c
 * @brief   MQTT bridge — Modbus config points → MQTT → Home Assistant
 *
 * Uses the lwIP built-in MQTT client (mqtt.h).  All lwIP MQTT callbacks
 * run in tcpip_thread context — no Trice calls and no flash access in
 * callbacks; everything is deferred to mqttTask.  All raw lwIP MQTT calls
 * go through the tcpip core lock.  Since docs/modbus.md §10 step 5 every
 * publish happens on mqttTask, so the lock is off the Modbus sequence path
 * entirely.
 */

#include "App/Mqtt/mqtt_bridge.h"
#include "App/Modbus/modbus.h"
#include "App/Mon/sysmon.h"
#include "cmsis_os.h"
#include "trice.h"

/* Shared/Modbus TYPES and pure functions only: decode/format is translation
 * and stays there for consumers to call (docs/modbus.md §5.3).  The flash
 * accessors (MbCfg_*, MbCfgStore_*) are gone from this file — everything the
 * bridge knows about the config now arrives as a catalogue. */
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
 * Publish core — the single path to the raw lwIP publish API, reached only
 * from mqttTask; the tcpip core lock serializes it against tcpip_thread.
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

static void publish_device_status(const char *topicPrefix, int online)
{
    char topic[48];
    snprintf(topic, sizeof(topic), "%s/availability", topicPrefix);
    do_publish(topic, online ? "online" : "offline",
               online ? 6 : 7, 1 /* retain */, 1);
}

/* ==========================================================================
 * The Modbus subscriber (docs/modbus.md §4.10)
 *
 * The bridge is a CONSUMER of the Modbus module: it gets every decoded reading
 * through Modbus_Subscribe and decides for itself what to publish.  It has no
 * access to the config at all — the catalogue is how it learns what exists.
 *
 * THE CALLBACK RUNS ON THE MODBUS TASK AND MUST NOT BLOCK, so it does the one
 * thing §4.7 prescribes for a consumer that does real work: allocate, copy the
 * fields it needs, post the pointer to its own queue, return.  Formatting,
 * LOCK_TCPIP_CORE and mqtt_publish all happen on mqttTask, which is what keeps
 * the tcpip core lock off the Modbus sequence path.
 *
 * Three things the module used to own and no longer does:
 *
 *   - Publish policy.  Every read is published; there is no threshold and no
 *     heartbeat anywhere.  Behaviour-neutral for the shipped config, which
 *     authors no publish block on any of its points.
 *   - HA AVAILABILITY, which is MQTT's semantic and is computed where it is
 *     published.  A device answering EXCEPTIONS is answering, so an exception
 *     reply must not count towards being offline (§8.3) — three misconfigured
 *     reads used to mark a healthy device offline.
 *   - Write range checking.  The capability states the bounds and the module
 *     enforces them (§4.6); the bridge stays the source of the HA number
 *     entity's min/max, which is rendering, not policy.
 * ========================================================================== */

#define DEVICE_OFFLINE_FAILS   3u
#define BRIDGE_QUEUE_DEPTH    24u
#define BRIDGE_MAX_WRITABLE   16u

typedef enum {
    bmsg_sample = 0,
    bmsg_desc,
} eBridgeMsgKind;

/* One borrowed event, copied.  ~120 B, freed by mqttTask after publishing;
 * heap_4 coalesces adjacent free blocks, so cycling equal-sized copies
 * recycles cleanly (§4.7). */
typedef struct {
    uint8_t  kind;
    uint8_t  last;                          /* desc: end of the burst      */
    uint8_t  isText;
    uint8_t  devOrd, decodeType, unit, flags;
    int8_t   scalePow10;
    uint16_t ptOrd;
    uint32_t period_sec;
    int32_t  value;
    int32_t  writeMin, writeMax;
    char     prefix[MB_TOPIC_PREFIX_LEN];
    char     name[MB_POINT_NAME_LEN];
    char     text[52];
} sBridgeMsg;

static int                s_modbusSub = -1;
static osMessageQueueId_t s_pubQueue;
static uint32_t           s_droppedMsgs;
static volatile int       s_catalogueLost;  /* a desc did not fit the queue */

/* A catalogue burst is the heaviest thing the dispatcher does (§4.5) — 27
 * back-to-back callbacks, and mqttTask is a priority below the modbus task, so
 * it cannot drain while the burst runs.  A burst is therefore always bigger
 * than any queue worth paying for, and simply re-asking would replay from
 * entry 0 and overflow at the same place forever.
 *
 * So a re-ask RESUMES: entries already published are skipped by ordinal, and
 * each round gets further.  s_discoveryDone counts what mqttTask has actually
 * published in this generation; s_burstIdx is the modbus task's position in
 * the burst it is dispatching.  A generation restarts (0) on connect, on a
 * config swap and on `mqtt publish now`.  The two tasks share a 16-bit
 * counter, so the worst a lost update can do is republish a retained
 * discovery message, which is idempotent. */
static volatile uint16_t  s_discoveryDone;
static uint16_t           s_burstIdx;

/* A burst replaces everything the previous one said, and the only marker of a
 * new burst is the entry after a `last`.  Modbus task only. */
static int                s_catFresh = 1;

/* Availability is counted in the callback (cheap) and published by mqttTask
 * (not cheap: it needs the core lock). */
static uint8_t s_devFails[MB_MAX_DEVICES];
static uint8_t s_devOffline;      /* bitmask */
static uint8_t s_devAnnounced;    /* bitmask */
static uint8_t s_devAvailDirty;   /* bitmask: publish pending */
static char    s_devPrefix[MB_MAX_DEVICES][MB_TOPIC_PREFIX_LEN];

static sBridgeMsg *msg_alloc(uint8_t kind)
{
    sBridgeMsg *m = (sBridgeMsg *)pvPortMalloc(sizeof(*m));

    if (m == NULL) {
        s_droppedMsgs++;
        return NULL;
    }
    memset(m, 0, sizeof(*m));
    m->kind = kind;
    return m;
}

static int msg_post(sBridgeMsg *m)
{
    /* Never block: this is the modbus task, and a consumer may not stall the
     * engine (§4.7).  A full queue drops, and says so. */
    if (s_pubQueue == NULL ||
        osMessageQueuePut(s_pubQueue, &m, 0, 0) != osOK) {
        vPortFree(m);
        s_droppedMsgs++;
        return -1;
    }
    return 0;
}

static void msg_fill_desc(sBridgeMsg *m, const sModbusPointDesc *pt)
{
    snprintf(m->prefix, sizeof(m->prefix), "%s", pt->topicPrefix);
    snprintf(m->name, sizeof(m->name), "%s", pt->name);
    m->devOrd     = pt->devOrd;
    m->ptOrd      = pt->ptOrd;
    m->decodeType = pt->decodeType;
    m->unit       = pt->unit;
    m->flags      = pt->flags;
    m->scalePow10 = pt->scalePow10;
    m->period_sec = pt->period_sec;
    m->writeMin   = pt->writeMin;
    m->writeMax   = pt->writeMax;
}

static void device_mark_result(uint8_t devOrd, int16_t err)
{
    if (devOrd >= MB_MAX_DEVICES) {
        return;
    }

    uint8_t bit = (uint8_t)(1u << devOrd);

    /* An exception reply is an answer: the slave is there and talking. */
    int answering = (err == mbErr_ok) ||
                    (err <= mbErr_excIllegalFunction && err >= mbErr_excOther);

    if (answering) {
        s_devFails[devOrd] = 0;
        if ((s_devOffline & bit) || !(s_devAnnounced & bit)) {
            s_devOffline    &= (uint8_t)~bit;
            s_devAnnounced  |= bit;
            s_devAvailDirty |= bit;
        }
        return;
    }

    if (s_devFails[devOrd] < 255u) {
        s_devFails[devOrd]++;
    }
    if (s_devFails[devOrd] >= DEVICE_OFFLINE_FAILS && !(s_devOffline & bit)) {
        s_devOffline    |= bit;
        s_devAnnounced  |= bit;
        s_devAvailDirty |= bit;
    }
}

/* Runs in the MODBUS task, synchronously (§4.7). */
static void modbus_event_cb(const sModbusEvent *ev, void *ctx)
{
    (void)ctx;

    switch (ev->type) {
    case mbEvt_sample: {
        sBridgeMsg *m = msg_alloc(bmsg_sample);
        if (m == NULL) {
            return;
        }
        msg_fill_desc(m, ev->u.sample.pt);
        if (ev->u.sample.text != NULL) {
            snprintf(m->text, sizeof(m->text), "%s", ev->u.sample.text);
            m->isText = 1u;
        } else {
            m->value = ev->u.sample.value;
        }
        msg_post(m);
        break;
    }

    case mbEvt_pointDesc: {
        int skip;

        if (s_catFresh) {
            s_burstIdx = 0;
            s_catFresh = 0;
        }
        skip = (ev->u.desc.pt == NULL) ||
               (s_burstIdx < s_discoveryDone) || s_catalogueLost;
        if (ev->u.desc.pt != NULL) {
            s_burstIdx++;
        }
        if (ev->u.desc.last) {
            s_catFresh = 1;
        }

        /* The end of the burst is always announced, even when its last entry
         * is one this round had no work for. */
        if (skip && !ev->u.desc.last) {
            break;
        }

        sBridgeMsg *m = msg_alloc(bmsg_desc);
        if (m == NULL) {
            s_catalogueLost = 1;
            break;
        }
        if (!skip) {
            msg_fill_desc(m, ev->u.desc.pt);
        }
        m->last = ev->u.desc.last;

        /* A dropped catalogue entry is a missing HA entity, so it is not just
         * counted: the burst is asked for again once the queue has drained,
         * and resumes where this round stopped (§4.5). */
        if (msg_post(m) != 0) {
            s_catalogueLost = 1;
        }
        break;
    }

    case mbEvt_txn:
        device_mark_result(ev->u.txn.devOrd, ev->u.txn.err);
        break;

    case mbEvt_config:
        /* Ordinals may mean something else now.  A fresh catalogue follows on
         * its own, and it is what rebuilds everything below. */
        memset(s_devFails, 0, sizeof(s_devFails));
        s_devOffline    = 0;
        s_devAnnounced  = 0;
        s_devAvailDirty = 0;
        s_discoveryDone = 0;      /* a new generation: publish it all again */
        s_catalogueLost = 0;
        s_catFresh      = 1;
        break;

    default:
        break;
    }
}

/* --------------------------------------------------------------------------
 * What the bridge remembers, and it is all built from the catalogue
 *
 * Writable points only.  A sample carries its own prefix and name, so
 * publishing needs no map at all; what does need one is the inbound
 * "<prefix>/<name>/set" topic, which has to become a {devOrd, ptOrd} pair
 * because that is the only way to address a reading (§4.10).
 * -------------------------------------------------------------------------- */

typedef struct {
    char     prefix[MB_TOPIC_PREFIX_LEN];
    char     name[MB_POINT_NAME_LEN];
    uint16_t ptOrd;
    uint8_t  devOrd;
    int8_t   scalePow10;
} sWritablePoint;

static sWritablePoint s_writable[BRIDGE_MAX_WRITABLE];
static uint8_t        s_writableCount;

/* Prefixes seen in this catalogue, so "<prefix>/+/set" is subscribed once. */
static char    s_subPrefix[MB_MAX_DEVICES][MB_TOPIC_PREFIX_LEN];
static uint8_t s_subPrefixCount;

static void catalogue_reset(void)
{
    s_writableCount  = 0;
    s_subPrefixCount = 0;
    memset(s_subPrefix, 0, sizeof(s_subPrefix));
    memset(s_devPrefix, 0, sizeof(s_devPrefix));
}

/* --------------------------------------------------------------------------
 * Set-topic resolution — "<prefix>/<name>/set" against the catalogue's
 * writable points, then Modbus_Request.  Runs in mqttTask.
 *
 * The bridge does NOT publish the read-back itself: an rw point is read back
 * by the module and arrives as an ordinary sample, so there is one publish
 * path and no dedupe question.  The completion callback is only a
 * success/failure line — which is why the item array may be static and never
 * has to be read.
 * -------------------------------------------------------------------------- */

static sModbusReqItem s_setItem;
static char           s_setTopic[TOPIC_MAX];
static volatile int   s_setBusy;

static void set_request_done(const sModbusReqReply *rep, void *ctx)
{
    (void)ctx;

    /* The array is ours again exactly here, and not one moment earlier. */
    if (rep->count > 0 && rep->items[0].result != mbErr_ok) {
        char buf[110];
        snprintf(buf, sizeof(buf), "%s rejected (%d)",
                 s_setTopic, (int)rep->items[0].result);
        TRiceS("MQTT: set %s\n", buf);
    }
    s_setBusy = 0;
}

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

    const sWritablePoint *wp = NULL;
    for (uint8_t i = 0; i < s_writableCount; i++) {
        if (strcmp(s_writable[i].prefix, prefix) == 0 &&
            strcmp(s_writable[i].name, name) == 0) {
            wp = &s_writable[i];
            break;
        }
    }

    char line[110];
    if (wp == NULL) {
        snprintf(line, sizeof(line), "%s/%s rejected (no writable point)",
                 prefix, name);
        TRiceS("MQTT: set %s\n", line);
        return;
    }

    /* Parse in the point's scaled-integer domain (= the raw register domain).
     * The RANGE is the module's business, not this file's (§4.6). */
    int32_t scaled;
    if (MbParse_Scaled(payload, wp->scalePow10, &scaled) != 0) {
        snprintf(line, sizeof(line), "%s/%s rejected (bad value)",
                 prefix, name);
        TRiceS("MQTT: set %s\n", line);
        return;
    }

    if (s_setBusy) {
        snprintf(line, sizeof(line), "%s/%s rejected (busy)", prefix, name);
        TRiceS("MQTT: set %s\n", line);
        return;
    }

    s_setItem.id     = wp->ptOrd;
    s_setItem.value  = scaled;
    s_setItem.result = mbErr_pending;
    snprintf(s_setTopic, sizeof(s_setTopic), "%s/%s", prefix, name);
    s_setBusy = 1;

    int r = Modbus_Request(wp->devOrd, &s_setItem, 1, 5000u,
                           set_request_done, NULL);
    if (r != 0) {
        s_setBusy = 0;
        snprintf(line, sizeof(line), "%s rejected (%d)", s_setTopic, r);
        TRiceS("MQTT: set %s\n", line);
        return;
    }

    /* §8.4: "MQTT: set periphnet/max_charge_soc = 95 -> req 1 item" */
    snprintf(line, sizeof(line), "%s = %s -> req 1 item", s_setTopic, payload);
    TRiceS("MQTT: set %s\n", line);
}

/* --------------------------------------------------------------------------
 * Home Assistant MQTT auto-discovery — generated from the CATALOGUE, one
 * message per entry as it arrives (design §8.3).  A sensor for every
 * monitored point, plus a number entity for writable ones.
 *
 * The rule the bridge publishes a sample by is "did I create an entity for
 * this point": sensors for period_sec > 0, numbers for writable points
 * regardless.  That is why a set's read-back on an unmonitored setpoint still
 * updates its number entity, while a diagnostic read of a point nothing
 * listens to is not published to a topic with no discovery behind it.
 * -------------------------------------------------------------------------- */

static void ha_publish_entity(const sBridgeMsg *m, int asNumber)
{
    const sMbUnitInfo *unit = MbUnits_FromCode(m->unit);
    int n;

    snprintf(s_topic, sizeof(s_topic), "homeassistant/%s/%s/%s%s/config",
             asNumber ? "number" : "sensor", m->prefix, m->name,
             asNumber ? "_set" : "");

    n = snprintf(s_payload, sizeof(s_payload),
        "{"
        "\"name\":\"%s\","
        "\"state_topic\":\"%s/%s\","
        "\"unique_id\":\"%s_%s%s\",",
        m->name,
        m->prefix, m->name,
        m->prefix, m->name, asNumber ? "_set" : "");

    if (asNumber) {
        char minBuf[16], maxBuf[16], stepBuf[16];
        MbFormat_Scaled(minBuf, sizeof(minBuf), m->writeMin, m->scalePow10);
        MbFormat_Scaled(maxBuf, sizeof(maxBuf), m->writeMax, m->scalePow10);
        MbFormat_Scaled(stepBuf, sizeof(stepBuf), 1, m->scalePow10);
        n += snprintf(s_payload + n, sizeof(s_payload) - (size_t)n,
            "\"command_topic\":\"%s/%s/set\","
            "\"min\":%s,\"max\":%s,\"step\":%s,",
            m->prefix, m->name, minBuf, maxBuf, stepBuf);
    } else {
        if (unit != NULL && unit->haDeviceClass != NULL) {
            n += snprintf(s_payload + n, sizeof(s_payload) - (size_t)n,
                "\"device_class\":\"%s\",", unit->haDeviceClass);
        }
        if (m->decodeType != mbDecode_ascii && unit != NULL) {
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
        s_cfg.prefix, m->prefix,
        m->prefix, m->prefix, s_cfg.prefix);

    if (n > 0 && n < (int)sizeof(s_payload)) {
        do_publish(s_topic, s_payload, (uint16_t)n, 1 /* retain */, 0);
        /* Small delay between discovery messages to avoid overwhelming
           the lwIP output buffer */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* One "<prefix>/+/set" subscription per device, first time that prefix is
 * seen in a catalogue burst. */
static void subscribe_set_prefix(const char *prefix)
{
    for (uint8_t i = 0; i < s_subPrefixCount; i++) {
        if (strcmp(s_subPrefix[i], prefix) == 0) {
            return;
        }
    }
    if (s_subPrefixCount >= MB_MAX_DEVICES || !s_connected) {
        return;
    }

    snprintf(s_subPrefix[s_subPrefixCount], MB_TOPIC_PREFIX_LEN, "%s", prefix);
    s_subPrefixCount++;

    snprintf(s_topic, sizeof(s_topic), "%s/+/set", prefix);
    LOCK_TCPIP_CORE();
    mqtt_subscribe(s_client, s_topic, 0, NULL, NULL);
    UNLOCK_TCPIP_CORE();
}

/* --------------------------------------------------------------------------
 * Queue drain — everything the bridge publishes goes through here, on
 * mqttTask.  This is the only place mqtt_publish is reached from.
 * -------------------------------------------------------------------------- */

static void handle_desc(const sBridgeMsg *m)
{
    if (s_discoveryDone == 0u) {
        catalogue_reset();        /* the start of a discovery generation */
    }

    /* An empty catalogue — and a skipped burst tail — is last = 1 with no
     * point (§4.5). */
    if (m->name[0] != '\0') {
        if (m->devOrd < MB_MAX_DEVICES) {
            snprintf(s_devPrefix[m->devOrd], MB_TOPIC_PREFIX_LEN, "%s",
                     m->prefix);
        }
        subscribe_set_prefix(m->prefix);

        if (m->period_sec > 0u) {
            ha_publish_entity(m, 0);
        }
        if ((m->flags & MB_PT_WRITE) != 0u) {
            ha_publish_entity(m, 1);
            if (s_writableCount < BRIDGE_MAX_WRITABLE) {
                sWritablePoint *w = &s_writable[s_writableCount++];
                snprintf(w->prefix, sizeof(w->prefix), "%s", m->prefix);
                snprintf(w->name, sizeof(w->name), "%s", m->name);
                w->devOrd     = m->devOrd;
                w->ptOrd      = m->ptOrd;
                w->scalePow10 = m->scalePow10;
            }
        }
        s_discoveryDone++;
    }

    if (m->last && !s_catalogueLost) {
        TRice("MQTT: catalogue done, %u points %u writable\n",
              (unsigned)s_discoveryDone, s_writableCount);
    }
}

static void handle_sample(const sBridgeMsg *m)
{
    char topic[TOPIC_MAX];
    char payload[52];

    if (m->isText) {
        snprintf(payload, sizeof(payload), "%s", m->text);
    } else if (m->decodeType == mbDecode_bitfield) {
        snprintf(payload, sizeof(payload), "%u", (unsigned)(uint16_t)m->value);
    } else {
        MbFormat_Scaled(payload, sizeof(payload), m->value, m->scalePow10);
    }

    snprintf(topic, sizeof(topic), "%s/%s", m->prefix, m->name);
    do_publish(topic, payload, (uint16_t)strlen(payload), 1, 1);
}

static void drain_publish_queue(void)
{
    sBridgeMsg *m;

    while (s_pubQueue != NULL &&
           osMessageQueueGet(s_pubQueue, &m, NULL, 0) == osOK) {
        if (m->kind == bmsg_desc) {
            handle_desc(m);
        } else {
            handle_sample(m);
        }
        vPortFree(m);
    }

    /* Per-device availability, published where it is computed (§8.3). */
    if (s_devAvailDirty != 0u) {
        uint8_t dirty = s_devAvailDirty;
        s_devAvailDirty = 0;
        for (uint8_t d = 0; d < MB_MAX_DEVICES; d++) {
            uint8_t bit = (uint8_t)(1u << d);
            if (!(dirty & bit) || s_devPrefix[d][0] == '\0') {
                continue;
            }
            int online = (s_devOffline & bit) ? 0 : 1;
            publish_device_status(s_devPrefix[d], online);
            TRiceS(online ? "MQTT: device online: %s\n"
                          : "MQTT: device offline: %s\n",
                   (char *)s_devPrefix[d]);
        }
    }

    /* A catalogue entry that did not fit is a missing entity, so ask again
     * now that the queue has room (§4.5). */
    if (s_catalogueLost && s_modbusSub >= 0 &&
        osMessageQueueGetCount(s_pubQueue) == 0u) {
        s_catalogueLost = 0;
        Modbus_RequestCatalogue(s_modbusSub);
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

    /* Last Will: prefix/status = "offline".  Bridge-wide, and deliberately
     * NOT the per-device availability topic: an LWT is a property of this one
     * TCP session and cannot express "this slave stopped answering while the
     * bridge is fine" (§8.3). */
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

    /* Reconnect backoff can hold the outer loop for reconnectDelay, which
     * grows to 30 s — so the check-in goes in the inner wait too, and the
     * deadline covers only the 200 ms service loop. */
    int8_t monId = SysMon_TaskRegister(512U, 3000U);

    while (!s_stopReq) {
        SysMon_TaskCheckin(monId);
        service_test_hooks();
        drain_publish_queue();

        /* Connect / reconnect */
        if (!s_connected) {
            if (mqtt_do_connect() != 0) {
                TRice("MQTT: connect failed, retry in %us\n",
                      reconnectDelay / 1000);
            }
            s_reconnectCount++;

            /* Wait for connection or timeout */
            for (uint32_t w = 0; w < reconnectDelay && !s_connected && !s_stopReq;
                 w += 100) {
                vTaskDelay(pdMS_TO_TICKS(100));
                SysMon_TaskCheckin(monId);
                service_test_hooks();
                /* Keep draining with no broker: monitor lines are how the
                 * bridge stays observable in broker-less CI (§9). */
                drain_publish_queue();
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

            /* Discovery and the set-topic subscriptions are both functions
             * of the catalogue, so asking for it is the whole of "set this
             * connection up".  After a config swap the module sends a fresh
             * one unasked, which is what deletes the old active-region poll. */
            if (s_modbusSub >= 0) {
                TRice("MQTT: publishing HA discovery\n");
                s_discoveryDone = 0;
                s_catalogueLost = 0;
                Modbus_RequestCatalogue(s_modbusSub);
            }
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

    memset(s_devFails, 0, sizeof(s_devFails));
    memset(s_devPrefix, 0, sizeof(s_devPrefix));
    s_devOffline    = 0;
    s_devAnnounced  = 0;
    s_devAvailDirty = 0;
    s_droppedMsgs   = 0;
    s_catalogueLost = 0;
    s_catFresh      = 1;
    s_discoveryDone = 0;
    s_burstIdx      = 0;
    s_setBusy       = 0;
    catalogue_reset();

    if (s_pubQueue == NULL) {
        s_pubQueue = osMessageQueueNew(BRIDGE_QUEUE_DEPTH,
                                       sizeof(sBridgeMsg *), NULL);
        if (s_pubQueue == NULL) {
            TRice("MQTT: publish queue create failed\n");
            return;
        }
    }

    /* A consumer controls the bus by subscribing (§4.3): with the bridge down
     * nothing asks for these devices, so nothing polls them.  The catalogue
     * that follows is what teaches the bridge what exists. */
    if (s_modbusSub < 0) {
        s_modbusSub = Modbus_Subscribe(MB_PLAN_ALL,
                                       mbEvt_sample | mbEvt_pointDesc |
                                       mbEvt_txn | mbEvt_config,
                                       modbus_event_cb, NULL);
        if (s_modbusSub < 0) {
            TRice("MQTT: Modbus subscribe failed (%d)\n", s_modbusSub);
        }
    }

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

    if (s_modbusSub >= 0) {
        Modbus_Unsubscribe(s_modbusSub);
        s_modbusSub = -1;
    }

    /* The subscription is released asynchronously (mbEvt_released, §4.3), so
     * anything already posted is drained and freed rather than leaked. */
    if (s_pubQueue != NULL) {
        sBridgeMsg *m;
        while (osMessageQueueGet(s_pubQueue, &m, NULL, 0) == osOK) {
            vPortFree(m);
        }
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
             "%s broker=%u.%u.%u.%u:%u pub=%u reconn=%u drop=%u monitor=%s",
             s_connected ? "connected" : (s_running ? "connecting" : "stopped"),
             (unsigned)s_cfg.brokerIp[0], (unsigned)s_cfg.brokerIp[1],
             (unsigned)s_cfg.brokerIp[2], (unsigned)s_cfg.brokerIp[3],
             (unsigned)s_cfg.brokerPort,
             (unsigned)s_publishCount, (unsigned)s_reconnectCount,
             (unsigned)s_droppedMsgs, s_monitorEnabled ? "on" : "off");
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
    /* There is no way to force a re-read, and deliberately so: a value arrives
     * at its plan's period and nothing outside the module steers that (§4.3 —
     * "values lag a reconnect by up to one period").  What CAN be redone is
     * the catalogue, so an operator who thinks HA is missing entities gets the
     * discovery burst republished. */
    if (s_modbusSub >= 0) {
        s_discoveryDone = 0;
        s_catalogueLost = 0;
        Modbus_RequestCatalogue(s_modbusSub);
        TRice("MQTT: catalogue requested\n");
    }
}
