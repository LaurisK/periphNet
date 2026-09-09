# CLAUDE.md

## Project Overview

**PeriphNet** is an STM32F407VET6 firmware project. Long-term goal: RS485/Modbus-RTU to Ethernet bridge for Solis inverter + Home Assistant, with dual-image OTA bootloader. **Home Assistant is reached by being a Modbus TCP gateway that `solis_modbus` polls** — MQTT was removed from the project 2026-09-05 ([docs/design_solis_modbus_link.md](docs/design_solis_modbus_link.md) §6.4, §9.1). **That gateway is BUILT AND VERIFIED IN THE FIELD** (`App/Gw/modbus_tcp.c`, a `:502` listener bound to the tunnel address). `Pd1.1.49` was left running, confirmed and golden, on both boards on 2026-09-05 (board 1 has since moved to `Pd1.1.51`); **sodas was off the tunnel 2026-09-06 02:01 → 2026-09-09 12:20 and is BACK on `Pd1.1.54`** — the board was healthy the whole time; a stale NAT mapping on its LTE uplink swallowed its handshakes, and its own 5 s retries kept that mapping refreshed so it could never age out. **The WireGuard source port is fixed for the life of a boot AND identical on every boot** (lwIP's ephemeral counter restarts at `0xc000`, so both boards sit on `:62510`), so neither the 15-minute peer rebuild nor a reboot could escape it. `Pd1.1.54` rotates the source port on every recovery; `/api/wg/status` now reports `local_port` and `port_rotations` — see [docs/issue_wg_sodas_offline_2026-09-06.md](docs/issue_wg_sodas_offline_2026-09-06.md) §0 and §8; **all 48 register groups `solis_modbus` reads returned data from the live Solis**, worst 268 ms against a 1500 ms budget, with the exception map behaving as designed. Measurements and what is still outstanding — step 6 unapplied, HA not yet pointed at the board, no write ever made — are in §10 of that document.

**Done and in place:** the encrypted FWU pipeline (Zhaga pattern, extended) — firmware is distributed only as encrypted+authenticated `.pnfw` blobs; the bootloader does streaming AES-128-GCM decrypt + HMAC verify during install, with confirm/rollback via a golden image. HMAC, AES-128 and GCM are real, NIST-vector-tested implementations (not stubs). Also done: **the Modbus module rebuild of [docs/modbus.md](docs/modbus.md) §1-§9** — the v2 record format (capabilities/devices/plans), the subscription + event surface, the frame-level port contract with a test peripheral, event-driven per-device timers, runtime plan editing, and a generic HA bridge that was an ordinary consumer (**that bridge was MQTT and has since been removed**, §9.1 of the Solis link doc; the Trice sink is now the reference subscriber). **Not yet run on hardware.**

**External-flash DMA is done and hardware-verified** (`Pd1.1.23`, 2026-08-25,
installed over the tunnel). SPI2 took DMA1 streams 3 and 4 from the Trice UART,
which is now opt-in behind `TRICE_UART_OUTPUT` and interrupt-driven when
enabled. Bulk reads and page programs go by DMA when the buffer is
DMA-reachable; commands and CCM buffers fall back to the polled HAL call.
**It did not make OTA cheaper, and that was expected**: an upload still pins the
CPU at 100 % because the cost is `W25Q128_WaitReady` *waiting*, not
transferring. Measurements, the ranked risk list for a tunnel-only board, and
the design assessment of moving that wait to a timer + callback:
[docs/reference_dma_and_cubemx_regen.md](docs/reference_dma_and_cubemx_regen.md) and
[docs/task_flash_wait_and_ota_cost.md](docs/task_flash_wait_and_ota_cost.md).

**Everything Modbus lives in one document: [docs/modbus.md](docs/modbus.md)** — the design (§2), shipped behaviour (§3), config JSON, operator reference, test contract, and known limits. **§3 is what is on the board; §2 is what it is being rebuilt into, and none of §2 is implemented yet** (`App/Modbus/modbus.h` is a proposed header that nothing includes). §2 covers the subscription API, a frame-level port contract with a test port instead of test hooks, devices/types/parameters (baud and port are config, not API), and an event-driven scheduler of per-device timers — no poll loop. §2.16 sequences it: steps 1–7 extract the API with behaviour held constant, 8–14 replace the engine. Still undesigned and listed in §12: dialects beyond an address stride, consumer-side rate policy, and **rate-limiting the gateway seam** — nothing bounds how fast a `:502` client may submit raw transfers, and the failure mode is bus starvation of the JK poll rather than a crash. §4.11 is the gateway seam itself.

**Current phase:** the device is growing from a bridge into an edge controller — poll a JK BMS on the same/second RS485 bus, fuse with inverter data, and present a synthetic Pylontech pack to the inverter over CAN (`App/Can/`). **The AGGREGATION half of that path now exists too**: `App/Cluster/` presents N packs on one DC bus to an inverter as one battery ([docs/design_battery_cluster.md](docs/design_battery_cluster.md), implemented 2026-09-07). **`Pd1.1.51` is confirmed and golden on board 1** and publishing a complete Pylontech `0x351` — the first time this project has been able to. **The two-pack case is still unexercised**: board 1 has one JK, and sodas (the site with two on one bus) was offline. §13 of that doc has the run, including the ramp-in defect only hardware found. Its headline is that **the current limit is MEASURED, not estimated** — publish a limit, watch how hard the worst pack works at it, rescale — gated so it only ever learns from a measurement taken *at* the limit. It fixes the sodas defect where the inverter sees one of two JK packs and is told 660 Ah at 21 % when the bus holds 1254.7 Ah at 47.3 %. **It drives nothing yet, but the reason has changed**: the §7.4 blocker is GONE — `pack_jkbms` now produces `packCap_voltageLimits` from the JK's per-cell `charge_voltage`/`power_off_voltage` and its own `cell_count` (56.0 V / 43.2 V measured on a 16S pack), so `0x351` is fully populated and no longer contractually un-transmittable. What is missing now is only the frame source itself, plus the CAN bus-role defect. **The CAN half of that path also exists**: a CAN1/CAN2 store-and-forward bridge that is transparent between battery and inverter, registers every identifier that crosses it, and can BREAK toward the inverter and be answered by a registered frame source instead — which is exactly the takeover the cluster needs ([docs/design_can_bridge.md](docs/design_can_bridge.md)). **Not yet run on hardware.** That makes autonomy (correct operation with the WAN and HA both down) a hard requirement, and constrains how remote access is done. Direction and open questions: [docs/design_remote_access_and_autonomy.md](docs/design_remote_access_and_autonomy.md).

## Build and Flash

```bash
# First time -- defaults to the `mixed` optimisation profile (vendor -Os,
# first-party -Og, crypto -O2).  -DPERIPHNET_OPT=debug|mixed|size|speed.
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
- `build/bootloader.elf` / `.bin` — **~14.8 KB flash of 32 KB (45 %)**, ~2.9 KB
  RAM. It was ~24 KB until the optimisation profile landed
- `build/application.elf` / `.bin` — **FLASH IS NO LONGER THE CONSTRAINT; SRAM
  IS** (measured 2026-09-07 with the cluster module in, on the default `mixed`
  profile):

  | Region | Used | Limit | | Free |
  |---|---|---|---|---|
  | Flash (`.text`+`.rodata`+`.data`+vectors+header) | 307,940 | 491,520 | **62.7 %** | **183.6 KB** |
  | Main SRAM (`.bss`+`.data`+heap/stack) | 128,700 | 131,072 | **98.2 %** | 2.3 KB |
  | CCM (`.ccmram`+`.ccmheap`) | 59,124 | 65,536 | **90.2 %** | 6.3 KB |

  **THE BUILD IS OPTIMISED PER BUCKET, NOT PER BUILD TYPE.** Until 2026-09-07
  `CMAKE_BUILD_TYPE` was unset while `CMAKE_C_FLAGS_DEBUG` and
  `CMAKE_C_FLAGS_RELEASE` were both defined, so **neither applied** and every
  image this project ever shipped or measured was `-O0` — which is how flash
  reached 99.85 % with 724 bytes free. `PERIPHNET_OPT` replaces that:

  ```bash
  cmake -B build -S .                       # mixed (default)
  cmake -B build -S . -DPERIPHNET_OPT=debug # everything -O0, the old behaviour
  cmake -B build -S . -DPERIPHNET_OPT=size  # everything -Os
  cmake -B build -S . -DPERIPHNET_OPT=speed # everything -O2
  ```

  | profile | app | vendor | crypto | app flash | free | BL flash |
  |---|---|---|---|---|---|---|
  | `debug` | `-O0` | `-O0` | `-O0` | 478,216 (97.3 %) | 13.0 KB | 24,380 |
  | **`mixed`** | **`-Og`** | **`-Os`** | **`-O2`** | **307,940 (62.7 %)** | **183.6 KB** | **14,780** |
  | `size` | `-Os` | `-Os` | `-Os` | 277,076 (56.4 %) | 209.4 KB | 13,708 |
  | `speed` | `-O2` | `-O2` | `-O2` | 329,728 (67.1 %) | 158.0 KB | 14,816 |

  **SRAM and CCM are the same to within 64 bytes in all four** — optimisation
  buys flash and nothing else here, so the profile is not a way out of the
  `.bss` budget.

  Why mixed rather than a whole-tree `-O2`: **vendor code** (HAL, lwIP,
  FreeRTOS, USB, BSP, wireguard-lwip, trice) is the majority of the image,
  nobody steps through it and nobody is going to fix it, so it goes at `-Os`;
  **first-party `App/`, `Shared/`, `Core/`, `bootloader/`** stay at `-Og`,
  which is the level GCC documents as not interfering with debugging — locals
  and stack frames stay recognisable and the LR-scan backtrace in
  `App/Log/crash.c` still resolves; **`Shared/Crypto`** goes at `-O2` because
  the OTA install streams the whole image through GCM three times and the BL
  HMACs it again at every boot. `-fno-strict-aliasing` is global and not
  optional — this tree reads flash records, Modbus frames, `.pnfw` manifests
  and JSON straight out of byte buffers. `-flto` is deliberately NOT enabled:
  weak HAL callbacks, `__attribute__((section))` placement and the in-place
  HMAC patch each need their own verification.

  **Optimisation changes what a debugger shows and what a stack high-water
  mark reads.** The sysmon percentages and any stack figure recorded before
  2026-09-07 were measured at `-O0`; re-measure rather than compare. Build
  with `-DPERIPHNET_OPT=debug` when stepping through something that has been
  optimised away. Note also that the **STM32CubeIDE managed build has its own
  `-O` flags** and no longer matches CMake.

  Two vendor warnings appear only once the optimiser runs
  (`x25519.c` `-Wstringop-overread`, `triceDoubleBuffer.c`
  `-Wmaybe-uninitialized`); both were analysed as false positives and are
  silenced per file in `CMakeLists.txt`, with the reasoning there. The 22
  host unit tests pass built at `-O2 -fno-strict-aliasing`.

  **Measured 2026-09-05 with the Modbus TCP gateway in.** Removing MQTT
  returned 20.1 KB of flash (97.2 % → 93.1 %), 1.3 KB of SRAM and 1.4 KB of
  CCM; the gateway then spent **6.0 KB of flash, 440 B of SRAM and 772 B of
  CCM** of it, plus 3 KB of `.ccmheap` for the `mbtcp` task stack at runtime.
  *(Those percentages are `-O0` and are superseded by the table above; the
  SRAM and CCM figures still stand.)*

  A new multi-KB `.bss` array still does not fit — budget SRAM before adding
  anything, and remember OTA is the only delivery path to a deployed board.
  The blob shrank with the image: `periphnet_fwu.pnfw` is **308 KB, down from
  478 KB**, which is directly less time with the CPU pinned at 100 % during an
  upload. The `.bin` is signed in-place (IMAGE_SIZE + HMAC patched) after
  every build
- `build/periphnet_full.hex` — BL + signed APP combined, factory/initial J-Link write
- `build/periphnet_fwu.pnfw` — encrypted+authenticated blob, the ONLY artifact
  used for OTA (needs python3 `cryptography` + `intelhex` packages)
- `build/periphnet_ui.pnui` — **the web UI, gzipped, and a SEPARATE ARTIFACT
  from the firmware**. The page lives in external flash (`nvdbUser_webUi`), not
  in the image: it had grown to 15.6 KB and more than doubled in the five weeks
  to 2026-09-07, so every new card was spending flash the CAN frame source
  needs. Built by `dfu_image_tool.py ui` from `App/Http/web/index.html`; no keys
  and no python packages involved. **A UI change needs no firmware cycle and no
  `confirm`, and a firmware update does NOT replace the page.** Upload it with
  `curl -X POST --data-binary @build/periphnet_ui.pnui http://HOST/api/ui`.
  `periphnet_full.hex` cannot carry it — that is internal flash only — so a
  **factory-flashed board serves the built-in fallback page until the UI is
  uploaded once**. The fallback says exactly that and names the command; the
  whole `/api/` surface works without it

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
those. **It does NOT carry the `PERIPHNET_OPT` buckets**: its Debug/Release
configurations have their own `-O` flags, so a CubeIDE image is a different
size from the CMake one and neither its flash figures nor its stack high-water
marks are comparable. Headless check:
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
curl --compressed http://10.42.0.203/           # UI is stored gzipped; plain
                                                # curl would print binary
curl http://10.42.0.203/api/ui                  # is a page stored? size/crc
curl http://10.42.0.203/api/image/info                 # stored image (name/version/size/crc)
curl http://10.42.0.203/api/fwu/status                 # FWU state (running/golden/confirmed)
curl http://10.42.0.203/api/system/status              # tasks, stacks, heap, CPU, IWDG margin

# THE SCRIPT: firmware and/or web UI, with the confirm/rollback semantics
# built in (docs/reference_deploy.md).  It deliberately does NOT confirm
# unless asked -- confirming disarms the only rollback the board has.
./tools/deploy.sh sodas                 # full cycle, stops before confirm
./tools/deploy.sh sodas --ui-only       # just the page: no reboot, safe
./tools/deploy.sh zaliakalnis --fw-only --confirm   # health-gated confirm
# sodas = 10.77.0.5, zaliakalnis = 10.77.0.64; any host/IP also works

# Full OTA cycle by hand (blob only — plaintext .bin uploads are rejected)
curl -X POST -H "X-Filename: periphnet_fwu.pnfw" \
  --data-binary @build/periphnet_fwu.pnfw \
  http://10.42.0.203/api/image/upload
curl -X POST http://10.42.0.203/api/fwu/install        # arms FWU + reboots
# ...device reboots, BL installs, new FW comes up UNCONFIRMED...
curl http://10.42.0.203/api/fwu/status                 # check health/version
curl -X POST http://10.42.0.203/api/fwu/kick           # "still testing" — reloads the
                                                       # self-reboot countdown (?window_sec=N)
curl -X POST http://10.42.0.203/api/fwu/confirm        # REQUIRED — stops the countdown,
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
image after 3 unconfirmed boots — and since an unconfirmed image now **reboots
itself every 15 minutes unless kicked**, those 3 boots happen on their own in
about 45 minutes rather than waiting for someone to power-cycle the board.
That is deliberate: it is the only thing that recovers a firmware which boots
fine but cannot be reached. Local-target (`'l'`) builds are exempt from both,
so JLink dev flashing is unaffected.

**Trice UART output** (USART3 PD8/TX, 460800 baud) — **off by default**;
set `TRICE_UART_OUTPUT` to 1 in `App/triceConfig.h` first (interrupt-driven,
not DMA — DMA1 S3/S4 belong to the flash now):
```bash
./tools/trice log -p COM -args "/dev/ttyUSB0:460800" -i ./til.json -li ./li.json
```

**Host-native unit tests** (no ARM toolchain; **the battery cluster's limit
arithmetic — the closed loop, its binding gate, every restart trigger, the slew
and all the aggregation — which R4.6 makes a REQUIREMENT rather than a
convenience, because what is being computed is a current limit for a live
inverter with a live battery behind it; two property sweeps of 10 000 and
4 000 pseudorandom buses back it,** crypto NIST/RFC vectors, version
gate, boot_status flag lifecycle, **the Modbus TCP (MBAP) codec against every
frame `solis_modbus` sends** — `App/Gw/mbap.c` is deliberately pure so the
gateway's framing needs no board — the JSON module — the writer against a
guard-banded buffer, the reader's escape decode and reject matrix, and the
round trip that only closes if both halves agree — the Modbus config machinery — record
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
Dear ImGui GUI, `build_integration/periphnet_gui`. Modbus test contracts
live in `docs/modbus.md` §6.

**The `gw_*` cases drive the Modbus TCP gateway over TCP, not over Trice/CLI**
— the gateway has no CLI surface by design. They replay the 48 register groups
`solis_modbus` really reads, with a client shaped like its `client_manager.py`
(one transaction at a time, 50/100 ms spacing, 5 s timeout), and they **skip**
cleanly when nothing answers on `:502` — which on a board with no tunnel is the
correct state, not a fault. Writes are registered as `gw_hw_write_*` and
skipped: they move real power, so run them by name after `confirm`.

**`tests/fixtures/solis_modbus_groups.h` is GENERATED** by
`tools/extract_solis_groups.py` from upstream's own `hybrid_sensors.py` plus a
real inverter read-out. It is committed, so nothing needs a `solis_modbus`
checkout; regenerate it when upstream's register map moves.

## Hardware

- **MCU:** STM32F407VET6 (512KB Flash, 128KB SRAM, 168MHz)
- **External Flash:** W25Q64 (8MB, SPI2 at 21MHz) — JEDEC 0xEF/0x40/0x17
- **EEPROM:** AT24C02BN (256 bytes, I2C)
- **Ethernet PHY:** DP83848IVV (RMII)
- **CAN1:** PD0/RX PD1/TX — the **inverter** side by default
- **CAN2:** PB5/RX PB6/TX — the **battery** side by default. bxCAN filter banks
  are shared: CAN1 owns 0-13, CAN2 owns 14-27, and CAN2 needs CAN1's clock. Both
  cells come up at 500 kbit with **wide-open filters** and the boot mode is
  `bridge` — the board sits in the path, so any other boot state cuts the
  inverter off from its BMS. `App/Can/can_bus.c` is the ONLY file allowed to
  name a CAN HAL function or handle; CMake fails the build otherwise
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
             │ Free              │ ~7.34MB — nvDb places packCfg, packState,
             └───────────────────┘   canLog (4MB) and clusterCfg above wgCfg
```

**`nvdbUser_webUi` (64 KB) is the newest user and `NVDB_TARGET_VER` is
now 5** — the gzipped web page, ~5.3 KB used of 64 KB. Before it,
`nvdbUser_clusterCfg` (4 KB) made it 4. A new user is itself a layout change even when the placement policy
does not otherwise move; it appends above the pinned areas, so the
bootloader contract (`FwuCtl_BlContractHolds()`) is unaffected.

**`nvDb` (`Shared/NvDb/`) now owns this address space** —
[docs/design_nv_db.md](docs/design_nv_db.md) §1-§4. Each client
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
the two defects it exposed are recorded here (`BTN1`, and the `type: Assert`
overwrite below).

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
areas plus every user this image has grown since — `mqttCfg` (**retired, slot reserved — never delete the enumerator, see the Solis link doc §9.2**)/`triceUdpCfg`,
`packCfg`/`packState`, and now `nvdbUser_canLog` (App/Can's flash trace,
[docs/design_can_bridge.md](docs/design_can_bridge.md) §10; 4 MB, the largest
single user on the board) — above them, and moves nothing else. `imageMeta`
is 12 KB rather than 4 KB because it absorbs the 8 KB hole the old map left,
so the packer reproduces the old addresses without learning to leave holes.
`FwuCtl_BlContractHolds()` checks the agreement at boot and
`POST /api/fwu/install` **refuses with 409** if a layout ever breaks it,
rather than letting a board discover it by not booting.

The compacting layout ships as `periphnet` v3 once that handoff exists —
bumped from v2 when `nvdbUser_canLog` landed, since a new user is itself a
layout change even though this image's *placement policy* did not otherwise
move.

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
    Can/                          # THE CAN MODULE: can_bus (the ONLY owner of
                                  #   both bxCAN cells — bit timing, wide-open
                                  #   filters, per-bus software TX queue, and
                                  #   THE RX dispatcher: one weak HAL callback,
                                  #   fanned out by (bus, id, mask)),
                                  #   can_bridge (modes: off/monitor/bridge/
                                  #   bms; per-direction policy; the frame
                                  #   source that answers the inverter),
                                  #   can_monitor (the tap: per-identifier
                                  #   register + trace ring), bms_reader and
                                  #   bms_sim (now an ordinary subscriber and
                                  #   the bridge's frame source), pylontech.h
      Cluster/                      # THE BATTERY CLUSTER MODULE: N packs on one
                                  #   DC bus presented to an inverter as ONE
                                  #   battery.  cluster.h (the only consumer
                                  #   header), cluster_calc (THE PURE CORE --
                                  #   the closed-loop limit search and its
                                  #   gate, restarts, sanitiser, slew,
                                  #   aggregation, divergence; libc-only, zero
                                  #   file statics, no Pack_ call),
                                  #   cluster_cfg (JSON + nine name
                                  #   accessors), cluster.c (tick, the
                                  #   lock-free double-buffered publish, nvDb).
                                  #   A consumer of App/Pack and a producer of
                                  #   one settled snapshot; owns no peripheral
  Cmd/cmd_parser.c/h            # CLI command parser (composition root)
    nv_record.h                   # a CRC'd, versioned record in one nvDb
                                  #   area. USER-side policy: nvDb never
                                  #   learns what a version is (Rule 3)
    Data/telemetry.c/h            # Neutral telemetry model — RESERVED, no
                                  #   producers or consumers today (its last
                                  #   consumer was mqtt_bridge, now removed);
                                  #   intended for the BMS→CAN path
    Gw/modbus_tcp.c/h             # THE MODBUS TCP GATEWAY: a :502 listener
                                  #   bound to the TUNNEL address, MBAP codec,
                                  #   one connection at a time, each request
                                  #   one queued Modbus_RawTransfer. A consumer
                                  #   of App/Modbus, outside it because it
                                  #   includes lwIP. TRANSPARENT both ways —
                                  #   the tunnel is its whole authorization
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
                                  #   hub peer), wg_ladder (THE PURE RECOVERY
                                  #   LADDER -- rung decisions only, libc-only,
                                  #   zero file statics, host-tested),
                                  #   wg_platform (port hooks: HW
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
    Func/func.c/h                 # the shared functionality task (ZhagaFW
                                  #   pattern): one task, one static queue, a
                                  #   packed event-ID space, one range per
                                  #   client. A client never sees the queue
    Pack/                         # THE BATTERY PACK MODULE: pack.h (the only
                                  #   consumer header), pack.c (core: state,
                                  #   condition, commands, persistence),
                                  #   pack_fsm (pure, host-tested condition/
                                  #   staleness/confidence), pack_cfg (JSON),
                                  #   pack_type.h (the vendor blueprint seam),
                                  #   pack_types.h (the registration roster),
                                  #   pack_jkbms (pull/Modbus),
                                  #   pack_pylontech (push/CAN — registers but
                                  #   STILL REFUSES TO BIND: the CAN RX
                                  #   dispatcher it waited for now exists, what
                                  #   is missing is its own frame parser)
  Shared/                         # First-party code compiled into BOTH targets
                                  #   (depends only on HAL + libc, no RTOS/lwIP)
    Crypto/                       # sha256, hmac_sha256, aes128, aes_gcm
                                  #   (NIST-vector-tested, see tests/)
    Json/                         # THE JSON module: json.h (the only consumer
                                  #   header), json_write.c (Json_Cat -- the
                                  #   CLAMPED append; the `n += snprintf(&buf[n],
                                  #   CAP - n, ...)` idiom it replaced underflows
                                  #   CAP - n into a ~4 GB size limit past the
                                  #   end of the buffer -- plus Json_Escape),
                                  #   json_read.c (ONE streaming tokenizer over
                                  #   a byte source, replacing three private
                                  #   ones; it is the only one that ever handled
                                  #   backslash escapes). APPLICATION-ONLY
                                  #   Shared code (SHARED_JSON_SOURCES)
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
(application), `bootloader/` (BL). `Shared/` must not depend on App/,
bootloader/ or lwIP, and must not depend on FreeRTOS **in the bootloader
build** — `Shared/Drivers/w25q128.c` does take the RTOS mutex in the
application, guarded by `#ifndef BOOTLOADER_BUILD`. The guard is right; an
absolute "never" here would be a wrong rule in a file used as an authority. FWU keys (`bootloader/secrets*.c`) never link into the
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
          verify health via /api/fwu/status, /api/system/status, etc.
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

**AN UNCONFIRMED IMAGE NOW REBOOTS ITSELF UNLESS IT IS KICKED**, and that is
what makes the rollback above reachable from the failure that actually
happens on this board. The attempt counter is only spent by a reset, so an
image that **boots fine and cannot be reached** — a broken WireGuard config,
a wedged HTTP task — used to sit there unconfirmed forever and the rollback
never fired. Now `FwuCtl_Init()` arms a countdown
(`FWU_CONFIRM_WINDOW_DEFAULT_SEC`, 900 s), `defaultTask` resets the board
when it expires, each expiry spends one attempt, and three unattended
expiries end with the golden image back — **≈45 minutes, unattended**.
The procedure is upload → install → `POST /api/fwu/kick` while you test →
`confirm` when satisfied. A kick is the ONLY thing that reloads it (an
implicit "traffic counts" rule would let the subsystem under test hold the
deadline open forever); the single bounded exception is an upload in flight,
which cannot outlive the HTTP recv timeout. **Not armed** for local (`'l'`)
builds, for a confirmed image, or — the non-obvious one — when there is **no
golden image**, since a virgin board's erased boot-status flags read as
unconfirmed and rebooting toward a rollback that must fail helps nobody.
**The cost is a false rollback if the WAN is down longer than the window ×3**;
that is the one way this violates the autonomy requirement, and the window is
the knob (`FWU_CONFIRM_WINDOW_DEFAULT_SEC`).

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
| defaultTask | 1024 words | osPriorityNormal (24) | Init, heartbeat (1s), IWDG kick (100ms), reboot/promotion jobs, **the FWU confirmation deadline** (an unconfirmed image that is not kicked resets here), button faults |
| http | 1024 words | osPriorityNormal (24) | HTTP server (netconn API, sequential connections) |
| trice | 256 words | osPriorityNormal+1 (25) | TriceTransfer() every 10ms |
| cmd | 1024 words | osPriorityNormal (24) | Command dispatch (20ms poll). Cmd_Feed only buffers in ISR context (USB CDC/UART1 RX); handlers may block and use RTOS/lwIP APIs |
| tudp | 512 words | osPriorityNormal (24) | Trice UDP broadcast consumer (runs lwIP TX path under core lock) |
| modbus | 640 words | osPriorityNormal (24) | The engine: drains one queue fed by three sources (FreeRTOS timers, port completions, mutating API calls), runs a sequence per due (device, plan, time table), dispatches samples to subscribers, drains the request FIFO, commits config swaps. **No poll loop and no start/stop** — `Modbus_Init` is the whole lifecycle and timers come and go with subscriptions (docs/modbus.md §4.2, §5.2) |
| func | 512 words | osPriorityNormal-1 (23) | The shared functionality task — one task, many clients, a packed event-ID space. Its clients are the **pack** module (binds at start-up, drains pack events) and the **cluster**, which takes no event range because it posts nothing. The 250 ms wall-clock tick runs `Pack_Tick` then `Cluster_Tick` **in that order, same iteration**, so the cluster aggregates the state the pack tick just settled. Queue and stack are **static in `.bss`, not `.ccmheap`** — CCM is the tight region |
| mbtcp | 768 words | osPriorityNormal-1 (23) | The Modbus TCP gateway on `:502`. **Below the engine deliberately** — a request arriving from the WAN must never delay the sequence that keeps the battery and inverter talking. Idle until a client connects; binds to the tunnel address only, and retries every 5 s while the board is unprovisioned rather than exiting. **Measured 2026-09-05: 537 of 768 words used under load, converged — 512 would have overflowed.** The depth is lwIP's `netconn_write` path and does not scale with frame size, since the frame buffers are static in `.ccmram` |
| nvdb | 256 words | osPriorityLow | The nvDb collector: erases deleted space in the background so erases stay off the write path. One erasable unit per lock acquisition, so a waiting writer gets in between units. Sleeps on a notify (1 s backstop); never reboots anything |
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
| `/api/fwu/confirm` | POST | Outside actor confirms running FW; promotes stored→golden. Also **stops the confirmation countdown** |
| `/api/fwu/kick` | POST | Reload the confirmation countdown (`?window_sec=N`, clamped 30..3600). **The new FWU procedure**: an unconfirmed image reboots itself unless kicked, so an updater that loses its tunnel to a bad build recovers the board by doing nothing. 409 when nothing is counting down, saying whether that is `exempt` (local build) or already `confirmed` |
| `/api/fwu/verify` | GET | Authenticate RUNNING image via BL HMAC (no FWU state change) |
| `/api/fwu/status` | GET | JSON: running/golden versions, confirmed, attempts_remaining, last_fwu_result, promote_pending, reset_cause, plus **`confirm_guard`** (armed/exempt/window_sec/remaining_sec/kicks) — the countdown to a self-reboot |
| `/api/ui` | GET | Is a web page stored: `present`, `size`, `crc32`, `encoding`, `max_bytes` |
| `/api/ui` | POST | Upload the `.pnui` blob the build produces. Streams to `nvdbUser_webUi`; **the header is written LAST, after the payload is read back and CRC-checked**, so an interrupted upload leaves the fallback serving rather than half a page. 422 names what was wrong with the blob |
| `/api/ui` | DELETE | Erase the stored page; `GET /` falls back to the built-in one |
| `/` | GET | **Serves the stored page STILL GZIPPED** (`Content-Encoding: gzip`; the board never decompresses). `Accept-Encoding` is deliberately not consulted — honouring it would mean storing an uncompressed copy too. Use `curl --compressed`; every `/api/` reply stays plain JSON. With no page stored, answers the built-in fallback |
| `/api/crash/latest` | GET/DELETE | Crash log read / clear. **`backtrace` is now `scan`** — LR candidates from the stack scan, not an ordered chain (the frame-pointer unwinder was removed 2026-09-07) |
| `/api/system/status` | GET | System monitor JSON: uptime, CPU load/idle (per-mille), heap free/min, IWDG gap max, plus one object per task (state, priority, stack free-min vs configured, CPU share + peak, lifetime run time, check-in count/age/deadline, stale flag). **Compare `tasks_total` / `tasks_sampled` / `tasks_listed`**: the array is capped by the reply buffer and the last entries are rolled back whole rather than truncated mid-object, so a short array is normal and now says so. At 3072 bytes it silently held only eleven of fourteen and dropped `tcpip_thread` — the stack carrying the WireGuard crypto chain — while still reporting `tasks_total:14` |
| `/api/system/reset-peaks` | POST | Clear peak CPU, the IWDG gap maximum and stale counters (stack high-water marks are FreeRTOS-owned and cannot be cleared) |
| `/api/system/last-restart` | GET/DELETE | **The black box.** What the board looked like immediately before it reset *itself* — uptime, how long the hub had been silent, the source port that was not working, heap floor, stale tasks, the previous `reset_cause`. `present:false` is the normal answer and means the last reset was a power cycle, an OTA or an operator, not the board rescuing itself. Lives in `.ccmnoinit` (CCM, outside the memset range), so it survives a soft reset and an OTA install but not a power cycle |
| `/api/system/reboot` | POST | Restart the board (`?delay_ms=N`, default 1000, floored at 500). Answers **before** it acts — the reset is armed on a deadline and performed by defaultTask, so the caller gets a 200 instead of a dropped connection. Arms **no** FWU state, unlike the `/api/fwu/install` trick that used to stand in for it |
| `/api/nvdb/layout` | GET | The storage layout in force, free space, and anything that has come aboard but not been applied |
| `/api/nvdb/layout` | POST | Take a layout aboard (JSON, §2.7 schema). **202 Accepted** — nothing moves now: a layout is applied at the NEXT boot and only there, so check `lastApplyResult` afterwards. 422 with the offending field on a parse error; 409 when the advisory structural check refuses (nothing written, previous layout untouched) |
| `/api/nvdb/layout` | DELETE | Discard a layout that came aboard but has not applied. Never touches the layout in force |
| `/api/nvdb/usage` | GET | Occupancy and wear per user plus a medium summary. Add `?scan=1` to **observe** occupancy — that reads every area (~1 MB of SPI), so it is off by default and the reply says which it was |
| `/api/fwu/install` | — | Now also returns **409 `storage layout no longer matches the bootloader`** when nvDb has placed the boot status or a blob somewhere the BL does not look. Installing then would brick the board |
| `/api/modbus/config/verify` | POST | Validate a config JSON **without writing anything** — an upload consumes the inactive region on success *and* on failure, and that region holds the previous config |
| `/api/modbus/config/upload` | POST | Upload Modbus register config JSON — streams straight through the JSON→records compiler into the inactive LUT region (compile = validation; 422 pinpoints device/txn/point/field on reject; 409 while apply pending) |
| `/api/modbus/config/apply` | POST | Arm the config swap; the engine commits it at its next safe point (hot reload, no reboot) |
| `/api/modbus/config/status` | GET | JSON: active region, valid, device/txn/point counts, staged/swap state, last upload result |
| `/api/modbus/gw` | GET | The Modbus TCP gateway: `listening`, the `bind_ip` it is bound to, connections, requests, exceptions and **`busy_exceptions`** (0x06 answers — a few are healthy back-pressure, a rising count is a bus-budget question). **`listening:false` with a zero `bind_ip` means no tunnel is configured**, which is deliberate and not a fault |
| `/api/modbus/bus` | GET | **Line occupancy, per port** — txns, busy ms, duty per-mille lifetime and over a 60 s window, last and longest frame. The budget answer for "will another device fit on this pair": a timeout counts as busy because the line is, so `max_ms` near the response timeout is a slave that is not answering and is eating the wire |
| `/api/modbus/bus/reset` | POST | Zero every port's counters and restart the window |
| `/api/modbus/config/download` | GET | Active config re-serialized to JSON (data-faithful, not byte-identical) |
| `/api/modbus/config` | DELETE | Erase the config — the board becomes **unprovisioned**. There is no built-in default, so there is nothing to reset *to* |
| `/api/modbus/write` | POST | Write points on one device: `{"device":N,"items":[{"id":P,"value":V}],"timeout_ms":T}`. Values are **scaled integers** (the `writeMin`/`writeMax` domain — `cell_ovp` is `3550`, not `3.550`); no floats accepted. **422 refuses a point the config did not mark writable** — a `/write` must never let a read masquerade as a write. Per-item `result` (0 = ok, `-13` = out of bounds, `-4` = no reply) rides a **200**, so check items, not just status. Max 16 items; `rw` points report the register read back after writing |
| `/api/modbus/plans` | GET | Every plan slot: name, capability, device set, time tables, live subscriber count |
| `/api/modbus/plans` | POST | Create a plan (201 + `{"id":N}`); body is one element of the config's `plans[]`, so one schema, one validator |
| `/api/modbus/plans/N` | PUT | Modify a plan — **409 if a subscription named it** (`MB_PLAN_ALL` subscribers do not lock a plan) |
| `/api/modbus/plans/N` | DELETE | Free a slot; 409 likewise. Deleting moves no other plan — that is what makes a slot a slot |
| `/api/pack/status` | GET | Every battery pack: condition + `why` **and `whyText`**, alarms as a mask **and `alarmNames`** — the numeric reason AND the sentence for it (*"bind key matched no device, or more than one"*), which used to exist only on the CLI and so needed physical access on a tunnel-only board. Plus caps/cmds/flags, V/A, SOC/SOH with **confidence**, amp-hours (remaining / capacity / nameplate), per-direction switch state (`chargeSwitch` + `chargeSwitchText`), current **and voltage** limits, alarms, and a **per-group age array** — a pack is not one clock (on a JK the cell group runs 4–5 s behind). Also **how it is anchoring**: `dcRes_uOhm`/`dcResSteps` (the fitted power-path resistance) and `anchorSamples`/`anchorIrSamples`. A pack whose baseline load never falls below C/50 cannot anchor from rest and its SOC free-runs — `anchorSamples` stuck at 0 is that, and it is why zaliakalnis read 82 % with its cells at 3235 mV |
| `/api/pack/cells` | GET | `?idx=N` — per-cell mV and balance-lead mΩ, plus balancer state. **404 when the type reports no cell detail** (a Pylontech-speaking pack never will) — an absent capability, not an error |
| `/api/pack/config` | GET | The active configuration, re-serialised (data-faithful, not byte-identical) |
| `/api/pack/config` | POST | Upload + apply a pack configuration. 422 names the offending **pack index and key**; 409 while another parse holds the shared scratch |
| `/api/pack/config/verify` | POST | Same parser, same pass, same result struct — writes nothing |
| `/api/pack/config` | DELETE | Erase it; the board becomes **unprovisioned**. There is no built-in default, so there is nothing to reset *to* |
| `/api/cluster/status` | GET | The battery cluster: the published figures, **the four-number causal chain per direction** (loop → derated → slewed → published) with the loop's state, `why` and **which pack is binding the limit**, plus every member's share, load, age and flags. `?packs=0` omits the member array. Every enum renders as a **name**. **`bindingSample*` is expected to be a small fraction of `ticks`, and zero on a quiet site** — the loop only learns while something is actually asking the battery for current; that is correct, not a fault |
| `/api/cluster/config` | GET | Active configuration, re-serialised (data-faithful) |
| `/api/cluster/config` | POST | **202 Accepted** — the configuration is STAGED and adopted by the next tick, never applied inline; **409** while a stage is pending; 422 names the member index and key |
| `/api/cluster/config/verify` | POST | Same parser, same pass, same result struct — writes nothing |
| `/api/cluster/config` | DELETE | Erase it; the board becomes **unprovisioned** and the published limits fall to zero at the next tick. There is no built-in default |
| `/api/can/status` | GET | The CAN bridge: mode, which cell is battery and which inverter, bitrate, the frame source, the override list, per-direction forward/suppress/drop counters, and both cells' health (rx/tx, overruns, errors, bus-off, ESR REC/TEC, TX queue depth) |
| `/api/can/traffic?bus=N` | GET | The identifier register of one bus: per ID the counts, last payload, age, **observed period band** and how often the payload actually changed — which is how a live measurement is told from a constant |
| `/api/can/trace` | GET | The newest N frames of the trace ring, oldest first (`?n=`, default 32) |
| `/api/can/trace/on` \| `/off` | POST | Arm / disarm the ring. Off by default — it costs an ISR-context copy per frame |
| `/api/can/mode` | POST | `?mode=off\|monitor\|bridge\|bms[&bitrate=B]`. One route for start, stop and the **live break**: `off` stops the cells, anything else starts them if down and changes policy in place if up, so becoming the inverter's BMS never drops its link. Replies with the full status |
| `/api/can/send` | POST | `?bus=N&id=HEX&data=HEX` — put one frame on a bus. How an inverter's reaction to a single frame is tried from a laptop over the tunnel, without the board pretending to be a battery first |
| `/api/can/reset` | POST | Zero every CAN counter; the wire stays up |
| `/api/wg/status` | GET | JSON: running/session_up/provisioned, config_source+version, **public_key** (never the private one), peer_public_key, tunnel addr/mask, allowed_ips, endpoint, keepalive, RNG health, time base. **`session_up` is a threshold on `alive_age_ms`, not the port's peer-is-up flag** — `keypair_valid`/`prev_keypair_valid`/`keypair_age_ms` are the raw state it refuses to trust, and `recoveries` counts the self-rebuilds |
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
transfers. Golden promotion runs in defaultTask
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

- **The hub endpoint is a LITERAL IPv4 and cannot be a name** — `wg_conf.c`
  refuses hostnames and `wg_cfg` persists `endpointIp[4]`, so a board on a
  dynamic-IP site strands itself the first time the address rotates and cannot
  be told otherwise remotely (that is exactly what happened on 2026-09-04, and
  `POST /api/wg/config` sets only addresses, never the peer key). The fix, with
  its threat analysis and record migration:
  [docs/task_wg_endpoint_by_name.md](docs/task_wg_endpoint_by_name.md).

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

- **"The tunnel is up" cannot be asked of the port, and getting that wrong
  cost a board 13 hours of silence** (2026-09-04). `wireguardif_peer_is_up()`
  is `curr_keypair.valid || prev_keypair.valid`, and **nothing ever
  invalidates `prev_keypair`**: `should_reset_peer()`, the only thing that
  would, is itself gated on `curr_keypair.valid`, so once the current keypair
  expires the reset can never fire. The predicate is structurally incapable of
  going false after the first successful handshake. The same expression drives
  the port's `netif_set_link_down()`, so the netif link flag lies for the same
  reason. `WgLink_IsUp()` therefore judges by **evidence the hub answered**:
  a *current* keypair (only valid once the handshake response authenticated —
  and a heartbeat in its own right, since the port destroys and re-handshakes
  every `REJECT_AFTER_TIME`, ~180 s, idle or not) or a data packet in.
  `last_tx` is not evidence — keepalives keep advancing it while
  `prev_keypair` is valid, i.e. exactly during the failure. Silent for
  `WG_LINK_STALE_MS` (6 min) = down; down for `WG_LINK_RECOVER_MS` (15 min) =
  `WgLink_Housekeep()` rebuilds the peer, which is the one action that clears
  the stuck state.

- **THE WIREGUARD SOURCE PORT IS FIXED FOR A BOOT AND THE SAME ON EVERY BOOT,
  and that stranded a board for three days.** It is chosen once by
  `udp_bind(..., listen_port = 0)` in `wireguardif_init()`; `WgLink_Stop()`
  keeps the netif (*"the port's device, UDP PCB and periodic timer outlive
  this"*), so a peer rebuild reuses it, and lwIP's ephemeral counter restarts
  at `UDP_LOCAL_PORT_RANGE_START` on reset, so a reboot reuses it too — both
  boards independently sit on `:62510`. When a NAT blackholes that one mapping
  the board cannot escape, and its own ~5 s retries keep the dead entry
  refreshed so it never ages out. `WgLink_Housekeep()` therefore **rotates the
  source port on every recovery** (`rotate_source_port()`, an
  `udp_bind(pcb, IP_ADDR_ANY, 0)` rebind that keeps the `udp_recv()`
  registration), and `local_port` / `port_rotations` are reported by
  `/api/wg/status` and `wg status`. A "just reboot it" watchdog is **not** a
  substitute — it retries the same dead tuple.

- **THE RECOVERY LADDER, and its one rule: evidence resets the clock, actions
  never do.** `App/Net/wg_ladder.c` is the pure decision core
  ([docs/design_tunnel_watchdog.md](docs/design_tunnel_watchdog.md),
  host-tested); `WgLink_Housekeep()` executes rungs 1–2 and defaultTask
  executes rung 3. Rung 1 at 15 min: rebuild + rotate the source port,
  repeating. Rung 2 at 60 min: re-read the stored config (refused when the
  config is not flash-backed — a `genkey` identity lives only in RAM). Rung 3
  at 6 h: a **bounded** (3 per power-on) terminal reboot, **gated** on the
  board's own link and default route being up, and it stamps
  `/api/system/last-restart` before it fires. A corrective action must NEVER
  reset the escalation clock — rung 1 firing every 15 min would otherwise hold
  rung 3 permanently 15 minutes away, which is how 325 rebuilds achieved
  nothing over three days.

- **Intent is not state.** `s_wantRunning` (asked for) is separate from
  `s_running` (actually up), because they differ exactly when
  `WgLink_Start()` failed — and both gates that would have retried it used to
  key on `s_running`, the flag the failure clears, so one transient failure
  stranded a board until a power cycle. `WgLink_Stop()` deliberately does not
  clear intent (`Restart()` and `cfg_reapply()` go through it);
  **`WgLink_StopRequested()` is the only thing that does**, and it is what the
  CLI `wg stop` and `WgLink_ResetCfg()` call. Getting this backwards
  resurrects a tunnel somebody deliberately stopped, which the host tests
  assert against.

- **Soft dependency, always.** A dead hub costs one handshake packet every
  5 s (`REKEY_TIMEOUT`) and nothing else; `peer->active` stays set so the port
  retries forever without app intervention. The 15-minute self-rebuild above
  keeps that property — it allocates nothing (the netif is reused; removing it
  would leak a UDP PCB and leave a timer on freed memory) and nothing waits on
  it. No WG path blocks a task, the RS485 bus, or the IWDG kick.
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
- ~~**Control stays outbound-only.**~~ **No longer true, and deliberately so.**
  This said metrics and actuation both ride MQTT over a board-initiated socket,
  with inbound reserved for debug. MQTT is gone and the chosen Solis path is
  **inbound**: `solis_modbus` polls a Modbus TCP server on the board
  ([docs/design_solis_modbus_link.md](docs/design_solis_modbus_link.md) §6.4).
  That is affordable only because the tunnel already exists per board — bind
  the listener to the tunnel address, never `IP_ADDR_ANY`.

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

**An enum's NAME belongs to the module that owns the enum, never to an
adapter.** `http_server.c` and `cmd_parser.c` had each grown their own
spelling of `ePackCondition`, and the HTTP side emitted `ePackAbsentReason`
as a bare integer while the CLI rendered it as a sentence — so the most
diagnostic field the pack module has was reachable only from a console
needing physical access, which on a tunnel-only board is backwards. The rule
now has fourteen instances: `Pack_CondName` / `Pack_WhyName` /
`Pack_SwitchName` (→ `pack_cfg.c`, which is what `tests/test_pack_cfg` links),
`Crash_TypeName`, `Modbus_PortName`, `SysMon_StateName`, `CanBridge_ModeName`,
`CanLog_ModeName`, `CanLog_StateName`, `NvDbCfg_ModeName`, `NvDb_UserName`,
`Pack_TypeName`, `PackCfg_ChemName`, `PackCfg_CmdName`. Adding one is a day's
work and it is where the fix goes.

**Write those accessors as a `switch` whose fallback sits AFTER the switch,
never in a `default:`** — a `default:` satisfies `-Wswitch` and defeats the
whole mechanism. `-Werror=switch` is then **scoped per-source** in
`CMakeLists.txt` (`ENUM_NAME_SOURCES`: `pack_cfg.c`, `crash.c`, `modbus.c`;
`tests/CMakeLists.txt` scopes it onto its copy of `pack_cfg.c` too), so
**adding an enumerator without a name fails the build** instead of shipping a
`"?"` for someone to find in the field — which is exactly how the old private
`crash_type_names[]` came to report a StackOverflow as `Unknown`. **Never
global**: lwIP, FreeRTOS, the HAL, trice and wireguard-lwip share
`CMAKE_C_FLAGS`. This is a deliberate departure from `C coding standard.md`'s
"every switch has a default": the standard's intent is that an unhandled case
must not pass silently, and a compile-time failure serves that intent better
than a runtime placeholder. Range-check and return `"?"`, never `NULL` — the
older `PackCfg_TypeName` NULL convention is not to be copied, since both
adapters pass its result straight into `%s`. Names that reach JSON must
contain no `"` or `\`, or be escaped where they are emitted; the host test
asserts it. [docs/design_http_server_adapter.md](docs/design_http_server_adapter.md) §1

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

**TRICE CANNOT BE USED IN lwIP CALLBACKS** (tcpip_thread context — raw-API callbacks of any kind). Use only in FreeRTOS tasks (the netconn-based HTTP task is fine), ISRs, and fault handlers.

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
- **JSON goes through `Shared/Json` and nowhere else.** `Json_Cat` saturates
  at the cap and `pos == cap` is the "did not fit" signal, which is what lets
  a loop emit whole objects or none — the roll-back idiom in `http_server.c`,
  which replaced hand-maintained byte reserves that were smaller than the
  object they reserved for (`/api/pack/status`: 512 reserved, 773 needed).
  **Never reintroduce `n += snprintf(...)`**; a truncated field turns the next
  append into an overflowing write. Operator-supplied strings — pack and point
  names, topic prefixes, an error `field` echoed out of an uploaded document —
  are escaped with `Json_Escape` on the way out and decoded on the way in, so
  a name containing `"` or `\` round-trips. `sanitize_filename()` in
  `http_server.c` is a deliberate exception and stays as it is. Like
  `Shared/Modbus` and `Shared/NvDb` this is application-only Shared code
  (`SHARED_JSON_SOURCES`), and CMake fails the build if a bootloader source
  includes a `json*` header. Details, measurements and the escape-vs-reject
  decision are recorded in `Shared/Json/json.h`.
- **`Shared/Modbus/` is application-only Shared code** — host-testable like the rest of Shared/, but kept out of `${SHARED_SOURCES}` (own `SHARED_MODBUS_SOURCES` list) so it never bloats the 32KB bootloader
- **`Shared/NvDb/` is application-only Shared code too** (`SHARED_NVDB_SOURCES`), and CMake **fails the build if any bootloader source includes an `nvdb*` header**. The BL has no knowledge of `nvDb` by design — it learns where the firmware blobs are from the FWU module, which is what leaves the directory format free to evolve without a bootloader in lockstep
- **`nvDb` is the only authority over the medium, and CMake enforces that too** — no `W25Q128_*` call and no `EXT_FLASH_*_ADDR` anywhere in `App/` or `Shared/` outside the driver, `Shared/NvDb/`, and the three exempt files listed in the External Flash section
- **A write that only clears bits is programmed in place, never erased.** `boot_status` and the Modbus selector both keep their flags outside their CRCs precisely so a flag update is atomic; routing them through a store that only knew "erased or not" would have turned every one into an erase-and-write-back
- **Absolute addresses leave `nvDb` only through `nvdb_exceptions.h`** — the crash handler (fault context) and the FWU module. Including `nvdb.h` cannot reach it; that is the enforcement
- **CCM (64KB at 0x10000000) is CPU-only memory and is ~89% full** — never put DMA or peripheral-accessed buffers there. **This now includes any bulk buffer handed to the flash driver**: `w25q128.c` checks the address and silently drops to polling for anything outside main SRAM / internal flash, and FreeRTOS task stacks are `pvPortMalloc`'d from `.ccmheap`, so a **stack local is CCM too**. A new bulk flash buffer must be static/`.bss` or it quietly loses DMA (`docs/task_flash_wait_and_ota_cost.md`). It holds **two** NOLOAD sections with different lifecycles:
  - `.ccmram` (~11KB) — Modbus engine scratch (one sequence's spans and derived blocks), compiler/plan-rewrite state, plus HTTP server and image-store upload buffers. Zeroed by `System_Init()`.
  - `.ccmheap` (48KB) — the FreeRTOS heap (`configAPPLICATION_ALLOCATED_HEAP=1`, `ucHeap[]` in `App/system.c`). **It is a separate section precisely so `System_Init()` does not zero it**: tasks are already allocated from the heap by the time that memset runs. Any new CCM section must stay out of the `_sccmram.._eccmram` range for the same reason.
- **One file owns the bxCAN cells, and CMake enforces it** — no `HAL_CAN_*` name and no CAN handle anywhere in `App/` except `App/Can/can_bus.c`. The HAL offers ONE weak RX-FIFO-pending callback for both cells, so whoever defines it takes a link-level monopoly: that is how `bms_reader.c` came to block `pack_pylontech` outright. Consumers subscribe by `(bus, id, mask)`, and **every subscriber callback runs in the RX ISR** — copy, count, enqueue, return
- **The CAN bridge is store-and-forward, not a wire** — each side is its own collision domain and the board ACKs on both, arbitration is per side (the TX FIFO is chronological so a burst is not re-sorted by identifier), error frames do not cross, one frame time of latency is added, and **both sides must run the same bitrate**; a rate that does not divide PCLK1/14 exactly is refused rather than rounded. Free for the 1 Hz one-way Pylontech dialect, and stated in [docs/design_can_bridge.md](docs/design_can_bridge.md) §3 because none of it is academic for a different protocol
- **There is no persisted CAN configuration** — mode, bitrate and bus roles are compile-time defaults, so a change made over the tunnel lasts until the next reset. **This is now a known defect, not just a simplification** — zaliakalnis is wired with the battery on CAN1, which the image asserts is the inverter side, and `bms` mode would take over the wrong cell silently ([docs/issue_can_bus_roles_not_configurable.md](docs/issue_can_bus_roles_not_configurable.md)). Adding one means a new nvDb user, which means moving the layout that is still pinned to the bootloader
- **`:502` IS AN UNGATED WRITE PATH, AND THE TUNNEL IS ITS ONLY AUTHORIZATION.** The Modbus TCP gateway is transparent both ways by decision, not by omission ([docs/design_solis_modbus_link.md](docs/design_solis_modbus_link.md) §7.2): a raw transfer consults no point, no access bit and no `writeMin`/`writeMax`, so anything reaching that port can write any holding register on a live inverter with a live battery behind it. Three things follow and none may be relaxed casually — the listener binds the **tunnel address and never `IP_ADDR_ANY`**; a board with no tunnel configured **serves nothing** rather than falling back to the site LAN; and §4.6's point-model protection is untouched but applies **only** to `Modbus_Request` / `POST /api/modbus/write`, never to `Modbus_RawTransfer`
- **A gateway request is a queued client of the engine, never a second bus master** — it rides the same request FIFO as `Modbus_Request`, so it takes its turn behind scheduled sequences and cannot displace the 1 Hz Pylontech CAN obligation. That is what keeps autonomy a scheduling property rather than an access rule, and it is why the `mbtcp` task sits *below* the engine's priority
- **FC16 is framed verbatim on the gateway path and never decomposed** — `count` registers from `addr` go out as one write-multiple frame, and the **client's** function code is passed through rather than the capability's `writeFc`. Solis's Remote Dispatch block (44100-44112) is silently dropped by the inverter if it arrives as scattered single-register writes, so this is correctness, not style. The integration suite asserts it on the **frame length**, because an outcome assertion would pass against two FC06 writes
- **A Modbus subscription's plan mask decides WHAT IS POLLED, not what is delivered** — every `dispatch()` passes `MB_PLAN_ALL`, so a subscriber gets every event whatever its mask says, while `ModbusSub_PlanUnion()` is what the scheduler loads. `MB_PLAN_ALL` therefore means *"put every plan on the wire"*, not *"tell me everything"*. Passing it because you filter in your own callback asks the board to poll devices you are about to discard: that is what `pack_jkbms` did, and it cost sodas 46 ‰ of a shared RS485 pair polling a Solis nobody read (docs/modbus.md §4.2, §11a.14). Use `Modbus_PlanList()`'s `devices` bitmask to ask for the plans that read your devices
- **Retuning a subscription is a SCHEDULER operation, not bookkeeping** — it destroys and rebuilds every plan's timers, so it must happen once per settled state and never once per step of reaching one. Reconciling inside `Bind()`/`Unbind()` turned one config apply into four unsubscribe/resubscribe cycles and left the engine with `arm_failures: 16`, two of four clocks live and both packs reading 0 mV. The type's 250 ms `tick` is where it belongs, so a rebind burst collapses into one call
- **Nothing takes `LOCK_TCPIP_CORE` on the Modbus sequence path** — the rule that kept it off there was written for the MQTT bridge (its callback copied and posted to `mqttTask` rather than publishing inline, docs/modbus.md §4.10). MQTT is gone, but the constraint outlives it and binds the Modbus TCP gateway next: a subscriber callback runs on the engine's own task, so it must never take the core lock or call a raw lwIP API
