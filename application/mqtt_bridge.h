#ifndef MQTT_BRIDGE_H
#define MQTT_BRIDGE_H

#include <stdint.h>
#include "modbus_rtu.h"

/**
 * Initialize the MQTT bridge.
 * Creates the Modbus poller task and MQTT client task.
 * Must be called after lwIP is initialized.
 *
 * @param broker_ip   MQTT broker IPv4 as 4 bytes (e.g. {192,168,0,182})
 * @param broker_port MQTT broker port (default 1883)
 * @param mb_port     Modbus UART port (MODBUS_PORT_NONE to disable RS485)
 */
void mqtt_bridge_init(const uint8_t broker_ip[4], uint16_t broker_port,
                      eModbusPort mb_port);

#endif /* MQTT_BRIDGE_H */
