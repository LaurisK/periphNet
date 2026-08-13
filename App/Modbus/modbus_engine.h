/**
 * @file    modbus_engine.h
 * @brief   The engine: timers, sequences, dispatch.
 *
 * MODULE-INTERNAL (docs/modbus.md §2.2).  A consumer includes modbus.h; this
 * is here so modbus.c can start the task and poke it.
 *
 * THERE IS NO POLLING LOOP (§5.2).  Scheduling is an independent, event-driven
 * instance that emits events at the periods the config asks for, and the
 * engine services them; nothing walks the config looking for work.  One timer
 * per device per time table of every live plan covering it, and only while
 * something is subscribed — creating and destroying them is what subscribing
 * and unsubscribing does.
 *
 * There is no Start/Stop pair either: Modbus_Init is the entire lifecycle.
 */
#ifndef MODBUS_ENGINE_H_
#define MODBUS_ENGINE_H_

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
 *         A plan going live creates one timer per (device, time table) it
 *         covers; a plan losing its last subscriber destroys them.
 */
void ModbusEngine_Resched(void);

/** @brief  Response timeout handed down with each frame. */
uint32_t ModbusEngine_ResponseTimeoutMs(void);

void ModbusEngine_Counters(uint32_t *polls, uint32_t *errors,
                           uint32_t *missed);

void ModbusEngine_LogStatus(void);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_ENGINE_H_ */
