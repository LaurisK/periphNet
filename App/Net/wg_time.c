/**
 * @file    wg_time.c
 * @brief   Reboot-surviving monotonic seconds counter — see wg_time.h.
 */

#include "App/Net/wg_time.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx_hal.h"
#include "image_mgmt.h"
#include "nvdb.h"

/* --------------------------------------------------------------------------
 * Flash slot ring
 * -------------------------------------------------------------------------- */

typedef struct {
    uint32_t magic;      /* WG_TIME_MAGIC                            */
    uint32_t seconds;    /* persisted monotonic value                */
    uint32_t seq;        /* write counter, diagnostics only          */
    uint32_t crc32;      /* CRC32 over magic..seq                    */
} sWgTimeSlot;           /* 16 B → 256 slots per 4 KB sector         */

#define WG_TIME_SLOT_SIZE   ((uint32_t)sizeof(sWgTimeSlot))

/* How many slots the ring holds, learned from nvDb at init.  A user is told
 * its own size and nothing else about the medium — no sector, no erase — so
 * this is a runtime value now rather than a constant derived from a map. */
static uint32_t s_slotCount;

/* Slots read per flash transaction — keeps the stack footprint small enough
 * for defaultTask (16 slots = 256 B). */
#define WG_TIME_SCAN_SLOTS  16u

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */

static uint32_t s_baseSeconds;   /* value at s_baseTick                     */
static uint32_t s_baseTick;      /* HAL tick the base was taken at          */
static uint32_t s_persisted;     /* last value written to flash             */
static uint32_t s_seq;           /* seq of the last written slot            */
static uint32_t s_slotIdx;       /* slot the NEXT write goes to             */
static uint8_t  s_initDone;
static uint8_t  s_flashOk;

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static uint32_t slot_crc(const sWgTimeSlot *slot)
{
    return ImgMgmt_Crc32((const uint8_t *)slot,
                         (uint32_t)offsetof(sWgTimeSlot, crc32));
}

static int slot_valid(const sWgTimeSlot *slot)
{
    return (slot->magic == WG_TIME_MAGIC) && (slot->crc32 == slot_crc(slot));
}

static int slot_erased(const sWgTimeSlot *slot)
{
    const uint8_t *p = (const uint8_t *)slot;

    for (uint32_t i = 0u; i < WG_TIME_SLOT_SIZE; i++) {
        if (p[i] != 0xFFu) {
            return 0;
        }
    }
    return 1;
}

/* Scan the ring for the newest valid slot.  Returns 1 if one was found.
 * *outSeconds / *outSeq carry it, *outNextIdx the slot to write next. */
static int ring_scan(uint32_t *outSeconds, uint32_t *outSeq,
                     uint32_t *outNextIdx)
{
    sWgTimeSlot batch[WG_TIME_SCAN_SLOTS];
    uint32_t    bestSeconds = 0u;
    uint32_t    bestSeq     = 0u;
    uint32_t    nextIdx     = 0u;
    int         found       = 0;

    for (uint32_t i = 0u; i < s_slotCount; i += WG_TIME_SCAN_SLOTS) {
        /* The ring is however many slots nvDb's area holds, which a layout
         * change can make a number that does not divide evenly. */
        uint32_t n = s_slotCount - i;

        if (n > WG_TIME_SCAN_SLOTS) {
            n = WG_TIME_SCAN_SLOTS;
        }
        if (NvDb_Read(nvdbUser_wgTime, batch, i * WG_TIME_SLOT_SIZE,
                      n * WG_TIME_SLOT_SIZE) != nvdbRes_ok) {
            return -1;
        }

        for (uint32_t j = 0u; j < n; j++) {
            if (!slot_valid(&batch[j])) {
                continue;
            }
            /* Highest index wins the write cursor; highest value wins the
             * base, so a stale-but-valid leftover can never pull time back. */
            nextIdx = i + j + 1u;
            if (!found || (batch[j].seconds > bestSeconds)) {
                bestSeconds = batch[j].seconds;
                bestSeq     = batch[j].seq;
            }
            found = 1;
        }
    }

    *outSeconds = bestSeconds;
    *outSeq     = bestSeq;
    *outNextIdx = nextIdx;
    return found;
}

/* Append one slot.  Restarts the ring when it is full, or when the target
 * slot is not blank (a torn write from a power cut).
 *
 * Append-shaped by design: every write lands in never-written space, which is
 * the case nvDb programs directly without an erase.  Restarting the ring is
 * the one moment that costs anything, and it is declared as what it is — the
 * old slots are no longer wanted. */
static int ring_append(uint32_t seconds)
{
    sWgTimeSlot slot;
    sWgTimeSlot existing;

    if (s_slotIdx < s_slotCount) {
        if (NvDb_Read(nvdbUser_wgTime, &existing,
                      s_slotIdx * WG_TIME_SLOT_SIZE,
                      WG_TIME_SLOT_SIZE) != nvdbRes_ok) {
            return -1;
        }
        if (!slot_erased(&existing)) {
            s_slotIdx = s_slotCount;          /* force a fresh ring */
        }
    }

    if (s_slotIdx >= s_slotCount) {
        if (NvDb_Wipe(nvdbUser_wgTime, NULL) != nvdbRes_ok) {
            return -1;
        }
        s_slotIdx = 0u;
    }

    slot.magic   = WG_TIME_MAGIC;
    slot.seconds = seconds;
    slot.seq     = ++s_seq;
    slot.crc32   = slot_crc(&slot);

    /* The write into just-wiped space pulls that erase forward itself, so the
     * slot is on the medium when this returns either way. */
    if (NvDb_Write(nvdbUser_wgTime, &slot, s_slotIdx * WG_TIME_SLOT_SIZE,
                   WG_TIME_SLOT_SIZE) != nvdbRes_ok) {
        return -1;
    }

    s_slotIdx++;
    s_persisted = seconds;
    return 0;
}

/* Move the base forward and re-anchor it to the current tick.  Keeps the
 * elapsed-tick delta far away from the 49-day HAL_GetTick() wrap. */
static void rebase(uint32_t seconds)
{
    taskENTER_CRITICAL();
    s_baseSeconds = seconds;
    s_baseTick    = HAL_GetTick();
    taskEXIT_CRITICAL();
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int WgTime_Init(void)
{
    uint32_t stored  = 0u;
    uint32_t seq     = 0u;
    uint32_t nextIdx = 0u;
    uint32_t size    = 0u;
    int      scan;

    if (s_initDone) {
        return 0;
    }
    s_initDone = 1u;

    rebase((uint32_t)WG_TIME_BUILD_EPOCH);

    /* How much room the ring has is nvDb's answer, and a layout change can
     * make it a different one.  Nothing else about the medium is asked. */
    if (NvDb_GetSize(nvdbUser_wgTime, &size) != nvdbRes_ok ||
        size < WG_TIME_SLOT_SIZE) {
        return -1;
    }
    s_slotCount = size / WG_TIME_SLOT_SIZE;

    scan = ring_scan(&stored, &seq, &nextIdx);
    if (scan < 0) {
        /* No flash: the counter still rises within this boot from the build
         * epoch, but two boots of the same image emit overlapping timestamps,
         * so the hub's peer state may need clearing after a reset. */
        return -1;
    }

    s_seq     = seq;
    s_slotIdx = nextIdx;

    /* Jump past anything the previous boot could have emitted before it died
     * (worst case: persisted value + one full persist interval), and never
     * start below the build epoch. */
    stored += WG_TIME_BOOT_BUMP_S;
    if (stored < (uint32_t)WG_TIME_BUILD_EPOCH) {
        stored = (uint32_t)WG_TIME_BUILD_EPOCH;
    }
    rebase(stored);

    if (ring_append(s_baseSeconds) != 0) {
        return -2;
    }

    s_flashOk = 1u;
    return 0;
}

uint32_t WgTime_Now(void)
{
    uint32_t base;
    uint32_t tick;

    taskENTER_CRITICAL();
    base = s_baseSeconds;
    tick = s_baseTick;
    taskEXIT_CRITICAL();

    return base + ((HAL_GetTick() - tick) / 1000u);
}

void WgTime_Tick(void)
{
    uint32_t now;

    if (!s_flashOk) {
        return;
    }

    now = WgTime_Now();
    if ((now - s_persisted) < WG_TIME_PERSIST_S) {
        return;
    }

    if (ring_append(now) == 0) {
        rebase(now);
    }
}

int WgTime_SetIfNewer(uint32_t seconds)
{
    if (seconds <= WgTime_Now()) {
        return 0;
    }

    rebase(seconds);
    if (s_flashOk) {
        (void)ring_append(seconds);
    }
    return 1;
}

void WgTime_GetStatus(uint32_t *now, uint32_t *persisted, int *flashOk)
{
    if (now != NULL) {
        *now = WgTime_Now();
    }
    if (persisted != NULL) {
        *persisted = s_persisted;
    }
    if (flashOk != NULL) {
        *flashOk = (int)s_flashOk;
    }
}
