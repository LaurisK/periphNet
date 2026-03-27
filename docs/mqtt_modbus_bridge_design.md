# RS485/Modbus-RTU to Ethernet/MQTT Bridge Design

## Goal

Read a Solis hybrid inverter over RS485 Modbus RTU and publish its telemetry
to an MQTT broker so that Home Assistant auto-discovers the inverter as a
native device. Compatible with the
[solis_modbus](https://github.com/Pho3niX90/solis_modbus) HACS integration's
entity naming and register selection.

## What Already Exists

| Component | Status |
|-----------|--------|
| **lwIP MQTT client** (`mqtt.c`) | Compiled into firmware (GLOB_RECURSE), header at `lwip/apps/mqtt.h`. Not initialized. |
| **USART2** (PD5 TX / PD6 RX, 115200 8N1) | CubeMX-configured, not used by application. |
| **RS485 DE pin** (PD7, `gpio_rs485de`) | GPIO output, initialized LOW (RX mode). |
| **Feature flags** | `APP_FEATURE_MQTT` (bit 1), `APP_FEATURE_MODBUS` (bit 2) defined in `bl_app_contract.h`. |
| **Ethernet + DHCP + mDNS** | Working. Board at `periphnet.local` / 10.42.0.203. |
| **FreeRTOS** | 48 KB heap, 4 tasks running, plenty of room. |
| **USART1** (PA9 TX / PB7 RX) | Currently used for command parser input. |

No Modbus RTU library is present. One must be added or written.

## Hardware Wiring

```
Solis Inverter              PeriphNet Board
  RS485 A ──────────────── USART2 TX (PD5) via MAX485/SP3485
  RS485 B ──────────────── USART2 RX (PD6) via MAX485/SP3485
  GND ─────────────────── GND
                           DE/RE ──── PD7 (gpio_rs485de)
```

USART2 reconfigured at runtime to 9600 baud 8N1 (Solis default).
PD7 HIGH = transmit, PD7 LOW = receive.

## Architecture

```
                    PeriphNet STM32F407
  ┌────────────────────────────────────────────────┐
  │                                                │
  │  ┌──────────┐    ┌───────────┐    ┌─────────┐ │
  │  │ Modbus   │───>│ Register  │───>│ MQTT    │ │  ──> Ethernet ──> MQTT Broker
  │  │ RTU      │    │ Cache     │    │ Client  │ │                   ──> Home Assistant
  │  │ Master   │    │           │    │         │ │
  │  │ (USART2) │<───│ Poll      │<───│ Command │ │  <── Subscribe to .../set topics
  │  └──────────┘    │ Scheduler │    │ Handler │ │
  │       │          └───────────┘    └─────────┘ │
  │       │ DE/RE                                  │
  │      PD7                                       │
  └────────────────────────────────────────────────┘
        │
    RS485 bus
        │
  Solis Inverter (slave addr 1)
```

### FreeRTOS Tasks

| Task | Stack | Priority | Role |
|------|-------|----------|------|
| `modbusTask` | 512 words | osPriorityNormal (24) | Modbus RTU master: poll scheduler + UART TX/RX + CRC |
| `mqttTask` | 512 words | osPriorityBelowNormal (23) | MQTT connect/publish/subscribe + HA discovery |

Communication between tasks: shared register cache protected by a mutex.
The modbus task writes values; the mqtt task reads them.

## Modbus RTU Implementation

### Why Write It Instead of Adding a Library

Modbus RTU master is simple: build a request frame (addr + FC + register +
count + CRC16), send over UART, receive response, check CRC, extract data.
A minimal implementation is ~300 lines. Adding FreeModbus or libmodbus would
pull in unnecessary slave/TCP code and complicate the build.

### UART & Timing

- **USART2** reconfigured to 9600 baud, 8N1 at runtime (same pattern as
  `can2_init_500k()` in `bms_reader.c`)
- **RS485 direction**: PD7 HIGH before TX, LOW after last byte shifted out
  (use `HAL_UART_TxCpltCallback` or poll TC flag)
- **Inter-frame gap**: 3.5 character times = 3.5 ms at 9600 baud (use
  `vTaskDelay(pdMS_TO_TICKS(4))`)
- **Response timeout**: 1000 ms (Solis spec allows up to 500 ms, add margin)

### Frame Format

Request (read input registers, FC 0x04):
```
[SlaveAddr 1B] [FC 0x04] [StartReg 2B BE] [RegCount 2B BE] [CRC16 2B LE]
```

Response:
```
[SlaveAddr 1B] [FC 0x04] [ByteCount 1B] [Data N×2B BE] [CRC16 2B LE]
```

Write single register (FC 0x06):
```
[SlaveAddr 1B] [FC 0x06] [Register 2B BE] [Value 2B BE] [CRC16 2B LE]
```

CRC16/Modbus: polynomial 0xA001, init 0xFFFF, bit-reversed.

### Register Address Mapping

Solis documentation uses "documentation addresses" (33xxx for input registers,
43xxx for holding registers). The actual Modbus wire register address:

- Input register: `wire_addr = doc_addr - 30001` (e.g., 33049 -> 3048)
- Holding register: `wire_addr = doc_addr - 40001` (e.g., 43110 -> 3109)

## Solis Register Map

### Batch Read Strategy

Group consecutive registers into minimal transactions. At 9600 baud, one
read of 48 registers takes ~120 ms (request + response). Keep total poll
cycle under 2 seconds.

#### Fast Group (every 5s) — 3 transactions

**Transaction 1: PV + Inverter** (33049-33095, 47 registers, FC 0x04)

| Doc Addr | Wire | Type | Name | Scale | Unit |
|----------|------|------|------|-------|------|
| 33049 | 3048 | U16 | PV1 Voltage | x0.1 | V |
| 33050 | 3049 | U16 | PV1 Current | x0.1 | A |
| 33051 | 3050 | U16 | PV2 Voltage | x0.1 | V |
| 33052 | 3051 | U16 | PV2 Current | x0.1 | A |
| 33057-58 | 3056-57 | U32 | Total PV Power | x1 | W |
| 33073 | 3072 | U16 | Grid Voltage | x0.1 | V |
| 33076 | 3075 | U16 | Grid Current | x0.1 | A |
| 33079-80 | 3078-79 | S32 | Active Power | x1 | W |
| 33093 | 3092 | S16 | Inverter Temperature | x0.1 | C |
| 33094 | 3093 | U16 | Grid Frequency | x0.01 | Hz |
| 33095 | 3094 | U16 | Inverter Status | raw | - |

**Transaction 2: Battery + Load + Grid Port** (33133-33152, 20 registers, FC 0x04)

| Doc Addr | Wire | Type | Name | Scale | Unit |
|----------|------|------|------|-------|------|
| 33133 | 3132 | U16 | Battery Voltage | x0.1 | V |
| 33134 | 3133 | S16 | Battery Current | x0.1 | A |
| 33135 | 3134 | U16 | Battery Direction | 0=chg 1=dsg | - |
| 33139 | 3138 | U16 | Battery SOC | x1 | % |
| 33140 | 3139 | U16 | Battery SOH | x1 | % |
| 33147 | 3146 | U16 | House Load Power | x1 | W |
| 33148 | 3147 | U16 | Backup Load Power | x1 | W |
| 33149-50 | 3148-49 | S32 | Battery Power | x1 | W |
| 33151-52 | 3150-51 | S32 | Grid Port Power | x1 | W |

**Transaction 3: Meter** (33263-33264, 2 registers, FC 0x04)

| Doc Addr | Wire | Type | Name | Scale | Unit |
|----------|------|------|------|-------|------|
| 33263-64 | 3262-63 | S32 | Meter Total Active Power | x1 | W |

#### Slow Group (every 60s) — 1 transaction

**Transaction 4: Energy Totals** (33029-33036 + 33161-33179, split into 2 reads)

| Doc Addr | Wire | Type | Name | Scale | Unit |
|----------|------|------|------|-------|------|
| 33029-30 | 3028-29 | U32 | Total PV Generation | x1 | kWh |
| 33035 | 3034 | U16 | Today PV Generation | x0.1 | kWh |
| 33161-62 | 3160-61 | U32 | Total Battery Charge | x1 | kWh |
| 33163 | 3162 | U16 | Today Battery Charge | x0.1 | kWh |
| 33165-66 | 3164-65 | U32 | Total Battery Discharge | x1 | kWh |
| 33167 | 3166 | U16 | Today Battery Discharge | x0.1 | kWh |
| 33169-70 | 3168-69 | U32 | Total Grid Import | x1 | kWh |
| 33171 | 3170 | U16 | Today Grid Import | x0.1 | kWh |
| 33173-74 | 3172-73 | U32 | Total Grid Export | x1 | kWh |
| 33175 | 3174 | U16 | Today Grid Export | x0.1 | kWh |
| 33177-78 | 3176-77 | U32 | Total Consumption | x1 | kWh |
| 33179 | 3178 | U16 | Today Consumption | x0.1 | kWh |

#### Startup (once)

| Doc Addr | Wire | Type | Name |
|----------|------|------|------|
| 33000 | 2999 | U16 | Inverter Model |
| 33004-19 | 3003-18 | ASCII | Serial Number |

#### Writable Holding Registers (on demand via MQTT set topics)

| Doc Addr | Wire | FC | Name | Scale |
|----------|------|-----|------|-------|
| 43110 | 3109 | 3/6 | Energy Storage Mode | bitfield |
| 43117 | 3116 | 3/6 | Charge Current Limit | x0.1 A |
| 43118 | 3117 | 3/6 | Discharge Current Limit | x0.1 A |
| 43010 | 3009 | 3/6 | Max Charge SOC | x1 % |
| 43011 | 3010 | 3/6 | Overdischarge SOC | x1 % |

## MQTT Design

### lwIP MQTT Client

The lwIP `mqtt.c` is already compiled into the firmware. API:

```c
#include "lwip/apps/mqtt.h"

mqtt_client_t *client = mqtt_client_new();
mqtt_client_connect(client, &broker_ip, 1883, mqtt_connection_cb, arg, &ci);
mqtt_publish(client, topic, payload, len, qos, retain, cb, arg);
mqtt_subscribe(client, topic, qos, cb, arg);
```

All callbacks run in `tcpip_thread` context. Publish data must be prepared
outside tcpip_thread and posted via `tcpip_callback()` or a thread-safe queue.

### Constraints

- `MQTT_OUTPUT_RINGBUF_SIZE` default is 256 bytes. HA discovery payloads can
  exceed this. Override to **1024** in `lwipopts.h`.
- `MQTT_VAR_HEADER_BUFFER_LEN` default is 128. Override to **256** for
  incoming command payloads.
- `MEMP_NUM_SYS_TIMEOUT` may need +1 for MQTT keepalive timer (currently 10,
  should be sufficient).
- **No TRice in MQTT/lwIP callbacks** (runs in tcpip_thread). Use a flag +
  deferred logging from the mqtt task.

### Topic Structure

Following the solis2mqtt convention for HA compatibility:

```
periphnet/                          # device prefix (configurable)
  pv1_voltage                       # state topics
  pv1_current
  pv_power
  grid_voltage
  grid_frequency
  active_power
  battery_voltage
  battery_current
  battery_soc
  battery_soh
  battery_power
  house_load_power
  inverter_temperature
  today_pv_generation
  today_grid_import
  today_grid_export
  today_consumption
  total_pv_generation
  ...
  storage_mode/set                  # command topics (writable)
  charge_current_limit/set
  discharge_current_limit/set
  max_charge_soc/set
  overdischarge_soc/set
```

### Home Assistant MQTT Auto-Discovery

On MQTT connect, publish discovery configs to
`homeassistant/sensor/periphnet/<entity>/config` with `retain=true`.

Example payload for `battery_soc`:
```json
{
  "name": "Battery SOC",
  "state_topic": "periphnet/battery_soc",
  "unique_id": "periphnet_battery_soc",
  "device_class": "battery",
  "state_class": "measurement",
  "unit_of_measurement": "%",
  "device": {
    "name": "Solis Inverter",
    "manufacturer": "Ginlong Solis",
    "model": "S6-EH1P",
    "identifiers": ["periphnet_solis"],
    "sw_version": "PeriphNet",
    "via_device": "periphnet"
  }
}
```

For writable entities, use `homeassistant/number/periphnet/<entity>/config`
with `command_topic`.

HA device classes to use:

| Measurement | device_class | state_class | unit |
|-------------|-------------|-------------|------|
| Voltage | `voltage` | `measurement` | V |
| Current | `current` | `measurement` | A |
| Power | `power` | `measurement` | W |
| Energy (today) | `energy` | `total_increasing` | kWh |
| Energy (total) | `energy` | `total_increasing` | kWh |
| Temperature | `temperature` | `measurement` | C |
| Battery SOC | `battery` | `measurement` | % |
| Frequency | `frequency` | `measurement` | Hz |

### Publish Strategy

- **QoS 0** for all sensor data (fire-and-forget, no TCP retransmit overhead)
- **Retain = true** for state topics (HA sees last value on restart)
- **Retain = true** for discovery configs
- Publish only changed values (compare with previous cache, threshold for
  floats: 0.1 for voltage/current, 1 for power/energy)
- Format values as plain text strings (e.g., `"51.2"`, `"85"`, `"-250"`)

### Connection Management

- Reconnect on disconnect with exponential backoff (2s, 4s, 8s, max 60s)
- MQTT keepalive: 60s
- Client ID: `periphnet` (or `periphnet_<last4_of_MAC>`)
- Clean session: true
- LWT (Last Will): `periphnet/status` = `"offline"`, retain=true
- On connect: publish `periphnet/status` = `"online"`, retain=true

## Proposed File Structure

```
App/
  Modbus/
    modbus_rtu.h/c          # Modbus RTU master: frame build, CRC16, UART TX/RX
    solis_registers.h       # Register map: addresses, types, scaling, names
    solis_poller.h/c        # Poll scheduler: fast/slow groups, register cache
  Mqtt/
    mqtt_client.h/c         # lwIP MQTT wrapper: connect, publish, subscribe
    mqtt_discovery.h/c      # HA auto-discovery payload generation
    mqtt_bridge.h/c         # Bridge logic: cache -> MQTT topics, set -> Modbus write
  app_freertos.c            # (modified) Create modbusTask + mqttTask after DHCP
```

## lwipopts.h Changes Required

```c
/* MQTT client buffers (in USER CODE BEGIN 1 section) */
#define LWIP_MQTT               1
#define MQTT_OUTPUT_RINGBUF_SIZE 1024
#define MQTT_VAR_HEADER_BUFFER_LEN 256
```

## CMakeLists.txt Changes

The lwIP `mqtt.c` is already compiled (GLOB_RECURSE). Only the new `App/`
source files need to be added to the application target's source list.

## Configuration

MQTT broker IP and Modbus slave address should be configurable at runtime.
Options (in order of preference for initial implementation):

1. **Compile-time defines** in a `mqtt_config.h` (simplest, good enough for now)
2. EEPROM storage (AT24C02, 256 bytes — enough for broker IP + port + prefix)
3. HTTP API endpoint (e.g., `GET/POST /api/config`)

Initial defaults:
```c
#define MQTT_BROKER_IP      "10.42.0.1"   /* host machine / HA instance */
#define MQTT_BROKER_PORT    1883
#define MQTT_DEVICE_PREFIX  "periphnet"
#define MODBUS_SLAVE_ADDR   1
#define MODBUS_BAUD         9600
```

## Implementation Order

1. `modbus_rtu.c` — CRC16, frame builder, UART TX/RX with DE pin, response parser
2. `solis_registers.h` — register table (address, type, scale, name, topic)
3. `solis_poller.c` — modbusTask: init USART2 at 9600, run fast/slow poll loops
4. `mqtt_client.c` — mqttTask: connect to broker, reconnect logic, publish helper
5. `mqtt_discovery.c` — generate and publish HA discovery JSON on connect
6. `mqtt_bridge.c` — glue: read cache -> format -> publish; subscribe set -> write
7. Wire into `app_freertos.c` — create tasks after DHCP lease
8. Add `bms` command: `modbus status` / `modbus read` for debugging via Trice

## Testing

### Without Inverter (loopback)

- Short USART2 TX to RX (remove RS485 transceiver)
- Verify Modbus frames are well-formed (check CRC in logic analyzer or echo)
- Use Mosquitto on host: `mosquitto_sub -v -t 'periphnet/#'`

### With Inverter

- Connect RS485 transceiver to Solis inverter COM port
- Verify register reads match SolisCloud / inverter LCD
- Verify HA auto-discovers all entities
- Test writable registers (storage mode, current limits) via HA UI

### Integration Test (extend existing test app)

Add to `tests/integration/src/core/TestCases.cpp`:
- `mqtt_connect` — verify `periphnet/status` = `"online"` on broker
- `mqtt_pv_power` — verify `periphnet/pv_power` published within 10s
- `mqtt_battery_soc` — verify `periphnet/battery_soc` is a valid 0-100 number
- `modbus_poll_status` — send `modbus status` command, verify "polling" in output

## References

- [Pho3niX90/solis_modbus](https://github.com/Pho3niX90/solis_modbus) — HACS
  integration, most complete Solis register map
- [hvoerman/solis2mqtt](https://github.com/hvoerman/solis2mqtt) — Python
  RS485-to-MQTT bridge, HA auto-discovery reference
- [fboundy/ha_solis_modbus](https://github.com/fboundy/ha_solis_modbus) — YAML
  Modbus config for HA built-in integration
- Solis RS485_MODBUS RTU Hybrid Inverter Protocol Ver3.2
- lwIP MQTT API: `Middlewares/Third_Party/LwIP/src/include/lwip/apps/mqtt.h`
