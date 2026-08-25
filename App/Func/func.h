/**
 * @file    func.h
 * @brief   The shared functionality task — one task, one static queue, one
 *          packed event-ID space (docs/design_battery_pack.md §7, §13).
 *
 * PACKS CANNOT EACH OWN A TASK.  SYSMON_MAX_TASKS is 16, the board already
 * runs about twelve, and uxTaskGetSystemState() returns ZERO rather than
 * truncating when over-subscribed — blinding all of sysmon at once.  So one
 * task serves N clients, each handed a contiguous range of a packed event-ID
 * space sized by that client's own count.
 *
 * A CLIENT NEVER SEES THE QUEUE OR THE TASK.  It is handed its event-id base
 * and a post function at init and posts through that; func.c is the ONE place
 * that chooses between the task-context and ISR-context post macros, and the
 * one place that knows what a packed id is.  Dispatch is a range-check chain
 * that subtracts the base to recover the client's own enum.
 *
 * WHY IT FITS: a PULL client posts from a callback on someone else's task
 * while a PUSH client posts from its RX ISR, AND THIS TASK CANNOT TELL THEM
 * APART.  That is the seam the pack module needs.
 *
 * TWO DEPARTURES from the ZhagaFW template this follows, both deliberate:
 *   - STATIC QUEUE AND STACK ALLOCATION.  CCM heap is the tight resource here
 *     (91 %), so the queue storage and the 512-word stack live in .bss via
 *     xQueueCreateStatic / xTaskCreateStatic.
 *   - DROPS ARE COUNTED, NOT ONLY LOGGED — and counted PER CLIENT RANGE, since
 *     the post function holds the packed id when the send fails and the id's
 *     range identifies the client.  A dropped client event is a missed state
 *     change and belongs in sysmon, not only in a log.  modbus_engine.c
 *     already proves the pattern with per-timer `missed` alongside
 *     s_droppedPokes.
 *
 * THE TASK MUST NOT BLOCK ON portMAX_DELAY.  This is the single most
 * important departure from the template: staleness is a WALL-CLOCK timeout,
 * SILENCE POSTS NO EVENTS, and an event-only task can never fire the timeout
 * it exists to enforce.  The receive carries FUNC_TICK_MS, exactly as
 * modbus_engine.c:459-465 established.
 *
 * STATUS: SCAFFOLDING.  NO TASK IS CREATED, nothing registers with sysmon,
 * and Func_Init is not called from App_DefaultTaskEntry.  Nothing runs on a
 * board yet.
 */

#ifndef FUNC_H_
#define FUNC_H_

/* Includes -----------------------------------------------------------------*/

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported types -----------------------------------------------------------*/

/** Tick period.  250 ms gives <= 250 ms of latency on a staleness transition
 *  against budgets of 5-60 s, for ~4 wakes/s of a few microseconds each.  It
 *  is also comfortably above the 200 ms floor that makes a sysmon deadline
 *  meaningful. */
#define FUNC_TICK_MS            250u

/** The task's sysmon deadline: the worst legitimate iteration is a tick plus
 *  a bind walking a Modbus point catalogue out of flash. */
#define FUNC_TASK_DEADLINE_MS   2000u

/** Statically allocated stack, in words.  Deliberately .bss and not
 *  .ccmheap, which is where every other task's stack comes from and is the
 *  constrained region. */
#define FUNC_TASK_STACK_WORDS   512u

/** Queue depth.  Worst case is three JK txn boundaries plus a six-frame
 *  Pylontech set plus a tick inside one scheduling gap — under 12. */
#define FUNC_QUEUE_DEPTH        16u

/** Ceiling on the packed event-ID space.  eFuncEvt is private to func.c and
 *  static-asserted against this, so the space cannot silently overflow. */
#define FUNC_EVT_MAX            64u

/** How many clients have their own drop-counter slot.  One today; the shape
 *  admits a second without touching the first, which is the whole point of
 *  the packed range. */
#define FUNC_CLIENT_COUNT       1u

/**
 * @brief  The post function handed to every client.
 *
 * LEGAL FROM ANY CONTEXT — func.c picks the task- or ISR-context send by
 * inspecting IPSR, so a client never learns which it got.  NEVER BLOCKS: a
 * full queue increments the client's drop counter and returns.
 *
 * @param  evtId - a PACKED event id, from the client's own range
 * @param  arg - the client's data word, carried through unchanged
 */
typedef void (*fFuncPost)(uint16_t evtId, void *arg);

/** Per-client and whole-task counters, for sysmon and the CLI. */
typedef struct {
    uint32_t posted;                        /* events accepted onto the queue */
    uint32_t handled;                       /* events dispatched              */
    uint32_t ticks;                         /* tick wakes                     */
    uint32_t dropped[FUNC_CLIENT_COUNT];    /* per CLIENT RANGE, not per task */
    uint32_t droppedUnknown;                /* an id in no client's range     */
} sFuncStats;

/* Exported functions -------------------------------------------------------*/

/**
 * @brief  Build the queue and hand every client its event-id base and the
 *         post function.
 *
 * WHEN IMPLEMENTED it must create the static queue, then call each client's
 * init in turn — today only Pack_Init(func_packEvt, Post) — and report a
 * client's failure without creating the task, so a broken client is a boot
 * diagnostic rather than a task spinning on an empty module.  It does NOT
 * create the task; Func_Start does, so the composition order stays explicit.
 *
 * @retval 0 on success, negative on failure
 * @note   defaultTask, at init, before Func_Start.  May block briefly.
 */
int Func_Init(void);

/**
 * @brief  Create the shared task.
 *
 * WHEN IMPLEMENTED: xTaskCreateStatic at osPriorityNormal-1 (23) — BELOW
 * modbus (24) so pack work never delays the bus, the same slot the MQTT
 * bridge occupies for the same reason — with the .bss stack, and
 * SysMon_TaskRegister(FUNC_TASK_STACK_WORDS, FUNC_TASK_DEADLINE_MS) FROM
 * INSIDE THE TASK BODY plus one check-in per loop.  That is 13 of
 * SYSMON_MAX_TASKS's 16.
 *
 * The loop is xQueueReceive with a FUNC_TICK_MS timeout — never
 * portMAX_DELAY — and does three things per wake: evaluate condition and
 * confidence for every instance, expire command deadlines, and call each
 * type's optional tick().
 *
 * @retval 0 on success, negative on failure
 * @note   defaultTask, at init.  Never blocks.
 */
int Func_Start(void);

/**
 * @brief  Copy the task's counters.
 * @param  out - destination
 * @retval 0 on success, negative on bad argument
 * @note   Any task; not ISR-callable.
 */
int Func_Stats(sFuncStats *out);

#ifdef __cplusplus
}
#endif

#endif /* FUNC_H_ */
