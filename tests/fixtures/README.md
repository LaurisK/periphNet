# Config fixtures

Uploadable Modbus configs (`docs/modbus.md` §6).  Kept as files rather than as
string constants so the same bytes are used twice: `tests/integration` uploads
one to a live board, and `tests/test_modbus_compiler` compiles it on the host.
A fixture the compiler rejects therefore fails in 40 ms instead of over HTTP.

**They carry no comments, and cannot.** Unknown keys are rejected rather than
silently ignored — that is the useful failure mode (§6), and it is what makes
an old config carrying `publish` fail by name instead of losing a setting.
So the commentary lives here.

## `modbus_solis.json`

What the integration suite provisions. One Solis-shaped capability whose two
blocks cover exactly the address ranges the tests touch — `3009..3010` holding
(the two writable setpoints) and `3132..3151` input — one device, and one plan
polling at **3600 s** on purpose: those tests drive data by injection, and a
fast plan would put timeout traffic and availability transitions in the middle
of every assertion.

Bound to `"port": "test"`, so the fixture answers through the test peripheral
rather than a UART. Whether a board has a test port is a **configuration**
question, not a build one (§5.1).

## `modbus_jk_pb.json`

The JK PB-series BMS as a capability, which is the module's whole claim: **the
next device after JK costs a JSON file, not a `.c` file** (§2.3). Everything
`jk_bms.c` used to do in firmware is data here:

| `jk_bms.c` did | this file says |
|---|---|
| byte-addressed registers | `addrStride: 2` |
| no FC06 handler on the slave | `writeFc: 16` |
| a quantity ceiling of 123 | `maxReadRegs: 123` |
| "quantity + wordOffset < 147" from a block base | `blocks[].regs: 147` |
| ASCII model / hw / sw decode | `decodeType: "ascii"` |
| its own USART2 init at 115200 | the device's `baud` |
| one hardcoded pack | two devices sharing one capability |

Register offsets follow `~/Projects/JK_BMS` `src/core/JkRegisters.h`, which is
read out of the BMS binaries and **overrides the vendor PDF**.

**Unverified against hardware.** It compiles, the dialect is right, and
`test_jk_fixture_derives_blocks` proves the byte-addressed derivation lands on
the registers it should — but nothing here has answered a real BMS yet. Running
a DeviceInfo read against one is still the cheapest next fact to acquire, and
it is now a config upload plus a `modbus get`.
