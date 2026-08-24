# Peripheral I/O audit — polling, interrupts and DMA

**Date:** 2026-08-24 · branch `develop` · target STM32F407VET6

> **⚠️ PARTLY SUPERSEDED — read this first.**
> §2's central claim ("SPI2 DMA and Trice UART DMA cannot coexist") is still
> true, but the trade has since been made: **the flash won.** Trice UART output
> is now opt-in behind `TRICE_UART_OUTPUT` (default 0, tracing is UDP + USB
> CDC), DMA1 streams 3 and 4 carry SPI2_RX/SPI2_TX, and the W25Q driver has a
> DMA transport. Item 7 of §07 is done; item 1 is deliberately **not** and now
> lives in [task_flash_wait_and_ota_cost.md](task_flash_wait_and_ota_cost.md).
> The `.ioc` is now the source of truth for both the DMA map and the lwIP
> checksum settings in §08. **§8 records the hardware verification** — the
> whole thing has since run on board #1 as `Pd1.1.23`, installed over the
> tunnel.

Goal set for this pass: *switch everything to DMA unless there is a clear
reason against.* This document is the survey, the per-peripheral verdict, and
the reasons against where they exist. Two of them are hardware facts that no
amount of driver work removes, and one of them kills the single biggest
candidate.

---

## 1. What is on the chip right now

### DMA controller occupancy

| Controller | Stream | Channel | Owner | Notes |
|---|---|---|---|---|
| DMA1 | 0 | — | *free* | |
| DMA1 | 1 | — | *free* | |
| DMA1 | 2 | — | *free* | |
| DMA1 | 3 | 4 | **USART3_TX** (Trice) | `Core/Src/usart.c` |
| DMA1 | 4 | — | *free* | |
| DMA1 | 5 | 4 | **USART2_RX** (RS485) | `App/Rs485/rs485_port.c` |
| DMA1 | 6 | 4 | **USART2_TX** (RS485) | `App/Rs485/rs485_port.c` |
| DMA1 | 7 | — | *free* | |
| DMA2 | 0–7 | — | ***entirely unused*** | |

Three of sixteen streams are in use. DMA2 has never been clocked.

### Interrupt inventory

| Source | Priority | Nature | Current handling |
|---|---|---|---|
| `DMA1_Stream3` | 5 | Trice TX complete | DMA ✅ |
| `DMA1_Stream5/6` | 5 | RS485 RX/TX | DMA ✅ |
| `USART3` | 5 | TC → `TriceConsumer_Done` | frame-level ✅ |
| `USART2` | 5 | IDLE + errors | frame-level ✅ |
| `USART1` | 5 | **RX, one byte per interrupt** | ⚠️ see §3.1 |
| `ETH` | 5 | RX/TX descriptor complete | MAC-internal DMA ✅ |
| `OTG_FS` | 5 | USB endpoint | packet-level ✅ |
| `CAN1/2 ×4 each` | 5 | RX FIFO0 pending (CAN2 only) | IRQ ✅ |
| `TIM6_DAC` | 15 | 1 kHz HAL time base | required |
| `TIM8_TRG_COM_TIM14` | 15 | 12 s software watchdog | required |
| `HASH_RNG` | 5 | — | ⚠️ enabled, does nothing (§3.5) |
| **SPI2** | — | **no interrupt at all** | ⚠️ fully polled (§2) |

---

## 2. The headline: SPI2 / W25Q64 external flash

`Shared/Drivers/w25q128.c` is the only bulk data path on the board with **no
interrupt and no DMA**. Every byte moves through blocking `HAL_SPI_Transmit` /
`HAL_SPI_Receive` at 21 MHz (APB1 42 MHz ÷ 2), and every wait for the chip is a
tight spin in `W25Q128_WaitReady`.

It is also the single largest consumer of CPU time on the board. And it is the
one candidate that **cannot** simply be moved to DMA. Three independent reasons:

### 2.1 A hard DMA request-mapping conflict with Trice

Verified against the CubeMX device database
(`com.st.stm32cube.common.mx_6.13.0/db/mcu/IP/DMA-STM32F417_dma_v2_0_Modes.xml`):

```
DMA1_Stream3  = XOR( SPI2_RX[ch0], … , USART3_TX[ch4], … )
DMA1_Stream4  = XOR( SPI2_TX[ch0], … , USART3_TX[ch7] )
```

- `SPI2_RX` exists on **DMA1 Stream 3 only**.
- `SPI2_TX` exists on **DMA1 Stream 4 only**.
- `USART3_TX` exists on **Stream 3 (ch4) or Stream 4 (ch7)** — and nowhere else.

SPI2 is an APB1 peripheral, so DMA2 is not reachable for it at all. The three
requests collide pairwise across exactly the two streams they can use.
Therefore:

> **SPI2 full-duplex DMA and Trice's USART3 TX DMA cannot coexist.** One of
> them has to give up its stream.

The HAL makes this worse: `HAL_SPI_Receive_DMA` with `SPI_DIRECTION_2LINES`
internally redirects to `HAL_SPI_TransmitReceive_DMA`, so even a *read* needs
both streams. The only escape is reconfiguring SPI2 into
`SPI_DIRECTION_2LINES_RXONLY` per transfer, which needs one stream (S3) — but
that is precisely the stream Trice holds, so Trice would have to move to
Stream 4 / channel 7 and SPI2 writes would stay polled.

### 2.2 The upload buffer lives in CCM, which DMA cannot address

`App/Img/image_store.c:187`:

```c
static sUploadSession upload __attribute__((section(".ccmram")));
```

CCM RAM at `0x10000000` is core-only on the F407 — it is not on the AHB bus
matrix that DMA1/DMA2 masters. Any DMA'd flash write from that buffer would
need a bounce buffer in main SRAM, which reintroduces the copy that DMA was
supposed to remove. The same applies to anything `pvPortMalloc`'d, since the
FreeRTOS heap is `.ccmheap`.

(The Modbus port buffers are in plain `.bss`, which is why RS485 DMA works.
That distinction is load-bearing and worth keeping in mind for any new DMA
buffer.)

### 2.3 The bootloader shares this driver

`w25q128.c` compiles into both targets. The bootloader has no RTOS, no DMA
init, and a 32 KB budget. Any DMA path has to be conditional on
`APPLICATION_BUILD` — doable, but it doubles the code under test on the one
driver whose failure bricks the board.

### 2.4 What actually costs the time — and it isn't the transfers

At 21 MHz a byte takes 381 ns. Against the current 394 520-byte `.pnfw` blob
(the areas hold up to 488 KB), using W25Q64FV datasheet timings:

| Operation | Bus time (DMA could reclaim) | Spin time (DMA cannot) |
|---|---|---|
| 256 B page program | 99 µs | **0.7 ms typ / 3 ms max** `tPP` in `WaitReady` |
| 4 KB sector erase | ~2 µs (command only) | **45 ms typ / 400 ms max** |
| Blob CRC scan at boot (×2 areas) | 150 ms each | ~0 |
| Full OTA upload | ~150 ms | **~5.5 s** (1541 programs + 97 erases) |

So an OTA upload burns roughly **five and a half seconds of 100 %-busy CPU at
`osPriorityNormal`** inside `W25Q128_WaitReady`, against ~150 ms of actual bus
time. DMA would not reclaim a microsecond of the former. `WaitReady` spins `HAL_GetTick()` and re-reads the status
register over SPI as fast as it can:

```c
while (W25Q128_IsBusy()) {
    if ((HAL_GetTick() - start) > timeout_ms) { return w25q_timeout; }
}
```

**Verdict: do not chase DMA on SPI2 first.** The two changes that pay far more,
for far less risk:

1. **Yield inside `WaitReady`** when the scheduler is running — `osDelay(1)`
   between status polls once the fast path (a few hundred µs) has elapsed.
   Guarded by `xTaskGetSchedulerState()`, so the bootloader and the pre-scheduler
   init path keep the current spin. Reclaims essentially all 6.3 s.
2. **Raise `SCAN_CHUNK_SIZE` from 256 B to 4 KB** (`App/Img/image_store.c:28`).
   The boot scan currently issues **~1540 separate `W25Q128_Read` calls** per
   blob — each with a mutex take/give, a CS toggle and a 4-byte command — where
   ~96 would do. Two blobs are scanned at boot. Note the 4 KB buffer is a stack
   local in `scan_area()`, so it has to move to static/`.bss` rather than grow
   the caller's stack.

Only after those, and only if profiling still shows SPI bus time mattering,
is it worth spending Trice's DMA stream on `SPI_DIRECTION_2LINES_RXONLY`.

⚠️ `Shared/Drivers/w25q_fault.c` is **out of scope entirely**. Its polling is
deliberate and CMake enforces it: no RTOS call, no tick, no HAL SPI. Do not
touch it.

---

## 3. Everything else

### 3.1 USART1 — the CLI. **→ Move to DMA. This is the clear win.**

`App/Cmd/cmd_parser.c:947`:

```c
Cmd_Feed(cmdSrc_uart, &s_uart1_rx_byte, 1);
HAL_UART_Receive_IT(&huart1, &s_uart1_rx_byte, 1);
```

One interrupt per byte, and the re-arm happens *after* the callback body. At
115200 baud that is a ~87 µs budget to get back into `Receive_IT`; anything
that delays the ISR drops a byte silently. Human typing never hits it, but the
integration harness (`tests/integration/`) pasting command lines does — this is
the same class of bug the RS485 driver's header describes at length for its old
polled receive.

**No contention whatsoever:** `USART1_RX` maps to DMA2 Stream 2 ch4 (or Stream 5
ch4), `USART1_TX` to DMA2 Stream 7 ch4. DMA2 is completely free.

Recommended shape — copy `rs485_port.c`, which already proves the pattern on
this board:

- `HAL_UARTEx_ReceiveToIdle_DMA` in **circular** mode over a main-SRAM ring
  (must **not** be `.ccmram`), so there is no re-arm window at all.
- `HAL_UARTEx_RxEventCallback` feeds `Cmd_Feed` with whatever arrived —
  IDLE-terminated, so a pasted line lands as one event instead of N.
- `HAL_UART_ErrorCallback` already exists globally; add a USART1 arm that
  clears ORE and restarts, rather than dying quietly.

Note the existing dispatch pattern: `HAL_UART_TxCpltCallback` and
`HAL_UART_RxCpltCallback` are **single global callbacks shared by every UART**,
already dispatched by `->Instance` in `Core/Src/usart.c` and
`Core/Src/stm32f4xx_it.c`. `HAL_UARTEx_RxEventCallback` is currently claimed
outright by `rs485_port.c` — adding USART1 means adding an `Instance` check
there too, exactly as `HAL_UART_TxCpltCallback` does for trice vs. RS485.

### 3.2 USART2 / RS485 — already correct, leave alone ✅

Full DMA both directions, IDLE-line framing for the 3.5-character silence, TC
interrupt for the DE drop. The file header documents why the previous polled
version failed (47 % CRC failures on a JK pack at 115200). This is the
reference implementation for §3.1.

One small note, unrelated to DMA: `submit()` calls `osDelay()` for the
turnaround gap, which is correct, but the response timer is armed *after* the
gap has already been slept — the timeout therefore covers gap + reply. That is
documented as deliberate.

### 3.3 USART3 / Trice — already DMA ✅, but one correctness smell

`Core/Src/usart.c:299`:

```c
void TriceNonBlockingWriteUartA(const void *buf, size_t nByte)
{
    HAL_UART_Abort(&huart3);
    HAL_UART_Transmit_DMA(&huart3, (uint8_t *)buf, nByte);
}
```

The unconditional `HAL_UART_Abort` will truncate an in-flight transfer if this
is ever entered while the DMA is running. `triceTask` guards with
`MX_USART3_Ready()`, so today it is unreachable — but the guard is in the
caller, not the function, and `TriceOutDepthUartA` reports depth from the DMA
counter, so the invariant is implicit. If Trice ever gains a second writer, this
silently eats log output. Worth a comment at minimum.

**If** the SPI2 RXONLY route in §2.1 is ever taken, this is the transfer that
moves to DMA1 Stream 4 / channel 7.

### 3.4 Ethernet — already optimal ✅

The ETH MAC has its own dedicated DMA with descriptor rings; `ETH_IRQHandler` →
`HAL_ETH_RxCpltCallback` → `osSemaphoreRelease(RxPktSemaphore)` → `EthIf` task.
There is nothing a general-purpose DMA stream could add.

`ethernet_link_thread` polls the DP83848 over MDIO every 100 ms
(`LWIP/Target/ethernetif.c:843`). The PHY's interrupt pin is not routed to an
EXTI on this board, so polling is the only option — but **1000 ms would do just
as well** as 100 ms for link up/down and saves ten MDIO transactions a second.

### 3.5 CAN1 / CAN2 — no DMA exists on F4; RX already IRQ-driven ✅

bxCAN on STM32F4 has **no DMA support at all** — three TX mailboxes and two
3-deep RX FIFOs, filled and drained by the core. Nothing to switch.

`bms_reader.c` correctly uses `CAN_IT_RX_FIFO0_MSG_PENDING`. Two observations:

- `bms_sim.c:94` calls `HAL_CAN_AddTxMessage` and returns `-1` if all three
  mailboxes are full, with no retry and no use of the TX-empty interrupt. At
  500 kbps with the Pylontech frame set this is unlikely to bite, but the
  frame is dropped without a counter. Since the CAN path is about to become
  load-bearing (synthetic pack presented to the inverter), a TX queue drained
  from `CAN1_TX_IRQHandler` is worth the small effort.
- All eight CAN interrupts are enabled on both controllers
  (`Core/Src/can.c`), but only CAN2 RX FIFO0 has a callback. The SCE (status
  change / error) interrupt in particular fires on bus-off and does nothing —
  on a bus shared with an inverter, bus-off detection is exactly the signal
  autonomy needs.

### 3.6 USB CDC — no DMA available, already packet-driven ✅

The F407's OTG_FS core runs in slave/FIFO mode — `usbd_conf.c:338` sets
`Init.dma_enable = DISABLE`, and CubeMX offers no DMA alternative for OTG_FS on
this part (only OTG_HS has an internal DMA). `CDC_Receive_FS`
already receives whole packets and calls `Cmd_Feed` once per packet. Optimal as
is.

### 3.7 RNG — interrupt enabled, does nothing

`Core/Src/rng.c:63` enables `HASH_RNG_IRQn` at priority 5, and
`HASH_RNG_IRQHandler` calls `HAL_RNG_IRQHandler`. But `App/Net/wg_platform.c`
uses the **blocking** `HAL_RNG_GenerateRandomNumber`, so no HAL RNG callback is
ever installed and the interrupt has no effect. RNG has no DMA request and
produces a word in ~40 RNG-clock cycles, so neither IRQ nor DMA is warranted —
just disable the IRQ, or leave it and document that it is inert.

### 3.8 I2C1, SDIO, RTC — configured, zero users

```
$ grep -rn "hi2c1\|HAL_I2C_" App/ Shared/ bootloader/   → nothing
$ grep -rn "hsd\b\|HAL_SD_"   App/ Shared/               → nothing
$ grep -rn "hrtc\|HAL_RTC_"   App/ Shared/               → nothing
```

`MX_I2C1_Init` and `MX_RTC_Init` run from `main()` and clock peripherals and
GPIO for hardware nothing talks to (the AT24C02 EEPROM is on I2C1). SDIO is not
even initialised despite being in the `.ioc`. No DMA question here — the
question is whether they should be in the build at all.

### 3.9 Buttons — polled on purpose, correctly

`gpio.c` configures the three buttons as `GPIO_MODE_IT_RISING`, but no EXTI line
is enabled and no EXTI handler exists — `App_DefaultTaskEntry` reconfigures them
as plain inputs (`app_freertos.c:95`) and polls at 100 ms. That is the right
call for debug fault triggers; an ISR would need debouncing to do a worse job.
The dead EXTI mode in the CubeMX output is harmless but confusing.

---

## 4. Recommended order of work

| # | Change | Effort | Payoff | Risk |
|---|---|---|---|---|
| 1 | `W25Q128_WaitReady` yields to the scheduler | S | ~5.5 s of CPU back per OTA; erases stop blocking Normal-priority tasks | low — guard on scheduler state, BL path unchanged |
| 2 | `SCAN_CHUNK_SIZE` 256 B → 4 KB | XS | ~1540 flash transactions → ~96, twice per boot | very low |
| 3 | USART1 CLI → circular RX DMA + IDLE | M | removes per-byte IRQ and a real dropped-byte window; DMA2 is free | low — `rs485_port.c` is the template |
| 4 | CAN TX queue + SCE/bus-off handling | M | no silent frame drops; bus-off is visible | low |
| 5 | PHY link poll 100 ms → 1000 ms | XS | 10× fewer MDIO transactions | very low |
| 6 | Drop or document I2C1 / SDIO / RTC / RNG IRQ | S | less dead configuration | very low |
| 7 | SPI2 → `2LINES_RXONLY` DMA, Trice → S4/ch7 | L | reclaims ~190 ms of bus time per 488 KB read | **medium-high** — touches the driver that bricks the board, and the BL shares it |

Items 1–2 deliver most of what "switch to DMA" was reaching for on the flash
path, without any of its hazards. Item 3 is the one place where the original
instruction applies cleanly and should just be done. Item 7 is the only genuine
"DMA everywhere" move left, and it is the one with a real reason against.

---

## 5. Constraints to carry forward

Three facts that any future DMA work on this board has to respect:

1. **DMA cannot address CCM.** `.ccmram` and `.ccmheap` (the FreeRTOS heap, so
   everything `pvPortMalloc`'d) are core-only. A DMA buffer must be in plain
   `.bss`/`.data`.
2. **SPI2 and USART3_TX contend for DMA1 streams 3 and 4, and have no
   alternatives.** SPI2 cannot reach DMA2 because it is an APB1 peripheral.
3. **`Shared/` code compiles into the bootloader too.** `w25q128.c` and
   `image_mgmt.c` run with no RTOS and no DMA init; anything added there needs
   an `APPLICATION_BUILD` guard.

---

## 6. Appendix — what the CubeMX regeneration changed (and broke)

Reviewed in the same pass, because it landed in the same working tree.

### 6.1 `PeriphNet.ioc` — harmless, actually a fix

One line changed. `ProjectManager.functionlistsort` gained
`16-MX_RNG_Init-RNG-false-HAL-true`.

`MX_RNG_Init()` was already being called from `main.c:120` (added by hand when
the WireGuard DRBG needed the hardware RNG, commit `765083c`), but the `.ioc`
did not know about it — so every regeneration risked dropping the call. This
change closes that drift. `main.c` is untouched by the regeneration, confirming
the call was already where CubeMX now expects it.

**No peripheral configuration changed.** No pin, clock, DMA or NVIC setting
differs.

### 6.2 `LWIP/Target/lwipopts.h` — **a silent WireGuard regression** 🔴

The regeneration reverted the software-checksum fix:

```diff
-#define LWIP_CHECKSUM_CTRL_PER_NETIF 1
-#define CHECKSUM_GEN_IP   1
-#define CHECKSUM_GEN_UDP  1
-#define CHECKSUM_GEN_TCP  1
-#define CHECKSUM_GEN_ICMP 1
+#define CHECKSUM_GEN_IP   0
+#define CHECKSUM_GEN_UDP  0
+#define CHECKSUM_GEN_TCP  0
+#define CHECKSUM_GEN_ICMP 0
```

…along with the 15-line comment explaining why. That block sat in the
CubeMX-**generated** region of the file, not in a `USER CODE` section, so
regeneration overwrote it. The custom settings further down (`TCPIP_THREAD_STACKSIZE`,
`MEM_SIZE`, `LWIP_HOOK_IP4_ROUTE`, `TCP_SND_BUF`) all survived — they live
inside `USER CODE BEGIN 1`.

**Why this is the dangerous kind of regression:** it does not break the build.
With `LWIP_CHECKSUM_CTRL_PER_NETIF` undefined, lwIP's `NETIF_SET_CHECKSUM_CTRL`
degrades to an empty macro, so `App_FreertosInit`'s call still compiles and does
nothing. The only symptom is the one already diagnosed in
`docs/status_wg_provisioning_2026-08-12.md`: the WireGuard handshake succeeds,
the peer looks connected, and every inner packet is dropped by the far side's
`ip_rcv()` for a bad checksum — because the ETH DMA checksums the *outer* UDP
datagram and never sees the encrypted inner one.

**Fixed** by moving the overrides into `USER CODE BEGIN 1` with `#undef` +
`#define`, so regeneration can no longer touch them. Verified by preprocessing
against the real build include path:

```
$ arm-none-eabi-gcc -E -dM … lwip/opt.h
#define LWIP_CHECKSUM_CTRL_PER_NETIF 1
#define CHECKSUM_GEN_IP 1
#define CHECKSUM_GEN_UDP 1
#define CHECKSUM_GEN_TCP 1
#define CHECKSUM_GEN_ICMP 1
```

**Rule this establishes:** anything hand-tuned in a CubeMX-owned file must live
inside a `USER CODE` block, using `#undef`/`#define` to override the generated
value rather than editing it in place. `lwipopts.h`'s own comment now says so.

### 6.3 The `.ioc` fix, and the dependency that bites (2026-08-25)

The checksum settings turned out to be genuine CubeMX LwIP parameters, so they
moved into `PeriphNet.ioc` — but only **two** of them belong there:

```
LWIP.CHECKSUM_BY_HARDWARE=0
LWIP.LWIP_CHECKSUM_CTRL_PER_NETIF=1
```

**Do not add `CHECKSUM_GEN_*` / `CHECKSUM_CHECK_*` keys.** A first attempt did,
and CubeMX complained. Two rules from the generator, both verifiable in the
plugin DB:

1. **The dependency.** `LWIP_CHECKSUM_CTRL_PER_NETIF=1` forces all ten
   `CHECKSUM_*` switches to 1 — the GUI states it outright ("if enabled, the
   CHECKSUM_GEN_* and CHECKSUM_CHECK_* defines must be enabled"), and the
   parameter's own default is
   `=IF(CHECKSUM_BY_HARDWARE,0,IF(LWIP_CHECKSUM_CTRL_PER_NETIF,1,0))`.
   A key holding `0` is therefore a conflict — exactly what the warnings were.
2. **Emission is one-sided.** `db/templates/lwip_conf.ftl` excludes all ten
   from the normal parameter loop and emits a `#define` **only when the value
   is `"0"`**. A key holding `1` writes nothing at all, and the value falls
   through to `opt.h` (which defaults to 1). Setting them to 1 in the `.ioc` is
   inert; the generated block correctly lists none of them.

Net effect after regeneration: `LWIP_CHECKSUM_CTRL_PER_NETIF 1`,
`CHECKSUM_BY_HARDWARE 0`, and the ten switches absent-and-therefore-1.

`CHECKSUM_CHECK_*` are then pinned back to **0** in `USER CODE BEGIN 1`, which
deliberately departs from rule 1. Generation is the half that was broken;
checking was 0 in the last known-good firmware, the ETH netif has its flags
cleared at runtime anyway, and inner tunnel packets arrive already verified by
the peer that encrypted them. Enabling them is reasonable on its own merits —
as its own change, with a board attached. See the comment in `lwipopts.h`.

### 6.4 CRLF: the other way regeneration hides a change

CubeMX writes **CRLF**; the repo has both (27 CubeMX-owned files CRLF, 13 LF —
whichever the last writer used). Every regeneration therefore rewrites the LF
ones end to end, and a one-line semantic change hides inside a whole-file
whitespace diff — precisely how §6.2 slipped through. A `.gitattributes`
(`* text=auto eol=lf`) now makes `git diff` normalise, so only real changes
show. `git status` can still mark a freshly regenerated file modified until Git
refreshes its stat cache; trust `git diff`. A one-time
`git add --renormalize .` clears the mixture for good and deserves its own
commit.

**What a regeneration should now produce: nothing.** The `.ioc` and the
generated files agree — `dma.c`, `spi.c` and `usart.c` came back
byte-identical (modulo line endings) after a full round trip, and the two
things the regeneration legitimately added have been folded in: the
`DMA1_Stream4_IRQHandler` / `SPI2_IRQHandler` prototypes in
`stm32f4xx_it.h`, and `SPI2_IRQHandler` moved into vector-table order in
`stm32f4xx_it.c`.

### 6.5 The third file regeneration touches: `.cproject`

The CubeIDE build failed with `fatal error: nvdb.h: No such file or directory`.
Not caused by the DMA work — `Shared/NvDb` had simply never been added to the
Eclipse include list, and this was the first CubeIDE build since nvDb landed.
`.cproject` is gitignored and machine-local, so nothing kept it in step with
`APP_INCLUDES`.

Fixed by adding `../Shared/NvDb` to **both** configurations (Debug and
Release), verified with a headless build: **0 errors**, signed
`Debug/application.bin` produced. `Shared` is already a whole source path in
`.cproject` with no exclusions, so the sources themselves were always being
compiled — only the header search path was missing.

To stop it recurring, `CMakeLists.txt` now compares `.cproject`'s include list
against `APP_INCLUDES` at configure time and **warns** on any gap. Warning, not
error: a fresh clone has no `.cproject` and CMake must not care. Verified both
ways — removing the entry produces the warning naming `Shared/NvDb`, restoring
it goes silent.

---

## 7. Pre-flash risk assessment (2026-08-25)

State: builds clean under both CMake and CubeIDE, 15/15 host tests pass,
regeneration is a no-op. **Never run on hardware.** What follows is what an
actual flash risks.

### 7.1 Which paths actually take DMA — corrected

`dma_usable()` refuses any buffer outside main SRAM / internal flash, and
**FreeRTOS task stacks are `pvPortMalloc`'d from `.ccmheap`**, so a stack
buffer is a CCM buffer. Checking only the *callers'* buffers gives a
comfortable answer, and it is wrong: **nvDb stages through a global 4 KB
`.bss` buffer of its own** (`nvdbUnitBuff`, `nvdb.c:61`), which is squarely
DMA-eligible.

| Path | Buffer | Transport |
|---|---|---|
| Boot image scan (`scan_area`) | `buf[256]`, stack | polled |
| HTTP image download | `buf[512]`, stack | polled |
| nvDb **read** (`NvDb_Read`) | caller's, usually stack | polled |
| `ProgrammableInPlace` peek | `peek[]`, stack | polled |
| **wg_cfg** save / `genkey` (private key!) | caller's `rec`, stack… | polled |
| **wg_time** slot write (16 B) | stack, under threshold | polled |
| **OTA upload page program** | `upload`, `.bss` | **DMA** |
| **Golden promotion** | `static buf[256]` | **DMA** |
| **nvDb read-modify-write** (any write not a pure bit-clear) | `nvdbUnitBuff`, **`.bss`** | **DMA** |
| **nvDb partial wipe** | `nvdbUnitBuff` | **DMA** |
| **nvDb relayout** (`NvDbInt_CopyRange`) | `nvdbUnitBuff` | **DMA** |

So a `wg_cfg` save reaches DMA **not through its own buffer but through
nvDb's**: rewriting an existing record is not programmable-in-place, so it
takes the read-4 KB / patch / erase / write-back path. Same for the `wg_time`
ring when it wraps (~every 64 h) and for any relayout.

**That means tunnel-critical flash does involve the new code path** — see
`project_board_access_is_tunnel_only` — and the earlier claim that the boot
path is untouched is only half true.

### 7.2 If the DMA path is wrong, how does it fail?

Not by hanging. `dma_wait()` bounds every transfer at 1 s, then calls
`HAL_SPI_Abort()` and returns `w25q_timeout`, which propagates as a normal
error. **And the ordering is safe everywhere it matters.** Every read-modify-write
in nvDb reads first, checks the result, and only then erases —
`NvDbInt_PutBytes`, the partial-wipe path and `NvDbInt_CopyRange` all follow
`res = RawRead(...); if (ok) { ... RawErase(...); }`. A DMA read that never
completes returns `nvdbRes_flash` **before anything is destroyed**, so the
dominant failure mode (a misconfigured stream never raising completion)
cannot cost data. The mode that *would* is "DMA reports success having moved
the wrong bytes", which then survives into the erase and write-back. A wrong
channel yields no completion rather than silent garbage, so this is the
unlikely branch — but it is the one that costs the tunnel, and it is why the
bring-up order below exercises DMA on a path that fails closed first.

The two operator-initiated paths also validate afterwards:

- **Upload** — the blob is checked (manifest + CRC32) when the upload
  completes. Corrupt DMA writes ⇒ upload rejected. The bootloader never sees a
  bad blob, because a blob that fails CRC never becomes the stored image.
- **Golden promotion** — `ImgStore_ScanArea(golden)` re-validates the copy
  immediately after. A bad copy is detected and reported through
  `/api/fwu/status` rather than silently accepted.

The residual case worth knowing: a promotion that fails leaves **golden
invalid**, i.e. the rollback safety net is gone until a good confirm runs.
It is visible (`/api/fwu/status`), not silent.

### 7.3 Ranked risks — for a board reachable ONLY over the tunnel

Access assumption: WireGuard is the normal path; **J-Link means a site visit**
(`project_board_access_is_tunnel_only`). That inverts the usual weighting —
"recoverable with a reflash" is no longer cheap.

| # | Risk | Severity | Notes |
|---|---|---|---|
| 1 | **Losing the tunnel** via corrupted `wg_cfg` (holds the device **private key**) or a `wg_time` counter that goes backwards. Both reach flash through nvDb's `.bss` staging buffer, so both now involve DMA. | **Highest consequence** | Requires the unlikely "DMA succeeds with wrong bytes" mode — a read that merely times out aborts before the erase (§7.2). Mitigation is procedural: see §7.5. |
| 2 | ~~OTA install refused — version identical to the device~~ | **RESOLVED** | Bumped to `Pd1.1.23` and installed successfully — see §8. |
| 3 | **J-Link flashing has no rollback.** It leaves boot status confirmed, so no attempt counting; a boot loop persists until someone reflashes on site. OTA-installed images *are* counted (target `'d'`) and roll back to golden after 3 boots. | **Favours OTA here** | This reverses the usual advice: for a remote board, OTA's rollback is the safety net J-Link lacks. |
| 4 | DMA misbehaves on an OTA upload | Medium | Fails closed — blob CRC32 rejects it, BL never sees a bad blob. |
| 5 | Golden promotion corrupted by DMA | Medium | Detected by the post-copy re-scan, but leaves **golden invalid**, i.e. the rollback net is gone until a good confirm. Visible in `/api/fwu/status`. |
| 6 | Blind if the tunnel is down | **Structural, unchanged** | USB CDC and `TRICE_UART_OUTPUT` both need someone at the board, so neither is a remote diagnostic. Not a regression — but it means a tunnel failure is also a diagnostics failure. |
| 7 | Fault mid-DMA vs. the crash recorder | Low, accepted | `W25qFault_Begin` clears `SPI2->CR2` (killing the DMA enables) but not DMA1 streams 3/4; one stale byte could land in an abandoned buffer on a board that is about to reset. Deliberately not fixed — that file is hardware-verified and CMake-fenced. |
| 8 | RNG / entropy | **No change** | Nothing in this work touches `wg_platform.c` or the DRBG. `MX_RNG_Init()` is still called (`main.c:120`) and is now recorded in the `.ioc`, which *removes* a latent risk: had a regeneration dropped that call, `HAL_RNG_GenerateRandomNumber` would fail and the DRBG would fall back to jitter — weak ephemeral keys, and weak identity keys from `wg genkey`. Watch `hw_seeded` in `wg status`. |
| 9 | lwIP checksums | **No change** | `GEN_*=1`, `CHECK_*=0`, per-netif on — byte-identical to the last known-good config, now carried by both the `.ioc` and the `USER CODE` backstop. This is the setting whose loss produces the "handshake succeeds, tunnel carries nothing" failure, so it is double-guarded on purpose. |

### 7.4 Fixed while assessing

Both transfer helpers passed a `uint32_t` length into HAL entry points that
take `uint16_t`. A transfer of 64 KB or more would have silently become a short
one **and still reported success** — a partial buffer with no error. Nothing in
the tree asks for more than 4 KB, so it was latent rather than live, and it
predates the DMA work (the polled calls had it too). Both helpers now chunk at
32 KB, which covers the polled path as well. CS stays asserted across runs, so
the chip sees one continuous stream.

### 7.5 Bring-up order for a tunnel-only board

The ordering principle: **exercise DMA first on a path that fails closed, and
keep away from the flash that holds the tunnel until it is trusted.**

1. **Prove it on a bench board over J-Link if one exists at all.** A remote
   board should never be the first to run untested firmware.
2. Bump `APP_FW_PATCH` to 23, then deploy by **OTA, not J-Link** — OTA keeps
   the 3-boot rollback to golden; J-Link does not (§7.3 #3). Confirm the
   golden image is valid in `/api/fwu/status` *before* installing, since that
   is the thing that will save you.
3. After it boots: **do not run `wg save`, `POST /api/wg/config`, `wg genkey`,
   or upload a `.conf`.** Those rewrite the record holding the private key
   through nvDb's read-modify-write path — the one place a bad DMA transfer
   could cost remote access outright.
4. Exercise DMA deliberately on the safe path instead: `POST
   /api/image/upload` a `.pnfw` and check `/api/image/info` reports it valid
   with the expected CRC32. That drives the same helper over thousands of
   page programs and **fails closed** if anything is wrong.
5. Only once that is clean, `POST /api/fwu/confirm` (which promotes golden and
   exercises the second DMA path), and treat WireGuard config edits as normal
   again.
6. Leave it running a few days before assuming the unattended paths are fine —
   the `wg_time` ring wraps roughly every 64 h and that is the one
   tunnel-critical DMA write nobody triggers by hand.

---

## 8. Verified on hardware — `Pd1.1.23`, 2026-08-25

Board #1, over the **WireGuard tunnel** (`10.77.0.64`), end to end. No J-Link,
no physical access.

### 8.1 What was run

| Step | Result |
|---|---|
| Baseline on `Pd1.1.22` | cpu 7‰, idle 993‰, IWDG gap max 750 ms, heap free 15552, 0 stale, 0 stack warnings |
| Golden checked **before** installing | `Pd1.1.22`, valid — rollback net confirmed present first |
| Upload #1 (on OLD polled firmware) | 8.65 s, stored CRC32 `0xA3EF4869` = build exactly |
| `POST /api/fwu/install` | back up in **~28 s**, running `Pd1.1.23`, unconfirmed, 2 attempts left, golden still `Pd1.1.22` |
| Crash log after first boot | `{"valid":false}` — clean, no fault |
| `GET /api/fwu/verify` | `{"verified":true}` — BL decrypt + program byte-perfect |
| **Upload #2 (DMA path live)** | 8.45 s, CRC32 `0xA3EF4869` — **1557 page programs through DMA, byte-identical** |
| `POST /api/fwu/confirm` | golden promoted; `golden_version` reads back `Pd1.1.23`, i.e. the post-copy manifest+CRC32 re-scan passed |

**Both DMA paths are exercised and correct**: the upload page programs
(`upload` in `.bss`) and the golden promotion (`static buf` in
`fwu_control.c`), the latter copying ~398 KB.

### 8.2 Tunnel-critical state after the update

Everything the board needs to stay reachable came through untouched:

```
running: true   session_up: true   provisioned: true
rng_hw_seeded: true    rng_failures: 0
time_flash_backed: true   time_now 1787667658 > time_persisted 1787667553
config_source "stored" v2, keys intact
```

`rng_hw_seeded` matters: it proves the DRBG is still drawing from the hardware
RNG and has not fallen back to jitter. The TAI64N counter is advancing **and
persisting**, which is the one unattended tunnel-critical flash write.

nvDb layout unchanged (`periphnet` v1, all 14 users at their expected sizes,
`lastApplyResult: 1`), and Modbus came back provisioned with its RS485 port up.

### 8.3 The measurement that matters most

Peaks were reset immediately before each operation:

| Operation | CPU peak | IWDG gap max |
|---|---|---|
| Upload (~398 KB, 1557 page programs) | **1000‰** — `http` task at 991‰ | 102 ms |
| Golden promotion (~398 KB copy) | **998‰** | 205 ms |

**The board still pins the CPU at 100 % for the whole of an OTA — with DMA
working.** That is the deferred `W25Q128_WaitReady` busy-spin, measured on real
hardware, and it is exactly what §2.4 predicted: DMA reclaimed the ~150 ms of
bus time, and the ~5.5 s of *waiting* is untouched because DMA cannot shorten a
wait. Upload wall time barely moved (8.65 s → 8.45 s), because the path is
network- and flash-program-bound, not SPI-bound.

So the hardware confirms both halves of the analysis: the DMA work is correct
and worth having, **and** it was never the thing standing between this board
and a cheap OTA. That remains
[task_flash_wait_and_ota_cost.md](task_flash_wait_and_ota_cost.md), whose
premise is now measured rather than calculated.

IWDG margin is comfortable throughout (205 ms worst against a 16.4 s timeout),
so the busy-spin is a CPU-availability problem, not a watchdog one.
