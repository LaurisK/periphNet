/*
 * func.c
 *
 * The shared functionality task: packed event-ID space, one task, one static
 * queue, per-client drop counters, dispatch and client wiring
 * (docs/design_battery_pack.md §7, §13).
 *
 * A COMPOSITION ROOT, like cmd_parser.c.  This is the only file that knows
 * what a packed event id means, the only one that chooses between the task-
 * and ISR-context post, and the only one that names a client's range.
 *
 * STATUS: SCAFFOLDING.  NO TASK IS CREATED and no queue exists; Func_Init is
 * not called from App_DefaultTaskEntry and nothing registers with sysmon.
 * The event-ID space below is real and static-asserted, because getting it
 * wrong later is the expensive part.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Func/func.h"
#include "App/Pack/pack.h"

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

/* Private variables --------------------------------------------------------*/

static sFuncStats s_stats;

/* Private function prototypes ----------------------------------------------*/

static void Post(uint16_t evtId, void *arg);

/* Private functions --------------------------------------------------------*/

/**
 * @brief  The post function every client is handed.
 *
 * WHEN IMPLEMENTED: inspect IPSR, use xQueueSendToBackFromISR or
 * xQueueSendToBack accordingly, and ON FAILURE attribute the loss to the
 * client whose range holds @p evtId — s_stats.dropped[client] — rather than
 * to one shared counter.  It never blocks and never logs from an ISR.
 *
 * @param  evtId - a packed event id
 * @param  arg - the client's data word
 * @note   ANY CONTEXT, including an ISR.  MUST NOT BLOCK.
 */
static void Post(uint16_t evtId, void *arg)
{
    (void)arg;

    /* STUB — there is no queue, so every post is a drop.  Counting it is the
     * honest report: a dropped client event is a missed state change. */
    if (((uint32_t)evtId >= (uint32_t)func_packEvt) &&
        ((uint32_t)evtId <  (uint32_t)func_packEvtLast)) {
        s_stats.dropped[0]++;
    } else {
        s_stats.droppedUnknown++;
    }
}

/* Exported functions -------------------------------------------------------*/

int Func_Init(void)
{
    (void)memset(&s_stats, 0, sizeof(s_stats));

    /* STUB.  The real one creates the static queue and then calls each
     * client's init with its base and Post — today only
     * Pack_Init(func_packEvt, Post).  It is deliberately NOT calling it here:
     * nothing in this module may run on a board until the §14 nvDb migration
     * and the §16-item-5 CAN RX dispatcher exist. */
    (void)Post;

    return -1;
}

int Func_Start(void)
{
    /* STUB — no task is created.  See Func_Init. */
    return -1;
}

int Func_Stats(sFuncStats *out)
{
    if (NULL == out) {
        return -1;
    }
    *out = s_stats;
    return 0;
}
