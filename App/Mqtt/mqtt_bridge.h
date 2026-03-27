/**
 * @file    mqtt_bridge.h
 * @brief   MQTT bridge — publishes Solis inverter data to HA via MQTT
 *
 * Uses the lwIP MQTT client.  Publishes HA auto-discovery configs on
 * connect, then periodically publishes register values from the Solis
 * poller cache.  Subscribes to .../set topics for writable registers.
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
    char     prefix[32];           /* topic prefix (default "periphnet") */
    uint32_t publishIntervalMs;    /* how often to publish (default 5000) */
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

#endif /* MQTT_BRIDGE_H_ */
