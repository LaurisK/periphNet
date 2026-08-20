/*
 * nvdb_wear.c
 *
 * Erase counts, per erasable unit.
 *
 * The whole of this file is INDICATION ONLY.  Nothing it produces may
 * influence an allocation, a relocation, a write or a result code (C13) —
 * which is exactly what allows it to be cheap and lossy, and what makes
 * losing the wear area cost a statistic rather than anything else.
 *
 * Counting rides in the one function that performs every erase, so it needs
 * no cooperation from users and no separate mechanism (§4.5).
 */

/* Includes -----------------------------------------------------------------*/
#include "nvdb_internal.h"
#include "nvdb_port.h"

#include <string.h>

/* Private defines ----------------------------------------------------------*/

/* Erases absorbed before the mirror is written back.  A statistic that is
 * out of date by at most this many erases is still a useful statistic, and
 * the alternative — a flash write per erase — would wear the medium out
 * measuring how worn it is. */
#define NVDB_WEAR_FLUSH_ERASES  64u

/* Idle visits from the collector before a small amount of dirt is written
 * back anyway.  The collector wakes about once a second when it has nothing
 * to do, so this is roughly a minute.  Flushing the instant it goes idle
 * would make a single erase anywhere on the medium cost a second one on this
 * one pinned sector — 2x amplification, always in the same place, to protect
 * a number C13 says may be lost. */
#define NVDB_WEAR_IDLE_TICKS    60u

/* Erased flash reads 0xFFFF, and a never-counted unit has been erased zero
 * times, so the two are the same fact.  Counting stops one short of it. */
#define NVDB_WEAR_UNKNOWN       0xFFFFu
#define NVDB_WEAR_MAX           0xFFFEu

/* Private variables --------------------------------------------------------*/

/* One counter per erasable unit of the whole medium.  It is a RAM mirror
 * rather than a flash counter because incrementing a count in NOR means
 * setting bits, which means an erase — and an erase is the thing being
 * counted. */
static uint16_t s_count[NVDB_UNIT_CNT];
static uint32_t s_dirty;
static uint32_t s_idleTicks;
static uint8_t  s_loaded;
static uint8_t  s_flushing;

_Static_assert(sizeof(s_count) <= NVDB_WEAR_SIZE,
               "the wear mirror must fit the wear area");

/* Exported functions -------------------------------------------------------*/

/**
 * @brief Load the counters from the medium.
 * @retval none
 * @note Called with the lock held, once the layout is in force.  A blank or
 *       half-written wear area simply reads as "never counted"; there is no
 *       CRC and nothing to repair, because there is nothing here worth
 *       repairing.
 */
void NvDbWear_Init(void)
{
    uint32_t i = 0u;

    memset(s_count, 0, sizeof(s_count));
    s_dirty     = 0u;
    s_idleTicks = 0u;
    s_flushing  = 0u;
    s_loaded    = 0u;

    if (nvdbRes_ok != NvDbInt_RawRead(NVDB_WEAR_ADDR, s_count,
                                      (uint32_t)sizeof(s_count))) {
        return;
    }
    for (i = 0u; i < NVDB_UNIT_CNT; i++) {
        if (NVDB_WEAR_UNKNOWN == s_count[i]) {
            s_count[i] = 0u;
        }
    }
    s_loaded = 1u;
}

/**
 * @brief Record that one erasable unit was erased.
 * @param  addr_bytes - any address inside the unit
 * @retval none
 * @note Called from the one place that erases anything.  It never fails and
 *       never reports: a lost count is a lost statistic.
 */
void NvDbWear_Count(uint32_t addr_bytes)
{
    uint32_t unit = addr_bytes / NVDB_UNIT_SIZE;

    if (0u == s_loaded || unit >= NVDB_UNIT_CNT) {
        return;
    }
    if (s_count[unit] < NVDB_WEAR_MAX) {
        s_count[unit]++;
    }
    s_dirty++;
}

/**
 * @brief Write the counters back if enough has changed to be worth it.
 * @param  idle - the collector has run out of work
 * @retval none
 * @note Called with the lock held, from the collector's own context.  The
 *       write is itself an erase, and it is counted like any other — the
 *       reentrancy guard is only there to stop that count triggering a second
 *       flush from inside the first.
 * @note Two triggers: enough dirt to be worth a sector, or long enough idle
 *       that a small amount will not get any larger.  Neither is urgent —
 *       these are numbers nothing reads back except a person.
 */
void NvDbWear_Flush(bool idle)
{
    if (0u == s_loaded || 0u == s_dirty || 0u != s_flushing) {
        s_idleTicks = 0u;
        return;
    }

    if (idle) {
        s_idleTicks++;
    } else {
        s_idleTicks = 0u;
    }

    if (s_dirty < NVDB_WEAR_FLUSH_ERASES && s_idleTicks < NVDB_WEAR_IDLE_TICKS) {
        return;
    }

    s_flushing = 1u;
    NvDbPort_Kick();
    if (nvdbRes_ok == NvDbInt_RawErase(NVDB_WEAR_ADDR) &&
        nvdbRes_ok == NvDbInt_RawProgram(NVDB_WEAR_ADDR, s_count,
                                         (uint32_t)sizeof(s_count))) {
        s_dirty = 0u;
    } else {
        /* If the wear area will not take a write, stop trying: retrying on
         * every idle visit forever would turn a lost statistic into a
         * permanent source of erases.  Losing the counters is allowed to
         * include losing the ability to write them. */
        s_loaded = 0u;
    }
    s_idleTicks = 0u;
    s_flushing  = 0u;
}

/**
 * @brief How many counted erases have not been written back yet.
 * @retval the count, which a reboot would lose
 */
uint32_t NvDbWear_Unsaved(void)
{
    return s_dirty;
}

/**
 * @brief Erase counts across a span of the medium.
 * @param  addr_bytes - start of the span
 * @param  len_bytes - its length
 * @param  max - filled with the highest count seen; may be NULL
 * @param  total - filled with the sum; may be NULL
 * @retval none
 */
void NvDbWear_Span(uint32_t addr_bytes, uint32_t len_bytes,
                   uint32_t *max, uint32_t *total)
{
    uint32_t first = addr_bytes / NVDB_UNIT_SIZE;
    uint32_t cnt   = (len_bytes + NVDB_UNIT_SIZE - 1u) / NVDB_UNIT_SIZE;
    uint32_t hi    = 0u;
    uint32_t sum   = 0u;
    uint32_t i     = 0u;

    for (i = 0u; i < cnt && (first + i) < NVDB_UNIT_CNT; i++) {
        uint32_t v = s_count[first + i];

        if (v > hi) {
            hi = v;
        }
        sum += v;
    }
    if (NULL != max) {
        *max = hi;
    }
    if (NULL != total) {
        *total = sum;
    }
}
