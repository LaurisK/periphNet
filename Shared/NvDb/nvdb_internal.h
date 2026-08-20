/**
 * @file    nvdb_internal.h
 * @brief   nvDb's private geometry, records and cross-file helpers.
 *
 * NOT a consumer header.  Everything here is a flash word — unit size, page
 * size, absolute anchors, the on-medium records — which is exactly why none
 * of it appears in nvdb.h.  Only nvdb.c, nvdb_layout.c and nvdb_config.c
 * include this.
 */
#ifndef NVDB_INTERNAL_H_
#define NVDB_INTERNAL_H_

#include "nvdb.h"
#include "nvdb_layout.h"
#include "w25q128.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * The medium
 * ========================================================================== */

/* The erasable unit.  Two distinct users never share one — that single fact
 * is what makes the user id a capability rather than a label. */
#define NVDB_UNIT_SIZE          ((uint32_t)W25Q128_SECTOR_SIZE)
#define NVDB_PROG_PAGE          ((uint32_t)W25Q128_PAGE_SIZE)

/* W25Q64 on this board.  Overridable so a host test can model a small part. */
#ifndef NVDB_MEDIUM_SIZE
#define NVDB_MEDIUM_SIZE        0x00800000u
#endif

#define NVDB_UNIT_CNT           (NVDB_MEDIUM_SIZE / NVDB_UNIT_SIZE)

#define NVDB_UNIT_BASE(a)       ((a) & ~(NVDB_UNIT_SIZE - 1u))
#define NVDB_UNIT_UP(a)         (((a) + NVDB_UNIT_SIZE - 1u) & ~(NVDB_UNIT_SIZE - 1u))
#define NVDB_ERASED_BYTE        0xFFu

/* ==========================================================================
 * The one immovable address in the design
 * ==========================================================================
 * nvDb's config area is the bootstrap: everything else is placed by the
 * layout that lives inside it, so its own placement cannot come from there.
 * The wear area is pinned beside it for the same reason and kept separate
 * because its write rate must never endanger the config.
 *
 * Both sit above the pre-nvDb hand-assigned map (which ended at 0x00104000),
 * so the first image carrying nvDb can claim them without disturbing a board
 * that already holds data.
 */
#define NVDB_CONFIG_ADDR        0x00104000u
#define NVDB_CONFIG_SIZE        0x00004000u     /* 4 units, roles below      */
#define NVDB_WEAR_ADDR          0x00108000u
#define NVDB_WEAR_SIZE          0x00001000u

/* Sector roles inside the config area. */
#define NVDB_CFG_DIR_A_OFF      0x0000u         /* directory slot A          */
#define NVDB_CFG_DIR_B_OFF      0x1000u         /* directory slot B          */
#define NVDB_CFG_STAGE_OFF      0x2000u         /* layout awaiting an init   */
#define NVDB_CFG_JOURNAL_OFF    0x3000u         /* relayout journal          */

/* Roles inside the journal sector.  The journal is what makes a relayout
 * resumable: it holds the whole plan, the directory to commit once the plan
 * has run, and one byte per step that is BIT-CLEARED as that step completes —
 * no erase, so recording progress costs nothing. */
#define NVDB_JRN_PLAN_OFF       0x000u
#define NVDB_JRN_TARGET_OFF     0x800u          /* the directory to commit   */
#define NVDB_JRN_DONE_OFF       0xC00u          /* one byte per step         */
#define NVDB_JRN_DONE_MARK      0x00u

/* ==========================================================================
 * Versions and magics
 * ========================================================================== */

#define NVDB_VERSION            1u              /* the module's own version  */

#define NVDB_DIR_MAGIC          0x4E564442u     /* "NVDB"                    */
#define NVDB_STAGE_MAGIC        0x4E564C59u     /* "NVLY"                    */
#define NVDB_JRN_MAGIC          0x4E564A52u     /* "NVJR"                    */
#define NVDB_RECORD_VER         1u

/* Entry slots in the on-medium records.  nvdbUser_last GROWS when a user is
 * added, so a record written by an older image holds fewer entries than a
 * newer image expects — which is why the record states its own count.  The
 * array is fixed so the whole record fits one program page. */
#define NVDB_MAX_ENTRIES        24u

/* Moves, scratch detours and tail erases all ride the same list, so this has
 * to cover more than one entry per user. */
#define NVDB_JRN_MAX_STEPS      64u

/* A step whose source is this erases its destination instead of copying to
 * it: growing an area means its tail must read as erased space, and a user
 * must never be handed the previous layout's leftovers. */
#define NVDB_STEP_ERASE         0xFFFFFFFFu

/* ==========================================================================
 * On-medium records
 * ==========================================================================
 * crc32 comes FIRST in each record and covers every byte after it, so the
 * variable tail is inside the check rather than beside it.
 */

/* The layout in force was put there by an operator, not by the image.  Once
 * set, the built-in layout stops applying itself: whoever authored a layout
 * for this board owns it, and a firmware update must not quietly take it
 * back. */
#define NVDB_DIRFLAG_OPERATOR   (1u << 0)

typedef struct {
    uint32_t    addr_bytes;
    uint32_t    size_bytes;
} sNvDbDirEntry;

typedef struct {
    uint32_t        crc32;                          /* over the rest         */
    uint32_t        magic;                          /* NVDB_DIR_MAGIC        */
    uint32_t        seq;                            /* higher slot wins      */
    uint16_t        recordVer;
    uint16_t        nvdbVer;
    uint16_t        layoutVer;
    uint16_t        entryCnt;
    char            layoutName[NVDB_LAYOUT_NAME_LEN];
    uint8_t         lastApplyMode;                  /* eNvDbApplyMode        */
    uint8_t         lastApplyResult;                /* eNvDbRes              */
    uint8_t         flags;                          /* NVDB_DIRFLAG_*        */
    uint8_t         _reserved;
    sNvDbDirEntry   entries[NVDB_MAX_ENTRIES];
} sNvDbDirRecord;

typedef struct {
    uint32_t        crc32;                          /* over the rest         */
    uint32_t        magic;                          /* NVDB_STAGE_MAGIC      */
    uint16_t        recordVer;
    uint16_t        entryCnt;
    uint16_t        version;
    uint8_t         operation;                      /* eNvDbApplyMode        */
    uint8_t         _reserved;
    char            name[NVDB_LAYOUT_NAME_LEN];
    uint32_t        size_bytes[NVDB_MAX_ENTRIES];
} sNvDbStageRecord;

typedef struct {
    uint32_t        src_bytes;
    uint32_t        dst_bytes;
    uint32_t        len_bytes;
    uint8_t         user;
    uint8_t         _reserved[3];
} sNvDbMoveStep;

typedef struct {
    uint32_t        crc32;                          /* over the rest         */
    uint32_t        magic;                          /* NVDB_JRN_MAGIC        */
    uint16_t        recordVer;
    uint16_t        stepCnt;
    uint32_t        targetSeq;                      /* seq the commit writes  */
    sNvDbMoveStep   steps[NVDB_JRN_MAX_STEPS];
} sNvDbJournal;

/* ==========================================================================
 * Cross-file helpers (nvdb.c owns the medium primitives and the buffer)
 * ========================================================================== */

/* The one 4 KB staging buffer, in main SRAM — never CCM, which is CPU-only
 * memory that SPI DMA cannot reach and is ~91 % full besides.  Shared by the
 * write path, the collector and relocation; all three hold the lock. */
extern uint8_t nvdbUnitBuff[NVDB_UNIT_SIZE];

/* The layout in force, in RAM.  Read by NvDb_GetAbsoluteAddress from fault
 * context, so it is plain state with no lock around it. */
extern sNvDbDirRecord nvdbDir;
extern bool           nvdbInitDone;

eNvDbRes NvDbInt_RawRead   (uint32_t addr_bytes, void *buff, uint32_t len_bytes);
eNvDbRes NvDbInt_RawProgram(uint32_t addr_bytes, const void *buff, uint32_t len_bytes);
eNvDbRes NvDbInt_RawErase  (uint32_t addr_bytes);

/* True if every byte of [addr, addr+len) reads as the erased value. */
bool NvDbInt_RangeErased(uint32_t addr_bytes, uint32_t len_bytes);

/* Write `len` bytes at `addr`, erasing and preserving whatever the erasable
 * units around them hold.  This is the write path (§4.1) with the bounds
 * check already done, and it is what relocation and the collector reuse. */
eNvDbRes NvDbInt_PutBytes(uint32_t addr_bytes, const void *buff, uint32_t len_bytes);

/* Copy `len` bytes from `src` to `dst`, unit by unit, kicking the watchdog
 * between units.  Handles overlap: both ends are unit-aligned, so a
 * direction-aware walk never erases a unit it has not yet copied. */
eNvDbRes NvDbInt_CopyRange(uint32_t src_bytes, uint32_t dst_bytes,
                           uint32_t len_bytes);

/* Observed occupancy: (index of the last non-erased unit + 1) * unit size.
 * The one thing about a user's bytes nvDb is allowed to look at, and it looks
 * at their STATE, never their meaning. */
uint32_t NvDbInt_OccupiedBytes(uint32_t addr_bytes, uint32_t size_bytes);

/* nvdb_layout.c — called by NvDb_Init() with the lock held. */
eNvDbRes NvDbLayout_Bringup(void);

/* nvdb_layout.c — the layout in force still describes a usable medium. */
eNvDbRes NvDbLayout_Check(void);

/* nvdb_wear.c — indication only, never an input to anything (C13). */
void NvDbWear_Init (void);
void NvDbWear_Count(uint32_t addr_bytes);
void NvDbWear_Flush(bool idle);
void NvDbWear_Span (uint32_t addr_bytes, uint32_t len_bytes,
                    uint32_t *max, uint32_t *total);
uint32_t NvDbWear_Unsaved(void);

/* nvdb.c — drop every collector mark belonging to `user`.  Called with the
 * lock held. */
void NvDbInt_DropMarks(eNvDbUser user);

#ifdef __cplusplus
}
#endif

#endif /* NVDB_INTERNAL_H_ */
