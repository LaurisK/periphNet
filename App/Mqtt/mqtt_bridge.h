/**
 * @file    mqtt_bridge.h
 * @brief   MQTT bridge — publishes Modbus config points to HA via MQTT
 *
 * Uses the lwIP MQTT client.  Publishes HA auto-discovery configs on
 * connect (generated from the active Modbus config), subscribes to
 * <topicPrefix>/+/set topics for writable points and resolves them against
 * the config.  Point values are published by the Modbus walker through
 * MqttBridge_Publish().  Values arrive through Modbus_Subscribe: the
 * module owns the cadence, this file owns what is worth sending.
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
 * @brief  Persist the running configuration so a reset does not lose it.
 *
 * The broker address used to live only in RAM, which meant a deployed board
 * pointed at whatever the image was compiled with — the bench broker — after
 * every reboot, until somebody re-issued a CLI command over USB.  That is the
 * defect this exists to close.
 *
 * @return 0 on success, -1 if nothing is running or the medium refused.
 */
int MqttBridge_SaveCfg(void);

/**
 * @brief  Load the persisted configuration, if there is one.
 * @return 0 and fills @p out, or -1 when the board has never been configured.
 */
int MqttBridge_LoadCfg(sMqttBridgeCfg *out);

/**
 * @brief  Discard the persisted configuration.
 * @return 0 on success, -1 if the medium refused.
 */
int MqttBridge_ForgetCfg(void);

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
/* Per-device availability. Internal: it is derived from mbEvt_txn failures
 * inside this file, because HA availability is MQTT's semantic and belongs
 * where it is published (docs/modbus.md §8.3). */

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
 *         there is no way to force a re-read — a value arrives at its
 *         period, docs/modbus.md §4.3).
 */
void MqttBridge_PublishNow(void);

#endif /* MQTT_BRIDGE_H_ */
