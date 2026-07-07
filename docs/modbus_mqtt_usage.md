# Modbus RTU / MQTT Bridge — Usage & Expected Behaviour

## Overview

The PeriphNet board acts as a bridge between a Solis hybrid inverter (RS485
Modbus RTU) and a Home Assistant instance (MQTT over Ethernet).  Two
independent subsystems can be started and stopped via commands:

- **Modbus poller** — reads inverter registers over RS485 on USART2
- **MQTT bridge** — publishes register values to an MQTT broker with
  Home Assistant auto-discovery

Both are controlled through the command interface (USB CDC or UART1).

## Hardware

```
Solis Inverter                PeriphNet Board            MQTT Broker / HA
  COM port                                                (Ethernet)
  RS485 A ─────── MAX485 A ─── PD5 (USART2 TX)
  RS485 B ─────── MAX485 B ─── PD6 (USART2 RX)          ┌──────────────┐
  GND ──────────── GND         PD7 (DE/RE) ──── MAX485   │ 10.42.0.1    │
                                                          │  mosquitto   │
                               ETH ──────────────────────>│  homeassistant│
                                                          └──────────────┘
```

RS485 transceiver (MAX485, SP3485, or similar) is required between
USART2 and the inverter.  PD7 controls direction: HIGH = transmit,
LOW = receive.

## Commands

### Modbus

```
modbus start [baud] [slave]
```
Start the Modbus poller.  Reconfigures USART2 to the given baud rate
(default 9600) and begins polling the inverter at the given slave
address (default 1).  Creates a background FreeRTOS task.

```
modbus stop
```
Stop the poller task, deinit USART2, log total polls and errors.

```
modbus read
```
Print the current register cache via Trice.  Output format:
```
Solis polls=42 errs=0
 PV: 234.5V 8.3A 1946W
 Grid: 237.1V 50.01Hz -1200W
 Bat: 51.2V -5.3A SOC=85% -271W
 Load: house=746W backup=0W meter=-1200W
 Today: PV=12.3 Grid+=0.0 Grid-=8.5 kWh
```

```
modbus set baud <rate>
```
Change baud rate (takes effect after stop + start).

```
modbus set slave <1-247>
```
Change slave address (takes effect immediately on next poll).

```
modbus write <register> <value>
```
Queue a single holding register write (FC 0x06).  Executed on the next
poll cycle.  Only one write can be queued at a time.

Example — set overdischarge SOC to 10%:
```
modbus write 3010 10
```

```
modbus status
```
Print poller state:
```
Modbus running port=uart2 monitor=off baud=9600 slave=1
 polls=42 errors=0
```

```
modbus port <uart2|uart6|disabled>
```
Select the active port (refused while the poller is running — stop first).
`disabled` = no physical bus, transactions time out instantly; used with
`modbus inject` for software-only testing.  `uart6` is not wired up yet.

```
modbus monitor <on|off>
```
Stream raw TX/RX frames via Trice: `Modbus TX[8]: 01 04 0c 3c ...`

```
modbus inject <hexbytes>
```
Process a raw response frame (no-space hex) as if received from the bus:
CRC checked, registers decoded and fed into the poller cache + telemetry.
Errors: `Modbus inject: ERR_SHORT|ERR_CRC|ERR_EXCEPTION`.

### MQTT

```
mqtt start [a.b.c.d] [port]
```
Start the MQTT bridge.  Connects to the broker at the given IP
(default 10.42.0.1) and port (default 1883).  Creates a background
FreeRTOS task.

```
mqtt stop
```
Publish offline status, disconnect from broker, stop the task.

```
mqtt set ip <a.b.c.d>
```
Change broker IP (takes effect on next reconnect).

```
mqtt status
```
Print MQTT state:
```
MQTT: connected broker=10.42.0.1:1883 pub=128 reconn=1 monitor=off
```

```
mqtt monitor <on|off>
```
Stream pub/sub messages via Trice: `MQTT pub: periphnet/battery_soc = 85`.
Publishes are logged even without a broker connection (CI observability).

```
mqtt inject <topic> <payload>
```
Process a message as if received from the broker (no connection needed).
Runs through the real incoming callbacks in tcpip_thread; acked with
`MQTT inject: <topic>`.  Set-topic messages queue Modbus writes.

```
mqtt publish now
```
Publish all values immediately instead of waiting for the 5 s interval.

## Startup Sequence

Typical usage after the board boots and obtains an IP:

```
modbus start
mqtt start
```

Or with non-default settings:

```
modbus start 9600 1
mqtt start 192.168.1.100 1883
```

The two subsystems are independent.  You can run `modbus start` alone
to read inverter data via `modbus read` without MQTT.  You can also
run `mqtt start` without the Modbus poller — it will publish zeroes
until poller data arrives.

## Modbus Polling Behaviour

### Poll Schedule

| Group | Interval | Transactions | Registers |
|-------|----------|-------------|-----------|
| Fast | 5 seconds | 3 | PV, grid, battery, load, meter |
| Slow | 60 seconds | 7 | Daily energy totals, lifetime totals |

The task loop runs at 100 ms resolution.  Actual poll timing has ~100 ms
jitter.

### Fast Poll — Transaction Details

**Transaction 1** (PV + inverter output):
Read 47 input registers starting at wire address 3048 (doc 33049).
Extracts: PV1/PV2 voltage and current, total PV power, grid voltage
and current, active power, inverter temperature, grid frequency,
inverter status.

**Transaction 2** (battery + load):
Read 20 input registers starting at wire address 3132 (doc 33133).
Extracts: battery voltage, current, direction, SOC, SOH, house load
power, backup load power, battery power, grid port power.

**Transaction 3** (meter):
Read 2 input registers starting at wire address 3262 (doc 33263).
Extracts: meter total active power.

### Slow Poll — Transaction Details

Seven individual single-register reads for daily energy counters
(today PV, battery charge/discharge, grid import/export, consumption)
plus one 2-register read for total PV generation.

### RS485 Timing

- **Inter-frame gap:** 4 ms before each transmission (>3.5 char times at 9600)
- **TX completion:** polls UART TC flag before switching DE to receive
- **RX end-of-frame:** 5 ms silence after last received byte
- **Response timeout:** 1000 ms per transaction (configurable)
- **Fast poll cycle time:** ~800 ms (3 transactions with gaps and responses)

### Error Handling

- Each failed transaction increments the error counter (visible in
  `modbus status` and `modbus read`)
- Errors in one transaction do not stop the poll cycle — remaining
  transactions still execute
- No automatic reconnect or baud rate fallback
- IWDG is kicked during fast poll to prevent watchdog reset during
  long Modbus transactions

### Register Writes

Writes use FC 0x06 (Write Single Register).  The write is executed at
the start of the next poll cycle, before the fast-poll reads.  Only one
write can be pending at a time; a second `modbus write` while one is
pending will be rejected.

Key writable registers:

| Register | Doc Addr | Description | Range |
|----------|----------|-------------|-------|
| 3109 | 43110 | Energy storage mode (bitfield) | See Solis manual |
| 3116 | 43117 | Charge current limit | 0-200 (x0.1 A) |
| 3117 | 43118 | Discharge current limit | 0-200 (x0.1 A) |
| 3009 | 43010 | Max charge SOC | 70-100 (%) |
| 3010 | 43011 | Overdischarge SOC | 5-40 (%) |

## MQTT Behaviour

### Connection

- Uses the lwIP built-in MQTT 3.1.1 client (QoS 0, no TLS)
- Client ID: same as topic prefix (default `periphnet`)
- Keepalive: 60 seconds
- Clean session: true
- Last Will Testament: `periphnet/status` = `"offline"` (retained)

### Reconnect Strategy

On disconnect or failed connect, the bridge retries with exponential
backoff: 2s, 4s, 8s, 16s, 32s, 60s (max).  Backoff resets after a
successful connect.

### On Connect

1. Publish `periphnet/status` = `"online"` (retained)
2. Publish Home Assistant MQTT discovery configs for all 25 sensors
   (50 ms delay between each to avoid overwhelming the lwIP TX buffer)

### Periodic Publishing

Every 5 seconds (configurable), all 25 sensor values are published from
the Modbus register cache.  Values are published as plain text strings.

All state topics use **QoS 0** and **retain = true**.

### Topic Map

All topics are prefixed with the configured prefix (default `periphnet`).

| Topic Suffix | Source Register | Format | Unit |
|-------------|----------------|--------|------|
| `pv1_voltage` | 33049 | `"234.5"` | V |
| `pv1_current` | 33050 | `"8.3"` | A |
| `pv2_voltage` | 33051 | `"234.5"` | V |
| `pv2_current` | 33052 | `"8.3"` | A |
| `pv_power` | 33057-58 | `"1946"` | W |
| `grid_voltage` | 33073 | `"237.1"` | V |
| `grid_frequency` | 33094 | `"50.01"` | Hz |
| `active_power` | 33079-80 | `"-1200"` | W |
| `inverter_temp` | 33093 | `"35.2"` | C |
| `battery_voltage` | 33133 | `"51.2"` | V |
| `battery_current` | 33134 | `"-5.3"` | A |
| `battery_soc` | 33139 | `"85"` | % |
| `battery_soh` | 33140 | `"99"` | % |
| `battery_power` | 33149-50 | `"-271"` | W |
| `house_load_power` | 33147 | `"746"` | W |
| `backup_load_power` | 33148 | `"0"` | W |
| `grid_port_power` | 33151-52 | `"-1200"` | W |
| `meter_power` | 33263-64 | `"-1200"` | W |
| `today_pv` | 33035 | `"12.3"` | kWh |
| `today_grid_import` | 33171 | `"0.0"` | kWh |
| `today_grid_export` | 33175 | `"8.5"` | kWh |
| `today_consumption` | 33179 | `"3.8"` | kWh |
| `today_bat_charge` | 33163 | `"4.2"` | kWh |
| `today_bat_discharge` | 33167 | `"1.1"` | kWh |
| `total_pv` | 33029-30 | `"12450"` | kWh |
| `status` | (internal) | `"online"` / `"offline"` | — |

Negative power values indicate:
- `active_power` negative = exporting to grid
- `battery_power` negative = battery charging
- `battery_current` negative = battery charging
- `meter_power` negative = exporting to grid

### Home Assistant Auto-Discovery

Discovery configs are published to:
```
homeassistant/sensor/periphnet/<suffix>/config
```

Each config is a JSON payload containing:
- `name` — human-readable sensor name
- `state_topic` — `periphnet/<suffix>`
- `unique_id` — `periphnet_<suffix>`
- `device_class` — HA device class (voltage, current, power, energy,
  battery, temperature, frequency)
- `state_class` — `measurement` for instantaneous, `total_increasing`
  for energy counters
- `unit_of_measurement`
- `device` block — groups all sensors under a single "Solis Inverter"
  device in HA

After discovery, HA will show a device named "Solis Inverter" with
25 sensor entities, updating every 5 seconds.

### Limitations

- No TLS/authentication — broker must accept anonymous connections
- Subscribes to `<prefix>/+/set` on connect; currently mapped writes:
  `overdischarge_soc/set` → reg 3010 (5-40), `max_charge_soc/set` →
  reg 3009 (70-100)
- All 25 values published every cycle regardless of changes
- No username/password support in this version

## Trice Output Examples

### Startup
```
Modbus: polling slave 1 at 9600 baud
MQTT: bridge starting, broker 10.42.0.1:1883 prefix="periphnet"
MQTT: connected to broker
MQTT: publishing HA discovery
```

### Steady State (via `modbus read`)
```
Solis polls=120 errs=0
 PV: 234.5V 8.3A 1946W
 Grid: 237.1V 50.01Hz -1200W
 Bat: 51.2V -5.3A SOC=85% -271W
 Load: house=746W backup=0W meter=-1200W
 Today: PV=12.3 Grid+=0.0 Grid-=8.5 kWh
```

### Error Conditions
```
Modbus: USART2 init failed
```
USART2 HAL_Init returned error.  Check wiring and clock configuration.

```
Modbus: write reg 3010 failed (-1)
```
Write timed out (no response from inverter).  Check slave address, baud
rate, and RS485 wiring.

```
MQTT: connect failed, retry in 2s
```
TCP connection to broker failed.  Check broker IP, port, and network.

### Shutdown
```
Modbus: stopped (polls=120 errors=0)
MQTT: stopped (published 128 times, reconnects 1)
```

## Verifying with Mosquitto

On the host machine with the MQTT broker:

```bash
# Subscribe to all PeriphNet topics:
mosquitto_sub -v -t 'periphnet/#'

# Expected output:
periphnet/status online
periphnet/pv1_voltage 234.5
periphnet/battery_soc 85
periphnet/active_power -1200
...

# Subscribe to HA discovery topics:
mosquitto_sub -v -t 'homeassistant/sensor/periphnet/#'

# Write a value (once subscribe handling is implemented):
mosquitto_pub -t 'periphnet/overdischarge_soc/set' -m '10'
```

## Resource Usage

| Resource | Usage |
|----------|-------|
| Flash | +20 KB (206 → 226 KB of 480 KB) |
| RAM | +800 B (107 → 108 KB of 128 KB) |
| FreeRTOS heap | ~4 KB (2 task stacks at 512 words each) |
| USART2 | Exclusive use by Modbus poller when running |
| PD5, PD6, PD7 | RS485 TX, RX, DE — cannot be shared |
| TCP connections | 1 (MQTT client to broker) |
