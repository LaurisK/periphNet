# Modbus RTU / MQTT Bridge — Usage & Expected Behaviour

## Overview

The PeriphNet board bridges Modbus RTU slaves on one RS485 bus (USART2) to
an MQTT broker / Home Assistant over Ethernet.  Which devices and registers
are polled is **not compiled in** — it is described by an uploadable JSON
configuration compiled into external flash
(see [modbus_multi_device_config_design.md](modbus_multi_device_config_design.md)
and [impl_modbus_multi_device_config.md](impl_modbus_multi_device_config.md)).
A built-in default config for the Solis hybrid inverter is provisioned on
first boot, so out-of-box behaviour matches the old hardcoded bridge.

Two independent subsystems are controlled through the command interface
(USB CDC or UART1):

- **Modbus walker** — polls every configured device/transaction on schedule
- **MQTT bridge** — connection, HA discovery, set-topic writes; values are
  published by the walker through the bridge

## Hardware

```
Modbus RTU slaves             PeriphNet Board            MQTT Broker / HA
 (Solis inverter,                                          (Ethernet)
  meters, ... on one bus)
  RS485 A ─────── MAX485 A ─── PD5 (USART2 TX)
  RS485 B ─────── MAX485 B ─── PD6 (USART2 RX)          ┌──────────────┐
  GND ──────────── GND         PD7 (DE/RE) ──── MAX485   │ 10.42.0.1    │
                                                          │  mosquitto   │
                               ETH ──────────────────────>│  homeassistant│
                                                          └──────────────┘
```

RS485 transceiver (MAX485, SP3485, or similar) is required between
USART2 and the bus.  PD7 controls direction: HIGH = transmit, LOW = receive.

## Configuration (JSON → flash)

### Format

```json
{
  "devices": [
    {
      "slaveAddr": 1,
      "topicPrefix": "periphnet",
      "transactions": [
        {
          "startAddr": 3132,
          "functionCode": "input",
          "readPeriodS": 5,
          "points": [
            { "offset": 0, "decodeType": "u16", "scale": 0.1,
              "unit": "V", "name": "battery_voltage" },
            { "offset": 6, "decodeType": "u16", "scale": 1,
              "unit": "%", "name": "battery_soc",
              "publish": { "threshold": 1, "heartbeatS": 300 } },
            { "offset": 7, "decodeType": "u16", "scale": 1,
              "unit": "%", "name": "overdischarge_soc",
              "writable": true, "writeMin": 5, "writeMax": 40 }
          ]
        }
      ]
    }
  ]
}
```

Field reference:

- **Device**: `slaveAddr` (1-247), `topicPrefix` (≤15 chars `[A-Za-z0-9_-]`;
  MQTT namespace AND the HA device identity), `transactions[]`.
- **Transaction**: `startAddr` (wire register address), `functionCode`
  (`"holding"` or `"input"`), `readPeriodS` (≥1), `points[]`.  The register
  block length is **derived** from the points — never authored.
- **Point**: `offset` (relative to `startAddr`), `decodeType` (`u16` `s16`
  `u32_be` `u32_le` `s32_be` `s32_le` `float32_be` `float32_le` `bitfield`
  `ascii`), `scale` (**exact power of ten**: 0.001 … 1000), `unit` (`""`,
  `V` `A` `W` `VA` `var` `Hz` `Wh` `kWh` `varh` `VAh` `%` `Ah` `C` `min`
  `s`), `name` (≤23 chars, MQTT topic suffix), `length` (registers, **ascii
  only**), `writable` (default false; 1-register types only),
  `writeMin`/`writeMax` (scaled-integer = raw register domain; omit for no
  range check), `publish.threshold` (min scaled-int change to republish;
  0/omitted = publish every read), `publish.heartbeatS` (force republish
  interval; 0/omitted = never).

Bounds: ≤8 devices, ≤64 transactions total (≤16/device), ≤192 points total
(≤24/transaction), ≤125 registers per transaction (Modbus FC03/04 ceiling).

### HTTP endpoints

```bash
# Upload → compiles into the INACTIVE flash region; compile IS validation
curl -X POST --data-binary @my_config.json \
  http://10.42.0.203/api/modbus/config/upload
# on error: 422 {"error":"...","field":"scale","device":0,"transaction":1,"point":2}

# Apply → walker hot-swaps at its next lap boundary (no reboot)
curl -X POST http://10.42.0.203/api/modbus/config/apply

# Status: active region, counts, staged/swap state, last upload result
curl http://10.42.0.203/api/modbus/config/status

# Download the ACTIVE config re-serialized as JSON (data-faithful)
curl http://10.42.0.203/api/modbus/config/download -o modbus_config.json

# Factory reset to the built-in Solis config (hot, no reboot)
curl -X DELETE http://10.42.0.203/api/modbus/config
```

Uploads are refused with 409 while an apply is pending.  The uploaded JSON
itself is not retained — download regenerates it from the compiled records
(field order/whitespace may differ; recompiling the download yields a
byte-identical config).

## Commands

### Modbus

```
modbus start [baud]
```
Start the config walker (default 9600 baud).  Devices and registers come
from the active flash config; a blank device is auto-provisioned with the
built-in Solis config.  Creates a background FreeRTOS task ("modbus").

```
modbus stop
```
Stop the walker task, deinit USART2, log total polls and errors.

```
modbus read
```
Log walker + config status via Trice:
```
Modbus walker: running baud=9600 polls=42 errors=0 lap=812ms swap=none
Modbus config: region 0, 1 devices 11 txns 27 points
 device: periphnet slave=1 fails=0
```

```
modbus set baud <rate>
```
Change baud rate (takes effect after stop + start).

```
modbus write <slave> <register> <value>
```
Queue a single holding register write (FC 0x06) to any slave.  Executed
between transactions on the next walker tick.  One write pending at a time.

Example — set overdischarge SOC to 10% on slave 1:
```
modbus write 1 3010 10
```

```
modbus status
```
Print walker state (port/monitor/baud line plus the `modbus read` output).

```
modbus port <uart2|uart6|disabled>
```
Select the active port (refused while the walker is running — stop first).
`disabled` = no physical bus, transactions time out instantly; used with
`modbus inject` for software-only testing.  `uart6` is not wired up yet.

```
modbus monitor <on|off>
```
Stream raw TX/RX frames via Trice: `Modbus TX[8]: 01 04 0c 3c ...`

```
modbus inject <startAddr> <hexbytes>
```
Process a raw response frame (no-space hex) as if received from the bus:
CRC checked, registers decoded, then fed to the config transaction matching
`{frame slave address, startAddr}` — points run the normal decode /
threshold / publish pipeline synchronously.
Acks: `Modbus inject: N bytes` then `Walker inject: slave S addr A regs N`,
or `Walker inject: no matching transaction`.
Frame errors: `Modbus inject: ERR_SHORT|ERR_CRC|ERR_EXCEPTION`.

### MQTT

```
mqtt start [a.b.c.d] [port]
```
Start the MQTT bridge.  Connects to the broker at the given IP
(default 10.42.0.1) and port (default 1883).  Creates a background
FreeRTOS task ("mqtt").

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
`MQTT inject: <topic>`.  Set-topic messages resolve against the config's
writable points and queue Modbus writes.

```
mqtt publish now
```
Clear the walker's publish/poll tracking so the next lap re-reads and
re-publishes every point.

## Startup Sequence

Typical usage after the board boots and obtains an IP:

```
modbus start
mqtt start
```

The two subsystems are independent.  `modbus start` alone polls the bus
(observable via `modbus read` / `mqtt monitor on`).  `mqtt start` without
the walker connects, publishes discovery and accepts set messages, but no
point values flow until the walker runs.

## Modbus Polling Behaviour

### Walker schedule

One walker lap runs every 100 ms: it scans the active config's records
(flash-resident, read on demand — no RAM copy) and issues any transaction
whose `readPeriodS` has elapsed.  The built-in Solis config polls the three
fast blocks (3048×47, 3132×20, 3262×2) every 5 s and the energy counters /
writable-SOC readback every 60 s — the same wire traffic as the old
hardcoded poller.

Per-point publishing: on every successful read a point republishes if its
scaled value changed by ≥ `publish.threshold` (0 = always), or its
`publish.heartbeatS` elapsed, or it has never been published.

### Per-device availability

Each configured device has a retained `"<topicPrefix>/availability"` topic
(`online`/`offline`), driven by a consecutive-failure counter: 3 failed
transactions in a row mark the device offline; the first success brings it
back.  Offline devices are probed at a reduced rate (every ≥30 s) so a dead
slave's timeouts cannot starve healthy devices on the bus.

Note this is distinct from the bridge-wide LWT `"<prefix>/status"` — that
covers the bridge's own TCP session; availability covers one slave on the
bus.  (The design doc's `<topicPrefix>/status` was renamed to
`/availability` because the default device prefix equals the bridge prefix.)

### RS485 Timing

- **Inter-frame gap:** 4 ms before each transmission (>3.5 char times at 9600)
- **TX completion:** polls UART TC flag before switching DE to receive
- **RX end-of-frame:** 5 ms silence after last received byte
- **Response timeout:** 1000 ms per transaction (configurable)

### Error Handling

- Each failed transaction increments the error counter and the device's
  consecutive-failure counter (see availability above)
- Errors in one transaction do not stop the lap — remaining transactions
  still execute
- IWDG is kicked by defaultTask independently; long laps cannot starve it

### Register Writes

Writes use FC 0x06 (Write Single Register), queued via `modbus write` or an
MQTT set message, executed between transactions.  Only one write can be
pending at a time.

Set messages are validated against the point's `writeMin`/`writeMax`
(scaled-int = raw register domain) — the built-in config preserves the old
safety ranges: `overdischarge_soc` 5-40, `max_charge_soc` 70-100.

## MQTT Behaviour

### Connection

- lwIP built-in MQTT 3.1.1 client (QoS 0, no TLS)
- Client ID: bridge prefix (default `periphnet`)
- Keepalive 60 s, clean session, LWT `periphnet/status` = `"offline"` retained
- Reconnect with exponential backoff: 2s, 4s, … 60s max

### On Connect

1. Publish `periphnet/status` = `"online"` (retained)
2. Subscribe `"<topicPrefix>/+/set"` for every configured device
3. Publish Home Assistant discovery configs for every point (50 ms apart)

Discovery is re-published automatically after a config hot-swap, so new
devices/points appear in HA without a reconnect.

### Topics

For every device in the config:

| Topic | Content |
|-------|---------|
| `<topicPrefix>/<name>` | point value, plain text, retained, QoS 0 |
| `<topicPrefix>/<name>/set` | inbound writes for `writable` points |
| `<topicPrefix>/availability` | retained `online`/`offline` per device |
| `<bridgePrefix>/status` | retained bridge-wide LWT status |

Values are integer-formatted from the scaled domain (`51.2`, `-5.0`, `85`);
no floating point is involved except decoding `float32_*` wire values.

### Home Assistant Auto-Discovery

- `homeassistant/sensor/<topicPrefix>/<name>/config` for every point —
  `device_class`/`state_class`/`unit_of_measurement` derived from the
  point's unit (DLMS code → HA class table in `Shared/Modbus/modbus_units.c`)
- `homeassistant/number/<topicPrefix>/<name>_set/config` additionally for
  writable points — `command_topic` = `<topicPrefix>/<name>/set`, min/max
  from `writeMin`/`writeMax`, step from `scale`
- Entities carry an `availability` array (bridge status AND device
  availability, mode `all`)
- HA device identity/name = `topicPrefix`; each configured Modbus slave
  appears as its own HA device

### Limitations

- No TLS/authentication — broker must accept anonymous connections
- One RS485 bus (USART2); multiple devices = multiple slave addresses on it
- `publishIntervalMs` in `mqtt start` config is accepted but unused — the
  walker + per-point `publish` settings own the cadence

## Trice Output Examples

### Startup
```
Modbus: default config provisioned (11 txns 27 points)
Modbus: walker started, 9600 baud
MQTT: bridge starting, broker 10.42.0.1:1883 prefix="periphnet"
MQTT: connected to broker
MQTT: publishing HA discovery
Modbus: device online: periphnet
```

### Config reload
```
Modbus config: staged 2 devices 13 txns 29 points
Modbus config: apply armed (walker swaps at lap boundary)
Modbus: config swapped, active region 1
MQTT: publishing HA discovery
```

### Error Conditions
```
Modbus: port init failed
```
USART2 HAL init returned error.  Check wiring and clock configuration.

```
Modbus: write reg 3010 failed (-1)
```
Write timed out (no response from the slave).  Check slave address, baud
rate, and RS485 wiring.

```
Modbus: device offline: periphnet
```
3 consecutive failed transactions — slave not answering.

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
periphnet/availability online
periphnet/pv1_voltage 234.5
periphnet/battery_soc 85
periphnet/active_power -1200
...

# Subscribe to HA discovery topics:
mosquitto_sub -v -t 'homeassistant/#'

# Write a value:
mosquitto_pub -t 'periphnet/overdischarge_soc/set' -m '10'
```

## Resource Usage

| Resource | Usage |
|----------|-------|
| Flash | ~269 KB of 480 KB (incl. JSON compiler + built-in config) |
| RAM | ~125 KB of 128 KB main SRAM + 3.4 KB CCM (walker/compiler state) |
| Ext flash | 2×16 KB LUT regions + 4 KB selector at 0xF9000-0x101FFF |
| FreeRTOS heap | ~4 KB (2 task stacks at 512 words each) |
| USART2 | Exclusive use by the walker when running |
| PD5, PD6, PD7 | RS485 TX, RX, DE — cannot be shared |
| TCP connections | 1 (MQTT client to broker) |
