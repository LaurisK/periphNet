/**
 * @file    modbus_test_port.h
 * @brief   Test peripheral — a Modbus port driver fed by the CLI.
 *
 * THIS IS THE INTEGRATION-TEST MECHANISM, NOT A STOP-GAP (docs/modbus.md
 * §5.1, §9).  Every module-side test hook is gone; what the harness talks to
 * is an ordinary registered driver in port slot 1, which the module cannot
 * distinguish from a UART.
 *
 * It lives OUTSIDE App/Modbus deliberately: it is a peripheral, and the module
 * knows nothing about any of its peripherals.  Its transport is likewise not
 * the module's business — today the `modbus inject` CLI command, HTTP being
 * the obvious next form, and either way an App-layer change with no module,
 * config or contract change.  That is the payoff of the module knowing
 * nothing.
 *
 * Whether a board HAS a test port is a CONFIGURATION question, not a build
 * one: a device names the port it lives on, so one image serves a bench board
 * and a real one.
 *
 * What it cannot do, whatever its transport: RTU line framing — the
 * 3.5-character silence, inter-octet timing and DE turnaround stay
 * hardware-only facts.
 */
#ifndef MODBUS_TEST_PORT_H_
#define MODBUS_TEST_PORT_H_

#include <stdint.h>

/* The longest RTU frame, so a caller need not include the port contract just
 * to size a buffer — the CLI is a consumer, not a driver. */
#define MB_TEST_FRAME_MAX  256u

/** @brief  Register the test driver into port slot 1. */
int ModbusTestPort_Register(void);

/**
 * @brief  Stage the reply the next submitted frame will receive.
 *
 *         A REPLY MAY BE STAGED BEFORE THE REQUEST EXISTS, which is what lets
 *         a harness say "answer the next read like this" and then let a timer
 *         fire.  One slot: staging twice without an intervening request
 *         replaces it.
 *
 * @param  frame  complete reply INCLUDING its CRC — the engine checks it, so
 *                a deliberately corrupt frame is a legitimate test input
 * @return 0, or -1 if the frame is too long
 */
int ModbusTestPort_StageReply(const uint8_t *frame, uint16_t len);

/**
 * @brief  Make the next submitted frame time out instead of answering.
 *
 *         The timeout and malformed-frame paths become reachable without
 *         hardware — the driver simply does not answer (§9).
 */
void ModbusTestPort_StageSilence(void);

/** @brief  How many frames the module has submitted to this port. */
uint32_t ModbusTestPort_RequestCount(void);

/**
 * @brief  The last frame the module SUBMITTED, for a harness that wants to
 *         assert on what the engine formed.
 *
 *         A bonus rather than the point: integration testing is upward-only
 *         (§9), and what the frame going down looks like is checked once
 *         against real hardware.
 *
 * @return length copied, or 0
 */
uint16_t ModbusTestPort_LastRequest(uint8_t *out, uint16_t maxLen);

#endif /* MODBUS_TEST_PORT_H_ */
