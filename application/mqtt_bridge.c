#include "mqtt_bridge.h"
#include "modbus_rtu.h"
#include "trice.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "lwip/apps/mqtt.h"
#include "lwip/ip_addr.h"
#include "lwip/dns.h"
#include <stdio.h>
#include <string.h>

/* ========================================================================
 * Configuration
 * ======================================================================== */

#define MODBUS_SLAVE_ADDR       1       /* Solis inverter default */
#define MODBUS_POLL_INTERVAL_MS 5000    /* Poll every 5 seconds */
#define MODBUS_TIMEOUT_MS       1000

#define MQTT_CLIENT_ID          "periphnet"
#define MQTT_TOPIC_PREFIX       "periphnet/modbus/"
#define MQTT_KEEP_ALIVE_S       60
#define MQTT_RECONNECT_MS       5000

#define BRIDGE_TASK_STACK       1024    /* words */
#define BRIDGE_TASK_PRIORITY    (tskIDLE_PRIORITY + 3)

/* ========================================================================
 * Modbus register map — Solis inverter common registers
 * Add/modify as needed for your specific inverter model.
 * ======================================================================== */

typedef struct {
    uint16_t    reg_addr;
    uint16_t    reg_count;       /* usually 1, 2 for 32-bit values */
    const char *name;            /* MQTT sub-topic */
    int         scale_div;       /* divide raw value by this (10 = x0.1) */
} sModbusRegDef;

static const sModbusRegDef s_reg_map[] = {
    /* Example registers - adjust for your Solis model */
    { 3000, 1, "inverter_temp",    10 },  /* 0.1 °C */
    { 3004, 1, "dc_voltage_1",     10 },  /* 0.1 V */
    { 3005, 1, "dc_current_1",     10 },  /* 0.1 A */
    { 3006, 1, "dc_voltage_2",     10 },  /* 0.1 V */
    { 3007, 1, "dc_current_2",     10 },  /* 0.1 A */
    { 3035, 1, "ac_voltage",       10 },  /* 0.1 V */
    { 3038, 1, "ac_current",       10 },  /* 0.1 A */
    { 3041, 1, "ac_frequency",    100 },  /* 0.01 Hz */
    { 3042, 2, "active_power",      1 },  /* W (32-bit) */
};

#define REG_MAP_COUNT  (sizeof(s_reg_map) / sizeof(s_reg_map[0]))

/* ========================================================================
 * MQTT client state
 * ======================================================================== */

static mqtt_client_t *s_mqtt_client;
static ip_addr_t      s_broker_addr;
static uint16_t       s_broker_port;
static volatile bool  s_mqtt_connected;

static void mqtt_connection_cb(mqtt_client_t *client, void *arg,
                               mqtt_connection_status_t status)
{
    (void)client;
    (void)arg;
    if (status == MQTT_CONNECT_ACCEPTED) {
        s_mqtt_connected = true;
        TRice("MQTT: connected to broker\n");
    } else {
        s_mqtt_connected = false;
        TRice("MQTT: connection failed/lost, status=%d\n", (int)status);
    }
}

static void mqtt_pub_request_cb(void *arg, err_t result)
{
    (void)arg;
    if (result != ERR_OK) {
        TRice("MQTT: publish failed err=%d\n", (int)result);
    }
}

static void mqtt_connect(void)
{
    struct mqtt_connect_client_info_t ci = {0};
    ci.client_id   = MQTT_CLIENT_ID;
    ci.keep_alive  = MQTT_KEEP_ALIVE_S;

    TRice("MQTT: connecting to %d.%d.%d.%d:%d\n",
          ip4_addr1(&s_broker_addr), ip4_addr2(&s_broker_addr),
          ip4_addr3(&s_broker_addr), ip4_addr4(&s_broker_addr),
          s_broker_port);

    err_t err = mqtt_client_connect(s_mqtt_client, &s_broker_addr,
                                    s_broker_port, mqtt_connection_cb,
                                    NULL, &ci);
    if (err != ERR_OK) {
        TRice("MQTT: connect call failed err=%d\n", (int)err);
    }
}

static void mqtt_publish_value(const char *sub_topic, const char *payload)
{
    if (!s_mqtt_connected)
        return;

    char topic[64];
    snprintf(topic, sizeof(topic), "%s%s", MQTT_TOPIC_PREFIX, sub_topic);

    err_t err = mqtt_publish(s_mqtt_client, topic, payload, strlen(payload),
                             0, 0, mqtt_pub_request_cb, NULL);
    if (err != ERR_OK) {
        TRice("MQTT: pub err=%d topic=%s\n", (int)err, topic);
    }
}

/* ========================================================================
 * Bridge task — polls Modbus, publishes to MQTT
 * ======================================================================== */

static void bridge_task(void *arg)
{
    /* Wait for network to be ready */
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* Init Modbus RTU master */
    eModbusPort mb_port = (eModbusPort)(uintptr_t)arg;
    modbus_rtu_init(mb_port);

    /* Create MQTT client */
    s_mqtt_client = mqtt_client_new();
    if (s_mqtt_client == NULL) {
        TRice("BRIDGE: MQTT client alloc failed!\n");
        vTaskDelete(NULL);
        return;
    }

    mqtt_connect();

    sModbusResponse resp;
    char payload[32];

    for (;;) {
        /* Reconnect MQTT if needed */
        if (!s_mqtt_connected) {
            TRice("BRIDGE: MQTT not connected, reconnecting...\n");
            mqtt_connect();
            vTaskDelay(pdMS_TO_TICKS(MQTT_RECONNECT_MS));
            continue;
        }

        /* Poll each register in the map */
        for (uint16_t i = 0; i < REG_MAP_COUNT; i++) {
            const sModbusRegDef *rd = &s_reg_map[i];

            int result = modbus_read_input_regs(
                MODBUS_SLAVE_ADDR, rd->reg_addr, rd->reg_count,
                &resp, MODBUS_TIMEOUT_MS);

            if (result == MODBUS_OK && resp.reg_count >= rd->reg_count) {
                int32_t raw;
                if (rd->reg_count == 2) {
                    raw = ((int32_t)resp.regs[0] << 16) | resp.regs[1];
                } else {
                    raw = (int16_t)resp.regs[0];
                }

                if (rd->scale_div > 1) {
                    int whole = (int)(raw / rd->scale_div);
                    int frac  = (int)(raw % rd->scale_div);
                    if (frac < 0) frac = -frac;
                    snprintf(payload, sizeof(payload), "%d.%d", whole, frac);
                } else {
                    snprintf(payload, sizeof(payload), "%ld", (long)raw);
                }

                mqtt_publish_value(rd->name, payload);
                TRice("BRIDGE: %s = %s (raw=%d)\n", rd->name, payload, (int)raw);
            } else {
                TRice("BRIDGE: read %s failed err=%d\n", rd->name, result);
            }

            /* Small gap between Modbus polls */
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        /* Publish connection status */
        mqtt_publish_value("status", "online");

        vTaskDelay(pdMS_TO_TICKS(MODBUS_POLL_INTERVAL_MS));
    }
}

/* ========================================================================
 * Public init
 * ======================================================================== */

void mqtt_bridge_init(const uint8_t broker_ip[4], uint16_t broker_port,
                      eModbusPort mb_port)
{
    IP4_ADDR(&s_broker_addr, broker_ip[0], broker_ip[1],
             broker_ip[2], broker_ip[3]);
    s_broker_port = broker_port;
    s_mqtt_connected = false;

    xTaskCreate(bridge_task, "Bridge",
                BRIDGE_TASK_STACK, (void *)(uintptr_t)mb_port,
                BRIDGE_TASK_PRIORITY, NULL);

    TRice("BRIDGE: task created, broker=%d.%d.%d.%d:%d, mb_port=%d\n",
          broker_ip[0], broker_ip[1], broker_ip[2], broker_ip[3],
          broker_port, (int)mb_port);
}
