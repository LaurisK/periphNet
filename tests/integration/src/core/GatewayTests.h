#pragma once

#include <string>

class TestRunner;

/**
 * Modbus TCP gateway tests — the board's `:502`, driven the way
 * `solis_modbus` drives it (docs/design_solis_modbus_link.md §10).
 *
 * THESE NEED A LIVE BOARD ON A REACHABLE ADDRESS, and unlike the rest of the
 * suite they need it over TCP rather than over the Trice/CLI transport: the
 * gateway has no CLI surface by design. They skip cleanly when nothing
 * answers on `:502`, which on an unprovisioned board is the CORRECT state —
 * the gateway binds the tunnel address only and serves nothing without one.
 *
 * WRITES ARE REGISTERED BUT SKIPPED. Everything named `gw_hw_write_*` moves
 * real power on a live inverter with a live battery behind it, and §10 puts
 * them last and after the image is confirmed. Run them deliberately, by name.
 */
void registerGatewayTests(TestRunner& runner, const std::string& deviceIp);
