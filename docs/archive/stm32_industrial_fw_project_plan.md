# STM32F4 Industrial Board Firmware - Project Plan

## Project Overview

**Target Hardware:** STM32F407VET6 industrial board
- MCU: STM32F407VET6 (512KB Flash, 192KB RAM)
- External Flash: W25Q128 (16MB, SPI)
- EEPROM: AT24C02BN (256 bytes, I2C)
- Ethernet PHY: DP83848IVV
- SD card slot
- Ethernet, RS485, RS232, 2x CAN

**Core Requirements:**
- FreeRTOS-based architecture
- Bootloader with firmware update capability (uses external flash)
- RS485 (Modbus RTU) to Ethernet (MQTT) bridge (Solis inverter → Home Assistant)
- Modular, testable codebase following TDD
- CMake-based build system
- Trice tracing over TCP/IP

**Primary Use Case:** Connect Solis inverter (RS485 Modbus) to Home Assistant via MQTT

---

## Architecture Overview

### High-Level System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Layer                         │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │ Modbus-MQTT  │  │ CAN Handler  │  │ RS232 Handler│      │
│  │   Bridge     │  │              │  │              │      │
│  └──────────────┘  └──────────────┘  └──────────────┘      │
│  ┌──────────────────────────────────────────────────┐      │
│  │       Firmware Update Service                     │      │
│  │  (Downloads image to external flash)              │      │
│  └──────────────────────────────────────────────────┘      │
└─────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────┐
│                   Middleware Layer                           │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │ MQTT Client  │  │ Modbus Stack │  │ Config Mgr   │      │
│  └──────────────┘  └──────────────┘  └──────────────┘      │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │  lwIP Stack  │  │   FreeRTOS   │  │Trice (TCP/IP)│      │
│  └──────────────┘  └──────────────┘  └──────────────┘      │
└─────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────┐
│                      HAL/Driver Layer                        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │   Ethernet   │  │  UART (485)  │  │  UART (232)  │      │
│  └──────────────┘  └──────────────┘  └──────────────┘      │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │   CAN1/2     │  │ Int. Flash   │  │  Ext. Flash  │      │
│  └──────────────┘  └──────────────┘  └──────────────┘      │
└─────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────┐
│                    Bootloader (Separate)                     │
│  - Verifies & loads firmware from external flash             │
│  - Validates application in internal flash                   │
│  - Exposes verification API for application use              │
│  - Minimal, no network stack                                 │
└─────────────────────────────────────────────────────────────┘
```

### Memory Layout

**Internal Flash (STM32F407VET6 - 512KB):**
```
┌──────────────────┐ 0x0800 0000
│   Bootloader     │ (32KB)
├──────────────────┤ 0x0800 8000
│   Application    │ (480KB)
└──────────────────┘ 0x0808 0000
```

**External Flash (W25Q128 - 16MB):**
```
┌──────────────────┐ 0x0000 0000
│ FWU Status & Data│ 4 KB
├──────────────────┤ 0x0000 1000
│ FWU Image        │ 480 KB
├──────────────────┤ 0x0007 9000
│ Golden Image     │ 480 KB
├──────────────────┤ 0x000F 1000
│ Undefined        │ ~15 MB
│ (Future use)     │
└──────────────────┘ 0x0100 0000
```

**EEPROM (AT24C02BN - 256 bytes):**
- Boot counter, hardware ID, factory config, network backup

---

## Development Phases

### Phase 0: Project Setup (Week 1)
**Goal:** Establish development infrastructure

**Steps:**
1. Create Git repository (FIRST STEP)
2. Generate initial project with STM32CubeIDE (STM32F407VET6 target)
3. Migrate to CMake build system
4. Set up CppUTest framework (manual test execution)
5. Set up Trice tracing (TCP/IP)
6. Create third_party/ directory structure

**Deliverables:**
- Git repository with clear structure
- Compilable CMake project
- CppUTest framework integrated
- Trice tracing working over TCP/IP

### Phase 1: Core Infrastructure (Weeks 2-3)
**Goal:** Build foundational modules with TDD

- HAL drivers (UART, Ethernet w/ DP83848, Internal Flash, W25Q128, AT24C02BN, CAN)
- FreeRTOS task management
- Trice integration

### Phase 2: Bootloader Development (Weeks 4-5)
**Goal:** Simple, robust bootloader

**Bootloader Responsibilities:**
- Boot decision logic
- Firmware installation from external flash
- Expose verification API at fixed addresses for application use
- Safety mechanisms

**Application Responsibilities (Phase 6):**
- Download firmware via Ethernet
- Call bootloader API to verify image
- Set update flag and reboot

### Phase 3: Modbus Stack (Weeks 6-7)
**Goal:** Modbus RTU for Solis inverter

### Phase 4: MQTT Client (Weeks 8-9)
**Goal:** Reliable MQTT connectivity

### Phase 5: Modbus-MQTT Bridge (Weeks 10-11)
**Goal:** Core feature - bridge Solis to Home Assistant

### Phase 6: Additional Features (Weeks 12-13)
**Goal:** Complete interfaces and firmware update

- Firmware update service (application-side)
- CAN bus support
- RS232 interface
- Logging system

### Phase 7: System Integration & Testing (Weeks 14-15)

### Phase 8: Documentation & Deployment (Week 16)

---

## Module Breakdown

```
project/
├── .git/
├── .gitignore
├── README.md
├── CMakeLists.txt
├── bootloader/
│   ├── src/
│   ├── inc/
│   │   └── boot_api.h          # API exposed to application
│   ├── linker/
│   │   └── bootloader.ld
│   └── tests/
├── application/
│   ├── src/
│   │   ├── core/
│   │   ├── hal/
│   │   │   ├── uart/
│   │   │   ├── ethernet/           # DP83848IVV
│   │   │   ├── flash_internal/     # STM32F407 512KB
│   │   │   ├── flash_external/     # W25Q128 SPI
│   │   │   ├── eeprom/             # AT24C02BN I2C
│   │   │   ├── can/
│   │   │   └── sdcard/
│   │   ├── middleware/
│   │   │   ├── modbus/
│   │   │   ├── mqtt/
│   │   │   ├── lwip_port/
│   │   │   ├── trice_port/
│   │   │   └── config_manager/
│   │   ├── app/
│   │   │   ├── modbus_mqtt_bridge/
│   │   │   ├── firmware_update/
│   │   │   ├── can_handler/
│   │   │   └── rs232_console/
│   │   └── utils/
│   ├── inc/
│   ├── linker/
│   │   └── application.ld
│   └── config/
├── tests/
│   ├── unit/                    # CppUTest
│   ├── integration/
│   └── mocks/
├── third_party/                 # All external/forked code
│   ├── FreeRTOS/
│   ├── lwIP/
│   ├── STM32_HAL/
│   ├── trice/
│   ├── CppUTest/
│   ├── MQTT_client/
│   └── README.md
├── docs/
├── tools/
└── scripts/
    └── run_tests.sh
```

---

## Key Technologies

- **RTOS:** FreeRTOS (third_party/)
- **TCP/IP:** lwIP (third_party/)
- **MQTT:** TBD (third_party/)
- **Modbus:** Custom or adapted
- **Tracing:** Trice - TCP/IP
- **Build:** CMake + ARM GCC
- **Testing:** CppUTest (manual execution)
- **Version Control:** Git

---

## Bootloader & Application Separation

**Bootloader (32KB, no network):**
- Reads update flag from external flash
- Verifies and loads firmware from external flash to internal flash
- Supports golden image fallback (480KB reserved in external flash)
- Exposes API at fixed address (0x08007F00) for application use

**Application (480KB, with network):**
- Downloads firmware via Ethernet
- Calls bootloader API to verify before update
- Sets update flag and reboots

**External Flash Usage:**
- 4KB: FWU status and data
- 480KB: FWU image (downloaded firmware)
- 480KB: Golden image (factory/known-good firmware)
- ~15MB: Undefined (future use)

---

## Next Steps

1. Create Git repository
2. STM32CubeIDE initial project (STM32F407VET6)
3. CMake migration
4. CppUTest setup
5. Trice integration
6. Begin Phase 1

**Target:** 16 weeks from start
