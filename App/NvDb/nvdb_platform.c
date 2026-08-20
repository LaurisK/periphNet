/*
 * nvdb_platform.c
 *
 * The nvDb port on the device: one mutex and one lowest-priority task.
 */

/* Includes -----------------------------------------------------------------*/
#include "nvdb_platform.h"
#include "nvdb_port.h"

#include "App/system.h"
#include "App/Mon/sysmon.h"
#include "trice.h"

#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

/* Private defines ----------------------------------------------------------*/

/* Stack: the deepest thing the core does is a unit-sized copy, and that
 * buffer is static.  256 words is the same budget the trice task gets. */
#define NVDB_TASK_STACK_WORDS   256u

/* The collector sleeps until there is something to do, then works through it
 * one unit at a time.  This is only a backstop for a notification lost to a
 * full queue, so it can afford to be slow. */
#define NVDB_IDLE_WAIT_MS       1000u

/* Private variables --------------------------------------------------------*/

static SemaphoreHandle_t s_lock;
static osThreadId_t      s_collector;

/* Private function prototypes ----------------------------------------------*/

static void CollectorTask(void *argument);

/* Private functions --------------------------------------------------------*/

/**
 * @brief Erase deleted space in the background, forever.
 * @param  argument - unused
 * @retval does not return
 * @note Lowest priority in the system, one erasable unit per call, so a
 *       waiting writer gets in between units.  It never reboots anything and
 *       never reports an error upwards: a delete that cannot be collected
 *       costs a slow write, never correctness.
 */
static void CollectorTask(void *argument)
{
    int8_t monId = 0;

    (void)argument;

    /* Deadline 0: this task legitimately blocks for as long as nothing has
     * been deleted, so liveness is not something to judge it on. */
    monId = SysMon_TaskRegister(NVDB_TASK_STACK_WORDS, 0u);

    for (;;) {
        SysMon_TaskCheckin(monId);

        if (!NvDb_CollectStep()) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(NVDB_IDLE_WAIT_MS));
        }
    }
}

/* Exported functions -------------------------------------------------------*/

/**
 * @brief Take the nvDb lock.
 * @retval none
 * @note MUST be priority-inheriting, which xSemaphoreCreateMutex gives: a
 *       lowest-priority collector holding this mid-erase would otherwise
 *       block a high-priority writer with no way to be boosted.
 * @note Before the scheduler runs, and in fault context, there is nothing to
 *       serialize against — the same carve-out the W25Q128 driver makes, and
 *       for the same reason.
 */
void NvDbPort_Lock(void)
{
    if (NULL == s_lock || taskSCHEDULER_RUNNING != xTaskGetSchedulerState() ||
        0u != __get_IPSR()) {
        return;
    }
    (void)xSemaphoreTake(s_lock, portMAX_DELAY);
}

/**
 * @brief Release the nvDb lock.
 * @retval none
 */
void NvDbPort_Unlock(void)
{
    if (NULL == s_lock || taskSCHEDULER_RUNNING != xTaskGetSchedulerState() ||
        0u != __get_IPSR()) {
        return;
    }
    (void)xSemaphoreGive(s_lock);
}

/**
 * @brief There is deferred erase work; wake the collector.
 * @retval none
 * @note Called with the lock held, so it must not block.  A lost notification
 *       costs at most NVDB_IDLE_WAIT_MS of latency, never a lost delete.
 */
void NvDbPort_CollectorNotify(void)
{
    if (NULL == s_collector) {
        return;
    }
    (void)xTaskNotifyGive((TaskHandle_t)s_collector);
}

/**
 * @brief Feed the watchdog during a long run of erases.
 * @retval none
 * @note A relayout can copy a 488 KB area, and a wipe can erase one.  Neither
 *       may outrun the ~16.4 s IWDG.
 * @note It deliberately does not count as a watchdog kick — see
 *       System_FeedWatchdogLongOp().
 */
void NvDbPort_Kick(void)
{
    /* Feed the hardware, but do NOT stamp the watchdog statistic: nvDb's
     * long operations run on whichever task asked for them — and one of them
     * is the lowest-priority collector.  Letting those count as kicks would
     * make System_GetIwdgStats()'s worst-ever gap read "since anybody
     * kicked" rather than "since defaultTask completed a loop", which is
     * optimistic precisely when something has gone wrong. */
    System_FeedWatchdogLongOp();
}

/**
 * @brief Create the lock and the collector, then bring nvDb up.
 * @retval whatever NvDb_Init() says: nvdbRes_ok, nvdbRes_refused when a
 *            layout was rejected and the previous one stands, or
 *            nvdbRes_flash
 * @note The lock exists before NvDb_Init() runs, because init takes it.
 */
eNvDbRes NvDbPlatform_Init(void)
{
    static const osThreadAttr_t attr = {
        .name       = "nvdb",
        .stack_size = NVDB_TASK_STACK_WORDS * 4u,
        .priority   = (osPriority_t)osPriorityLow,
    };
    eNvDbRes res = nvdbRes_ok;

    if (NULL == s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (NULL == s_lock) {
            return nvdbRes_flash;
        }
    }

    res = NvDb_Init();

    if (NULL == s_collector) {
        s_collector = osThreadNew(CollectorTask, NULL, &attr);
    }
    return res;
}
