/**
 * @file    sysmon.c
 * @brief   System monitor — task liveness, stack, heap and CPU accounting
 *
 * Sampling model
 * --------------
 * Every SYSMON_WINDOW_MS the monitor enumerates the whole system with
 * uxTaskGetSystemState() and matches each task to a slot by handle.  Slots
 * are created on first sight, so tasks the application does not own
 * (tcpip_thread, EthIf, IDLE, Tmr Svc) are tracked too — they just have no
 * check-in and no deadline.
 *
 * CPU is measured as a DELTA per window rather than read out of the FreeRTOS
 * lifetime totals.  Those totals are uint32_t and the run-time clock ticks at
 * ~10.5 MHz, so a busy task wraps them every ~409 s; unsigned delta
 * arithmetic over a 1 s window is immune to that, and the monitor keeps its
 * own 64-bit accumulator for the lifetime figure.
 *
 * Idle time is simply the idle task's share of the window, which is why the
 * board's total load is reported as 1000 - idle: interrupt time is charged by
 * FreeRTOS to whichever task was interrupted, so summing task shares would
 * double-count nothing but would also hide nothing.
 */

#include "App/Mon/sysmon.h"
#include "App/system.h"
#include "FreeRTOS.h"
#include "task.h"
#include "trice.h"
#include "stm32f4xx.h"
#include "lwipopts.h"      /* TCPIP_THREAD_STACKSIZE — see s_externalTasks */
#include <string.h>
#include <stdio.h>

/* --------------------------------------------------------------------------
 * Private types
 * -------------------------------------------------------------------------- */

typedef struct {
    uint64_t     runTimeTicks;        /* lifetime, immune to the 32-bit wrap */
    TaskHandle_t handle;
    uint32_t     lastRunTimeCtr;      /* FreeRTOS total at the last sample   */
    uint32_t     deadline_ms;
    volatile uint32_t lastCheckin_ms;
    volatile uint32_t checkinCnt;
    uint32_t     staleCnt;
    uint32_t     sinceCheckin_ms;
    char         name[configMAX_TASK_NAME_LEN];
    uint16_t     stackSize_words;
    uint16_t     stackFreeMin_words;
    uint16_t     cpuLoad_permille;
    uint16_t     cpuPeak_permille;
    uint8_t      priority;
    uint8_t      state;
    uint8_t      inUse;
    uint8_t      present;
    uint8_t      primed;              /* 0 until one full window has elapsed */
    uint8_t      stale;
    uint8_t      stackWarned;
    uint8_t      hasCheckin;          /* 1 once the task has registered      */
} sSysMonSlot;

/**
 * Configured depth of tasks created outside the application, so their
 * high-water mark can be shown as a percentage.  Getting one wrong costs a
 * misleading percentage and nothing else — the free-words figure comes from
 * FreeRTOS either way.
 */
typedef struct {
    const char *name;
    uint16_t    stackSize_words;
} sExternalTask;

static const sExternalTask s_externalTasks[] = {
    { "IDLE",         (uint16_t)configMINIMAL_STACK_SIZE                   },
    { "Tmr Svc",      (uint16_t)configTIMER_TASK_STACK_DEPTH               },
    { "tcpip_thread", (uint16_t)(TCPIP_THREAD_STACKSIZE / sizeof(StackType_t)) },
    /* LWIP/Target/ethernetif.c: INTERFACE_THREAD_STACK_SIZE, local to that
     * CubeMX-owned file and therefore not reachable from here. */
    { "EthIf",        (uint16_t)(1024u / sizeof(StackType_t))              },
};

/* --------------------------------------------------------------------------
 * Private data — plain .bss (main SRAM).  Deliberately NOT .ccmram: CCM is
 * the constrained region on this part and none of this is hot.
 * -------------------------------------------------------------------------- */

static sSysMonSlot   s_slots[SYSMON_MAX_TASKS];
static TaskStatus_t  s_status[SYSMON_MAX_TASKS];
static sSysMonSummary s_summary;

static uint32_t s_slotCnt;
static uint32_t s_lastSample_ms;
static uint32_t s_lastCtr;
static uint32_t s_ticksPerMs;         /* run-time clock ticks in one ms      */
static uint8_t  s_inited;

static const char * const s_stateNames[] = {
    "Run", "Rdy", "Blk", "Sus", "Del", "Inv"
};

/* --------------------------------------------------------------------------
 * Run-time stats clock (DWT cycle counter / 16)
 *
 * The raw cycle counter would wrap every 25.6 s at 168 MHz, which is inside
 * the sampling window's error budget once a sample is ever late; /16 pushes
 * the wrap out to ~409 s and still resolves ~95 ns.
 * -------------------------------------------------------------------------- */

void SysMon_RunTimeCounterInit(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT       = 0u;
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t SysMon_RunTimeCounter(void)
{
    return DWT->CYCCNT >> 4;
}

/* --------------------------------------------------------------------------
 * Private helpers
 * -------------------------------------------------------------------------- */

static uint16_t external_stack_words(const char *name)
{
    for (uint32_t i = 0u; i < (sizeof(s_externalTasks) /
                               sizeof(s_externalTasks[0])); i++) {
        if (strncmp(name, s_externalTasks[i].name,
                    configMAX_TASK_NAME_LEN) == 0) {
            return s_externalTasks[i].stackSize_words;
        }
    }
    return 0u;
}

static void slot_reset(sSysMonSlot *slot, TaskHandle_t handle, const char *name)
{
    memset(slot, 0, sizeof(*slot));
    slot->handle = handle;
    if (name != NULL) {
        strncpy(slot->name, name, sizeof(slot->name) - 1u);
    }
    slot->stackSize_words = external_stack_words(slot->name);
    slot->inUse           = 1u;
    slot->present         = 1u;
}

/**
 * Find the slot bound to @p handle, creating one if there is room.
 *
 * A handle can be recycled after a task is deleted (mqtt starts and stops at
 * runtime), so a slot whose stored name no longer matches is treated as a
 * different task and reset — otherwise the new task would inherit the dead
 * one's counters and, worse, its deadline.
 */
static sSysMonSlot *slot_find(TaskHandle_t handle, const char *name)
{
    sSysMonSlot *spare = NULL;

    for (uint32_t i = 0u; i < s_slotCnt; i++) {
        sSysMonSlot *slot = &s_slots[i];

        if (slot->inUse && slot->handle == handle) {
            if (name != NULL &&
                strncmp(slot->name, name, sizeof(slot->name)) != 0) {
                slot_reset(slot, handle, name);
            }
            return slot;
        }
        /* Departed tasks keep their slot until it is needed, so a task that
         * died is still visible in one more report. */
        if (spare == NULL && (!slot->inUse || !slot->present)) {
            spare = slot;
        }
    }

    if (s_slotCnt < SYSMON_MAX_TASKS) {
        spare = &s_slots[s_slotCnt];
        s_slotCnt++;
    } else if (spare == NULL) {
        return NULL;
    }

    slot_reset(spare, handle, name);
    return spare;
}

/* --------------------------------------------------------------------------
 * Sampling
 * -------------------------------------------------------------------------- */

static void sample(uint32_t now_ms)
{
    uint32_t ctrNow  = SysMon_RunTimeCounter();
    uint32_t window  = ctrNow - s_lastCtr;      /* wraps correctly */
    uint32_t denom   = window / 1000u;          /* ticks per permille */
    uint16_t idle      = 0u;
    uint8_t  idleValid = 0u;
    uint8_t  staleCnt  = 0u;
    uint8_t  warnCnt   = 0u;
    UBaseType_t n;

    s_lastCtr = ctrNow;

    if (window != 0u) {
        s_summary.runTimeCounterOk = 1u;
    }

    /* uxTaskGetSystemState() returns 0 — not a truncated list — when the array
     * is too small, so an over-subscribed monitor would silently report
     * nothing at all.  Say so once, and leave the existing task data alone
     * rather than marking every task gone. */
    n = uxTaskGetSystemState(s_status, SYSMON_MAX_TASKS, NULL);
    if (n == 0u) {
        static uint8_t s_overflowLogged;
        if (!s_overflowLogged) {
            s_overflowLogged = 1u;
            TRice("err:SysMon: %u tasks exceed SYSMON_MAX_TASKS (%u) — "
                  "no task data\n", (unsigned)uxTaskGetNumberOfTasks(),
                  (unsigned)SYSMON_MAX_TASKS);
        }
    } else {
        for (uint32_t i = 0u; i < s_slotCnt; i++) {
            s_slots[i].present = 0u;
        }
    }

    TaskHandle_t idleHandle = xTaskGetIdleTaskHandle();

    for (UBaseType_t i = 0u; i < n; i++) {
        const TaskStatus_t *st = &s_status[i];
        sSysMonSlot        *slot;

        /* Same allocator SysMon_TaskRegister() uses, and a task can register
         * at any time (the mqtt bridge starts and stops at runtime). */
        taskENTER_CRITICAL();
        slot = slot_find(st->xHandle, st->pcTaskName);
        taskEXIT_CRITICAL();

        if (slot == NULL) {
            continue;
        }

        slot->present            = 1u;
        slot->priority           = (uint8_t)st->uxCurrentPriority;
        slot->state              = (uint8_t)st->eCurrentState;
        slot->stackFreeMin_words = (uint16_t)st->usStackHighWaterMark;

        if (slot->primed && denom != 0u) {
            uint32_t delta    = st->ulRunTimeCounter - slot->lastRunTimeCtr;
            uint32_t permille = delta / denom;

            slot->runTimeTicks += delta;
            if (permille > 1000u) {
                permille = 1000u;   /* a late sample, not 101 % of a CPU */
            }
            slot->cpuLoad_permille = (uint16_t)permille;
            if (permille > slot->cpuPeak_permille) {
                slot->cpuPeak_permille = (uint16_t)permille;
            }
            if (st->xHandle == idleHandle) {
                idle      = (uint16_t)permille;
                idleValid = 1u;
            }
        } else {
            slot->primed = 1u;
        }
        slot->lastRunTimeCtr = st->ulRunTimeCounter;

        /* Stack: warn once per crossing, so a chronically tight task does not
         * flood the log but a NEW one is impossible to miss. */
        if (slot->stackFreeMin_words < SYSMON_STACK_WARN_WORDS) {
            warnCnt++;
            if (!slot->stackWarned) {
                char line[64];
                slot->stackWarned = 1u;
                snprintf(line, sizeof(line), "%s: %u words free of %u",
                         slot->name, (unsigned)slot->stackFreeMin_words,
                         (unsigned)slot->stackSize_words);
                TRiceS("err:SysMon: stack low — %s\n", line);
            }
        }

        /* Liveness.  Only a registered task with a deadline can go stale;
         * everything else is reported but never judged. */
        /* Signed difference, not a bare unsigned subtract.  now_ms is
         * sampled once before this walk, but a HIGHER-PRIORITY task
         * preempts it mid-walk and checks in.  Trice at priority 25,
         * checking in every 10 ms, against defaultTask at 24 is exactly
         * that race: its lastCheckin_ms then sits AHEAD of now_ms, the
         * unsigned wrap produced ~4294967294 ms, and that tripped the
         * deadline and reported a healthy task as stalled.  The signed
         * form is also correct across the 49.7-day tick wrap. */
        int32_t age_ms = (int32_t)(now_ms - slot->lastCheckin_ms);
        slot->sinceCheckin_ms = (age_ms > 0) ? (uint32_t)age_ms : 0u;
        if (slot->hasCheckin && slot->deadline_ms != 0u) {
            if (slot->sinceCheckin_ms > slot->deadline_ms) {
                staleCnt++;
                if (!slot->stale) {
                    char line[64];
                    slot->stale = 1u;
                    slot->staleCnt++;
                    snprintf(line, sizeof(line), "%s: no check-in for %u ms "
                             "(deadline %u ms)", slot->name,
                             (unsigned)slot->sinceCheckin_ms,
                             (unsigned)slot->deadline_ms);
                    TRiceS("err:SysMon: task stalled — %s\n", line);
                }
            } else if (slot->stale) {
                char line[32];
                slot->stale = 0u;
                snprintf(line, sizeof(line), "%s", slot->name);
                TRiceS("SysMon: task recovered — %s\n", line);
            }
        }
    }

    s_summary.uptime_sec        = now_ms / 1000u;
    s_summary.heapSize_bytes    = (uint32_t)configTOTAL_HEAP_SIZE;
    s_summary.heapFree_bytes    = (uint32_t)xPortGetFreeHeapSize();
    s_summary.heapFreeMin_bytes = (uint32_t)xPortGetMinimumEverFreeHeapSize();
    s_summary.sampleCnt++;
    s_summary.taskCnt           = (uint8_t)s_slotCnt;
    s_summary.staleCnt          = staleCnt;
    s_summary.stackWarnCnt      = warnCnt;

    /* The first window has nothing to compare against, so the idle share is
     * unknown rather than zero — reporting 0 would read as "100 % busy". */
    if (idleValid) {
        s_summary.idle_permille    = idle;
        s_summary.cpuLoad_permille = (uint16_t)(1000u - idle);
        if (s_summary.cpuLoad_permille > s_summary.cpuPeakLoad_permille) {
            s_summary.cpuPeakLoad_permille = s_summary.cpuLoad_permille;
        }
    }

    System_GetIwdgStats(&s_summary.iwdgGapMax_ms, &s_summary.iwdgSinceKick_ms);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void SysMon_Init(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    memset(&s_summary, 0, sizeof(s_summary));
    s_slotCnt       = 0u;
    /* Not SysMon_RunTimeCounter(): this runs BEFORE the scheduler, and
     * vTaskStartScheduler() zeroes CYCCNT through
     * portCONFIGURE_TIMER_FOR_RUN_TIME_STATS.  Reading a DWT a debug session
     * left running would make the first window's delta a wrapped 4-billion. */
    s_lastCtr       = 0u;
    s_lastSample_ms = HAL_GetTick();
    s_ticksPerMs    = (SystemCoreClock >> 4) / 1000u;
    if (s_ticksPerMs == 0u) {
        s_ticksPerMs = 1u;
    }
    s_inited = 1u;
}

int8_t SysMon_TaskRegister(uint32_t stackSize_words, uint32_t deadline_ms)
{
    TaskHandle_t handle = xTaskGetCurrentTaskHandle();
    const char  *name   = pcTaskGetName(NULL);
    sSysMonSlot *slot;
    int8_t       id;

    if (!s_inited) {
        return SYSMON_ID_NONE;
    }

    taskENTER_CRITICAL();
    slot = slot_find(handle, name);
    if (slot != NULL) {
        slot->stackSize_words = (uint16_t)stackSize_words;
        slot->deadline_ms     = deadline_ms;
        slot->hasCheckin      = 1u;
        slot->lastCheckin_ms  = HAL_GetTick();
        id = (int8_t)(slot - s_slots);
    } else {
        id = SYSMON_ID_NONE;
    }
    taskEXIT_CRITICAL();

    return id;
}

void SysMon_TaskCheckin(int8_t id)
{
    if (id < 0 || (uint32_t)id >= SYSMON_MAX_TASKS) {
        return;
    }
    s_slots[id].lastCheckin_ms = HAL_GetTick();
    s_slots[id].checkinCnt++;
}

void SysMon_Poll(void)
{
    uint32_t now_ms;

    if (!s_inited) {
        return;
    }

    now_ms = HAL_GetTick();
    if ((now_ms - s_lastSample_ms) < SYSMON_WINDOW_MS) {
        return;
    }
    s_lastSample_ms = now_ms;

    sample(now_ms);
}

void SysMon_GetSummary(sSysMonSummary *out)
{
    if (out == NULL) {
        return;
    }
    vTaskSuspendAll();
    *out = s_summary;
    xTaskResumeAll();
}

uint8_t SysMon_GetTasks(sSysMonTaskInfo *out, uint8_t max)
{
    uint8_t written = 0u;

    if (out == NULL) {
        return 0u;
    }

    vTaskSuspendAll();
    for (uint32_t i = 0u; i < s_slotCnt && written < max; i++) {
        const sSysMonSlot *slot = &s_slots[i];

        if (!slot->inUse) {
            continue;
        }
        out[written].name               = slot->name;
        out[written].runTime_ms         = (uint32_t)(slot->runTimeTicks /
                                                     s_ticksPerMs);
        out[written].checkinCnt         = slot->checkinCnt;
        out[written].sinceCheckin_ms    = slot->hasCheckin
                                          ? slot->sinceCheckin_ms : 0u;
        out[written].deadline_ms        = slot->deadline_ms;
        out[written].staleCnt           = slot->staleCnt;
        out[written].stackSize_words    = slot->stackSize_words;
        out[written].stackFreeMin_words = slot->stackFreeMin_words;
        out[written].cpuLoad_permille   = slot->cpuLoad_permille;
        out[written].cpuPeak_permille   = slot->cpuPeak_permille;
        out[written].priority           = slot->priority;
        out[written].state              = slot->state;
        out[written].present            = slot->present;
        out[written].stale              = slot->stale;
        written++;
    }
    xTaskResumeAll();

    return written;
}

bool SysMon_AllTasksAlive(void)
{
    return (s_summary.staleCnt == 0u);
}

void SysMon_ResetPeaks(void)
{
    vTaskSuspendAll();
    for (uint32_t i = 0u; i < s_slotCnt; i++) {
        s_slots[i].cpuPeak_permille = 0u;
        s_slots[i].staleCnt         = 0u;
        s_slots[i].stackWarned      = 0u;
    }
    s_summary.cpuPeakLoad_permille = 0u;
    xTaskResumeAll();

    System_ResetIwdgStats();
}

/* --------------------------------------------------------------------------
 * Trice report
 * -------------------------------------------------------------------------- */

const char *SysMon_StateName(uint8_t state)
{
    return (state < (sizeof(s_stateNames) / sizeof(s_stateNames[0])))
           ? s_stateNames[state] : "???";
}

void SysMon_Report(void)
{
    sSysMonTaskInfo tasks[SYSMON_MAX_TASKS];
    sSysMonSummary  sum;
    uint8_t         n;
    char            line[112];

    SysMon_GetSummary(&sum);
    n = SysMon_GetTasks(tasks, SYSMON_MAX_TASKS);

    snprintf(line, sizeof(line),
             "up=%us cpu=%u.%u%% peak=%u.%u%% idle=%u.%u%% samples=%u",
             (unsigned)sum.uptime_sec,
             (unsigned)(sum.cpuLoad_permille / 10u),
             (unsigned)(sum.cpuLoad_permille % 10u),
             (unsigned)(sum.cpuPeakLoad_permille / 10u),
             (unsigned)(sum.cpuPeakLoad_permille % 10u),
             (unsigned)(sum.idle_permille / 10u),
             (unsigned)(sum.idle_permille % 10u),
             (unsigned)sum.sampleCnt);
    TRiceS("SysMon: %s\n", line);

    snprintf(line, sizeof(line),
             "heap %u/%u B free (min %u) | iwdg gap max %u ms, last kick %u "
             "ms ago", (unsigned)sum.heapFree_bytes,
             (unsigned)sum.heapSize_bytes, (unsigned)sum.heapFreeMin_bytes,
             (unsigned)sum.iwdgGapMax_ms, (unsigned)sum.iwdgSinceKick_ms);
    TRiceS("SysMon: %s\n", line);

    if (!sum.runTimeCounterOk) {
        TRice("err:SysMon: run-time clock never advanced, CPU figures are 0\n");
    }

    TRice("SysMon: task           pri st   cpu%%  peak  stack(free/size)  checkins\n");

    for (uint8_t i = 0u; i < n; i++) {
        const sSysMonTaskInfo *t = &tasks[i];
        char stack[24];
        char live[32];

        if (t->stackSize_words != 0u) {
            snprintf(stack, sizeof(stack), "%u/%u", (unsigned)t->stackFreeMin_words,
                     (unsigned)t->stackSize_words);
        } else {
            snprintf(stack, sizeof(stack), "%u/?", (unsigned)t->stackFreeMin_words);
        }

        if (t->deadline_ms != 0u) {
            snprintf(live, sizeof(live), "%u (%u ms ago)%s",
                     (unsigned)t->checkinCnt, (unsigned)t->sinceCheckin_ms,
                     t->stale ? " STALE" : "");
        } else {
            snprintf(live, sizeof(live), "%u", (unsigned)t->checkinCnt);
        }

        snprintf(line, sizeof(line),
                 "%-14s %3u %s %3u.%u%% %3u.%u%% %16s  %s%s",
                 t->name, (unsigned)t->priority,
                 SysMon_StateName(t->state),
                 (unsigned)(t->cpuLoad_permille / 10u),
                 (unsigned)(t->cpuLoad_permille % 10u),
                 (unsigned)(t->cpuPeak_permille / 10u),
                 (unsigned)(t->cpuPeak_permille % 10u),
                 stack, live, t->present ? "" : " GONE");
        TRiceS("  %s\n", line);
    }
}
