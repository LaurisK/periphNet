/**
 * @file    mqtt_bridge.h
 * @brief   MQTT bridge — publishes Modbus config points to HA via MQTT
 *
 * Uses the lwIP MQTT client.  Publishes HA auto-discovery configs on
 * connect (generated from the active Modbus config), subscribes to
 * <topicPrefix>/+/set topics for writable points and resolves them against
 * the config.  Point values are published by the Modbus walker through
 * MqttBridge_Publish() — the walker owns the data cadence.
 *
 * Runs as a FreeRTOS task.
 */

#ifndef MQTT_BRIDGE_H_
#define MQTT_BRIDGE_H_

#include <stdint.h>

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */

typedef struct {
    uint32_t brokerIp[4];          /* IPv4 octets (e.g., {10,42,0,1}) */
    uint16_t brokerPort;           /* default 1883 */
    char     prefix[32];           /* bridge topic prefix (default "periphnet"):
                                      client id + bridge-wide LWT status topic */
    uint32_t publishIntervalMs;    /* retained for CLI compatibility; unused —
                                      the Modbus walker owns publish cadence */
} sMqttBridgeCfg;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief  Start the MQTT bridge task.
 *         Must be called after lwIP is up (IP obtained).
 */
void MqttBridge_Start(const sMqttBridgeCfg *cfg);

/**
 * @brief  Stop the MQTT bridge task and disconnect.
 */
void MqttBridge_Stop(void);

/**
 * @brief  Returns 1 if the MQTT bridge task is running.
 */
int MqttBridge_IsRunning(void);

/**
 * @brief  Returns 1 if currently connected to the MQTT broker.
 */
int MqttBridge_IsConnected(void);

/**
 * @brief  Update broker IP at runtime.
 */
void MqttBridge_SetBrokerIp(uint8_t a, uint8_t b, uint8_t c, uint8_t d);

/**
 * @brief  Log MQTT status to Trice output.
 */
void MqttBridge_LogStatus(void);

/* --------------------------------------------------------------------------
 * Publish interface (used by the Modbus walker; safe from any task)
 * -------------------------------------------------------------------------- */

/**
 * @brief  Publish a value (QoS 0). Monitor-logs "MQTT pub: topic = value"
 *         even without a broker so output is observable in broker-less
 *         (CI) testing. lwIP access is serialized via the tcpip core lock.
 * @return 0 if handed to lwIP, -1 if not connected / publish failed
 */
int MqttBridge_Publish(const char *topic, const char *payload,
                       uint16_t payloadLen, uint8_t retain);

/**
 * @brief  Publish a device's retained availability topic
 *         ("<topicPrefix>/availability" = online/offline, design §10).
 */
void MqttBridge_PublishDeviceStatus(const char *topicPrefix, int online);

/* --------------------------------------------------------------------------
 * Integration-test support
 * -------------------------------------------------------------------------- */

/**
 * @brief  Enable/disable message monitoring — logs
 *         "MQTT pub: topic = value" / "MQTT sub: topic = value".
 */
void MqttBridge_SetMonitor(int enable);
int  MqttBridge_GetMonitor(void);

/**
 * @brief  Inject an MQTT message as if it had arrived from the broker.
 *         Processed through the real incoming-publish callbacks in
 *         tcpip_thread context; acknowledged with "MQTT inject: topic"
 *         from the bridge task.
 * @return 0 if accepted, -1 if bridge not running / previous inject pending
 */
int MqttBridge_Inject(const char *topic, const char *payload,
                      uint16_t payloadLen);

/**
 * @brief  Request an immediate re-publish of all points (delegates to
 *         ModbusWalker_ForceRepublish — the walker owns the data).
 */
void MqttBridge_PublishNow(void);

#endif /* MQTT_BRIDGE_H_ */
