/*
 * nvdb_layout.c
 *
 * The layout: the directory on the medium, the two built-in layouts, the
 * structural check, and relocation.
 *
 * A layout is loaded and validated at NvDb_Init(), and only there.  A new one
 * may be SUPPLIED at any time; it is APPLIED at the next init.  Validation
 * runs on every init regardless of whether anything changed, and it is
 * structural only — never about content (Rule 3).
 *
 * See docs/task_nv_db.md §4.3, §4.4, §4.4.1.
 */

/* Includes -----------------------------------------------------------------*/
#include "nvdb_internal.h"
#include "nvdb_port.h"

#include "bl_app_contract.h"
#include "image_mgmt.h"

#include <string.h>

/* Private defines ----------------------------------------------------------*/

/* The name the first adoption records, so an operator can see on the status
 * that the board came from the pre-nvDb map. */
#define NVDB_LEGACY_NAME        "legacy"

#define NVDB_UNITS_FOR(len)     (((len) + NVDB_UNIT_SIZE - 1u) / NVDB_UNIT_SIZE)

/* Every on-medium record is written with one program pass and must fit the
 * role it was given a sector for.  Checked here rather than reviewed. */
_Static_assert(sizeof(sNvDbDirRecord) <= NVDB_PROG_PAGE,
               "directory record must fit one program page");
_Static_assert(sizeof(sNvDbStageRecord) <= NVDB_PROG_PAGE,
               "staged layout record must fit one program page");
_Static_assert(NVDB_JRN_PLAN_OFF + sizeof(sNvDbJournal) <= NVDB_JRN_TARGET_OFF,
               "journal plan overruns the record that follows it");
_Static_assert(NVDB_JRN_TARGET_OFF + sizeof(sNvDbDirRecord) <= NVDB_JRN_DONE_OFF,
               "journal target directory overruns the done marks");
_Static_assert(NVDB_JRN_DONE_OFF + NVDB_JRN_MAX_STEPS <= NVDB_UNIT_SIZE,
               "journal done marks overrun the journal unit");
_Static_assert((uint32_t)nvdbUser_last <= NVDB_MAX_ENTRIES,
               "more users than the on-medium records can hold");
_Static_assert(NVDB_CONFIG_ADDR + NVDB_CONFIG_SIZE == NVDB_WEAR_ADDR,
               "nvDb's own areas must be adjacent and unit-aligned");
_Static_assert(NVDB_WEAR_ADDR + NVDB_WEAR_SIZE <= NVDB_MEDIUM_SIZE,
               "nvDb's own areas must fit the medium");

/* Private types ------------------------------------------------------------*/

typedef struct {
    sNvDbMoveStep   steps[NVDB_JRN_MAX_STEPS];
    uint16_t        stepCnt;
} sNvDbPlan;

/* One user's part in a relayout, while the plan is being worked out. */
typedef struct {
    uint32_t    liveAddr_bytes;     /* where the bytes are RIGHT NOW         */
    uint32_t    dstAddr_bytes;      /* where the new layout wants them       */
    uint32_t    copyLen_bytes;      /* how much is worth carrying across     */
    uint8_t     pending;            /* still to be placed                    */
    uint8_t     detoured;           /* already parked in free space once     */
} sNvDbMove;

/* Private variables --------------------------------------------------------*/

/* The layout this image ships.  Sizes only — placement is nvDb's business,
 * which is what lets a layout be authored without knowing an address.
 *
 * It is chosen to land every existing user on the address it already had,
 * because ten modules still reach the medium themselves (docs/task_nv_db.md
 * §5) and moving their bytes out from under them would break a board that is
 * otherwise working.  nvDb is the authority here before it is the only
 * caller; the compacting layout ships once the last direct caller is gone,
 * as an ordinary layout change with a higher version.
 *
 * That is why imageMeta is 12 KB: the hand-assigned map left a 8 KB hole
 * after it, and giving that hole to the user in front of it reproduces the
 * old addresses exactly without teaching the packer to leave holes.  The
 * space was wasted before and is merely named now. */
static const uint32_t s_targetSizes[nvdbUser_last] = {
    [nvdbUser_nvdbConfig]       = NVDB_CONFIG_SIZE,
    [nvdbUser_nvdbWear]         = NVDB_WEAR_SIZE,
    [nvdbUser_bootStatus]       = EXT_FLASH_FWU_STATUS_SIZE,
    [nvdbUser_fwuStored]        = EXT_FLASH_FWU_IMG_SIZE,
    [nvdbUser_fwuGolden]        = EXT_FLASH_GOLDEN_IMG_SIZE,
    [nvdbUser_imageMeta]        = (EXT_FLASH_CRASH_LOG_ADDR -
                                   EXT_FLASH_IMG_META_ADDR),
    [nvdbUser_crashLog]         = EXT_FLASH_CRASH_LOG_SIZE,
    [nvdbUser_modbusLutA]       = EXT_FLASH_MODBUS_LUT_SIZE,
    [nvdbUser_modbusLutB]       = EXT_FLASH_MODBUS_LUT_SIZE,
    [nvdbUser_modbusSelector]   = EXT_FLASH_MODBUS_SEL_SIZE,
    [nvdbUser_wgTime]           = EXT_FLASH_WG_TIME_SIZE,
    [nvdbUser_wgCfg]            = EXT_FLASH_WG_CFG_SIZE,
    [nvdbUser_mqttCfg]          = 0x1000u,
    [nvdbUser_triceUdpCfg]      = 0x1000u,
    [nvdbUser_packCfg]          = 0x1000u,   /*  4 KB, rarely written */
    [nvdbUser_packState]        = 0x4000u,   /* 16 KB, reserved       */
};

/* The assumed-current layout (§4.4.1): the board's existing hand-assigned map
 * written out as ordinary users, addresses and all.  This one cannot come
 * from sizes — the old map had a hole in it, and reproducing a hole is not
 * something a packer should ever learn to do.
 *
 * At init, finding no directory, nvDb adopts this as the state of the world
 * and then performs an ordinary relayout to the target.  Existing data is
 * carried across by the same mechanism that serves every later layout change,
 * and on the next boot the directory already records the target, so the "did
 * it already run" question answers itself.
 *
 * It can only be dropped from the image once no board can still be running
 * the pre-nvDb map: a board that skips this image and lands on a later one
 * would find no directory and adopt whatever THAT image assumes. */
static const sNvDbDirEntry s_legacyMap[NVDB_MAX_ENTRIES] = {
    [nvdbUser_nvdbConfig]       = { NVDB_CONFIG_ADDR,             NVDB_CONFIG_SIZE            },
    [nvdbUser_nvdbWear]         = { NVDB_WEAR_ADDR,               NVDB_WEAR_SIZE              },
    [nvdbUser_bootStatus]       = { EXT_FLASH_FWU_STATUS_ADDR,    EXT_FLASH_FWU_STATUS_SIZE   },
    [nvdbUser_fwuStored]        = { EXT_FLASH_FWU_IMG_ADDR,       EXT_FLASH_FWU_IMG_SIZE      },
    [nvdbUser_fwuGolden]        = { EXT_FLASH_GOLDEN_IMG_ADDR,    EXT_FLASH_GOLDEN_IMG_SIZE   },
    [nvdbUser_imageMeta]        = { EXT_FLASH_IMG_META_ADDR,      EXT_FLASH_IMG_META_SIZE     },
    [nvdbUser_crashLog]         = { EXT_FLASH_CRASH_LOG_ADDR,     EXT_FLASH_CRASH_LOG_SIZE    },
    [nvdbUser_modbusLutA]       = { EXT_FLASH_MODBUS_LUT_A_ADDR,  EXT_FLASH_MODBUS_LUT_SIZE   },
    [nvdbUser_modbusLutB]       = { EXT_FLASH_MODBUS_LUT_B_ADDR,  EXT_FLASH_MODBUS_LUT_SIZE   },
    [nvdbUser_modbusSelector]   = { EXT_FLASH_MODBUS_SEL_ADDR,    EXT_FLASH_MODBUS_SEL_SIZE   },
    [nvdbUser_wgTime]           = { EXT_FLASH_WG_TIME_ADDR,       EXT_FLASH_WG_TIME_SIZE      },
    [nvdbUser_wgCfg]            = { EXT_FLASH_WG_CFG_ADDR,        EXT_FLASH_WG_CFG_SIZE       },
};

/* Which directory slot the record in force came from; the next write goes to
 * the other one, so a power cut can never destroy both. */
static uint32_t s_dirSlotOff;

static sNvDbMove s_moves[nvdbUser_last];
static uint8_t   s_busyUnits[(NVDB_UNIT_CNT + 7u) / 8u];

/* Scratch for the layout machinery.  Static for the same reason nvdbUnitBuff,
 * s_moves and s_busyUnits are: every one of them is touched only under the
 * nvDb lock, and every one is far too big for the stack of the two tasks that
 * reach this code.  defaultTask has 4 KB and the http task 4 KB; the
 * bring-up chain alone wanted 5.4 KB of frames, which is a guaranteed
 * overflow on the FIRST boot of every board — before the directory commits,
 * so it would have repeated forever. */
static sNvDbDirRecord   s_dirA;
static sNvDbDirRecord   s_dirB;
static sNvDbDirRecord   s_target;
static sNvDbStageRecord s_stage;
static sNvDbPlan        s_plan;
static sNvDbJournal     s_journal;
static sNvDbDirEntry    s_placed[NVDB_MAX_ENTRIES];

/* Private function prototypes ----------------------------------------------*/

static uint32_t CfgAddr(uint32_t off_bytes);
static uint32_t DirCrc(const sNvDbDirRecord *rec);
static int      DirRead(uint32_t off_bytes, sNvDbDirRecord *rec);
static eNvDbRes DirWrite(uint32_t off_bytes, sNvDbDirRecord *rec);
static eNvDbRes ValidateEntries(const sNvDbDirEntry *entries, uint16_t cnt);
static eNvDbRes Place(const uint32_t *size_bytes, sNvDbDirEntry *out);
static eNvDbRes PlanRelayout(const sNvDbDirEntry *newEntries,
                             eNvDbApplyMode mode, sNvDbPlan *plan);
static eNvDbRes RunPlan(const sNvDbPlan *plan, uint32_t firstStep);
static eNvDbRes JournalWrite(const sNvDbPlan *plan,
                             const sNvDbDirRecord *target);
static int      JournalRead(sNvDbPlan *plan, sNvDbDirRecord *target);
static void     JournalMarkDone(uint32_t step);
static int      JournalIsDone(uint32_t step);
static eNvDbRes JournalErase(void);
static int      StageRead(sNvDbStageRecord *rec);
static eNvDbRes StageErase(void);
static eNvDbRes ApplyLayout(const char *name, uint16_t version,
                            eNvDbApplyMode mode, const uint32_t *size_bytes,
                            uint8_t operatorOwned);
static void     BusyClear(void);
static void     BusyMark(uint32_t addr_bytes, uint32_t len_bytes);
static int      BusyFind(uint32_t len_bytes, uint32_t *addr_bytes);
static int      BusyOverlaps(uint32_t addr_bytes, uint32_t len_bytes);

/* Private functions --------------------------------------------------------*/

/**
 * @brief Absolute address of an offset inside nvDb's own config area.
 * @param  off_bytes - offset inside the area
 * @retval absolute address
 * @note The config area is the bootstrap and is the one immovable address in
 *       the design: everything else is placed by the layout that lives inside
 *       it, so its own placement cannot come from there.
 */
static uint32_t CfgAddr(uint32_t off_bytes)
{
    return NVDB_CONFIG_ADDR + off_bytes;
}

/**
 * @brief CRC of a directory record, covering every byte after the CRC field.
 * @param  rec - the record
 * @retval the CRC32
 */
static uint32_t DirCrc(const sNvDbDirRecord *rec)
{
    return ImgMgmt_Crc32((const uint8_t *)rec + sizeof(uint32_t),
                         (uint32_t)sizeof(*rec) - (uint32_t)sizeof(uint32_t));
}

/**
 * @brief Read one directory slot and check that it describes a real layout.
 * @param  off_bytes - NVDB_CFG_DIR_A_OFF or NVDB_CFG_DIR_B_OFF
 * @param  rec - filled on success
 * @retval 0 if the slot holds a usable record, -1 otherwise
 * @note The structural check runs HERE, not after a slot has been chosen: a
 *       record that does not describe a layout is not a candidate at all, so
 *       there is no state in which access opens onto a directory nvDb has
 *       already rejected.  Two bad slots mean no directory, which is first
 *       adoption — a working board built on an assumption beats a board that
 *       cannot say where anything is.
 */
static int DirRead(uint32_t off_bytes, sNvDbDirRecord *rec)
{
    if (nvdbRes_ok != NvDbInt_RawRead(CfgAddr(off_bytes), rec, sizeof(*rec))) {
        return -1;
    }
    if (NVDB_DIR_MAGIC != rec->magic ||
        NVDB_RECORD_VER != rec->recordVer ||
        rec->entryCnt > NVDB_MAX_ENTRIES ||
        DirCrc(rec) != rec->crc32) {
        return -1;
    }
    if (nvdbRes_ok != ValidateEntries(rec->entries, rec->entryCnt)) {
        return -1;
    }
    return 0;
}

/**
 * @brief Stamp and write a directory record into one slot.
 * @param  off_bytes - the slot to write
 * @param  rec - the record; its CRC is computed here
 * @retval nvdbRes_ok or nvdbRes_flash
 */
static eNvDbRes DirWrite(uint32_t off_bytes, sNvDbDirRecord *rec)
{
    eNvDbRes res = nvdbRes_ok;

    rec->magic     = NVDB_DIR_MAGIC;
    rec->recordVer = NVDB_RECORD_VER;
    rec->nvdbVer   = NVDB_VERSION;
    rec->crc32     = DirCrc(rec);

    res = NvDbInt_RawErase(CfgAddr(off_bytes));
    if (nvdbRes_ok != res) {
        return res;
    }
    return NvDbInt_RawProgram(CfgAddr(off_bytes), rec, sizeof(*rec));
}

/**
 * @brief The structural check — and it is structural only, never about
 *        content.
 * @param  entries - the placement to check
 * @param  cnt - how many entries it holds
 * @retval nvdbRes_ok, or nvdbRes_refused naming nothing: a refusal changes
 *            nothing, so there is nothing to unwind
 * @note Every area aligned to an erasable unit, no two overlapping, each
 *       inside the medium, and nvDb's own areas where they actually are.  A
 *       forced layout may cost a user its data; it may never cost nvDb its
 *       ability to find anything.
 */
static eNvDbRes ValidateEntries(const sNvDbDirEntry *entries, uint16_t cnt)
{
    uint32_t i = 0u;
    uint32_t j = 0u;

    if (cnt > NVDB_MAX_ENTRIES) {
        return nvdbRes_refused;
    }
    if (0u != entries[nvdbUser_undefined].size_bytes) {
        return nvdbRes_refused;
    }

    if (cnt <= (uint16_t)nvdbUser_nvdbWear ||
        NVDB_CONFIG_ADDR != entries[nvdbUser_nvdbConfig].addr_bytes ||
        NVDB_CONFIG_SIZE != entries[nvdbUser_nvdbConfig].size_bytes ||
        NVDB_WEAR_ADDR   != entries[nvdbUser_nvdbWear].addr_bytes ||
        NVDB_WEAR_SIZE   != entries[nvdbUser_nvdbWear].size_bytes) {
        return nvdbRes_refused;
    }

    for (i = 1u; i < cnt; i++) {
        uint32_t a = entries[i].addr_bytes;
        uint32_t n = entries[i].size_bytes;

        if (0u == n) {
            continue;
        }
        if (0u != (a & (NVDB_UNIT_SIZE - 1u)) ||
            0u != (n & (NVDB_UNIT_SIZE - 1u))) {
            return nvdbRes_refused;
        }
        if (a > NVDB_MEDIUM_SIZE || n > (NVDB_MEDIUM_SIZE - a)) {
            return nvdbRes_refused;
        }
        for (j = 1u; j < i; j++) {
            uint32_t b = entries[j].addr_bytes;
            uint32_t m = entries[j].size_bytes;

            if (0u == m) {
                continue;
            }
            if (a < (b + m) && b < (a + n)) {
                return nvdbRes_refused;
            }
        }
    }
    return nvdbRes_ok;
}

/**
 * @brief Turn a table of sizes into a placement.
 * @param  size_bytes - one entry per user, indexed by eNvDbUser
 * @param  out - filled with addresses and unit-rounded sizes
 * @retval nvdbRes_ok, or nvdbRes_refused if it does not fit or if the layout
 *            claims one of nvDb's own areas at the wrong size
 * @note Sizes are rounded UP to the erasable unit here rather than rejected,
 *       because alignment is granularity and no granularity is visible in the
 *       surface an operator writes against (C3).
 */
static eNvDbRes Place(const uint32_t *size_bytes, sNvDbDirEntry *out)
{
    uint32_t cursor = 0u;
    uint32_t i      = 0u;

    memset(out, 0, sizeof(sNvDbDirEntry) * NVDB_MAX_ENTRIES);

    /* nvDb's own areas are pinned.  A layout that asks for them at a
     * different size is refused in ANY mode; a layout that does not mention
     * them at all simply gets them, because their size was never the
     * operator's business. */
    if (0u != size_bytes[nvdbUser_nvdbConfig] &&
        NVDB_UNIT_UP(size_bytes[nvdbUser_nvdbConfig]) != NVDB_CONFIG_SIZE) {
        return nvdbRes_refused;
    }
    if (0u != size_bytes[nvdbUser_nvdbWear] &&
        NVDB_UNIT_UP(size_bytes[nvdbUser_nvdbWear]) != NVDB_WEAR_SIZE) {
        return nvdbRes_refused;
    }
    out[nvdbUser_nvdbConfig].addr_bytes = NVDB_CONFIG_ADDR;
    out[nvdbUser_nvdbConfig].size_bytes = NVDB_CONFIG_SIZE;
    out[nvdbUser_nvdbWear].addr_bytes   = NVDB_WEAR_ADDR;
    out[nvdbUser_nvdbWear].size_bytes   = NVDB_WEAR_SIZE;

    for (i = 1u; i < (uint32_t)nvdbUser_last; i++) {
        uint32_t want = 0u;

        if (nvdbUser_nvdbConfig == i || nvdbUser_nvdbWear == i) {
            continue;
        }
        want = NVDB_UNIT_UP(size_bytes[i]);
        if (0u == want) {
            continue;
        }

        /* Step over the pinned block rather than around it: users keep their
         * relative order, which is what keeps relocation cheap. */
        if (cursor < (NVDB_WEAR_ADDR + NVDB_WEAR_SIZE) &&
            (cursor + want) > NVDB_CONFIG_ADDR) {
            cursor = NVDB_WEAR_ADDR + NVDB_WEAR_SIZE;
        }
        if (want > (NVDB_MEDIUM_SIZE - cursor)) {
            return nvdbRes_refused;
        }
        out[i].addr_bytes = cursor;
        out[i].size_bytes = want;
        cursor += want;
    }

    return ValidateEntries(out, (uint16_t)nvdbUser_last);
}

/* ==========================================================================
 * The free-unit map — used only to find somewhere to put a moving area
 * ========================================================================== */

static void BusyClear(void)
{
    memset(s_busyUnits, 0, sizeof(s_busyUnits));
}

static void BusyMark(uint32_t addr_bytes, uint32_t len_bytes)
{
    uint32_t first = addr_bytes / NVDB_UNIT_SIZE;
    uint32_t cnt   = NVDB_UNITS_FOR(len_bytes);
    uint32_t i     = 0u;

    for (i = 0u; i < cnt && (first + i) < NVDB_UNIT_CNT; i++) {
        s_busyUnits[(first + i) / 8u] |= (uint8_t)(1u << ((first + i) % 8u));
    }
}

static int BusyOverlaps(uint32_t addr_bytes, uint32_t len_bytes)
{
    uint32_t first = addr_bytes / NVDB_UNIT_SIZE;
    uint32_t cnt   = NVDB_UNITS_FOR(len_bytes);
    uint32_t i     = 0u;

    for (i = 0u; i < cnt && (first + i) < NVDB_UNIT_CNT; i++) {
        if (0u != (s_busyUnits[(first + i) / 8u] &
                   (uint8_t)(1u << ((first + i) % 8u)))) {
            return 1;
        }
    }
    return 0;
}

static int BusyFind(uint32_t len_bytes, uint32_t *addr_bytes)
{
    uint32_t need = NVDB_UNITS_FOR(len_bytes);
    uint32_t run  = 0u;
    uint32_t i    = 0u;

    for (i = 0u; i < NVDB_UNIT_CNT; i++) {
        if (0u != (s_busyUnits[i / 8u] & (uint8_t)(1u << (i % 8u)))) {
            run = 0u;
            continue;
        }
        run++;
        if (run >= need) {
            *addr_bytes = (i + 1u - need) * NVDB_UNIT_SIZE;
            return 0;
        }
    }
    return -1;
}

/**
 * @brief Work out what has to be carried where, before anything is touched.
 * @param  newEntries - the placement being adopted
 * @param  mode - normal refuses a shrink below a user's occupied extent;
 *            forced truncates it
 * @param  plan - filled with the steps to run
 * @retval nvdbRes_ok, or nvdbRes_refused
 * @note Validation happens BEFORE adoption, never during it, which is what
 *       makes refusal safe: the layout in force stays in force and every user
 *       keeps the area it already had.  Refusal is never a route to data
 *       loss.
 */
static eNvDbRes PlanRelayout(const sNvDbDirEntry *newEntries,
                             eNvDbApplyMode mode, sNvDbPlan *plan)
{
    uint32_t i      = 0u;
    uint32_t guard  = 0u;
    uint32_t placed = 0u;
    uint32_t needed = 0u;

    plan->stepCnt = 0u;
    memset(s_moves, 0, sizeof(s_moves));

    for (i = 1u; i < (uint32_t)nvdbUser_last; i++) {
        uint32_t oldAddr  = 0u;
        uint32_t oldSize  = 0u;
        uint32_t occupied = 0u;

        if (i < nvdbDir.entryCnt) {
            oldAddr = nvdbDir.entries[i].addr_bytes;
            oldSize = nvdbDir.entries[i].size_bytes;
        }

        s_moves[i].liveAddr_bytes = oldAddr;
        s_moves[i].dstAddr_bytes  = newEntries[i].addr_bytes;

        if (0u == oldSize || nvdbUser_nvdbConfig == i || nvdbUser_nvdbWear == i) {
            continue;
        }

        occupied = NvDbInt_OccupiedBytes(oldAddr, oldSize);
        if (occupied > newEntries[i].size_bytes) {
            if (nvdbMode_forced != mode) {
                return nvdbRes_refused;
            }
            occupied = newEntries[i].size_bytes;   /* truncated, knowingly   */
        }
        s_moves[i].copyLen_bytes = occupied;
        if (0u != occupied && oldAddr != newEntries[i].addr_bytes) {
            s_moves[i].pending = 1u;
            needed++;
        }
    }

    /* Order the moves.  A user may be placed as soon as its destination does
     * not sit on top of another user's live bytes; when every remaining move
     * is blocked by another, one of them is detoured through free space —
     * which is the whole content of the feasibility rule, that free space is
     * at least as large as the largest area that has to move. */
    for (guard = 0u; placed < needed && guard < (2u * (uint32_t)nvdbUser_last + 4u);
         guard++) {
        uint32_t progress = 0u;

        for (i = 1u; i < (uint32_t)nvdbUser_last; i++) {
            uint32_t j = 0u;
            int      blocked = 0;

            if (0u == s_moves[i].pending) {
                continue;
            }
            BusyClear();
            for (j = 1u; j < (uint32_t)nvdbUser_last; j++) {
                if (j != i && 0u != s_moves[j].pending) {
                    BusyMark(s_moves[j].liveAddr_bytes,
                             s_moves[j].copyLen_bytes);
                }
            }
            blocked = BusyOverlaps(s_moves[i].dstAddr_bytes,
                                   s_moves[i].copyLen_bytes);
            if (0 != blocked) {
                continue;
            }
            if (plan->stepCnt >= NVDB_JRN_MAX_STEPS) {
                return nvdbRes_refused;
            }
            plan->steps[plan->stepCnt].user      = (uint8_t)i;
            plan->steps[plan->stepCnt].src_bytes = s_moves[i].liveAddr_bytes;
            plan->steps[plan->stepCnt].dst_bytes = s_moves[i].dstAddr_bytes;
            plan->steps[plan->stepCnt].len_bytes = s_moves[i].copyLen_bytes;
            plan->stepCnt++;
            s_moves[i].liveAddr_bytes = s_moves[i].dstAddr_bytes;
            s_moves[i].pending        = 0u;
            placed++;
            progress++;
        }

        if (0u == progress && placed < needed) {
            uint32_t victim  = 0u;
            uint32_t scratch = 0u;
            uint32_t j       = 0u;

            /* Park somebody in free space to break the deadlock — but it has
             * to be somebody who is actually IN it.  Taking the lowest-index
             * pending user would keep re-parking a bystander whose own move
             * blocks nobody, burning a free region each round until the guard
             * gives up and a perfectly achievable layout is refused. */
            for (i = 1u; i < (uint32_t)nvdbUser_last; i++) {
                uint32_t k = 0u;

                if (0u == s_moves[i].pending || 0u != s_moves[i].detoured) {
                    continue;
                }
                BusyClear();
                BusyMark(s_moves[i].liveAddr_bytes, s_moves[i].copyLen_bytes);
                for (k = 1u; k < (uint32_t)nvdbUser_last; k++) {
                    if (k != i && 0u != s_moves[k].pending &&
                        0 != BusyOverlaps(s_moves[k].dstAddr_bytes,
                                          s_moves[k].copyLen_bytes)) {
                        victim = i;
                        break;
                    }
                }
                if (0u != victim) {
                    break;
                }
            }
            if (0u == victim) {
                return nvdbRes_refused;
            }

            BusyClear();
            BusyMark(NVDB_CONFIG_ADDR, NVDB_CONFIG_SIZE);
            BusyMark(NVDB_WEAR_ADDR, NVDB_WEAR_SIZE);
            for (j = 1u; j < (uint32_t)nvdbUser_last; j++) {
                BusyMark(newEntries[j].addr_bytes, newEntries[j].size_bytes);
                if (0u != s_moves[j].pending) {
                    BusyMark(s_moves[j].liveAddr_bytes,
                             s_moves[j].copyLen_bytes);
                }
            }
            if (0 != BusyFind(s_moves[victim].copyLen_bytes, &scratch)) {
                return nvdbRes_refused;
            }
            if (plan->stepCnt >= NVDB_JRN_MAX_STEPS) {
                return nvdbRes_refused;
            }
            plan->steps[plan->stepCnt].user      = (uint8_t)victim;
            plan->steps[plan->stepCnt].src_bytes = s_moves[victim].liveAddr_bytes;
            plan->steps[plan->stepCnt].dst_bytes = scratch;
            plan->steps[plan->stepCnt].len_bytes = s_moves[victim].copyLen_bytes;
            plan->stepCnt++;
            s_moves[victim].liveAddr_bytes = scratch;
            s_moves[victim].detoured       = 1u;
        }
    }

    if (placed < needed) {
        return nvdbRes_refused;
    }

    /* Whatever a user did not bring with it must read as erased space: an
     * area that grew, or that landed where somebody else's bytes used to be,
     * would otherwise hand its user the previous layout's leftovers. */
    for (i = 1u; i < (uint32_t)nvdbUser_last; i++) {
        uint32_t tail    = newEntries[i].addr_bytes + s_moves[i].copyLen_bytes;
        uint32_t len     = newEntries[i].size_bytes - s_moves[i].copyLen_bytes;
        uint32_t oldSize = 0u;
        uint32_t oldAddr = 0u;

        if (i < nvdbDir.entryCnt) {
            oldAddr = nvdbDir.entries[i].addr_bytes;
            oldSize = nvdbDir.entries[i].size_bytes;
        }
        if (nvdbUser_nvdbConfig == i || nvdbUser_nvdbWear == i || 0u == len) {
            continue;
        }
        /* An area that stayed put and kept everything already has an erased
         * tail — that is what "occupied" means. */
        if (0u != oldSize && newEntries[i].addr_bytes == oldAddr &&
            newEntries[i].size_bytes <= oldSize) {
            continue;
        }
        if (plan->stepCnt >= NVDB_JRN_MAX_STEPS) {
            return nvdbRes_refused;
        }
        plan->steps[plan->stepCnt].user      = (uint8_t)i;
        plan->steps[plan->stepCnt].src_bytes = NVDB_STEP_ERASE;
        plan->steps[plan->stepCnt].dst_bytes = tail;
        plan->steps[plan->stepCnt].len_bytes = len;
        plan->stepCnt++;
    }

    return nvdbRes_ok;
}

/**
 * @brief Carry out a plan, skipping the steps a previous attempt finished.
 * @param  plan - the steps
 * @param  firstStep - where to start; the journal says which are already done
 * @retval nvdbRes_ok or nvdbRes_flash
 * @note Every step is idempotent — a copy re-erases and re-programs its
 *       destination, an erase re-erases — so a step interrupted halfway is
 *       simply run again.  The ordering guarantees the source of an unfinished
 *       step is still intact, which is what makes that safe.
 */
static eNvDbRes RunPlan(const sNvDbPlan *plan, uint32_t firstStep)
{
    uint32_t i = 0u;

    for (i = firstStep; i < plan->stepCnt; i++) {
        const sNvDbMoveStep *st  = &plan->steps[i];
        eNvDbRes             res = nvdbRes_ok;

        if (0 != JournalIsDone(i)) {
            continue;
        }

        if (NVDB_STEP_ERASE == st->src_bytes) {
            uint32_t off = 0u;

            for (off = 0u; off < st->len_bytes; off += NVDB_UNIT_SIZE) {
                NvDbPort_Kick();
                if (NvDbInt_RangeErased(st->dst_bytes + off, NVDB_UNIT_SIZE)) {
                    continue;
                }
                res = NvDbInt_RawErase(st->dst_bytes + off);
                if (nvdbRes_ok != res) {
                    return res;
                }
            }
        } else {
            res = NvDbInt_CopyRange(st->src_bytes, st->dst_bytes,
                                    st->len_bytes);
            if (nvdbRes_ok != res) {
                return res;
            }
        }
        JournalMarkDone(i);
    }
    return nvdbRes_ok;
}

/* ==========================================================================
 * The journal — what makes a relayout resumable
 * ========================================================================== */

static eNvDbRes JournalErase(void)
{
    return NvDbInt_RawErase(CfgAddr(NVDB_CFG_JOURNAL_OFF));
}

static eNvDbRes JournalWrite(const sNvDbPlan *plan,
                             const sNvDbDirRecord *target)
{
    eNvDbRes res = nvdbRes_ok;

    memset(&s_journal, 0, sizeof(s_journal));
    s_journal.magic     = NVDB_JRN_MAGIC;
    s_journal.recordVer = NVDB_RECORD_VER;
    s_journal.stepCnt   = plan->stepCnt;
    s_journal.targetSeq = target->seq;
    memcpy(s_journal.steps, plan->steps, sizeof(s_journal.steps));
    s_journal.crc32 = ImgMgmt_Crc32((const uint8_t *)&s_journal + sizeof(uint32_t),
                              (uint32_t)sizeof(s_journal) - (uint32_t)sizeof(uint32_t));

    res = JournalErase();
    if (nvdbRes_ok == res) {
        res = NvDbInt_RawProgram(CfgAddr(NVDB_CFG_JOURNAL_OFF +
                                         NVDB_JRN_PLAN_OFF),
                                 &s_journal, sizeof(s_journal));
    }
    if (nvdbRes_ok == res) {
        res = NvDbInt_RawProgram(CfgAddr(NVDB_CFG_JOURNAL_OFF +
                                         NVDB_JRN_TARGET_OFF),
                                 target, sizeof(*target));
    }
    return res;
}

static int JournalRead(sNvDbPlan *plan, sNvDbDirRecord *target)
{
    if (nvdbRes_ok != NvDbInt_RawRead(CfgAddr(NVDB_CFG_JOURNAL_OFF +
                                              NVDB_JRN_PLAN_OFF),
                                      &s_journal, sizeof(s_journal))) {
        return -1;
    }
    if (NVDB_JRN_MAGIC != s_journal.magic ||
        NVDB_RECORD_VER != s_journal.recordVer ||
        s_journal.stepCnt > NVDB_JRN_MAX_STEPS) {
        return -1;
    }
    if (s_journal.crc32 != ImgMgmt_Crc32((const uint8_t *)&s_journal + sizeof(uint32_t),
                                   (uint32_t)sizeof(s_journal) -
                                   (uint32_t)sizeof(uint32_t))) {
        return -1;
    }
    if (nvdbRes_ok != NvDbInt_RawRead(CfgAddr(NVDB_CFG_JOURNAL_OFF +
                                              NVDB_JRN_TARGET_OFF),
                                      target, sizeof(*target))) {
        return -1;
    }
    if (NVDB_DIR_MAGIC != target->magic || DirCrc(target) != target->crc32) {
        return -1;
    }
    /* The resume path COMMITS this record to a directory slot, so it has to
     * clear the same bar DirRead sets.  A bit-flip that happened to survive
     * the CRC would otherwise put a structurally impossible layout in force,
     * and the check that catches it runs too late to do anything but refuse
     * to open. */
    if (nvdbRes_ok != ValidateEntries(target->entries, target->entryCnt)) {
        return -1;
    }
    memcpy(plan->steps, s_journal.steps, sizeof(plan->steps));
    plan->stepCnt = s_journal.stepCnt;
    return 0;
}

/**
 * @brief Record that a step finished, without erasing anything.
 * @param  step - index into the plan
 * @retval none
 * @note NOR bit-clearing: the done byte goes from erased to 0x00 in place, so
 *       progress costs a program and never an erase.
 */
static void JournalMarkDone(uint32_t step)
{
    uint8_t mark = NVDB_JRN_DONE_MARK;

    if (step >= NVDB_JRN_MAX_STEPS) {
        return;
    }
    (void)NvDbInt_RawProgram(CfgAddr(NVDB_CFG_JOURNAL_OFF + NVDB_JRN_DONE_OFF +
                                     step), &mark, 1u);
}

static int JournalIsDone(uint32_t step)
{
    uint8_t mark = 0xFFu;

    if (step >= NVDB_JRN_MAX_STEPS) {
        return 0;
    }
    if (nvdbRes_ok != NvDbInt_RawRead(CfgAddr(NVDB_CFG_JOURNAL_OFF +
                                              NVDB_JRN_DONE_OFF + step),
                                      &mark, 1u)) {
        return 0;
    }
    return (NVDB_JRN_DONE_MARK == mark) ? 1 : 0;
}

/* ==========================================================================
 * The staged layout — one that has come aboard but has not been applied
 * ========================================================================== */

static int StageRead(sNvDbStageRecord *rec)
{
    if (nvdbRes_ok != NvDbInt_RawRead(CfgAddr(NVDB_CFG_STAGE_OFF), rec,
                                      sizeof(*rec))) {
        return -1;
    }
    if (NVDB_STAGE_MAGIC != rec->magic ||
        NVDB_RECORD_VER != rec->recordVer ||
        rec->entryCnt > NVDB_MAX_ENTRIES) {
        return -1;
    }
    if (rec->crc32 != ImgMgmt_Crc32((const uint8_t *)rec + sizeof(uint32_t),
                                    (uint32_t)sizeof(*rec) -
                                    (uint32_t)sizeof(uint32_t))) {
        return -1;
    }
    return 0;
}

static eNvDbRes StageErase(void)
{
    return NvDbInt_RawErase(CfgAddr(NVDB_CFG_STAGE_OFF));
}

/**
 * @brief Adopt a layout: plan it, journal it, run it, then commit it.
 * @param  name - the layout's name
 * @param  version - the layout's version
 * @param  mode - normal or forced
 * @param  size_bytes - one size per user
 * @param  operatorOwned - non-zero when a person supplied this layout
 * @retval nvdbRes_ok, nvdbRes_refused with the previous layout still in
 *            force, or nvdbRes_flash
 */
static eNvDbRes ApplyLayout(const char *name, uint16_t version,
                            eNvDbApplyMode mode, const uint32_t *size_bytes,
                            uint8_t operatorOwned)
{
    eNvDbRes       res = nvdbRes_ok;

    res = Place(size_bytes, s_placed);
    if (nvdbRes_ok != res) {
        return res;
    }

    res = PlanRelayout(s_placed, mode, &s_plan);
    if (nvdbRes_ok != res) {
        return res;
    }

    memset(&s_target, 0, sizeof(s_target));
    s_target.seq             = nvdbDir.seq + 1u;
    s_target.layoutVer       = version;
    s_target.entryCnt        = (uint16_t)nvdbUser_last;
    s_target.lastApplyMode   = (uint8_t)mode;
    s_target.lastApplyResult = (uint8_t)nvdbRes_ok;
    s_target.flags           = (uint8_t)(nvdbDir.flags |
                                       (operatorOwned ? NVDB_DIRFLAG_OPERATOR
                                                      : 0u));
    strncpy(s_target.layoutName, name, NVDB_LAYOUT_NAME_LEN - 1u);
    memcpy(s_target.entries, s_placed, sizeof(s_target.entries));
    /* Stamped here, not in DirWrite: the journal carries this record so a
     * resumed relayout can commit exactly what the interrupted one planned. */
    s_target.magic     = NVDB_DIR_MAGIC;
    s_target.recordVer = NVDB_RECORD_VER;
    s_target.nvdbVer   = NVDB_VERSION;
    s_target.crc32     = DirCrc(&s_target);

    res = JournalWrite(&s_plan, &s_target);
    if (nvdbRes_ok != res) {
        return res;
    }

    res = RunPlan(&s_plan, 0u);
    if (nvdbRes_ok != res) {
        return res;
    }

    res = DirWrite((NVDB_CFG_DIR_A_OFF == s_dirSlotOff) ? NVDB_CFG_DIR_B_OFF
                                                        : NVDB_CFG_DIR_A_OFF,
                   &s_target);
    if (nvdbRes_ok != res) {
        return res;
    }
    s_dirSlotOff = (NVDB_CFG_DIR_A_OFF == s_dirSlotOff) ? NVDB_CFG_DIR_B_OFF
                                                        : NVDB_CFG_DIR_A_OFF;
    nvdbDir = s_target;

    (void)JournalErase();
    return nvdbRes_ok;
}

/* Exported functions -------------------------------------------------------*/

/**
 * @brief Load the layout, finish or perform whatever relayout is due, and
 *        leave a layout in force.
 * @retval nvdbRes_ok, nvdbRes_refused when a layout was rejected and the
 *            previous one still stands, nvdbRes_flash if the medium is
 *            unusable
 * @note Boot always succeeds.  This is the ONLY place a layout is loaded,
 *       validated or applied.
 */
eNvDbRes NvDbLayout_Bringup(void)
{
    int              haveA   = 0;
    int              haveB   = 0;
    eNvDbRes         res     = nvdbRes_ok;
    eNvDbRes         applied = nvdbRes_ok;
    int              didApply = 0;

    haveA = (0 == DirRead(NVDB_CFG_DIR_A_OFF, &s_dirA)) ? 1 : 0;
    haveB = (0 == DirRead(NVDB_CFG_DIR_B_OFF, &s_dirB)) ? 1 : 0;

    if (0 != haveA && (0 == haveB || s_dirA.seq >= s_dirB.seq)) {
        nvdbDir      = s_dirA;
        s_dirSlotOff = NVDB_CFG_DIR_A_OFF;
    } else if (0 != haveB) {
        nvdbDir      = s_dirB;
        s_dirSlotOff = NVDB_CFG_DIR_B_OFF;
    } else {
        /* First adoption (§4.4.1): no directory, so the board is either
         * factory-fresh or still holding the pre-nvDb hand-assigned map.
         * Both are described by the same assumed-current layout — on a fresh
         * board those areas simply hold erased space, so the relayout that
         * follows moves nothing. */
        memset(&nvdbDir, 0, sizeof(nvdbDir));
        nvdbDir.seq       = 1u;
        nvdbDir.layoutVer = 0u;
        nvdbDir.entryCnt  = (uint16_t)nvdbUser_last;
        nvdbDir.lastApplyMode   = (uint8_t)nvdbMode_normal;
        nvdbDir.lastApplyResult = (uint8_t)nvdbRes_ok;
        strncpy(nvdbDir.layoutName, NVDB_LEGACY_NAME, NVDB_LAYOUT_NAME_LEN - 1u);
        memcpy(nvdbDir.entries, s_legacyMap, sizeof(nvdbDir.entries));

        if (nvdbRes_ok != ValidateEntries(nvdbDir.entries, nvdbDir.entryCnt)) {
            return nvdbRes_refused;
        }
        res = DirWrite(NVDB_CFG_DIR_A_OFF, &nvdbDir);
        if (nvdbRes_ok != res) {
            return res;
        }
        s_dirSlotOff = NVDB_CFG_DIR_A_OFF;
    }

    /* A relayout that was interrupted: the steps not yet marked done are run
     * again, and the directory the plan was built to commit is committed. */
    if (0 == JournalRead(&s_plan, &s_target)) {
        if (s_target.seq == (nvdbDir.seq + 1u)) {
            res = RunPlan(&s_plan, 0u);
            if (nvdbRes_ok != res) {
                return res;
            }
            res = DirWrite((NVDB_CFG_DIR_A_OFF == s_dirSlotOff)
                               ? NVDB_CFG_DIR_B_OFF : NVDB_CFG_DIR_A_OFF,
                           &s_target);
            if (nvdbRes_ok != res) {
                return res;
            }
            s_dirSlotOff = (NVDB_CFG_DIR_A_OFF == s_dirSlotOff)
                               ? NVDB_CFG_DIR_B_OFF : NVDB_CFG_DIR_A_OFF;
            nvdbDir = s_target;
            /* That journal was this staged layout being applied. */
            (void)StageErase();
        }
        (void)JournalErase();
    }

    /* A supplied layout takes precedence over the one the image ships: the
     * person who authored it is the one holding the board. */
    if (0 == StageRead(&s_stage)) {
        uint32_t sizes[nvdbUser_last];
        uint32_t i = 0u;

        memset(sizes, 0, sizeof(sizes));
        for (i = 0u; i < (uint32_t)nvdbUser_last && i < s_stage.entryCnt; i++) {
            sizes[i] = s_stage.size_bytes[i];
        }
        applied  = ApplyLayout(s_stage.name, s_stage.version,
                               (eNvDbApplyMode)s_stage.operation, sizes, 1u);
        didApply = 1;
        /* Dropped once it has been ANSWERED — adopted or refused.  A medium
         * failure is not an answer: the likely cause is a power cut, and a
         * board that comes back up should carry on applying the layout its
         * operator supplied rather than quietly forget it. */
        if (nvdbRes_flash != applied) {
            (void)StageErase();
        }
    } else if (0u == (nvdbDir.flags & NVDB_DIRFLAG_OPERATOR) &&
               (0 != strncmp(nvdbDir.layoutName, NVDB_TARGET_NAME,
                             NVDB_LAYOUT_NAME_LEN) ||
                nvdbDir.layoutVer < NVDB_TARGET_VER)) {
        applied  = ApplyLayout(NVDB_TARGET_NAME, NVDB_TARGET_VER,
                               nvdbMode_normal, s_targetSizes, 0u);
        didApply = 1;
    }

    if (0 != didApply && nvdbRes_refused == applied) {
        /* REFUSED only.  The layout in force is untouched and no journal was
         * ever written, so bumping the sequence number is free — and the
         * record of what happened is the whole point, because an operator can
         * only learn the outcome after the reboot that produced it.
         *
         * A MEDIUM failure gets no note, deliberately.  A relayout that died
         * partway leaves a journal whose targetSeq is nvdbDir.seq + 1, and
         * that is precisely how the next boot recognises work to resume.
         * Writing a note here would consume that sequence number, the resume
         * test would miss, the journal would be erased as stale — and users
         * whose bytes had already moved would be stranded at addresses the
         * directory no longer describes.  The journal IS the record of that
         * state, and it is a better one than the note. */
        sNvDbDirRecord note = nvdbDir;

        note.seq++;
        note.lastApplyResult = (uint8_t)applied;
        if (nvdbRes_ok == DirWrite((NVDB_CFG_DIR_A_OFF == s_dirSlotOff)
                                       ? NVDB_CFG_DIR_B_OFF
                                       : NVDB_CFG_DIR_A_OFF, &note)) {
            s_dirSlotOff = (NVDB_CFG_DIR_A_OFF == s_dirSlotOff)
                               ? NVDB_CFG_DIR_B_OFF : NVDB_CFG_DIR_A_OFF;
            nvdbDir = note;
        }
        return applied;
    }
    if (0 != didApply && nvdbRes_ok != applied) {
        return applied;
    }
    return nvdbRes_ok;
}

/**
 * @brief Does the layout in force still describe a usable medium?
 * @retval nvdbRes_ok, or nvdbRes_refused
 * @note Checked on every init whether or not anything changed, and checked
 *       again by NvDb_Init() before it opens access — a layout that no longer
 *       holds is worth learning about before a user reads through it.
 */
eNvDbRes NvDbLayout_Check(void)
{
    return ValidateEntries(nvdbDir.entries, nvdbDir.entryCnt);
}

/**
 * @brief What one user is using, and how worn its area is.
 * @param  user - the holder of storage
 * @param  scan - observe occupancy, which reads the whole area
 * @param  out - filled with the answer
 * @retval nvdbRes_ok, nvdbRes_notInit, nvdbRes_badUser
 * @note BLOCKS when `scan` is set — for a 488 KB blob this reads 488 KB.
 *       Reporting only: nothing here is an input to any decision (C13).
 */
eNvDbRes NvDb_GetUsage(eNvDbUser user, bool scan, sNvDbUsage *out)
{
    uint32_t idx  = (uint32_t)user;
    uint32_t addr = 0u;
    uint32_t size = 0u;

    if (NULL == out) {
        return nvdbRes_badUser;
    }
    if (!nvdbInitDone) {
        return nvdbRes_notInit;
    }
    if (nvdbUser_undefined == user || idx >= (uint32_t)nvdbUser_last) {
        return nvdbRes_badUser;
    }

    NvDbPort_Lock();

    memset(out, 0, sizeof(*out));
    if (idx < nvdbDir.entryCnt) {
        addr = nvdbDir.entries[idx].addr_bytes;
        size = nvdbDir.entries[idx].size_bytes;
    }
    out->size_bytes = size;
    out->units      = size / NVDB_UNIT_SIZE;

    if (0u != size) {
        NvDbWear_Span(addr, size, &out->eraseCntMax, &out->eraseCntTotal);
        if (scan) {
            out->occupied_bytes = NvDbInt_OccupiedBytes(addr, size);
        }
    }

    NvDbPort_Unlock();
    return nvdbRes_ok;
}

/**
 * @brief What the medium as a whole looks like.
 * @param  out - filled with the answer
 * @retval nvdbRes_ok or nvdbRes_notInit
 * @note Does not scan: free space comes from the layout, and wear from the
 *       counters already in RAM.
 */
eNvDbRes NvDb_GetMediumUsage(sNvDbMediumUsage *out)
{
    uint32_t i    = 0u;
    uint32_t used = 0u;

    if (NULL == out) {
        return nvdbRes_badUser;
    }
    if (!nvdbInitDone) {
        return nvdbRes_notInit;
    }

    NvDbPort_Lock();

    memset(out, 0, sizeof(*out));
    for (i = 1u; i < nvdbDir.entryCnt; i++) {
        used += nvdbDir.entries[i].size_bytes;
    }
    out->medium_bytes    = NVDB_MEDIUM_SIZE;
    out->allocated_bytes = used;
    out->freeSpace_bytes = NVDB_MEDIUM_SIZE - used;
    out->unitSize_bytes  = NVDB_UNIT_SIZE;
    out->units           = NVDB_UNIT_CNT;
    NvDbWear_Span(0u, NVDB_MEDIUM_SIZE, &out->eraseCntMax,
                  &out->eraseCntTotal);
    out->eraseCntUnsaved = NvDbWear_Unsaved();

    NvDbPort_Unlock();
    return nvdbRes_ok;
}

/**
 * @brief The status of the last layout application.
 * @param  out - filled with the persisted status
 * @retval nvdbRes_ok or nvdbRes_notInit
 * @note This is for the operator, not for a user.  Loading can fail even when
 *       validation passed, and lastApplyResult of nvdbRes_refused means the
 *       previous layout is still in force.
 */
eNvDbRes NvDb_GetStatus(sNvDbStatus *out)
{
    if (NULL == out) {
        return nvdbRes_badUser;
    }
    if (!nvdbInitDone) {
        return nvdbRes_notInit;
    }

    NvDbPort_Lock();
    memset(out, 0, sizeof(*out));
    out->nvdbVer   = nvdbDir.nvdbVer;
    out->layoutVer = nvdbDir.layoutVer;
    memcpy(out->layoutName, nvdbDir.layoutName, NVDB_LAYOUT_NAME_LEN);
    out->layoutName[NVDB_LAYOUT_NAME_LEN - 1u] = '\0';
    out->lastApplyMode   = (eNvDbApplyMode)nvdbDir.lastApplyMode;
    out->lastApplyResult = (eNvDbRes)nvdbDir.lastApplyResult;
    NvDbPort_Unlock();
    return nvdbRes_ok;
}

/**
 * @brief The layout in force, free space, and anything waiting to be applied.
 * @param  out - filled with the read-back form
 * @retval nvdbRes_ok or nvdbRes_notInit
 * @note Free space is an answer to the consumer, never an entry in the user
 *       table — it is not a user.
 */
eNvDbRes NvDb_GetLayout(sNvDbLayoutInfo *out)
{
    sNvDbStageRecord stage;
    uint32_t         used = 0u;
    uint32_t         i    = 0u;

    if (NULL == out) {
        return nvdbRes_badUser;
    }
    if (!nvdbInitDone) {
        return nvdbRes_notInit;
    }

    NvDbPort_Lock();

    memset(out, 0, sizeof(*out));
    memcpy(out->name, nvdbDir.layoutName, NVDB_LAYOUT_NAME_LEN);
    out->name[NVDB_LAYOUT_NAME_LEN - 1u] = '\0';
    out->version = nvdbDir.layoutVer;

    for (i = 1u; i < (uint32_t)nvdbUser_last; i++) {
        if (i < nvdbDir.entryCnt) {
            out->size_bytes[i] = nvdbDir.entries[i].size_bytes;
            used += nvdbDir.entries[i].size_bytes;
        }
    }
    out->freeSpace_bytes = NVDB_MEDIUM_SIZE - used;

    out->onboarding = nvdbOnboard_none;
    if (0 == StageRead(&stage)) {
        memcpy(out->received.name, stage.name, NVDB_LAYOUT_NAME_LEN);
        out->received.name[NVDB_LAYOUT_NAME_LEN - 1u] = '\0';
        out->received.version   = stage.version;
        out->received.operation = (eNvDbApplyMode)stage.operation;
        out->onboarding = (nvdbMode_forced == (eNvDbApplyMode)stage.operation)
                              ? nvdbOnboard_forced : nvdbOnboard_validated;
    }

    NvDbPort_Unlock();
    return nvdbRes_ok;
}

/**
 * @brief Take a configuration aboard, to be applied at the next init.
 * @param  cfg - the layout an operator authored
 * @retval nvdbRes_ok, nvdbRes_refused if the ADVISORY check rejects it,
 *            nvdbRes_notInit, nvdbRes_flash
 * @note Advisory means exactly that: users keep writing between now and the
 *       next init, so a shrink that is safe today may not be safe when it is
 *       applied, and flash can fail during the copy besides.  The binding
 *       check runs at init and its outcome lands in the status.
 */
eNvDbRes NvDb_SupplyLayout(const sNvDbLayoutCfg *cfg)
{
    eNvDbRes         res = nvdbRes_ok;

    if (NULL == cfg) {
        return nvdbRes_badUser;
    }
    if (!nvdbInitDone) {
        return nvdbRes_notInit;
    }
    if (nvdbMode_normal != cfg->operation && nvdbMode_forced != cfg->operation) {
        return nvdbRes_refused;
    }

    NvDbPort_Lock();

    res = Place(cfg->size_bytes, s_placed);
    if (nvdbRes_ok == res) {
        /* The same planner the init runs, so an operator sees the mistake now
         * rather than after a reboot.  It touches nothing — planning is pure
         * arithmetic over observed occupancy. */
        res = PlanRelayout(s_placed, cfg->operation, &s_plan);
    }
    if (nvdbRes_ok != res) {
        NvDbPort_Unlock();
        return res;
    }

    memset(&s_stage, 0, sizeof(s_stage));
    s_stage.magic     = NVDB_STAGE_MAGIC;
    s_stage.recordVer = NVDB_RECORD_VER;
    s_stage.entryCnt  = (uint16_t)nvdbUser_last;
    s_stage.version   = cfg->version;
    s_stage.operation = (uint8_t)cfg->operation;
    strncpy(s_stage.name, cfg->name, NVDB_LAYOUT_NAME_LEN - 1u);
    memcpy(s_stage.size_bytes, cfg->size_bytes, sizeof(uint32_t) * (uint32_t)nvdbUser_last);
    s_stage.crc32 = ImgMgmt_Crc32((const uint8_t *)&s_stage + sizeof(uint32_t),
                              (uint32_t)sizeof(s_stage) - (uint32_t)sizeof(uint32_t));

    res = StageErase();
    if (nvdbRes_ok == res) {
        res = NvDbInt_RawProgram(CfgAddr(NVDB_CFG_STAGE_OFF), &s_stage,
                                 sizeof(s_stage));
    }

    NvDbPort_Unlock();
    return res;
}

/**
 * @brief Discard a configuration that came aboard but has not been applied.
 * @retval nvdbRes_ok, nvdbRes_notInit, nvdbRes_flash
 */
eNvDbRes NvDb_DropSuppliedLayout(void)
{
    eNvDbRes res = nvdbRes_ok;

    if (!nvdbInitDone) {
        return nvdbRes_notInit;
    }
    NvDbPort_Lock();
    res = StageErase();
    NvDbPort_Unlock();
    return res;
}
