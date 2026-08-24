# CLAUDE.md

## Project Overview

**PeriphNet** is an STM32F407VET6 firmware project. Long-term goal: RS485/Modbus-RTU to Ethernet/MQTT bridge for Solis inverter + Home Assistant, with dual-image OTA bootloader.

**Done and in place:** the encrypted FWU pipeline (Zhaga pattern, extended) — firmware is distributed only as encrypted+authenticated `.pnfw` blobs; the bootloader does streaming AES-128-GCM decrypt + HMAC verify during install, with confirm/rollback via a golden image. HMAC, AES-128 and GCM are real, NIST-vector-tested implementations (not stubs). Also done: **the Modbus module rebuild of [docs/modbus.md](docs/modbus.md) §1-§9** — the v2 record format (capabilities/devices/plans), the subscription + event surface, the frame-level port contract with a test peripheral, event-driven per-device timers, runtime plan editing, and a generic MQTT/HA bridge that is now an ordinary consumer. **Not yet run on hardware.**

**External-flash DMA is done and hardware-verified** (`Pd1.1.23`, 2026-08-25,
installed over the tunnel). SPI2 took DMA1 streams 3 and 4 from the Trice UART,
which is now opt-in behind `TRICE_UART_OUTPUT` and interrupt-driven when
enabled. Bulk reads and page programs go by DMA when the buffer is
DMA-reachable; commands and CCM buffers fall back to the polled HAL call.
**It did not make OTA cheaper, and that was expected**: an upload still pins the
CPU at 100 % because the cost is `W25Q128_WaitReady` *waiting*, not
transferring. Measurements, the ranked risk list for a tunnel-only board, and
the design assessment of moving that wait to a timer + callback:
[docs/task_dma_interrupt_audit.md](docs/task_dma_interrupt_audit.md) §7-§8 and
[docs/task_flash_wait_and_ota_cost.md](docs/task_flash_wait_and_ota_cost.md).

**Everything Modbus lives in one document: [docs/modbus.md](docs/modbus.md)** — the design (§2), shipped behaviour (§3), config JSON, operator reference, test contract, and known limits. **§3 is what is on the board; §2 is what it is being rebuilt into, and none of §2 is implemented yet** (`App/Modbus/modbus.h` is a proposed header that nothing includes). §2 covers the subscription API, a frame-level port contract with a test port instead of test hooks, devices/types/parameters (baud and port are config, not API), and an event-driven scheduler of per-device timers — no poll loop. §2.16 sequences it: steps 1–7 extract the API with behaviour held constant, 8–14 replace the engine. Still undesigned and listed in §7: the write path (FC06-only, one register, one pending), dialects beyond an address stride, and MQTT-side rate policy.

**Current phase:** the device is growing from a bridge into an edge controller — poll a JK BMS on the same/second RS485 bus, fuse with inverter data, and present a synthetic Pylontech pack to the inverter over CAN (`App/Can/`). That makes autonomy (correct operation with the WAN, HA and broker all down) a hard requirement, and constrains how remote access is done. Direction and open questions: [docs/design_remote_access_and_autonomy.md](docs/design_remote_access_and_autonomy.md).

## Build and Flash

```bash
# First time
cmake -B build -S .

# Build both targets
cmake --build build -j8

# Flash application only (development)
./flash_nokill.sh flash_application.jlink

# Flash both bootloader + application
./flash_nokill.sh flash_both.jlink

# Flash bootloader only
./flash_nokill.sh flash_bootloader.jlink

# One-liner (build + flash app)
cmake --build build -j8 && ./flash_nokill.sh flash_application.jlink
```

**Prerequisites:** ARM GCC toolchain, CMake 3.22+, JLinkExe

**Build output:**
- `build/bootloader.elf` / `.bin` — ~23 KB flash, ~2.7 KB RAM (32 KB limit)
- `build/application.elf` / `.bin` — ~383 KB flash (480 KB limit), ~86 KB main
  SRAM of 128 KB and ~58 KB CCM of 64 KB (**CCM is the tight one — ~92 %**);
  the `.bin` is signed in-place (IMAGE_SIZE + HMAC patched) after every build
- `build/periphnet_full.hex` — BL + signed APP combined, factory/initial J-Link write
- `build/periphnet_fwu.pnfw` — encrypted+authenticated blob, the ONLY artifact
  used for OTA (needs python3 `cryptography` + `intelhex` packages)

**FWU keys:** `bootloader/secrets.c` holds committed DEVELOPMENT keys (matching
the defaults in `tools/dfu_image_tool.py`). For production: create gitignored
`bootloader/secrets_prod.c` (auto-picked by CMake) and export
`DFU_AES_KEY`/`DFU_HMAC_KEY` for the build tool. Keys are known ONLY to the
build process and the bootloader — never stored in ext flash, never linked
into the application.

**Always size with `-A`** — plain `arm-none-eabi-size` reports one `bss`
column that silently sums `.bss` + `.ccmram` + `.ccmheap` + `._user_heap_stack`
(127996 B), which reads as "main SRAM nearly full" when main SRAM is actually
about half free and CCM is the constrained region:
```bash
arm-none-eabi-size -A build/application.elf
```

**Clean rebuild:**
```bash
rm -rf build && cmake -B build -S . && cmake --build build -j8
```

**STM32CubeIDE managed build** — CMake stays canonical, but the Eclipse
`Debug`/`Release` configurations build the *application* target with the same
sources, includes, defines (`APPLICATION_BUILD`), linker script
(`App/application.ld`), Trice insert/clean pre/post steps and post-build
`dfu_image_tool.py sign`, so `Debug/application.bin` is a signed, flashable
image. It cannot build the bootloader or the `.pnfw` blob — use CMake for
those. Headless check:
```bash
/opt/st/stm32cubeide_1.17.0/headless-build.sh \
    -data /tmp/ws -import . -cleanBuild PeriphNet/Debug
```
`.cproject` / `.project` are **gitignored**, so this configuration is
machine-local; a fresh clone gets a CubeMX-default project again. **That also
means nothing keeps its include list in step with `APP_INCLUDES` in
`CMakeLists.txt`** — the drift only shows up as a header not found (this is how
`Shared/NvDb` went missing until the first CubeIDE build after nvDb landed).
CMake now **warns at configure time** when `.cproject` is missing a path the
CMake build has; add missing ones under *Project > Properties > C/C++ Build >
Settings > MCU GCC Compiler > Include paths*, **for both configurations**. Whenever
CubeMX adds a peripheral it also adds `Core/Src/<periph>.c` — that file must be
added to `APP_CORE_SOURCES` in `CMakeLists.txt` by hand (the app's Core list is
explicit, not globbed).

## Testing

**Device IP:** 10.42.0.203 (DHCP on 10.42.0.x subnet)

```bash
ping 10.42.0.203
curl http://10.42.0.203/
curl http://10.42.0.203/api/image/info                 # stored image (name/version/size/crc)
curl http://10.42.0.203/api/fwu/status                 # FWU state (running/golden/confirmed)
curl http://10.42.0.203/api/system/status              # tasks, stacks, heap, CPU, IWDG margin

# Full OTA cycle (blob only — plaintext .bin uploads are rejected)
curl -X POST -H "X-Filename: periphnet_fwu.pnfw" \
  --data-binary @build/periphnet_fwu.pnfw \
  http://10.42.0.203/api/image/upload
curl -X POST http://10.42.0.203/api/fwu/install        # arms FWU + reboots
# ...device reboots, BL installs, new FW comes up UNCONFIRMED...
curl http://10.42.0.203/api/fwu/status                 # check health/version
curl -X POST http://10.42.0.203/api/fwu/confirm        # REQUIRED within 3 boots,
                                                       # also promotes stored→golden

# Download stored blob / verify round-trip
curl http://10.42.0.203/api/image/download -o downloaded.pnfw
md5sum build/periphnet_fwu.pnfw downloaded.pnfw
```

**An OTA pins the CPU at 100 % for its duration** (measured: `http` 991‰,
`nvdb` collector 735‰ concurrently, IWDG margin still fine at 102 ms of
16400). That is `W25Q128_WaitReady` busy-waiting on page programs and sector
erases — DMA does not help it, because DMA cannot shorten a wait. Nothing
missed a deadline, but treat OTA as a "board is busy" window until
[docs/task_flash_wait_and_ota_cost.md](docs/task_flash_wait_and_ota_cost.md)
is done.

**IMPORTANT:** without `confirm`, the bootloader rolls back to the golden
image after 3 unconfirmed boots. Local-target (`'l'`) builds are exempt from
attempt counting, so JLink dev flashing is unaffected.

**Trice UART output** (USART3 PD8/TX, 460800 baud) — **off by default**;
set `TRICE_UART_OUTPUT` to 1 in `App/triceConfig.h` first (interrupt-driven,
not DMA — DMA1 S3/S4 belong to the flash now):
```bash
./tools/trice log -p COM -args "/dev/ttyUSB0:460800" -i ./til.json -li ./li.json
```

**Host-native unit tests** (no ARM toolchain; crypto NIST/RFC vectors, version
gate, boot_status flag lifecycle, the Modbus config machinery — record
store/selector, JSON compiler accept+reject matrix, export round-trip,
decode/format vectors — and all of `nvDb`: bounds at every edge, the erased
fast path asserted against an erase counter, delete/collector semantics,
layout JSON accept+reject, and relocation including a power-cut sweep. All
over a NOR-faithful flash mock — full 8 MB, page-program boundaries enforced,
and a "cut the power on operation N" knob):
```bash
cmake -B tests/build -S tests && cmake --build tests/build -j8
ctest --test-dir tests/build --output-on-failure
```

**Integration test harness** (`tests/integration/`, see its README) — host-side
C++ tool that drives a live board over USB/UART/UDP, sends CLI commands and
asserts against decoded Trice output. Needs the `trice` binary plus `til.json`
/ `li.json` in the project root:
```bash
cmake -B build_integration -S tests/integration -DBUILD_GUI=OFF   # CLI only
cmake --build build_integration -j8                               # → periphnet_cli
```
Drop `-DBUILD_GUI=OFF` (and install `libsdl2-dev libgl-dev`) to also build the
Dear ImGui GUI, `build_integration/periphnet_gui`. Modbus/MQTT test contracts
live in `docs/modbus.md` §6.

## Hardware

- **MCU:** STM32F407VET6 (512KB Flash, 128KB SRAM, 168MHz)
- **External Flash:** W25Q64 (8MB, SPI2 at 21MHz) — JEDEC 0xEF/0x40/0x17
- **EEPROM:** AT24C02BN (256 bytes, I2C)
- **Ethernet PHY:** DP83848IVV (RMII)
- **Trice:** USART3 PD8/TX PD9/RX, 460800 baud — **output OFF by default**
  (`TRICE_UART_OUTPUT` in `App/triceConfig.h`); tracing is UDP + USB CDC
- **DMA1 Stream3/Stream4:** SPI2_RX / SPI2_TX (external flash). These are
  the ONLY streams either request can use, and `USART3_TX` can use only
  these two as well — hence the trade. **DMA1 S5/S6** are USART2 RX/TX
  (RS485); **DMA2 is entirely free**
- **IWDG:** ~16.4s timeout (PRESCALER_128, RELOAD 4095)
- **TIM14:** Software watchdog, 12s pre-IWDG warning

## Memory Architecture

### Internal Flash (512KB)

```
0x0800_0000  ┌───────────────────┐
             │ Bootloader        │ 32KB (Sectors 0-1)
             │  .isr_vector      │
             │  .text, .rodata   │
0x0800_7F00  │  .bl_api (256B)   │ ← sBootloaderApi function pointer table
0x0800_8000  ├───────────────────┤
             │ Application       │ 480KB (Sectors 2-7)
             │  .isr_vector      │ Vector table (0x188 bytes)
0x0800_8200  │  .app_header      │ ← sAppInfo (version, HMAC, features)
0x0800_8300  │  .text, .rodata   │
0x0808_0000  └───────────────────┘
```

### External Flash (W25Q64 — 8MB SPI)

Both blob areas hold encrypted `.pnfw` blobs — firmware is never stored in
plaintext outside internal flash. Each area is self-describing (cleartext
manifest + trailing CRC32), so no metadata lives in the boot status.

```
0x0000_0000  ┌───────────────────┐
             │ Boot Status (4KB) │ sBootStatus v3: flags + last_fwu_result
0x0000_1000  ├───────────────────┤
             │ Stored Blob       │ 488KB — uploaded .pnfw (image store;
0x0007_B000  ├───────────────────┤          installed from here by the BL)
             │ Golden Blob       │ 488KB — last CONFIRMED image (encrypted),
0x000F_5000  ├───────────────────┤          rollback target
             │ Image Meta (4KB)  │ sImageMeta: file name, bound by blob CRC32
0x000F_6000  ├───────────────────┤
             │ (gap)             │
0x000F_8000  ├───────────────────┤
             │ Crash Log (4KB)   │
0x000F_9000  ├───────────────────┤
             │ Modbus LUT A(16KB)│ compiled register-config record stream
0x000F_D000  ├───────────────────┤
             │ Modbus LUT B(16KB)│ A/B roles; selector says which is active,
0x0010_1000  ├───────────────────┤ uploads compile into the inactive one
             │ Modbus Sel (4KB)  │ active-region selector (NOR bit-clear
0x0010_2000  ├───────────────────┤ pattern like sBootStatus)
             │ WG Time (4KB)     │ monotonic seconds for the WireGuard TAI64N
0x0010_3000  ├───────────────────┤ handshake stamp; append-only 16B slot ring
             │ WG Config (4KB)   │ tunnel addr/mask + hub endpoint, one CRC'd
0x0010_4000  ├───────────────────┤ record; absent = use built-in defaults
             │ Free              │ ~7.34MB
             └───────────────────┘
```

**`nvDb` (`Shared/NvDb/`) now owns this address space** —
[docs/task_nv_db.md](docs/task_nv_db.md) §1-§5, phases 0-6 of §7. Each client
("user", `eNvDbUser`) gets a flat bounds-checked span starting at `0x00`;
placement, relocation and isolation live in one module and no consumer header
mentions a sector, a page, an erase or an address. It runs on the board:
`NvDbPlatform_Init()` in `defaultTask` right after `W25Q128_Init()`, with a
priority-inheriting mutex and a lowest-priority collector task. `nvdb
status|layout|usage|wear|drop` on the CLI, `/api/nvdb/*` over HTTP.

**Wear and occupancy are indication only.** Erase counts ride in the one
function that performs every erase, so no user cooperates and nothing can
forget to. They live in a RAM mirror (4 KB `.bss`, one `uint16` per erasable
unit) written back by the collector when it runs out of work — a flash
counter would need an erase to increment, and an erase is the thing being
counted. Neither wear nor occupancy ever influences an allocation, a
relocation, a write or a result code (C13); that is what lets both be cheap
and lossy, and why losing the wear area costs a statistic and nothing else.

**No application code reaches the medium any more, and CMake enforces
it.** `W25Q128_*` and `EXT_FLASH_*_ADDR` are banned throughout `App/` and
`Shared/`; the build fails at configure time if one appears. Three files are
exempt, each for a stated reason: `App/Log/crash.c` (writes in fault context,
address from `NvDb_GetAbsoluteAddress`), `App/Fwu/fwu_control.c` (compares
nvDb's placement against what the BL was built for), and
`Shared/Fwu/image_mgmt.c` (streams an HMAC over flash for the *bootloader*).

**The crash recorder does its I/O through a second driver, not the normal
one** — `Shared/Drivers/w25q_fault.c/.h`, application-only
(`SHARED_FAULT_SOURCES`, never `SHARED_SOURCES`). The HAL time base is a TIM6
interrupt at NVIC priority 15, which neither a fault handler (priority −1) nor
the TIM14 software watchdog (priority 15, equal so no preemption) can be
preempted by — so `HAL_GetTick()` is a **constant** in every context the
recorder runs in, and every `(HAL_GetTick() - start) > timeout` on the old
path was permanently false. A recorder that hangs destroys the evidence it
exists to preserve. The fault path is therefore register-level (SPI2 rebuilt
from compile-time constants, never from `hspi2`, whose RAM the fault may have
wrecked), budgeted in **DWT cycles** — 1 ms per bus flag, 600 ms per chip-BUSY
poll, 1.5 s for the whole save — and refreshes the IWDG exactly once, at the
top of `saveToFlash`. It never issues `66h`/`99h`: resetting a W25Q mid
program/erase would leave somebody else's page indeterminate, so it waits for
BUSY or gives up. CMake **fails the build** if the tick, a delay, `HAL_SPI_*`,
`HAL_GPIO_*`, `W25Q128_*` or an RTOS call appears in that one file.
**Verified on board #1 over the air 2026-08-24** (`Pd1.1.21`): a fault with
the flash mid-erase records a full log and the board is back in ~4 s by
software reset, and a build with the busy budget cut to 1 ms writes nothing
and still resets promptly instead of hanging 16 s for the IWDG. Design,
results and two defects the testing exposed:
[docs/task_fault_context_flash.md](docs/task_fault_context_flash.md) §9.

**There is no way to trigger a fault remotely, deliberately.** A temporary
`POST /api/system/fault/...` endpoint existed only long enough to run those
acceptance tests and was **removed in `Pd1.1.22`** — a network-reachable
"crash the board" route is not something to leave on a device. Re-running §5.3
means re-adding it; the doc records what it took. The three physical buttons
in `defaultTask` remain the only triggers, and **`BTN1` does not work**: a
write to `0x00000000` is an alias of internal flash and fails silently rather
than faulting (`bkpt #0` is the trigger that does work).

**Every crash record currently reads `type: Assert`** whatever the real fault
was: a `configASSERT` trips late in `Crash_GenerateReport` — in
`printTaskList` / `scanAllStacks`, which call FreeRTOS from fault context —
and writes a second record over the first, then resets the board itself
(hence `SFTRSTF`, not IWDG). PC/LR/SP/CFSR stay correct because
`g_crashEntry` is still latched; only the type is wrong. Its own task.

**The map above is still exactly what ships, and that is deliberate** — but
now for one reason only: **the bootloader**. It reads the boot status and
both blobs at addresses compiled into it, and the FWU→BL handoff that would
let it learn otherwise is still undesigned. So the built-in layout
(`s_targetSizes` in `nvdb_layout.c`) reproduces the hand-assigned map exactly:
on first boot `nvDb` adopts it (§4.4.1 first adoption), adds its two pinned
areas plus `mqttCfg`/`triceUdpCfg` above them, and moves nothing. `imageMeta`
is 12 KB rather than 4 KB because it absorbs the 8 KB hole the old map left,
so the packer reproduces the old addresses without learning to leave holes.
`FwuCtl_BlContractHolds()` checks the agreement at boot and
`POST /api/fwu/install` **refuses with 409** if a layout ever breaks it,
rather than letting a board discover it by not booting.

The compacting layout ships as `periphnet` v2 once that handoff exists.
Relocation itself is fully tested host-side — including a sweep that cuts the
power at all 60 steps of a relayout and checks every user's bytes
afterwards.

## Project Structure

```
PeriphNet/
  App/                            # Application modules (+ linker/metadata)
    app_freertos.c/h              # FreeRTOS task init, default task, trice task
    system.c/h                    # Reset cause, KickIwdg(), System_Init()
    triceConfig.h                 # Trice config; TRICE_UART_OUTPUT gates the
                                  #   USART3 wire (default OFF, UDP + USB CDC)
    app_info.c                    # sAppInfo const in .app_header section
    application.ld                # Linker: 0x08008000, 480KB + APP_HEADER region
    Can/                          # Pylontech BMS reader + simulator (CAN)
    Cmd/cmd_parser.c/h            # CLI command parser (composition root)
    nv_record.h                   # a CRC'd, versioned record in one nvDb
                                  #   area. USER-side policy: nvDb never
                                  #   learns what a version is (Rule 3)
    Data/telemetry.c/h            # Neutral telemetry model — RESERVED, no
                                  #   producers or consumers today (mqtt_bridge
                                  #   stopped consuming it in the config
                                  #   redesign); intended for the BMS→CAN path
    Fwu/fwu_control.c/h           # FWU process: install/confirm/verify/golden
    Img/image_store.c/h           # Image management: stored blob + metadata
    Http/http_server.c/h          # HTTP server task (netconn API, port 80)
    Log/                          # crash handler + backtrace, trice transports
                                  #   crash.c writes through Shared/Drivers/
                                  #   w25q_fault.c, NOT the normal driver
    Mon/sysmon.c/h                # System monitor: per-task liveness check-ins,
                                  #   stack high-water marks, heap, per-task CPU
                                  #   share and idle time, IWDG kick margin
    Net/                          # WireGuard peer: wg_link (tunnel netif +
                                  #   hub peer), wg_platform (port hooks: HW
                                  #   RNG-backed DRBG, TAI64N), wg_time
                                  #   (reboot-surviving monotonic seconds),
                                  #   wg_cfg (per-device net config in flash)
    NvDb/nvdb_platform.c/h        # the nvDb port: priority-inheriting mutex
                                  #   + lowest-priority collector task. The
                                  #   ONLY part of nvDb that knows FreeRTOS
    Modbus/                       # THE module: modbus.h (the only consumer
                                  #   header), modbus.c (surface: subscriptions,
                                  #   requests, plans, config), modbus_engine
                                  #   (timers + sequences), modbus_port
                                  #   (the frame-level driver contract),
                                  #   modbus_trice_sink
    Rs485/rs485_port.c/h          # RS485 port driver (USART2 + DE) — a
                                  #   peripheral, so OUTSIDE the module
    Test/modbus_test_port.c/h     # test peripheral in port slot 1, fed by
                                  #   `modbus inject`; a config binding, not a
                                  #   build flag
    Mqtt/mqtt_bridge.c/h          # MQTT bridge + HA discovery (generated
                                  #   from the active Modbus config)
  Shared/                         # First-party code compiled into BOTH targets
                                  #   (depends only on HAL + libc, no RTOS/lwIP)
    Crypto/                       # sha256, hmac_sha256, aes128, aes_gcm
                                  #   (NIST-vector-tested, see tests/)
    Fwu/                          # bl_app_contract.h, dfu_types.h,
                                  #   version.c/h, boot_status.c/h, image_mgmt.c/h
                                  #   boot_status_medium.h — boot_status.c is
                                  #   in BOTH targets so it cannot call nvDb;
                                  #   the BL answers this with direct flash,
                                  #   the app (App/Fwu/) with nvDb
    Modbus/                       # APPLICATION-ONLY Shared code (host-testable,
                                  #   never linked into the 32KB BL): register
                                  #   config records/store/selector, streaming
                                  #   JSON compiler, JSON export, decode/format,
                                  #   DLMS unit table
    NvDb/                         # APPLICATION-ONLY Shared code, same rule:
                                  #   nvdb.h (the only consumer header — no
                                  #   flash word in it), nvdb_exceptions.h
                                  #   (absolute addresses, crash handler + FWU
                                  #   only), nvdb_port.h (the RTOS contract),
                                  #   nvdb_layout.c (directory, placement,
                                  #   relayout, journal), nvdb_config.c (JSON)
    Drivers/w25q128.c/h           # W25Q64/128 SPI flash driver (tasks: HAL,
                                  #   tick deadlines, mutex). Bulk read/page
                                  #   program go by DMA (SPI2, DMA1 S3/S4);
                                  #   commands and DMA-unreachable buffers
                                  #   fall back to the polled HAL call
    Drivers/w25q_fault.c/h        # the SAME chip from FAULT context: register
                                  #   level, DWT-cycle budgets, no interrupt
                                  #   dependency. APPLICATION ONLY
  Core/                           # CubeMX-OWNED ONLY (regeneration-safe)
    Inc/ Src/                     # main.c, gpio.c, spi.c, HAL config ...
    Startup/startup_stm32f407vetx.s
  bootloader/                     # Bootloader (32KB, no RTOS/network)
    boot_main.c                   # Entry point, boot flow, jump-to-app
    boot_api.c                    # API table at 0x08007F00 (.bl_api section)
    boot_stm32f4xx_it.c           # Minimal ISRs (faults → while(1), SysTick)
    secrets.c/h                   # DEV FWU keys (secrets_prod.c overrides)
    bootloader.ld                 # Linker: 0x08000000, 32KB + BL_API region
  tests/                          # Host-native unit tests (no ARM toolchain):
                                  #   crypto NIST/RFC vectors, version gate,
                                  #   boot_status + Modbus config machinery
                                  #   over a NOR-faithful flash mock
    integration/                  # Host-side C++ harness (CLI + ImGui GUI)
                                  #   driving a LIVE board over USB/UART/UDP;
                                  #   builds to build_integration/
  Drivers/                        # STM32 HAL + CMSIS (vendor)
  LWIP/                           # lwIP integration (CubeMX)
  USB_DEVICE/                     # USB CDC (CubeMX)
  Middlewares/Third_Party/
    FreeRTOS/                     # RTOS
    LwIP/                         # TCP/IP stack
    trice/                        # Trice library (git submodule, uartDma branch)
    backtrace/                    # Cortex-M4 FP unwinder
    wireguard-lwip/               # smartalock WireGuard-lwIP (git submodule):
                                  #   netif + Curve25519/ChaCha20-Poly1305/
                                  #   BLAKE2s. APPLICATION ONLY, never the BL
  cmake/gcc-arm-none-eabi.cmake   # ARM toolchain file
  CMakeLists.txt                  # Dual-target build (bootloader.elf + application.elf)
  flash_nokill.sh                 # J-Link clone flash wrapper
  flash_both.jlink                # Flash BL + APP
  flash_application.jlink         # Flash APP only
  flash_bootloader.jlink          # Flash BL only
```

**Files grouped by module/functionality, NOT by file type.** Keep .c and .h together. Third-party libraries go in `Middlewares/Third_Party/`.

**Ownership rules:** `Core/` is CubeMX-generated only — never hand-edit outside
USER CODE sections; first-party code lives in `Shared/` (both targets), `App/`
(application), `bootloader/` (BL). `Shared/` must not depend on App/, bootloader/,
FreeRTOS, or lwIP. FWU keys (`bootloader/secrets*.c`) never link into the
application — CMake fails the build if a secrets file leaks into App sources.

## Firmware Version (Zhaga Pattern)

### Version Structure

```c
typedef struct {
    uint8_t  deviceType;   /* 'P' = PeriphNet                          */
    uint8_t  target;       /* 'v' release, 'd' dev, 'l' local          */
    uint16_t major;
    uint8_t  minor;
    uint8_t  patch;
    uint16_t hwId;         /* hardware variant (0 = generic)            */
} __attribute__((packed)) sFwVer;   /* 8 bytes */

typedef struct {
    sFwVer  ver;
    uint8_t reserved[24];
} __attribute__((packed)) sFwVerArea;  /* 32 bytes, padded for flash alignment */
```

**String format:** `Pv1.2.3` or `Pv1.2.3_ABCD` (with hwId). Use `ver_toString()`.

### Application Info Header (sAppInfo at 0x08008200)

```c
typedef struct {
    uint32_t    magic;                      /* 0x41505049 "APPI"             */
    sFwVerArea  fw_version;                 /* 32B firmware version          */
    uint32_t    image_size;                 /* binary size (0xFFFFFFFF=raw)  */
    uint8_t     image_hmac[32];             /* HMAC-SHA256 (build-patched)   */
    uint32_t    features;                   /* APP_FEATURE_* flags           */
    uint32_t    min_bl_version;             /* minimum BL API version        */
    uint32_t    reserved[8];
} sAppInfo;   /* 112 bytes, placed by linker in .app_header section */
```

Current version defined in `App/app_info.c`. The `image_size` and `image_hmac` are patched into the `.bin` by `tools/dfu_image_tool.py sign` after every build.

### Version Compatibility (ver_checkCompatibility)

- Device type must match
- Hardware ID must match
- Local-target (`'l'`) builds skip version ordering (allow any update)
- Otherwise incoming version must be strictly newer (major.minor.patch)

## Firmware Update (FWU) Architecture

### FWU Blob (.pnfw) Format

```
[0x00] sFwuManifest (64B)   cleartext: magic "PNFW", format, fw_version,
                            image_size, blob_size — authenticated as GCM AAD
[0x40] GCM nonce (12B)
[0x4C] ciphertext           AES-128-GCM over the SIGNED plaintext binary
[...]  GCM tag (16B)
[...]  CRC32 (4B)           over everything before it — keyless transfer check
```

blob_size = image_size + 96. The app treats blobs as opaque: it checks only
manifest + CRC32; all crypto (tag, HMAC, version gate) is bootloader-side.

### Boot Status (sBootStatus v3 in ext flash sector 0)

Uses **NOR flash bit-clearing semantics** — erased state is 0xFF (all 1s), individual bits can be cleared to 0 without erase.

```c
typedef union {
    uint32_t word;              /* erased = 0xFFFFFFFF                   */
    struct {
        uint32_t fwu_requested  : 1;  /* 0 = FWU requested by APP       */
        uint32_t confirmed      : 1;  /* 0 = outside actor confirmed    */
        uint32_t boot_attempt_0 : 1;  /* 0 = 1st unconfirmed boot used  */
        uint32_t boot_attempt_1 : 1;  /* 0 = 2nd                        */
        uint32_t boot_attempt_2 : 1;  /* 0 = 3rd → triggers rollback    */
    } bits;
} sBootFlags;
```

`sBootStatus` v3 is minimal: magic (`"BOOT"`), version, `last_fwu_result`
(eFwuRes of the last install/rollback, `fwuRes_noResult` if none — exposed via
`/api/fwu/status` so BL-side failures are diagnosable), header CRC32
(verified on read), and flags. Flags are **outside the CRC** so they can be
bit-cleared independently. Staged/golden metadata lives in the blob
manifests, not here.

### Bootloader Boot Flow

```
1. Init hardware (GPIO, SPI)
2. Init external flash (W25Q64)
3. Ensure boot status sector has valid header
4. Read FWU action from flags:
   ├─ fwuAction_install:  streaming blob install from staged area
   │     pass 0: manifest sanity + whole-blob CRC32
   │     pass 1: stream GCM decrypt (discarded) → verify tag + plaintext
   │             HMAC + capture decrypted sAppInfo; version gate (skipped
   │             if internal app header invalid — blank device accepts
   │             any authentic image)
   │     pass 2: erase sectors 2-7 → decrypt again → program → verify
   │     success → FinishFwu(fwuRes_ok, unconfirmed) + consume 1st attempt
   │     failure → FinishFwu(result, pre-confirmed) → boot old app
   ├─ fwuAction_rollback: same install from GOLDEN area (no version gate),
   │     erase staged manifest, FinishFwu(fwuRes_rollback, pre-confirmed)
   └─ fwuAction_none:     if unconfirmed → consume boot attempt
                    (local-target 'l' builds exempt)
5. Validate internal application (magic + size + HMAC-SHA256)
6. Jump to application at 0x08008000
```

Internal flash is never touched before the image authenticates (pass 1), so
a power cut mid-install retries cleanly on next boot.

### FWU Update Flow

```
Build:    application.bin → sign (HMAC) → package → periphnet_fwu.pnfw

Operator: POST /api/image/upload  (blob → image store; independent of FWU)
          POST /api/fwu/install   (arms fwu_requested flag → reboot)
          ...BL installs, new FW boots UNCONFIRMED...
          verify health via /api/fwu/status, MQTT, etc.
          POST /api/fwu/confirm   (REQUIRED — clears confirmed bit AND
                                   promotes stored blob → golden area)
```

Image management and FWU are fully independent modules: the stored blob is
persistent and self-describing (rescanned at boot), installing needs no
upload session, and an upload alone never triggers an install.  FWU code
consumes the stored image only through the image-store API (a read hold
protects it during golden promotion).

### Boot Attempt Counter & Rollback

After FWU install the new image is **unconfirmed** (install consumes attempt
1 of 3). Each further unconfirmed boot consumes another. When all 3 are
gone, the BL installs the **golden blob** (last confirmed image, kept
encrypted in ext flash), marks it pre-confirmed, and erases the staged
manifest so the failed image can't be re-installed by accident.

The app never self-confirms — `POST /api/fwu/confirm` is the outside
actor's job. Local-target (`'l'`) builds skip attempt counting entirely
(developer owns the device; JLink flashing stays friction-free).

## Bootloader API (sBootloaderApi at 0x08007F00)

Function pointer table placed in `.bl_api` linker section. API version 3.
All crypto uses real, NIST-vector-tested implementations.

| Function | Signature | Notes |
|----------|-----------|-------|
| `get_bl_version` | `void (uint32_t *major, *minor, *patch)` | |
| `verify_internal_app` | `int (void)` → BL_OK / BL_WRONG_MAGIC | |
| `verify_hmac` | `bool (data, size, expected[32])` | HMAC-SHA256, RAM buffer |
| `decrypt_blob` | `eFwuRes (data, size)` | AES-128-GCM, RAM blob |
| `verify_image_hmac` | `eFwuRes (addr, is_external, size, expected[32])` | streaming; app-usable for internal flash only (BL SPI globals unavailable to app) |

**Application usage:**
```c
const sBootloaderApi *bl = (const sBootloaderApi *)BL_API_TABLE_ADDR;
if (bl->magic == BL_API_MAGIC) {
    uint32_t maj, min, pat;
    bl->get_bl_version(&maj, &min, &pat);
}
```

**Note:** BL API functions must not rely on bootloader global state (RAM overwritten by application). Current functions use only stack, const data, and memory-mapped flash reads.

## Boot Status API (boot_status.h)

Shared code compiled into both bootloader and application.

| Function | Description |
|----------|-------------|
| `BootStatus_Read(status)` | Read + validate (magic, version, header CRC) |
| `BootStatus_Write(status)` | Erase sector + write full status |
| `BootStatus_EnsureValid()` | Create default header if corrupt |
| `BootStatus_RequestFwu()` | Arm FWU flag (NOR bit-clear, no metadata) |
| `BootStatus_ConfirmApp()` | Confirm healthy boot (NOR bit-clear) |
| `BootStatus_ConsumeBootAttempt()` | Use one boot attempt (BL calls this) |
| `BootStatus_IsUnconfirmed()` | Check if actor confirmation is pending |
| `BootStatus_GetFwuAction()` | Determine BL action: install/rollback/none |
| `BootStatus_FinishFwu(result, pre_confirmed)` | Record result + fresh flags |
| `BootStatus_GetFlags(flags)` / `BootStatus_AttemptsRemaining()` | Flag inspection |

## FreeRTOS Tasks

| Task | Stack | Priority | Role |
|------|-------|----------|------|
| defaultTask | 1024 words | osPriorityNormal (24) | Init, heartbeat (1s), IWDG kick (100ms), reboot/promotion jobs, button faults |
| http | 1024 words | osPriorityNormal (24) | HTTP server (netconn API, sequential connections) |
| trice | 256 words | osPriorityNormal+1 (25) | TriceTransfer() every 10ms |
| cmd | 1024 words | osPriorityNormal (24) | Command dispatch (20ms poll). Cmd_Feed only buffers in ISR context (USB CDC/UART1 RX); handlers may block and use RTOS/lwIP APIs |
| tudp | 512 words | osPriorityNormal (24) | Trice UDP broadcast consumer (runs lwIP TX path under core lock) |
| modbus | 640 words | osPriorityNormal (24) | The engine: drains one queue fed by three sources (FreeRTOS timers, port completions, mutating API calls), runs a sequence per due (device, plan, time table), dispatches samples to subscribers, drains the request FIFO, commits config swaps. **No poll loop and no start/stop** — `Modbus_Init` is the whole lifecycle and timers come and go with subscriptions (docs/modbus.md §4.2, §5.2) |
| nvdb | 256 words | osPriorityLow | The nvDb collector: erases deleted space in the background so erases stay off the write path. One erasable unit per lock acquisition, so a waiting writer gets in between units. Sleeps on a notify (1 s backstop); never reboots anything |
| mqtt | 512 words | osPriorityNormal-1 (23) | MQTT bridge: connect/reconnect backoff, HA discovery, set-topic resolution deferred out of tcpip_thread. Started/stopped at runtime (`mqtt start`), and **auto-started at boot when a broker has been saved** (`mqtt save`) — a board that was never configured still waits for the command |
| tcpip_thread | 6144 bytes | 24 | lwIP TCP/IP processing — **also runs all WireGuard crypto** (handshake + per-packet ChaCha20-Poly1305), which is why it is above the CubeMX 4096 default |
| EthIf | 1024 bytes | 48 (osPriorityRealtime) | Ethernet frame receive (was 350 B CubeMX default — overflowed, see docs/issue_idle_iwdg_crashloop.md) |

Stack overflow checking is ON (`configCHECK_FOR_STACK_OVERFLOW=2`):
overflow → crash report (`crashType_stackOverflow`) + reset. `configASSERT`
also records a crash report (`crashType_assert`) + resets instead of silently
spinning with interrupts masked.

## System Monitor (`App/Mon/sysmon.c`)

Answers "is the firmware healthy?" without a debugger. `sysmon` on the CLI,
`GET /api/system/status` as JSON, and a **System** card in the web UI.
`SysMon_Init()` runs from `App_FreertosInit()` — *before* any task exists,
because registration is a no-op until it has.

- **Liveness is a check-in, not a heartbeat.** A task registers itself from
  inside its own body (`SysMon_TaskRegister(stackWords, deadline_ms)`) and
  calls `SysMon_TaskCheckin(id)` once per loop iteration. A task that is still
  being scheduled but has stopped completing its loop is invisible to both the
  scheduler and the IWDG; that is exactly what a deadline catches. `deadline_ms
  = 0` means "legitimately blocks forever" (http on `netconn_accept`, tudp on a
  notify take) — still tracked for stack and CPU, never judged.
- **Nothing here reboots the board.** Going stale logs once and increments a
  counter. `SysMon_AllTasksAlive()` is the hook if that policy ever changes —
  a false positive would reset a healthy board, so it is the caller's call.
- **Sampling runs in defaultTask** (`SysMon_Poll()` next to `KickIwdg()`, real
  work once per second). Deliberate: the one task the monitor cannot report on
  is the one TIM14 + IWDG already cover.
- **CPU comes from the DWT cycle counter / 16** wired to
  `portGET_RUN_TIME_COUNTER_VALUE` (`configGENERATE_RUN_TIME_STATS=1`) — no
  timer peripheral is consumed. FreeRTOS's lifetime totals wrap every ~409 s at
  that rate, so every figure is a **delta over the 1 s window**; sysmon keeps
  its own 64-bit accumulator for lifetime run time. Board load is reported as
  `1000 - idle` per-mille, since interrupt time is charged to whichever task
  was interrupted.
- **Stacks** are the FreeRTOS high-water mark for *every* task, registered or
  not. Tasks created outside the application (IDLE, Tmr Svc, tcpip_thread,
  EthIf) have their configured depth in a small table in `sysmon.c` so the
  percentage means something; getting one wrong costs a wrong percentage and
  nothing else. Crossing below 64 free words logs once per task.
- **Watchdog margin** — `System_GetIwdgStats()` reports the longest gap ever
  seen between `KickIwdg()` calls. A maximum creeping toward 16.4 s is the
  early warning the reset itself never gives.
- Its own state lives in plain `.bss` (~1.6 KB), **not CCM** — CCM is the
  constrained region here and none of this is hot. The HTTP handler builds its
  JSON in a transient `pvPortMalloc` block for the same reason.

## HTTP API

Two independent sections: **image management** (`/api/image/*`, owned by
`App/Img/image_store.c`) and the **FWU process** (`/api/fwu/*`, owned by
`App/Fwu/fwu_control.c`).

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Web UI (Image Management + Firmware Update cards + crash log) |
| `/api/image/upload` | POST | Upload `.pnfw` blob (Content-Length required; optional `X-Filename` header, persisted). **Also accepts a WireGuard `.conf`** — the body is sniffed for the `PNFW` manifest magic, anything else under 4 KB is parsed as a tunnel config and applied+persisted (422 with the offending line number on reject) |
| `/api/image/info` | GET | JSON: status, present, name, version, size, image_size, crc32, progress, error |
| `/api/image/download` | GET | Download stored blob (still encrypted), original filename |
| `/api/image` | DELETE | Erase stored image (manifest + metadata) |
| `/api/fwu/install` | POST | Arm FWU flag + reboot (needs valid stored image) |
| `/api/fwu/confirm` | POST | Outside actor confirms running FW; promotes stored→golden |
| `/api/fwu/verify` | GET | Authenticate RUNNING image via BL HMAC (no FWU state change) |
| `/api/fwu/status` | GET | JSON: running/golden versions, confirmed, attempts_remaining, last_fwu_result, promote_pending, reset_cause |
| `/api/crash/latest` | GET/DELETE | Crash log read / clear |
| `/api/system/status` | GET | System monitor JSON: uptime, CPU load/idle (per-mille), heap free/min, IWDG gap max, plus one object per task (state, priority, stack free-min vs configured, CPU share + peak, lifetime run time, check-in count/age/deadline, stale flag) |
| `/api/system/reset-peaks` | POST | Clear peak CPU, the IWDG gap maximum and stale counters (stack high-water marks are FreeRTOS-owned and cannot be cleared) |
| `/api/nvdb/layout` | GET | The storage layout in force, free space, and anything that has come aboard but not been applied |
| `/api/nvdb/layout` | POST | Take a layout aboard (JSON, §2.7 schema). **202 Accepted** — nothing moves now: a layout is applied at the NEXT boot and only there, so check `lastApplyResult` afterwards. 422 with the offending field on a parse error; 409 when the advisory structural check refuses (nothing written, previous layout untouched) |
| `/api/nvdb/layout` | DELETE | Discard a layout that came aboard but has not applied. Never touches the layout in force |
| `/api/nvdb/usage` | GET | Occupancy and wear per user plus a medium summary. Add `?scan=1` to **observe** occupancy — that reads every area (~1 MB of SPI), so it is off by default and the reply says which it was |
| `/api/fwu/install` | — | Now also returns **409 `storage layout no longer matches the bootloader`** when nvDb has placed the boot status or a blob somewhere the BL does not look. Installing then would brick the board |
| `/api/modbus/config/verify` | POST | Validate a config JSON **without writing anything** — an upload consumes the inactive region on success *and* on failure, and that region holds the previous config |
| `/api/modbus/config/upload` | POST | Upload Modbus register config JSON — streams straight through the JSON→records compiler into the inactive LUT region (compile = validation; 422 pinpoints device/txn/point/field on reject; 409 while apply pending) |
| `/api/modbus/config/apply` | POST | Arm the config swap; the engine commits it at its next safe point (hot reload, no reboot) |
| `/api/modbus/config/status` | GET | JSON: active region, valid, device/txn/point counts, staged/swap state, last upload result |
| `/api/modbus/config/download` | GET | Active config re-serialized to JSON (data-faithful, not byte-identical) |
| `/api/modbus/config` | DELETE | Erase the config — the board becomes **unprovisioned**. There is no built-in default, so there is nothing to reset *to* |
| `/api/modbus/write` | POST | Write points on one device: `{"device":N,"items":[{"id":P,"value":V}],"timeout_ms":T}`. Values are **scaled integers** (the `writeMin`/`writeMax` domain — `cell_ovp` is `3550`, not `3.550`); no floats accepted. **422 refuses a point the config did not mark writable** — a `/write` must never let a read masquerade as a write. Per-item `result` (0 = ok, `-13` = out of bounds, `-4` = no reply) rides a **200**, so check items, not just status. Max 16 items; `rw` points report the register read back after writing |
| `/api/modbus/plans` | GET | Every plan slot: name, capability, device set, time tables, live subscriber count |
| `/api/modbus/plans` | POST | Create a plan (201 + `{"id":N}`); body is one element of the config's `plans[]`, so one schema, one validator |
| `/api/modbus/plans/N` | PUT | Modify a plan — **409 if a subscription named it** (`MB_PLAN_ALL` subscribers do not lock a plan) |
| `/api/modbus/plans/N` | DELETE | Free a slot; 409 likewise. Deleting moves no other plan — that is what makes a slot a slot |
| `/api/wg/status` | GET | JSON: running/session_up/provisioned, config_source+version, **public_key** (never the private one), peer_public_key, tunnel addr/mask, allowed_ips, endpoint, keepalive, RNG health, time base |
| `/api/wg/config` | POST | Set `tunnel_ip`/`tunnel_mask`/`endpoint_ip`/`endpoint_port` (JSON, all optional); persists unless `"save":false`. Changing the tunnel address restarts the netif |
| `/api/wg/config` | DELETE | Erase the stored config **including the private key** — the board becomes unprovisioned and the tunnel stops |
| `/api/wg/keygen` | POST | Mint a new identity key on-device from the DRBG, persist it, return the public half to register with the hub |
| `/api/wg/restart` | POST | Stop + start the tunnel (forces a fresh handshake) |

**Server architecture:** dedicated `http` task using the lwIP **netconn API**
(one connection at a time; 10s recv/send timeouts so dead clients can't stall
it). `App/Http/http_server.c` owns all HTTP parsing/JSON; `image_store.c`
(blob storage, `ImgStore_Upload*` streaming API, filename metadata) and
`fwu_control.c` (install/confirm/verify/golden, boot status) are
protocol-agnostic domain logic. Headers are read through a byte-stream
cursor, so TCP segmentation cannot break parsing; `Expect: 100-continue` is
answered (no curl stall). Upload does synchronous flash writes with lazy 4KB
sector erase; on completion the blob is validated (manifest + CRC32) and
becomes the persistent stored image. tcpip_thread is never blocked by image
transfers (MQTT keepalives unaffected). Golden promotion runs in defaultTask
under an image-store read hold; the W25Q128 driver serializes SPI access
with a mutex (skipped in BL and fault-handler context).

## Remote Access — WireGuard peer (`App/Net/`)

The board carries its own WireGuard tunnel rather than relying on a VPN box at
the site, so it is reachable identically on the HA LAN or on a foreign LAN it
does not own. `wireguardif` is a second lwIP netif; the Ethernet netif stays
the **default route**, so on-LAN traffic and the encapsulated WG UDP itself
take the short path and only the tunnel subnet routes through WG.

| Piece | Role |
|-------|------|
| `wg_link.c/h` | netif + hub peer bring-up, endpoint override, up/running state |
| `wg_platform.c/h` | the port's four required hooks + the printf sink |
| `wg_time.c/h` | reboot-surviving monotonic seconds for the TAI64N stamp |
| `wg_cfg.c/h` | persisted network config (tunnel addr/mask, hub endpoint) |

Bring-up runs in **defaultTask** (`App_DefaultTaskEntry`), not `MX_LWIP_Init()`
— `WgTime_Init()` needs `W25Q128_Init()` first. CLI: `wg start|stop|status|
endpoint <ip> [port]|ip <addr> [mask]|genkey|save|reset`.

- **NOTHING WireGuard lives in the image.** Keys, tunnel address, endpoint and
  routes are all per-device data in ext flash (`wg_cfg` record v2). The normal
  way to provision is to upload the `.conf` WGDashboard issues to
  `POST /api/image/upload`, which sniffs it apart from a `.pnfw` by content.
  An unprovisioned board **does not start the tunnel** and says so — it does
  not fall back to a shared identity, because two boards presenting one key to
  the hub take turns stealing the peer's endpoint and the greatest-timestamp
  rule locks the loser out. `App/Net/wg_conf.c` is the parser (host-tested,
  libc only); v1 records (address-only, pre-2026-08-12) are still read, and a
  board holding one has no keys, so it needs a `.conf` before it tunnels again.

- **Interface address and AllowedIPs are different things and must not be
  derived from each other.** `[Interface] Address` is a /32 in every `.conf`;
  `[Peer] AllowedIPs` is what routes into the tunnel. lwIP picks an output
  netif by subnet match, which a /32 never satisfies, so routing comes from
  `LWIP_HOOK_IP4_ROUTE` → `WgLink_Ip4Route()` walking the configured ranges
  (declared in `LWIP/Target/lwip_hooks.h`, wired in `lwipopts.h`). Up to
  `WIREGUARD_MAX_SRC_IPS` (2) ranges are honoured, so a second range such as
  the site LAN reaches hosts beyond the tunnel subnet. The hook refuses to
  route the hub's own endpoint address, so an over-broad `AllowedIPs` cannot
  swallow the encapsulated UDP and deadlock the link.

- **The tunnel address is device config, not network-assigned.** WireGuard has
  no address-assignment protocol: the hub's `AllowedIPs` is simultaneously the
  route *and* the rule for which inner source addresses a peer's key may claim,
  so both ends must be configured to agree. It is deliberately independent of
  whatever LAN the board lands on (that address comes from DHCP), which is what
  makes the board portable across foreign LANs. Mismatch fails in a confusing
  way: **the handshake still succeeds** (it carries no inner addresses), so the
  peer looks connected while every packet is dropped. `wg_cfg.c` moves the
  address out of the image into per-device flash — `POST /api/wg/config` is the
  only remote way to fix it, since the CLI needs physical access.

- **Soft dependency, always.** A dead hub costs one handshake packet every
  5 s (`REKEY_TIMEOUT`) and nothing else; `peer->active` stays set so the port
  retries forever without app intervention. No WG path blocks a task, the
  RS485 bus, or the IWDG kick.
- **Entropy:** hardware RNG (`hrng`) whitened through a SHA-256 DRBG, with a
  fresh HW word mixed into every output block. WireGuard draws ephemeral
  session keys from this, so RNG failures are counted and reported by
  `wg status` (`hw_seeded=0` means the DRBG fell back to jitter).
- **TAI64N must never go backwards.** The hub keeps the greatest stamp seen
  per peer, so a `sys_now()`-based clock breaks the tunnel after every reset.
  `wg_time.c` persists a seconds counter to a 4 KB ext-flash slot ring
  (append-only, one write per 15 min), jumps it forward 1 h every boot, and
  floors it at the CMake-supplied `WG_TIME_BUILD_EPOCH`.
- **Device private key is per-device flash data, never in the image.** It
  arrives with an uploaded `.conf`, or is minted on-device by `wg genkey` /
  `POST /api/wg/keygen` (DRBG + Curve25519 clamping) in which case it exists
  nowhere else. No endpoint ever returns it; `/api/wg/status` reports only the
  derived public key. It is NOT protected by the FWU key mechanism, which is
  bootloader-only by design — anything with SPI access to the W25Q64 can read
  it, so the guarantee is per-board isolation, not secrecy from a local
  attacker.
- **Control stays outbound-only.** Metrics and actuation both ride MQTT over a
  board-initiated socket; inbound (HTTP OTA UI, Trice) is debug-only. Do not
  add an HA→board request/response path that needs inbound routing per site.

Hub facts, address plan and the WGDashboard gotchas:
`docs/task_board_as_wireguard_peer.md`.

Provisioning, the `.conf` upload path and the still-open "handshake succeeds
but the tunnel carries no data" problem:
`docs/status_wg_provisioning_2026-08-12.md`.

## Coding Standards

**[`C coding standard.md`](C%20coding%20standard.md) in the repo root is
authoritative** — naming, formatting, Doxygen placement, the lot. Read it before
naming anything new. What follows is only the part most often got wrong here.

### Naming Conventions

- **Structures**: `s` + PascalCase (`sAppInfo`, `sBootStatus`)
- **Unions**: `u` + PascalCase (`sBootFlags` uses union internally)
- **Function pointers**: `f` + PascalCase (`fVerifyHmac`, `fDecryptBlob`)
- **Enum types**: `e` + PascalCase (`eFwuRes`, `eFwTarget`)
- **Enum values**: `<modulePrefix><Category>_<value>` — lowercase prefix,
  camelCase value, **never ALL_CAPS** (that spelling is for `#define` only):
  `fwuRes_errImageHmac`, `mbDecode_u32Be`, `crashType_stackOverflow`,
  `imgStore_uploading`, `sysRst_iwdg`. A `_last` sentinel closes the enum where
  it is meaningful — dense, runtime-only enums have one; bit-flag enums
  (`eResetCause`, `eModbusEventType`), sparse ones (`eFwuRes` ends at `0xFF`)
  and negative-valued ones (`eModbusErr`) do not.
- **Quantifiable values carry a unit suffix**: `timeout_ms`, `period_sec`,
  `voltage_mV`. This is mandatory in the standard and is the rule most often
  missed in existing code.

**Enum values must never be renumbered.** `eFwuRes` is persisted in the boot
status, `eModbusDecodeType` in the Modbus LUT records, and `eCrashType` in the
crash log — so a `_undefined = 0` prepended to any of them would silently
reinterpret flash written by an older image. That is why none of them has one,
despite the standard recommending it: appending is safe, prepending is not.

### Trice Usage

```c
#include "trice.h"
TRice("Message: %d\n", value);
```

**DO NOT** manually specify IDs — `trice insert` adds them automatically during build.

**Trice does NOT come out of USART3 by default.** Tracing is UDP
(`App/Log/trice_udp.c`) plus USB CDC. Set `TRICE_UART_OUTPUT` to 1 in
`App/triceConfig.h` to get the serial wire back — it then transmits
**interrupt-driven, not DMA**, because DMA1 streams 3 and 4 belong to the
external flash now and `USART3_TX` has no third stream to move to. Two
consequences: one interrupt per byte at 460800 while it is enabled, and with
it off **nothing logged before `Trice_UdpInit()` is captured anywhere** — the
UDP and USB sinks both hang off the auxiliary hook that init installs. That is
what the switch is for during bring-up.

**TRICE CANNOT BE USED IN lwIP CALLBACKS** (tcpip_thread context — e.g. MQTT client callbacks). Use only in FreeRTOS tasks (the netconn-based HTTP task is fine), ISRs, and fault handlers.

## Key Constraints

- **Bootloader must fit in 32KB** — no FreeRTOS, no lwIP, no Trice. Currently ~23KB. Monitor size.
- **Application starts at 0x08008000** — VTOR relocation via `APPLICATION_BUILD` define in `system_stm32f4xx.c`
- **BL API at 0x08007F00** — fixed address, function pointers must not use BL globals
- **tcpip_thread stack is 6144 bytes** (raised from the CubeMX 4096 in
  `LWIP/Target/lwipopts.h`; WireGuard's `chacha20poly1305_decrypt` →
  `poly1305_blocks` chain alone adds ~1.5 KB) — still heap-allocate large
  structs. The stack is `pvPortMalloc`'d, so the extra 2 KB comes out of the
  48 KB CCM FreeRTOS heap, not main SRAM
- **IWDG must be kicked every 16.4s** — `KickIwdg()` in default task + upload/scan/promotion paths
- **NOR flash bit-clearing** — boot flags can be modified without sector erase (1→0 only)
- **FWU keys are build+BL only** — never store keys in ext flash, never link `secrets.c` into the application, never expose key material through the BL API
- **OTA accepts only .pnfw blobs** — plaintext binaries are rejected at upload (manifest check); plaintext exists only in `build/` and internal flash
- **Confirm or roll back** — non-local builds must be confirmed via `POST /api/fwu/confirm` within 3 boots of an install, otherwise the BL restores the golden image
- **No raw lwIP callbacks for app code** — the HTTP server uses the netconn API in its own task; if raw callbacks are ever needed again, remember the recv-callback contract (return ERR_OK after consuming a pbuf, or tcp_abort + ERR_ABRT — anything else makes lwIP re-deliver a freed pbuf)
- **`Shared/Modbus/` is application-only Shared code** — host-testable like the rest of Shared/, but kept out of `${SHARED_SOURCES}` (own `SHARED_MODBUS_SOURCES` list) so it never bloats the 32KB bootloader
- **`Shared/NvDb/` is application-only Shared code too** (`SHARED_NVDB_SOURCES`), and CMake **fails the build if any bootloader source includes an `nvdb*` header**. The BL has no knowledge of `nvDb` by design — it learns where the firmware blobs are from the FWU module, which is what leaves the directory format free to evolve without a bootloader in lockstep
- **`nvDb` is the only authority over the medium, and CMake enforces that too** — no `W25Q128_*` call and no `EXT_FLASH_*_ADDR` anywhere in `App/` or `Shared/` outside the driver, `Shared/NvDb/`, and the three exempt files listed in the External Flash section
- **A write that only clears bits is programmed in place, never erased.** `boot_status` and the Modbus selector both keep their flags outside their CRCs precisely so a flag update is atomic; routing them through a store that only knew "erased or not" would have turned every one into an erase-and-write-back
- **Absolute addresses leave `nvDb` only through `nvdb_exceptions.h`** — the crash handler (fault context) and the FWU module. Including `nvdb.h` cannot reach it; that is the enforcement
- **CCM (64KB at 0x10000000) is CPU-only memory and is ~91% full** — never put DMA or peripheral-accessed buffers there. **This now includes any bulk buffer handed to the flash driver**: `w25q128.c` checks the address and silently drops to polling for anything outside main SRAM / internal flash, and FreeRTOS task stacks are `pvPortMalloc`'d from `.ccmheap`, so a **stack local is CCM too**. A new bulk flash buffer must be static/`.bss` or it quietly loses DMA (`docs/task_flash_wait_and_ota_cost.md`). It holds **two** NOLOAD sections with different lifecycles:
  - `.ccmram` (~11KB) — Modbus engine scratch (one sequence's spans and derived blocks), compiler/plan-rewrite state, plus MQTT bridge, HTTP server and image-store upload buffers. Zeroed by `System_Init()`.
  - `.ccmheap` (48KB) — the FreeRTOS heap (`configAPPLICATION_ALLOCATED_HEAP=1`, `ucHeap[]` in `App/system.c`). **It is a separate section precisely so `System_Init()` does not zero it**: tasks are already allocated from the heap by the time that memset runs. Any new CCM section must stay out of the `_sccmram.._eccmram` range for the same reason.
- **MQTT publishes happen on `mqttTask` only** — the bridge's Modbus callback copies and posts, so `LOCK_TCPIP_CORE` is off the Modbus sequence path entirely (docs/modbus.md §4.10); never call the raw lwIP MQTT API from app tasks without the core lock
