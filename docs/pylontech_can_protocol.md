# Pylontech Battery CAN Protocol Specification

## Overview

This document describes the Pylontech CAN bus protocol used for communication
between lithium battery management systems (BMS) and hybrid/off-grid inverters.
It is the de-facto standard adopted by Pylontech, JK BMS, PACE BMS, and many
others for interoperability with inverters from Solis, Deye, Victron, Growatt,
SMA, Goodwe, Luxpower, etc.

The protocol was reconstructed from multiple open-source implementations, BMS
vendor documentation fragments, and Pylontech's own V1.2/V1.3 CAN specs
referenced in various community forums.

**Implementation in this project:** `App/Can/pylontech.h`, `App/Can/bms_sim.c`,
`App/Can/bms_reader.c`

## Bus Parameters

| Parameter       | Value                                  |
|-----------------|----------------------------------------|
| Standard        | CAN 2.0A (11-bit standard identifiers) |
| Baud rate       | 500 kbps                               |
| Byte order      | Little-endian (LSB first)              |
| Frame interval  | 1000 ms (all frames sent every 1 s)    |
| Direction       | BMS -> Inverter (unidirectional)       |
| Termination     | 120 ohm at each end of bus             |

Some inverters also transmit a keepalive frame (CAN ID 0x305, all zeros) at
1 Hz. The BMS may monitor this to detect inverter presence but it is not
required.

## Frame Definitions

### 0x351 -- Battery Charge/Discharge Limits

Tells the inverter the safe operating window for voltage and current.

| Byte | Field                    | Type     | Scale | Unit | Range         |
|------|--------------------------|----------|-------|------|---------------|
| 0-1  | Charge voltage limit     | int16 LE | 0.1   | V    | 0 -- 75.0     |
| 2-3  | Max charge current       | int16 LE | 0.1   | A    | 0 -- 500.0    |
| 4-5  | Max discharge current    | int16 LE | 0.1   | A    | 0 -- 500.0    |
| 6-7  | Discharge voltage limit  | uint16 LE| 0.1   | V    | 0 -- 6553.5   |

**DLC:** 8

**Example (16S LiFePO4 48V):**
```
Charge limit  = 56.0V  -> 0x0230 (560)
Max charge I  = 50.0A  -> 0x01F4 (500)
Max disch I   = 50.0A  -> 0x01F4 (500)
Disch limit   = 44.8V  -> 0x01C0 (448)

Wire: 30 02 F4 01 F4 01 C0 01
```

The inverter uses these limits to regulate its charge/discharge behaviour. The
BMS dynamically adjusts them (e.g., reducing charge current near full, reducing
discharge current at low SOC or low temperature).

### 0x355 -- State of Charge / State of Health

| Byte | Field | Type      | Scale | Unit | Range   |
|------|-------|-----------|-------|------|---------|
| 0-1  | SOC   | uint16 LE | 1     | %    | 0 -- 100 |
| 2-3  | SOH   | uint16 LE | 1     | %    | 0 -- 100 |

**DLC:** 4

**Example:**
```
SOC = 85%  -> 0x0055 (85)
SOH = 99%  -> 0x0063 (99)

Wire: 55 00 63 00
```

### 0x356 -- Voltage / Current / Temperature

Real-time electrical measurements of the battery pack.

| Byte | Field       | Type     | Scale | Unit | Notes                       |
|------|-------------|----------|-------|------|-----------------------------|
| 0-1  | Voltage     | int16 LE | 0.01  | V    | Pack voltage                |
| 2-3  | Current     | int16 LE | 0.1   | A    | Positive = discharge        |
| 4-5  | Temperature | int16 LE | 0.1   | C    | Highest cell/sensor temp    |

**DLC:** 6

**Current sign convention:** Positive values indicate discharge (energy flowing
out of battery). Negative values indicate charge (energy flowing into battery).
This matches the Pylontech convention. Some BMS implementations invert this --
always verify against a known load/charger.

**Example:**
```
Voltage = 51.20V  -> 0x1400 (5120)
Current = -2.5A   -> 0xFFE7 (-25, signed)  [charging at 2.5A]
Temp    = 25.0C   -> 0x00FA (250)

Wire: 00 14 E7 FF FA 00
```

### 0x359 -- Errors and Warnings

Two-level alarm system: errors (protection events, may trip output) and
warnings (advisory, no immediate action).

| Byte | Bit | Error Field               |
|------|-----|---------------------------|
| 0    | 1   | Cell/pack overvoltage      |
| 0    | 2   | Cell/pack undervoltage     |
| 0    | 3   | Over-temperature           |
| 0    | 4   | Under-temperature          |
| 0    | 7   | Discharge overcurrent      |
| 1    | 0   | Charge overcurrent         |
| 1    | 3   | System/internal error      |

| Byte | Bit | Warning Field              |
|------|-----|----------------------------|
| 2    | 1   | High voltage warning        |
| 2    | 2   | Low voltage warning         |
| 2    | 3   | High temperature warning    |
| 2    | 4   | Low temperature warning     |
| 2    | 7   | Discharge high current warn |
| 3    | 0   | Charge high current warning |
| 3    | 3   | Internal error warning      |

| Byte | Field                      |
|------|----------------------------|
| 4    | Module/pack number (0x01)  |
| 5    | 0x50 ('P')                 |
| 6    | 0x4E ('N')                 |

**DLC:** 7

Bytes 5-6 are ASCII identifiers. Pylontech uses 'P','N'. Some implementations
set these to identify the BMS brand.

**Example (all clear):**
```
Wire: 00 00 00 00 01 50 4E
```

### 0x35C -- Charge/Discharge Enable Flags

Single control byte that tells the inverter what operations are permitted.

| Byte 0 Bit | Flag                 | Value |
|------------|----------------------|-------|
| 7          | Charge enable        | 0x80  |
| 6          | Discharge enable     | 0x40  |
| 5          | Force charge req I   | 0x20  |
| 4          | Force charge req II  | 0x10  |
| 3          | Full charge request  | 0x08  |

**DLC:** 2 (byte 1 is reserved, set to 0x00)

**Common values:**
- `0xC0` = charge + discharge enabled (normal operation)
- `0x80` = charge only (discharge disabled, e.g., low SOC protection)
- `0x40` = discharge only (charge disabled, e.g., fully charged)
- `0x00` = all disabled (fault condition)
- `0xE0` = charge + discharge + force charge I (BMS requests grid charge)

**Force charge** is used when SOC drops critically low. The inverter should
charge from grid regardless of time-of-use settings. Force charge I is a
soft request; Force charge II is urgent.

**Full charge request** asks the inverter to charge to 100% for cell balancing
purposes. Typically sent periodically (e.g., every 30 days).

### 0x35E -- Manufacturer Name

8 bytes of ASCII text identifying the battery manufacturer.

**DLC:** 8

| Byte | Content               |
|------|-----------------------|
| 0-7  | ASCII string, padded  |

**Standard values:**
- `"PYLON   "` -- Pylontech (and most emulators)
- `"JKBMS   "` -- JK BMS native
- Other BMS brands may set their own identifier

## Extension Frames (Optional)

These additional frames are present in some implementations (SMA, BYD,
Luxpower compatibility). They are not part of the core protocol and may not
be supported by all inverters.

### 0x35F -- Battery Info (SMA)

| Byte | Field            | Type      | Scale | Unit |
|------|------------------|-----------|-------|------|
| 0-1  | Cell chemistry   | uint16 LE | --    | enum |
| 2-3  | Hardware version | uint16 LE | --    | --   |
| 4-5  | Capacity         | uint16 LE | 1     | Ah   |
| 6-7  | Software version | uint16 LE | --    | --   |

Cell chemistry values: 0 = LFP (LiFePO4), 1 = Li-ion, 2 = Lead-acid

### 0x373 -- Cell Extremes (BYD)

| Byte | Field           | Type      | Scale | Unit |
|------|-----------------|-----------|-------|------|
| 0-1  | Min cell voltage| uint16 LE | 1     | mV   |
| 2-3  | Max cell voltage| uint16 LE | 1     | mV   |
| 4-5  | Min cell temp   | uint16 LE | 0.1   | K    |
| 6-7  | Max cell temp   | uint16 LE | 0.1   | K    |

### 0x305 -- Inverter Keepalive (RX from inverter)

| Byte | Content      |
|------|--------------|
| 0-7  | All 0x00     |

**DLC:** 8

Transmitted by the inverter at 1 Hz. The BMS can monitor this to detect
whether the inverter is connected and communicating. Loss of keepalive for
>5 s may indicate communication failure.

## Typical 48V LiFePO4 Parameters

Reference values for a 16S LiFePO4 (3.2V nominal per cell) battery:

| Parameter              | Value    | Notes                          |
|------------------------|----------|--------------------------------|
| Nominal voltage        | 51.2 V   | 16 x 3.2V                     |
| Charge voltage limit   | 56.0 V   | 16 x 3.5V (absorption)        |
| Discharge voltage limit| 44.8 V   | 16 x 2.8V (cutoff)            |
| Float voltage          | 54.4 V   | 16 x 3.4V                     |
| Max charge current     | 50 A     | 0.5C for 100Ah pack           |
| Max discharge current  | 50 A     | 0.5C for 100Ah pack           |
| Cell voltage range     | 2.5-3.65V| Absolute min/max per cell      |
| Operating temperature  | 0-45 C   | Charge; discharge down to -20C |

## Wire Example -- Complete Frame Set

All 6 frames as transmitted for a healthy 48V pack at 85% SOC, charging at
2.5A, 25C ambient:

```
CAN ID  DLC  Data (hex)            Decoded
------  ---  --------------------  ----------------------------------
0x351    8   30 02 F4 01 F4 01     CVL=56.0V ChgI=50.0A DsgI=50.0A
             C0 01                 DVL=44.8V
0x355    4   55 00 63 00           SOC=85% SOH=99%
0x356    6   00 14 E7 FF FA 00    V=51.20V I=-2.5A T=25.0C
0x359    7   00 00 00 00 01 50 4E Errors=0 Warnings=0 Mod=1 "PN"
0x35C    2   C0 00                 Charge=EN Discharge=EN
0x35E    8   50 59 4C 4F 4E 20    "PYLON   "
             20 20
```

## References

- Pylontech CAN Protocol V1.2 / V1.3 (referenced in photovoltaikforum.com,
  setfirelabs.com)
- ArminJo/JK-BMSToPylontechCAN (GitHub, MIT license) -- Arduino/C++ structs
- maxx-ukoo/jk-bms2pylontech (GitHub) -- STM32 HAL C implementation
- dalathegreat/Battery-Emulator (GitHub) -- ESP32 Pylontech LV CAN
- mr-manuel/venus-os_dbus-serialbattery (GitHub) -- Python JK BMS CAN parser
- Uksa007/esphome-jk-bms-can (GitHub, protocol docs in docs/ directory)
