/**
 * @file    modbus_engine.h
 * @brief   The engine: timers, sequences, dispatch.
 *
 * MODULE-INTERNAL (docs/modbus.md §2.2).  A consumer includes modbus.h; this
 * is here so modbus.c can start the task and poke it.
 *
 * THERE IS NO POLLING LOOP (§5.2).  Scheduling is an independent, event-driven
 * instance that emits events at the periods the config asks for, and the
 * engine services them; nothing walks the config looking for work.
 *
 * A SEQUENCE is named {device, plan, time table} and is what one run of the
 * wire covers; a CLOCK is named by period and is shared by every sequence at
 * that period.  Sequences come and go with subscriptions, but the clocks only
 * change when the set of distinct periods does — so the common case, a
 * subscriber arriving or a config being applied, issues no timer command at
 * all.
 *
 * There is no Start/Stop pair either: Modbus_Init is the entire lifecycle.
 */
#ifndef MODBUS_ENGINE_H_
#define MODBUS_ENGINE_H_

#include "App/Modbus/modbus.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief  Create the queue and the modbus task.  Idempotent. */
int ModbusEngine_Start(void);

int ModbusEngine_IsRunning(void);

/**
 * @brief  Wake the task: there is pending work on one of the three sources
 *         (timers, port completions, mutating API calls).
 *
 *         AN API POST MUST NEVER BLOCK (§4.8): a self-post from the very task
 *         draining the queue is legal, so a blocking post on a full queue
 *         would deadlock.  A full queue is counted, not waited on.
 */
void ModbusEngine_Poke(void);

/**
 * @brief  The set of live plans changed (a subscription came or went, or a
 *         config went live), so the timers must be rebuilt.
 *
 *         Handled as a DIFF: a sequence whose {device, plan, time table} and
 *         period are unchanged keeps its slot, its clock, its pending `due`
 *         and its `missed` count, and costs nothing.
 */
void ModbusEngine_Resched(void);

/** @brief  Response timeout handed down with each frame. */
uint32_t ModbusEngine_ResponseTimeoutMs(void);

void ModbusEngine_Counters(uint32_t *polls, uint32_t *errors,
                           uint32_t *missed);

/**
 * @brief  The scheduler's own state: how many sequences are live, how many
 *         clocks carry them, and how many of those clocks are ACTUALLY
 *         RUNNING.
 *
 *         `ticksArmed != ticksLive`, or a non-zero `armFailures`, means
 *         sequences are riding a clock that is not ticking — the engine would
 *         then sit silent with every other status field reading healthy, which
 *         is the failure this reporting exists to make visible
 *         (docs/issue_modbus_engine_stall.md).
 */
void ModbusEngine_ScheduleStats(sModbusScheduleStats *out);

void ModbusEngine_LogStatus(void);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_ENGINE_H_ */
