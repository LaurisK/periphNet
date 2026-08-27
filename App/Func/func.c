/*
 * func.c
 *
 * The shared functionality task: one task, one statically allocated queue, a
 * packed event-ID space with one contiguous range per client, per-client drop
 * counters, dispatch and client wiring (docs/design_battery_pack.md §7, §13).
 *
 * A COMPOSITION ROOT, like cmd_parser.c.  It is the only file that knows both
 * that clients exist and that a queue exists; a client sees neither.
 *
 * Imported from ZhagaFW's App/Func/func.c with two deliberate departures,
 * both stated in §13: the queue is STATICALLY allocated (CCM heap is the tight
 * resource on this part), and drops are COUNTED PER CLIENT RANGE rather than
 * only logged — a dropped pack event is a missed state change, and it belongs
 * where sysmon can see it.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Func/func.h"
#include "App/Pack/pack.h"
#include "App/Mon/sysmon.h"

#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "trice.h"

#include <stddef.h>
#include <string.h>

/* Private types ------------------------------------------------------------*/

/**
 * THE PACKED EVENT-ID SPACE.  One contiguous range per client, each sized by
 * that client's own exported count — so a client's internal enum can grow
 * without this file learning what any of its values mean, and the ranges
 * cannot drift from the counts.
 *
 * Nothing outside this file may name one of these ids.
 */
typedef enum {
    func_start = 0,
    func_tick,
    func_packedEventsStart,
    func_packEvt = func_packedEventsStart,
    func_packEvtLast = (func_packEvt + PACK_EVT_COUNT),
    func_packedEventsEnd = func_packEvtLast,
    func_last
} eFuncEvt;

_Static_assert((int)func_last <= (int)FUNC_EVT_MAX,
               "packed event space overflow");

/** One queue entry.  An id and one payload word — for the pack client the
 *  word is an instance index, and this file never looks at it. */
typedef struct {
    uint16_t evtId;
    void    *arg;
} sFuncEvt;

/* Private variables --------------------------------------------------------*/

static sFuncStats         s_stats;
static osMessageQueueId_t s_queue;
static osThreadId_t       s_task;
static int8_t             s_monId = -1;
static volatile int       s_running;
static uint32_t           s_lastTick_ms;

/* Static allocation, deliberately: every other task's stack and queue in this
 * project comes from the 48 KB .ccmheap, and CCM is ~91 % full.  These live in
 * .bss instead, where ~35 KB of main SRAM is free (§15). */
static StaticQueue_t s_queueCb;
static uint8_t       s_queueStore[FUNC_QUEUE_DEPTH * sizeof(sFuncEvt)];
static StaticTask_t  s_taskCb;
static StackType_t   s_taskStack[FUNC_TASK_STACK_WORDS];

/* Private function prototypes ----------------------------------------------*/

static void Post(uint16_t evtId, void *arg);
static void Dispatch(const sFuncEvt *evt);
static void FuncTask(void *arg);

/* Private functions --------------------------------------------------------*/

/**
 * @brief  The post function every client is handed.
 *
 *         LEGAL FROM ANY CONTEXT, INCLUDING AN ISR, and it never blocks:
 *         osMessageQueuePut with a zero timeout is ISR-safe by contract, and
 *         it is the CMSIS layer — not this file — that branches on IPSR.  So
 *         unlike Zhaga, which picks between two macros by event id, there is
 *         exactly one path here and no way to pick the wrong one.
 *
 *         On failure the loss is attributed to the client whose RANGE holds
 *         the id.  A shared counter would say "something was lost"; this says
 *         which client lost it, which is what makes it actionable.
 */
static void Post(uint16_t evtId, void *arg)
{
    sFuncEvt evt;

    if (s_queue == NULL) {
        return;
    }

    evt.evtId = evtId;
    evt.arg   = arg;

    if (osMessageQueuePut(s_queue, &evt, 0u, 0u) == osOK) {
        s_stats.posted++;
        return;
    }

    if ((evtId >= (uint16_t)func_packEvt) &&
        (evtId < (uint16_t)func_packEvtLast)) {
        s_stats.dropped[0]++;
    } else {
        s_stats.droppedUnknown++;
    }
}

/** Range-check chain, subtracting the base so a client recovers its own enum.
 *  This file never learns what the remainder means. */
static void Dispatch(const sFuncEvt *evt)
{
    s_stats.handled++;

    if (evt->evtId == (uint16_t)func_start) {
        /* Deferred client bring-up: a bind walks a Modbus catalogue, which is
         * flash I/O, and belongs on the task that owns the module rather than
         * on defaultTask (§13). */
        (void)Pack_Init((uint16_t)func_packEvt, Post);
        return;
    }

    if ((evt->evtId >= (uint16_t)func_packEvt) &&
        (evt->evtId < (uint16_t)func_packEvtLast)) {
        Pack_HandleEvent((uint16_t)(evt->evtId - (uint16_t)func_packEvt),
                         evt->arg);
        return;
    }

    TRice("wrn:[Func] unhandled event %u\n", (unsigned)evt->evtId);
}

/**
 * @brief  The task.
 *
 *         THE QUEUE WAIT IS BOUNDED, and that is the single most important
 *         departure from the ZhagaFW template.  Staleness is a wall-clock
 *         timeout and a silent pack posts nothing, so an event-only task
 *         blocked on osWaitForever could never fire the timeout it exists to
 *         enforce.  modbus_engine.c:464 already established this shape and
 *         says why in a comment.
 */
static void FuncTask(void *arg)
{
    (void)arg;

    s_monId = SysMon_TaskRegister(FUNC_TASK_STACK_WORDS,
                                  FUNC_TASK_DEADLINE_MS);

    for (;;) {
        sFuncEvt evt;
        uint32_t now;

        if (osMessageQueueGet(s_queue, &evt, NULL,
                              (uint32_t)FUNC_TICK_MS) == osOK) {
            Dispatch(&evt);
        }

        /* THE TICK IS A WALL CLOCK, NOT AN IDLE DETECTOR.  Running it only on
         * the queue-timeout branch meant any event arriving more often than
         * FUNC_TICK_MS suppressed it indefinitely -- and §11.2 budgets a
         * push type at 6 CAN frames per second, which alone guarantees the
         * timeout never fires.  Staleness detection, command-deadline expiry
         * and each type's own tick all live in Pack_Tick, so a board with one
         * push pack would never have detected a stale pack nor timed out a
         * command. */
        now = (uint32_t)osKernelGetTickCount();
        if ((uint32_t)(now - s_lastTick_ms) >= (uint32_t)FUNC_TICK_MS) {
            s_lastTick_ms = now;
            s_stats.ticks++;
            Pack_Tick(now);
        }

        SysMon_TaskCheckin(s_monId);
    }
}

/* Exported functions -------------------------------------------------------*/

int Func_Init(void)
{
    static const osMessageQueueAttr_t qattr = {
        .name    = "funcq",
        .cb_mem  = &s_queueCb,
        .cb_size = sizeof(s_queueCb),
        .mq_mem  = s_queueStore,
        .mq_size = sizeof(s_queueStore),
    };
    static const osThreadAttr_t tattr = {
        .name       = "func",
        .cb_mem     = &s_taskCb,
        .cb_size    = sizeof(s_taskCb),
        .stack_mem  = s_taskStack,
        .stack_size = sizeof(s_taskStack),
        /* Below modbus (24) so pack work never delays the bus or a sequence;
         * the slot the MQTT bridge occupies, for the same reason (§13). */
        .priority   = (osPriority_t)(osPriorityNormal - 1),
    };

    if (s_running) {
        return 0;                       /* idempotent                       */
    }

    (void)memset(&s_stats, 0, sizeof(s_stats));

    s_queue = osMessageQueueNew(FUNC_QUEUE_DEPTH, sizeof(sFuncEvt), &qattr);
    if (s_queue == NULL) {
        TRice("err:[Func] queue creation failed\n");
        return -1;
    }

    s_task = osThreadNew(FuncTask, NULL, &tattr);
    if (s_task == NULL) {
        TRice("err:[Func] task creation failed\n");
        return -1;
    }

    s_running = 1;
    TRice("[Func] task up, %u event ids, queue %u\n",
          (unsigned)func_last, (unsigned)FUNC_QUEUE_DEPTH);
    return 0;
}

int Func_Start(void)
{
    if (!s_running) {
        return -1;
    }
    /* Clients are brought up ON THE TASK, not here: see Dispatch. */
    Post((uint16_t)func_start, NULL);
    return 0;
}

int Func_Stats(sFuncStats *out)
{
    if (NULL == out) {
        return -1;
    }
    *out = s_stats;
    return 0;
}
