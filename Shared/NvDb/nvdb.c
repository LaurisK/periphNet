/*
 * nvdb.c
 *
 * The core of nvDb: bounds, the medium primitives, the read and write paths,
 * the collector, and the one place absolute addresses leave the module.
 *
 * See docs/task_nv_db.md.  Layout loading, validation and relocation live in
 * nvdb_layout.c; this file only asks it to bring the directory up.
 */

/* Includes -----------------------------------------------------------------*/
#include "nvdb.h"
#include "nvdb_exceptions.h"
#include "nvdb_internal.h"
#include "nvdb_port.h"

#include <string.h>

/* Private defines ----------------------------------------------------------*/

/* The mark list is bounded and coalescing.  When it is nevertheless full the
 * incoming NvDb_Delete performs its erase inline rather than being dropped or
 * refused — the user that caused the pressure is the one that pays for it,
 * and a delete is never silently lost.  An internal number, tunable without
 * touching the API. */
#define NVDB_MARK_MAX           64u

/* Scratch for the "is this already erased?" content check.  Deliberately
 * small and on the stack: the check runs on the write path and must not
 * contend for the one 4 KB unit buffer, which the caller may already hold. */
#define NVDB_PEEK_CHUNK         256u

/* Private types ------------------------------------------------------------*/

/* A deleted byte range, in USER offsets.  Offsets survive relocation by
 * construction (offset 0x00 stays offset 0x00), so a mark never has to be
 * rewritten when an area moves. */
typedef struct {
    uint32_t        off_bytes;      /* what is still to collect              */
    uint32_t        len_bytes;
    uint32_t        origOff_bytes;  /* what the completion will report       */
    uint32_t        origLen_bytes;
    fNvDbEraseDone  onDone;
    uint8_t         user;
    uint8_t         inUse;
} sNvDbMark;

/* A completion waiting to be delivered.  Callbacks are invoked OUTSIDE the
 * lock so a user may call back into nvDb from one, which means they cannot be
 * fired where they are discovered. */
typedef struct {
    uint32_t        off_bytes;
    uint32_t        len_bytes;
    fNvDbEraseDone  onDone;
    eNvDbUser       user;
    eNvDbRes        result;
} sNvDbDone;

/* Private variables --------------------------------------------------------*/

uint8_t        nvdbUnitBuff[NVDB_UNIT_SIZE];
sNvDbDirRecord nvdbDir;
bool           nvdbInitDone;

static sNvDbMark s_marks[NVDB_MARK_MAX];

/* Bounded by the mark list: every entry queued here is a mark being freed, so
 * the queue can never hold more completions than there are marks. */
static sNvDbDone s_done[NVDB_MARK_MAX];
static uint32_t  s_doneCnt;

/* The users allowed to resolve an absolute address (nvdb_exceptions.h).  A
 * property of the build layout, not of the caller's discipline. */
static const uint8_t s_absoluteAllowed[] = {
    nvdbUser_bootStatus,
    nvdbUser_fwuStored,
    nvdbUser_fwuGolden,
    nvdbUser_imageMeta,
    nvdbUser_crashLog,
};

/* The JSON key each user is known by.  Indexed by eNvDbUser; an entry left
 * NULL is a user with no operator-facing name. */
static const char *const s_userNames[nvdbUser_last] = {
    [nvdbUser_undefined]        = NULL,
    [nvdbUser_nvdbConfig]       = "nvdbConfig",
    [nvdbUser_nvdbWear]         = "nvdbWear",
    [nvdbUser_bootStatus]       = "bootStatus",
    [nvdbUser_fwuStored]        = "fwuStored",
    [nvdbUser_fwuGolden]        = "fwuGolden",
    [nvdbUser_imageMeta]        = "imageMeta",
    [nvdbUser_crashLog]         = "crashLog",
    [nvdbUser_modbusLutA]       = "modbusLutA",
    [nvdbUser_modbusLutB]       = "modbusLutB",
    [nvdbUser_modbusSelector]   = "modbusSelector",
    [nvdbUser_wgTime]           = "wgTime",
    [nvdbUser_wgCfg]            = "wgCfg",
    [nvdbUser_mqttCfg]          = "mqttCfg",
    [nvdbUser_triceUdpCfg]      = "triceUdpCfg",
};

/* Private function prototypes ----------------------------------------------*/

static eNvDbRes ResolveArea(eNvDbUser user, uint32_t *addr_bytes,
                                            uint32_t *size_bytes);
static eNvDbRes CheckAccess(eNvDbUser user, uint32_t offset_bytes,
                            uint32_t len_bytes, uint32_t *abs_bytes);
static bool     ProgrammableInPlace(uint32_t addr_bytes, const uint8_t *buff,
                                    uint32_t len_bytes);
static bool     BuffIsErased(uint32_t len_bytes);
static int32_t  FreeMark(void);
static void     QueueDone(const sNvDbMark *mark, eNvDbRes result);
static void     DrainDone(void);
static void     CollectSpan(eNvDbUser user, uint32_t off_bytes,
                            uint32_t len_bytes);
static eNvDbRes CollectUnit(uint32_t areaAddr, uint32_t off_bytes,
                            uint32_t len_bytes, uint32_t *consumed_bytes);

/* Private functions --------------------------------------------------------*/

/**
 * @brief Look up a user's area in the layout in force.
 * @param  user - the holder of storage
 * @param  addr_bytes - absolute base of the area (may be NULL)
 * @param  size_bytes - area size, 0 when the layout does not place this user
 * @retval nvdbRes_ok on a known id, nvdbRes_badUser otherwise
 * @note A user the layout does not place is NOT an error here — absence and
 *       emptiness are the same state, reported as size 0.
 */
static eNvDbRes ResolveArea(eNvDbUser user, uint32_t *addr_bytes,
                                            uint32_t *size_bytes)
{
    uint32_t idx = (uint32_t)user;

    if (nvdbUser_undefined == user || idx >= (uint32_t)nvdbUser_last) {
        return nvdbRes_badUser;
    }

    if (idx >= nvdbDir.entryCnt) {
        /* Written by an older image that did not know this user yet. */
        if (NULL != addr_bytes) *addr_bytes = 0u;
        if (NULL != size_bytes) *size_bytes = 0u;
        return nvdbRes_ok;
    }

    if (NULL != addr_bytes) *addr_bytes = nvdbDir.entries[idx].addr_bytes;
    if (NULL != size_bytes) *size_bytes = nvdbDir.entries[idx].size_bytes;
    return nvdbRes_ok;
}

/**
 * @brief The bound is the user id: check a request against that user's size.
 * @param  user - the holder of storage
 * @param  offset_bytes - offset in the user's flat 0..size-1 address space
 * @param  len_bytes - length of the request
 * @param  abs_bytes - filled with the absolute address on success
 * @retval nvdbRes_ok, or the reason the request is not permitted
 *            nvdbRes_notInit     - NvDb_Init() has not run
 *            nvdbRes_badUser     - unknown id
 *            nvdbRes_noOperation - len_bytes == 0, nothing was asked for
 *            nvdbRes_outOfBounds - past the end, size-0 users included
 */
static eNvDbRes CheckAccess(eNvDbUser user, uint32_t offset_bytes,
                            uint32_t len_bytes, uint32_t *abs_bytes)
{
    uint32_t base = 0u;
    uint32_t size = 0u;
    eNvDbRes res  = nvdbRes_ok;

    if (!nvdbInitDone) {
        return nvdbRes_notInit;
    }

    res = ResolveArea(user, &base, &size);
    if (nvdbRes_ok != res) {
        return res;
    }

    if (0u == len_bytes) {
        return nvdbRes_noOperation;
    }

    /* Written this way on purpose: offset + len can wrap, and a wrap must
     * read as out of bounds rather than as a tiny request. */
    if (offset_bytes >= size || len_bytes > (size - offset_bytes)) {
        return nvdbRes_outOfBounds;
    }

    *abs_bytes = base + offset_bytes;
    return nvdbRes_ok;
}

/**
 * @brief Find an unused mark slot.
 * @retval the slot index, or -1 when the list is full
 */
static int32_t FreeMark(void)
{
    uint32_t i = 0u;

    for (i = 0u; i < NVDB_MARK_MAX; i++) {
        if (0u == s_marks[i].inUse) {
            return (int32_t)i;
        }
    }
    return -1;
}

/**
 * @brief Queue a mark's completion for delivery once the lock is dropped.
 * @param  mark - the mark being retired
 * @param  result - what to report to the user
 * @retval none
 */
static void QueueDone(const sNvDbMark *mark, eNvDbRes result)
{
    if (NULL == mark->onDone || s_doneCnt >= NVDB_MARK_MAX) {
        return;
    }
    s_done[s_doneCnt].user       = (eNvDbUser)mark->user;
    s_done[s_doneCnt].off_bytes  = mark->origOff_bytes;
    s_done[s_doneCnt].len_bytes  = mark->origLen_bytes;
    s_done[s_doneCnt].onDone     = mark->onDone;
    s_done[s_doneCnt].result     = result;
    s_doneCnt++;
}

/**
 * @brief Deliver queued completions with the lock NOT held.
 * @retval none
 * @note Callers may re-enter nvDb from a callback, which is exactly why this
 *       takes one entry at a time under the lock and calls outside it.
 */
static void DrainDone(void)
{
    for (;;) {
        sNvDbDone entry;

        NvDbPort_Lock();
        if (0u == s_doneCnt) {
            NvDbPort_Unlock();
            return;
        }
        entry = s_done[0];
        s_doneCnt--;
        memmove(&s_done[0], &s_done[1], s_doneCnt * sizeof(s_done[0]));
        NvDbPort_Unlock();

        entry.onDone(entry.user, entry.off_bytes, entry.len_bytes,
                     entry.result);
    }
}

/**
 * @brief Erase one erasable unit's worth of a deleted range, preserving the
 *        bytes inside that unit which were NOT deleted.
 * @param  areaAddr - absolute base of the owning user's area
 * @param  off_bytes - user offset the deleted range starts at
 * @param  len_bytes - how much of it is still to collect
 * @param  consumed_bytes - filled with how much this call dealt with
 * @retval nvdbRes_ok or nvdbRes_flash
 * @note Byte granularity is what makes NvDb_Delete a real operation rather
 *       than a hint: collecting a partially deleted unit costs MORE than
 *       collecting a whole one, and nothing tells the user so (§6).
 */
static eNvDbRes CollectUnit(uint32_t areaAddr, uint32_t off_bytes,
                            uint32_t len_bytes, uint32_t *consumed_bytes)
{
    uint32_t abs      = areaAddr + off_bytes;
    uint32_t unitBase = NVDB_UNIT_BASE(abs);
    uint32_t inUnit   = abs - unitBase;
    uint32_t chunk    = NVDB_UNIT_SIZE - inUnit;
    eNvDbRes res      = nvdbRes_ok;

    if (chunk > len_bytes) {
        chunk = len_bytes;
    }
    *consumed_bytes = chunk;

    NvDbPort_Kick();

    if (0u == inUnit && NVDB_UNIT_SIZE == chunk) {
        return NvDbInt_RawErase(unitBase);
    }

    /* Partial: everything else in this unit belongs to the same user (areas
     * are unit-aligned and no user can reach another's), so preserving it
     * blind is both correct and all nvDb is entitled to know about it. */
    res = NvDbInt_RawRead(unitBase, nvdbUnitBuff, NVDB_UNIT_SIZE);
    if (nvdbRes_ok != res) {
        return res;
    }
    memset(&nvdbUnitBuff[inUnit], NVDB_ERASED_BYTE, chunk);

    res = NvDbInt_RawErase(unitBase);
    if (nvdbRes_ok != res) {
        return res;
    }
    return NvDbInt_RawProgram(unitBase, nvdbUnitBuff, NVDB_UNIT_SIZE);
}

/**
 * @brief Pull the erase of every mark overlapping a span forward, now.
 * @param  user - the holder of storage
 * @param  off_bytes - start of the span, in user offsets
 * @param  len_bytes - length of the span
 * @retval none
 * @note Called with the lock held, from the write path.  A write into space
 *       with a pending delete waits for that erase and then proceeds — it
 *       does NOT wait for a lowest-priority task to reach it, which would
 *       turn a bounded wait into an unbounded one.
 */
static void CollectSpan(eNvDbUser user, uint32_t off_bytes, uint32_t len_bytes)
{
    uint32_t areaAddr = 0u;
    uint32_t areaSize = 0u;
    uint32_t spanEnd  = 0u;
    uint32_t i        = 0u;

    if (nvdbRes_ok != ResolveArea(user, &areaAddr, &areaSize)) {
        return;
    }

    /* Widen to whole erasable units.  Collecting only the bytes the write
     * covers would erase the unit anyway and then leave the rest of it still
     * marked, so the next write into the same unit would erase it a second
     * time — the caller pays one erase either way, and this way it buys the
     * whole unit.  Areas are unit-aligned, so a user offset boundary IS a
     * unit boundary. */
    spanEnd   = NVDB_UNIT_UP(off_bytes + len_bytes);
    off_bytes = NVDB_UNIT_BASE(off_bytes);
    if (spanEnd > areaSize) {
        spanEnd = areaSize;
    }

    for (i = 0u; i < NVDB_MARK_MAX; i++) {
        sNvDbMark *m     = &s_marks[i];
        uint32_t   mEnd  = 0u;
        uint32_t   from  = 0u;
        uint32_t   to    = 0u;
        uint32_t   head  = 0u;

        if (0u == m->inUse || (uint8_t)user != m->user) {
            continue;
        }
        mEnd = m->off_bytes + m->len_bytes;
        if (mEnd <= off_bytes || m->off_bytes >= spanEnd) {
            continue;
        }

        /* Collect the intersection only.  Taking the whole mark would drag a
         * 488 KB wipe onto the write path of the first small write after it,
         * which is precisely what the deferred delete exists to avoid. */
        from = (m->off_bytes > off_bytes) ? m->off_bytes : off_bytes;
        to   = (mEnd < spanEnd) ? mEnd : spanEnd;
        head = from;                 /* the collect loop advances `from`     */

        while (from < to) {
            uint32_t consumed = 0u;
            if (nvdbRes_ok != CollectUnit(areaAddr, from, to - from,
                                          &consumed) || 0u == consumed) {
                break;
            }
            from += consumed;
        }

        if (m->off_bytes >= off_bytes && mEnd <= spanEnd) {
            QueueDone(m, nvdbRes_ok);           /* nothing of it is left     */
            m->inUse = 0u;
        } else if (m->off_bytes >= off_bytes) {
            m->len_bytes = mEnd - to;           /* the head was collected    */
            m->off_bytes = to;
        } else if (mEnd <= spanEnd) {
            /* The tail was collected; what survives is the head the write
             * did not reach.  `from` has walked to the end of the collected
             * range by now, so the length has to come from where the collect
             * STARTED — using `from` here left the mark covering the bytes
             * the write was about to put down, and the collector then erased
             * them. */
            m->len_bytes = head - m->off_bytes;
        } else {
            /* The write landed in the middle of a pending delete, so what is
             * left of it is two ranges.  Splitting needs a second slot; if
             * there is none the tail is collected here instead — more erases
             * on this write, never a lost delete. */
            int32_t slot = FreeMark();

            if (slot >= 0) {
                s_marks[slot].user          = m->user;
                s_marks[slot].off_bytes     = to;
                s_marks[slot].len_bytes     = mEnd - to;
                s_marks[slot].origOff_bytes = to;
                s_marks[slot].origLen_bytes = mEnd - to;
                s_marks[slot].onDone        = m->onDone;
                s_marks[slot].inUse         = 1u;
            } else {
                while (to < mEnd) {
                    uint32_t consumed = 0u;
                    if (nvdbRes_ok != CollectUnit(areaAddr, to, mEnd - to,
                                                  &consumed) || 0u == consumed) {
                        break;
                    }
                    to += consumed;
                }
            }
            m->len_bytes     = off_bytes - m->off_bytes;
            m->origOff_bytes = m->off_bytes;
            m->origLen_bytes = m->len_bytes;
        }

        if (0u != m->inUse && 0u == m->len_bytes) {
            QueueDone(m, nvdbRes_ok);
            m->inUse = 0u;
        }
    }
}

/* Exported functions -------------------------------------------------------*/

/**
 * @brief Read `len_bytes` from the medium with no interpretation.
 * @param  addr_bytes - absolute address
 * @param  buff - destination
 * @param  len_bytes - length
 * @retval nvdbRes_ok, nvdbRes_outOfBounds past the medium, nvdbRes_flash
 */
eNvDbRes NvDbInt_RawRead(uint32_t addr_bytes, void *buff, uint32_t len_bytes)
{
    if (addr_bytes + len_bytes > NVDB_MEDIUM_SIZE) {
        return nvdbRes_outOfBounds;
    }
    if (w25q_ok != W25Q128_Read(addr_bytes, (uint8_t *)buff, len_bytes)) {
        return nvdbRes_flash;
    }
    return nvdbRes_ok;
}

/**
 * @brief Program bytes, splitting at program-page boundaries.
 * @param  addr_bytes - absolute address, assumed already erased where needed
 * @param  buff - source
 * @param  len_bytes - length
 * @retval nvdbRes_ok, nvdbRes_outOfBounds, nvdbRes_flash
 * @note The part wraps a page program at the page boundary instead of
 *       carrying into the next page, so the split is correctness, not
 *       tidiness.
 */
eNvDbRes NvDbInt_RawProgram(uint32_t addr_bytes, const void *buff,
                            uint32_t len_bytes)
{
    const uint8_t *src = (const uint8_t *)buff;

    if (addr_bytes + len_bytes > NVDB_MEDIUM_SIZE) {
        return nvdbRes_outOfBounds;
    }

    while (0u != len_bytes) {
        uint32_t inPage = addr_bytes & (NVDB_PROG_PAGE - 1u);
        uint32_t chunk  = NVDB_PROG_PAGE - inPage;

        if (chunk > len_bytes) {
            chunk = len_bytes;
        }
        if (w25q_ok != W25Q128_WritePage(addr_bytes, src, chunk)) {
            return nvdbRes_flash;
        }
        addr_bytes += chunk;
        src        += chunk;
        len_bytes  -= chunk;
    }
    return nvdbRes_ok;
}

/**
 * @brief Erase the erasable unit containing `addr_bytes`.
 * @param  addr_bytes - absolute address anywhere inside the unit
 * @retval nvdbRes_ok, nvdbRes_outOfBounds, nvdbRes_flash
 */
eNvDbRes NvDbInt_RawErase(uint32_t addr_bytes)
{
    uint32_t base = NVDB_UNIT_BASE(addr_bytes);

    if (base + NVDB_UNIT_SIZE > NVDB_MEDIUM_SIZE) {
        return nvdbRes_outOfBounds;
    }
    NvDbPort_Kick();
    if (w25q_ok != W25Q128_EraseSector(base)) {
        return nvdbRes_flash;
    }
    /* The one place anything is erased, so the one place wear can be counted
     * without asking a single user to cooperate (§4.5). */
    NvDbWear_Count(base);
    return nvdbRes_ok;
}

/**
 * @brief Is every byte of a range still in the erased state?
 * @param  addr_bytes - absolute address
 * @param  len_bytes - length
 * @retval true if nothing has been programmed there
 * @note This reads the STATE of the medium, never a wear counter and never
 *       the meaning of the bytes.  It is what makes append-shaped use cheap,
 *       and what a user buys by deleting ahead of time.
 */
bool NvDbInt_RangeErased(uint32_t addr_bytes, uint32_t len_bytes)
{
    uint8_t peek[NVDB_PEEK_CHUNK];

    while (0u != len_bytes) {
        uint32_t chunk = (len_bytes > NVDB_PEEK_CHUNK) ? NVDB_PEEK_CHUNK
                                                       : len_bytes;
        uint32_t i     = 0u;

        if (nvdbRes_ok != NvDbInt_RawRead(addr_bytes, peek, chunk)) {
            return false;
        }
        for (i = 0u; i < chunk; i++) {
            if (NVDB_ERASED_BYTE != peek[i]) {
                return false;
            }
        }
        addr_bytes += chunk;
        len_bytes  -= chunk;
    }
    return true;
}

/**
 * @brief Can these bytes be programmed onto what is already there?
 * @param  addr_bytes - absolute address
 * @param  buff - the bytes the user wants there
 * @param  len_bytes - length
 * @retval true if every bit the user wants clear is clear or can be cleared,
 *            and no bit that is already clear needs to be set again
 * @note The generalisation of the erased check, and it exists for the same
 *       reason: to skip an erase that is not needed.  A user updating a flag
 *       word by clearing one more bit is asking for something the medium can
 *       do in place, and doing it in place is not merely cheaper — it is
 *       ATOMIC, where erase-and-write-back is not.  The boot status and the
 *       Modbus selector both depend on that.
 * @note Still blind.  It compares bit patterns and knows nothing about what
 *       any of them mean; "already erased" is simply its special case where
 *       every current bit is set.
 */
static bool ProgrammableInPlace(uint32_t addr_bytes, const uint8_t *buff,
                                uint32_t len_bytes)
{
    uint8_t peek[NVDB_PEEK_CHUNK];

    while (0u != len_bytes) {
        uint32_t chunk = (len_bytes > NVDB_PEEK_CHUNK) ? NVDB_PEEK_CHUNK
                                                       : len_bytes;
        uint32_t i     = 0u;

        if (nvdbRes_ok != NvDbInt_RawRead(addr_bytes, peek, chunk)) {
            return false;
        }
        for (i = 0u; i < chunk; i++) {
            /* Programming can only clear bits, so the wanted byte must be
             * reachable from the current one: (current & wanted) == wanted. */
            if ((uint8_t)(peek[i] & buff[i]) != buff[i]) {
                return false;
            }
        }
        addr_bytes += chunk;
        buff       += chunk;
        len_bytes  -= chunk;
    }
    return true;
}

/**
 * @brief The write path: put bytes down, erasing only where that is needed.
 * @param  addr_bytes - absolute address
 * @param  buff - source
 * @param  len_bytes - length
 * @retval nvdbRes_ok, nvdbRes_flash, nvdbRes_outOfBounds
 * @note Every write reads its target first.  If the bytes can be programmed
 *       onto what is already there — which includes, but is not limited to,
 *       an erased target — they are, and no erase happens at all.  Otherwise
 *       the unit is buffered, patched, erased and written back: blind,
 *       without nvDb knowing what any of those bytes are.
 */
eNvDbRes NvDbInt_PutBytes(uint32_t addr_bytes, const void *buff,
                          uint32_t len_bytes)
{
    const uint8_t *src = (const uint8_t *)buff;
    eNvDbRes       res = nvdbRes_ok;

    while (0u != len_bytes) {
        uint32_t unitBase = NVDB_UNIT_BASE(addr_bytes);
        uint32_t inUnit   = addr_bytes - unitBase;
        uint32_t chunk    = NVDB_UNIT_SIZE - inUnit;

        if (chunk > len_bytes) {
            chunk = len_bytes;
        }

        if (ProgrammableInPlace(addr_bytes, src, chunk)) {
            res = NvDbInt_RawProgram(addr_bytes, src, chunk);
        } else {
            res = NvDbInt_RawRead(unitBase, nvdbUnitBuff, NVDB_UNIT_SIZE);
            if (nvdbRes_ok == res) {
                memcpy(&nvdbUnitBuff[inUnit], src, chunk);
                res = NvDbInt_RawErase(unitBase);
            }
            if (nvdbRes_ok == res) {
                res = NvDbInt_RawProgram(unitBase, nvdbUnitBuff,
                                         NVDB_UNIT_SIZE);
            }
        }

        if (nvdbRes_ok != res) {
            return res;
        }
        addr_bytes += chunk;
        src        += chunk;
        len_bytes  -= chunk;
    }
    return nvdbRes_ok;
}

/**
 * @brief Is the staging buffer's first `len_bytes` entirely erased-valued?
 * @param  len_bytes - how much of it to look at
 * @retval true when programming it would change nothing
 */
static bool BuffIsErased(uint32_t len_bytes)
{
    uint32_t i = 0u;

    for (i = 0u; i < len_bytes; i++) {
        if (NVDB_ERASED_BYTE != nvdbUnitBuff[i]) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Move opaque bytes from one place on the medium to another.
 * @param  src_bytes - absolute source, unit-aligned
 * @param  dst_bytes - absolute destination, unit-aligned
 * @param  len_bytes - how much to carry across
 * @retval nvdbRes_ok, nvdbRes_flash, nvdbRes_outOfBounds
 * @note Overlap is safe because both ends are unit-aligned: the shift is then
 *       a whole number of units, so walking away from the overlap never
 *       erases a unit that has not been copied yet.
 */
eNvDbRes NvDbInt_CopyRange(uint32_t src_bytes, uint32_t dst_bytes,
                           uint32_t len_bytes)
{
    uint32_t units = (len_bytes + NVDB_UNIT_SIZE - 1u) / NVDB_UNIT_SIZE;
    uint32_t n     = 0u;

    if (src_bytes == dst_bytes || 0u == len_bytes) {
        return nvdbRes_ok;
    }

    for (n = 0u; n < units; n++) {
        uint32_t idx    = (dst_bytes < src_bytes) ? n : (units - 1u - n);
        uint32_t off    = idx * NVDB_UNIT_SIZE;
        uint32_t chunk  = len_bytes - off;
        eNvDbRes res    = nvdbRes_ok;

        if (chunk > NVDB_UNIT_SIZE) {
            chunk = NVDB_UNIT_SIZE;
        }

        NvDbPort_Kick();

        res = NvDbInt_RawRead(src_bytes + off, nvdbUnitBuff, chunk);

        /* A relayout destination is very often erased already — a fresh
         * board, a tail a previous step cleared, or free space picked for a
         * detour.  Moving the two blob areas is 244 units; erasing the ones
         * that need it is the difference between eleven seconds of wear and
         * none. */
        if (nvdbRes_ok == res &&
            !NvDbInt_RangeErased(dst_bytes + off, chunk)) {
            res = NvDbInt_RawErase(dst_bytes + off);
        }
        if (nvdbRes_ok == res && !BuffIsErased(chunk)) {
            res = NvDbInt_RawProgram(dst_bytes + off, nvdbUnitBuff, chunk);
        }
        if (nvdbRes_ok != res) {
            return res;
        }
    }
    return nvdbRes_ok;
}

/**
 * @brief Observed occupancy of an area, at erasable-unit granularity.
 * @param  addr_bytes - absolute base of the area
 * @param  size_bytes - area size
 * @retval bytes up to and including the last unit that holds anything
 * @note Occupancy is OBSERVED, never declared: nvDb never asks what a user
 *       considers meaningful and never infers meaning from fill.  Trailing
 *       bytes a user legitimately wrote as the erased value are
 *       indistinguishable from never-written space, so a shrink may truncate
 *       them — a user that cares writes a marker or declares a bigger area.
 */
uint32_t NvDbInt_OccupiedBytes(uint32_t addr_bytes, uint32_t size_bytes)
{
    uint32_t units = size_bytes / NVDB_UNIT_SIZE;
    uint32_t i     = 0u;

    for (i = units; i > 0u; i--) {
        NvDbPort_Kick();
        if (!NvDbInt_RangeErased(addr_bytes + (i - 1u) * NVDB_UNIT_SIZE,
                                 NVDB_UNIT_SIZE)) {
            return i * NVDB_UNIT_SIZE;
        }
    }
    return 0u;
}

/**
 * @brief Bring nvDb up: load the layout, apply whatever is due, open access.
 * @retval nvdbRes_ok once a layout is in force, nvdbRes_flash if the medium
 *            could not be brought up at all
 * @note Requires W25Q128_Init(), which runs in defaultTask — a user
 *       initialising from an earlier task sees nvdbRes_notInit.  Boot always
 *       succeeds: a refused layout leaves the layout in force unchanged and
 *       says so through NvDb_GetStatus().
 */
eNvDbRes NvDb_Init(void)
{
    eNvDbRes res = nvdbRes_ok;

    NvDbPort_Lock();

    memset(s_marks, 0, sizeof(s_marks));
    s_doneCnt    = 0u;
    nvdbInitDone = false;

    /* Wear counters come up BEFORE the layout does.  The wear area is
     * pinned, so it needs no directory to be found — and bringing it up
     * second would leave first adoption and a whole relayout uncounted,
     * which are the erase-heaviest things this board ever does. */
    NvDbWear_Init();

    res = NvDbLayout_Bringup();
    /* nvdbRes_refused means a NEW layout was rejected and the previous one
     * still stands, which is a perfectly usable board.  Access opens only on
     * a directory that validates, though — never on one nvDb could not make
     * sense of. */
    if ((nvdbRes_ok == res || nvdbRes_refused == res) &&
        nvdbRes_ok == NvDbLayout_Check()) {
        nvdbInitDone = true;
    }

    NvDbPort_Unlock();
    return res;
}

/**
 * @brief How much space this user holds.
 * @param  user - the holder of storage
 * @param  size_bytes - filled with the area size; 0 means the layout in force
 *            does not place this user
 * @retval nvdbRes_ok, nvdbRes_notInit, nvdbRes_badUser
 * @note This is how a user discovers it has no space without having to fail
 *       an access to learn it.
 */
eNvDbRes NvDb_GetSize(eNvDbUser user, uint32_t *size_bytes)
{
    eNvDbRes res = nvdbRes_ok;

    if (NULL == size_bytes) {
        return nvdbRes_badUser;
    }
    if (!nvdbInitDone) {
        return nvdbRes_notInit;
    }

    NvDbPort_Lock();
    res = ResolveArea(user, NULL, size_bytes);
    NvDbPort_Unlock();
    return res;
}

/**
 * @brief Read from a user's own flat address space.
 * @param  user - the holder of storage
 * @param  buff - destination
 * @param  offset_bytes - offset inside the user's area
 * @param  len_bytes - length
 * @retval nvdbRes_ok, nvdbRes_notInit, nvdbRes_badUser, nvdbRes_noOperation,
 *            nvdbRes_outOfBounds, nvdbRes_flash
 * @note BLOCKS, possibly for seconds: a read can wait behind an erase in
 *       progress, so a latency-sensitive caller must treat this as a blocking
 *       call and not as a memory access.  A read inside bounds always
 *       succeeds; never-written space reads as the erased value, and telling
 *       "mine" from "never written" is the user's own job.
 */
eNvDbRes NvDb_Read(eNvDbUser user, void *buff, uint32_t offset_bytes,
                   uint32_t len_bytes)
{
    uint32_t abs = 0u;
    eNvDbRes res = nvdbRes_ok;

    if (NULL == buff) {
        return nvdbRes_badUser;
    }

    NvDbPort_Lock();
    res = CheckAccess(user, offset_bytes, len_bytes, &abs);
    if (nvdbRes_ok == res) {
        res = NvDbInt_RawRead(abs, buff, len_bytes);
    }
    NvDbPort_Unlock();
    return res;
}

/**
 * @brief Write into a user's own flat address space.
 * @param  user - the holder of storage
 * @param  buff - source
 * @param  offset_bytes - offset inside the user's area
 * @param  len_bytes - length
 * @retval nvdbRes_ok, nvdbRes_notInit, nvdbRes_badUser, nvdbRes_noOperation,
 *            nvdbRes_outOfBounds, nvdbRes_flash
 * @note BLOCKS, possibly for seconds.  Returns when the bytes are on the
 *       medium — there is no mirror and no commit, so the store-without-flush
 *       defect cannot occur inside nvDb.  A write into space with a pending
 *       delete pulls that erase forward first.
 * @note A reboot during an overwrite loses that area's erasable unit.  nvDb
 *       guarantees isolation BETWEEN users, never durability of a user's own
 *       overwrite; writes into never-written or deleted space carry no such
 *       risk because they need no erase.
 */
eNvDbRes NvDb_Write(eNvDbUser user, const void *buff, uint32_t offset_bytes,
                    uint32_t len_bytes)
{
    uint32_t abs = 0u;
    eNvDbRes res = nvdbRes_ok;

    if (NULL == buff) {
        return nvdbRes_badUser;
    }

    NvDbPort_Lock();
    res = CheckAccess(user, offset_bytes, len_bytes, &abs);
    if (nvdbRes_ok == res) {
        CollectSpan(user, offset_bytes, len_bytes);
        res = NvDbInt_PutBytes(abs, buff, len_bytes);
    }
    NvDbPort_Unlock();

    DrainDone();
    return res;
}

/**
 * @brief Declare that a range of a user's bytes is no longer wanted.
 * @param  user - the holder of storage
 * @param  offset_bytes - offset inside the user's area
 * @param  len_bytes - length
 * @param  onDone - fired when those bytes are actually gone; NULL if the user
 *            does not care, which is most of them
 * @retval nvdbRes_ok, nvdbRes_notInit, nvdbRes_badUser, nvdbRes_noOperation,
 *            nvdbRes_outOfBounds, nvdbRes_flash
 * @note Returns immediately, EXCEPT when the mark list is full, in which case
 *       it performs the erase before returning and onDone fires before the
 *       call returns.  A delete is never silently dropped.
 * @note The marks live in RAM: a reset before the collector reaches them
 *       loses the request and those bytes are still readable afterwards.  A
 *       user that requires bytes to be gone before a call returns has no call
 *       that provides it.
 */
eNvDbRes NvDb_Delete(eNvDbUser user, uint32_t offset_bytes, uint32_t len_bytes,
                     fNvDbEraseDone onDone)
{
    uint32_t abs  = 0u;
    eNvDbRes res  = nvdbRes_ok;
    uint32_t i    = 0u;
    int32_t  slot = -1;

    NvDbPort_Lock();

    res = CheckAccess(user, offset_bytes, len_bytes, &abs);
    if (nvdbRes_ok != res) {
        NvDbPort_Unlock();
        return res;
    }

    /* Adjacent and overlapping ranges merge, but only when they would report
     * to the same place: a completion belongs to the request that asked for
     * it, and merging two different ones would silently retarget it. */
    for (i = 0u; i < NVDB_MARK_MAX; i++) {
        sNvDbMark *m    = &s_marks[i];
        uint32_t   mEnd = 0u;

        if (0u == m->inUse || (uint8_t)user != m->user || onDone != m->onDone) {
            if (0u == m->inUse && slot < 0) {
                slot = (int32_t)i;
            }
            continue;
        }
        mEnd = m->off_bytes + m->len_bytes;
        if (mEnd < offset_bytes || m->off_bytes > offset_bytes + len_bytes) {
            continue;
        }
        if (m->off_bytes > offset_bytes) {
            m->off_bytes = offset_bytes;
        }
        if (mEnd < offset_bytes + len_bytes) {
            mEnd = offset_bytes + len_bytes;
        }
        m->len_bytes     = mEnd - m->off_bytes;
        m->origOff_bytes = m->off_bytes;
        m->origLen_bytes = m->len_bytes;
        NvDbPort_CollectorNotify();
        NvDbPort_Unlock();
        return nvdbRes_ok;
    }

    if (slot >= 0) {
        sNvDbMark *m     = &s_marks[slot];
        m->user          = (uint8_t)user;
        m->off_bytes     = offset_bytes;
        m->len_bytes     = len_bytes;
        m->origOff_bytes = offset_bytes;
        m->origLen_bytes = len_bytes;
        m->onDone        = onDone;
        m->inUse         = 1u;
        NvDbPort_CollectorNotify();
        NvDbPort_Unlock();
        return nvdbRes_ok;
    }

    /* The list is full.  The user that caused the pressure pays for it. */
    {
        uint32_t   areaAddr = 0u;
        uint32_t   from     = offset_bytes;
        uint32_t   to       = offset_bytes + len_bytes;
        sNvDbMark  inline_m;

        (void)ResolveArea(user, &areaAddr, NULL);
        while (from < to && nvdbRes_ok == res) {
            uint32_t consumed = 0u;
            res = CollectUnit(areaAddr, from, to - from, &consumed);
            if (0u == consumed) {
                break;
            }
            from += consumed;
        }

        inline_m.user          = (uint8_t)user;
        inline_m.off_bytes     = offset_bytes;
        inline_m.len_bytes     = len_bytes;
        inline_m.origOff_bytes = offset_bytes;
        inline_m.origLen_bytes = len_bytes;
        inline_m.onDone        = onDone;
        inline_m.inUse         = 1u;
        QueueDone(&inline_m, res);
    }

    NvDbPort_Unlock();
    DrainDone();
    return res;
}

/**
 * @brief Declare a user's whole area no longer wanted.
 * @param  user - the holder of storage
 * @param  onDone - fired when the area is actually erased, or NULL
 * @retval nvdbRes_ok, nvdbRes_notInit, nvdbRes_badUser, nvdbRes_outOfBounds
 *            on a user with no space, nvdbRes_flash
 * @note Collapses that user's outstanding marks into this one.  A superseded
 *       delete's callback does NOT fire — nvDb reports that an erase
 *       happened, it never promises that one will, and the bytes those marks
 *       covered are inside the wipe regardless.
 */
eNvDbRes NvDb_Wipe(eNvDbUser user, fNvDbEraseDone onDone)
{
    uint32_t size = 0u;
    eNvDbRes res  = nvdbRes_ok;

    if (!nvdbInitDone) {
        return nvdbRes_notInit;
    }

    NvDbPort_Lock();
    res = ResolveArea(user, NULL, &size);
    if (nvdbRes_ok == res) {
        NvDbInt_DropMarks(user);
    }
    NvDbPort_Unlock();

    if (nvdbRes_ok != res) {
        return res;
    }
    if (0u == size) {
        return nvdbRes_outOfBounds;
    }
    return NvDb_Delete(user, 0u, size, onDone);
}

/**
 * @brief Forget every outstanding mark belonging to a user.
 * @param  user - the holder of storage
 * @retval none
 * @note Called with the lock held.
 */
void NvDbInt_DropMarks(eNvDbUser user)
{
    uint32_t i = 0u;

    for (i = 0u; i < NVDB_MARK_MAX; i++) {
        if (0u != s_marks[i].inUse && (uint8_t)user == s_marks[i].user) {
            s_marks[i].inUse = 0u;
        }
    }
}

/**
 * @brief Erase one erasable unit's worth of deleted space.
 * @retval true if work was done and there may be more, false if nothing was
 *            pending
 * @note One unit per lock acquisition, so a waiting writer gets in between
 *       units.  Deleted space is erased in the background so that erases stay
 *       off the write path — that, not reclaiming freed areas, is the whole
 *       reason this exists.
 */
bool NvDb_CollectStep(void)
{
    uint32_t i        = 0u;
    bool     didWork  = false;

    NvDbPort_Lock();

    for (i = 0u; i < NVDB_MARK_MAX; i++) {
        sNvDbMark *m        = &s_marks[i];
        uint32_t   areaAddr = 0u;
        uint32_t   consumed = 0u;
        eNvDbRes   res      = nvdbRes_ok;

        if (0u == m->inUse) {
            continue;
        }
        if (nvdbRes_ok != ResolveArea((eNvDbUser)m->user, &areaAddr, NULL)) {
            m->inUse = 0u;
            continue;
        }

        res = CollectUnit(areaAddr, m->off_bytes, m->len_bytes, &consumed);
        didWork = true;

        if (nvdbRes_ok != res) {
            QueueDone(m, res);
            m->inUse = 0u;
            break;
        }

        m->off_bytes += consumed;
        m->len_bytes -= consumed;
        if (0u == m->len_bytes) {
            QueueDone(m, nvdbRes_ok);   /* reports the range, not the unit  */
            m->inUse = 0u;
        }
        break;
    }

    /* The collector's context is the only one that may write the counters
     * back: a flush is an erase, and it must not land in the middle of
     * somebody's write.  Whether it is worth doing yet is the wear module's
     * call, and it is deliberately in no hurry. */
    NvDbWear_Flush(!didWork);

    NvDbPort_Unlock();
    DrainDone();
    return didWork;
}

/**
 * @brief Where a user's bytes actually are — crash handler and FWU only.
 * @param  user - the holder of storage
 * @param  addr_bytes - filled with the absolute base
 * @param  size_bytes - filled with the area size
 * @retval nvdbRes_ok, nvdbRes_notInit, nvdbRes_badUser for a user the build
 *            layout does not flag for this
 * @note Does not block and is callable from ANY context, fault handlers
 *       included: a RAM lookup with no RTOS call, no mutex and no allocation.
 */
eNvDbRes NvDb_GetAbsoluteAddress(eNvDbUser user, uint32_t *addr_bytes,
                                                 uint32_t *size_bytes)
{
    uint32_t i = 0u;

    if (NULL == addr_bytes || NULL == size_bytes) {
        return nvdbRes_badUser;
    }
    if (!nvdbInitDone) {
        return nvdbRes_notInit;
    }

    for (i = 0u; i < (sizeof(s_absoluteAllowed) / sizeof(s_absoluteAllowed[0]));
         i++) {
        if ((uint8_t)user == s_absoluteAllowed[i]) {
            return ResolveArea(user, addr_bytes, size_bytes);
        }
    }
    return nvdbRes_badUser;
}

/**
 * @brief The JSON key a user is known by.
 * @param  user - the holder of storage
 * @retval a static string, or NULL for an unknown id
 */
const char *NvDb_UserName(eNvDbUser user)
{
    if (nvdbUser_undefined == user || (uint32_t)user >= (uint32_t)nvdbUser_last) {
        return NULL;
    }
    return s_userNames[(uint32_t)user];
}

/**
 * @brief The user a JSON key names.
 * @param  name - NUL-terminated key
 * @retval the id, or nvdbUser_undefined if no user is called that
 */
eNvDbUser NvDb_UserByName(const char *name)
{
    uint32_t i = 0u;

    if (NULL == name) {
        return nvdbUser_undefined;
    }
    for (i = 1u; i < (uint32_t)nvdbUser_last; i++) {
        if (NULL != s_userNames[i] && 0 == strcmp(s_userNames[i], name)) {
            return (eNvDbUser)i;
        }
    }
    return nvdbUser_undefined;
}
