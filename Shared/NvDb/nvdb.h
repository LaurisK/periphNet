/**
 * @file    nvdb.h
 * @brief   The application's only authority over non-volatile storage.
 *
 * `nvDb` owns the external flash address space.  It decides where every
 * user's bytes live, moves them when the layout changes, refuses access
 * outside a user's own area, and presents each user a flat span of
 * non-volatile memory starting at zero.  It does not read, interpret,
 * validate, checksum, mirror or repair anything.
 *
 * See docs/task_nv_db.md.  The three rules, condensed:
 *
 *   1. A user knows nothing it does not need in order to perform its
 *      operation.  Nothing in THIS header is a flash word: no address, no
 *      sector, no page, no alignment, no erase, no CRC, no commit.
 *   2. `nvDb` is the only authority over the medium.  The two clients that
 *      cannot call it to perform their I/O — the crash handler and the FWU
 *      module — obtain locations FROM it, through nvdb_exceptions.h.
 *   3. `nvDb` knows nothing about the data it holds that it does not need in
 *      order to keep it.
 *
 * What this module promises is isolation: distinct users never share an
 * erasable unit, so no operation by one user can reach another's bytes —
 * including an erase.  The user id is a capability, not a label.
 *
 * What it does NOT promise: durability of a user's own overwrite (§2.5), any
 * validation of content, or that a delete will ever happen.  Each user is
 * responsible for the validity of its own data; a user that stores garbage
 * stores it durably, and that is correct behaviour.
 *
 * CONTRACT (docs/task_nv_db.md §2.3) — task context only.  No ISRs, no lwIP
 * callbacks.  Nothing enforces this.
 *
 *   NvDb_Init     blocks; requires W25Q128_Init() first
 *   NvDb_Read     BLOCKS, up to seconds — it can wait behind an erase
 *   NvDb_Write    BLOCKS, up to seconds — it may perform an erase
 *   NvDb_Delete   returns immediately, except when the mark list is full
 *   NvDb_Wipe     returns immediately
 *   NvDb_GetSize  does not block
 *
 * Shared-layer module: depends only on libc + the W25Q128 driver plus the
 * port in nvdb_port.h, so it is host-testable over the NOR-faithful flash
 * mock.  It must NEVER link into the 32 KB bootloader.
 */
#ifndef NVDB_H_
#define NVDB_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * Identity
 * ==========================================================================
 * A user is a holder of storage.  One user, one area, always 1:1.  A module
 * may hold any number of users — the usual reason is separable concerns (a
 * module's configuration and its accumulated data differ in size, lifecycle
 * and in when each should be wiped), not redundancy.
 *
 * THESE VALUES ARE PERSISTED in the directory on the medium and are NEVER
 * renumbered.  Append only.  `nvdbUser_last` is a count, never a stored
 * value.  Removing a user is not yet designed (docs/task_nv_db.md §6).
 */
typedef enum {
    nvdbUser_undefined = 0,     /* sentinel                                 */

    /* nvDb's own two areas.  Pinned: their placement does not come from the
     * layout, because the layout lives inside the first of them.  They are
     * kept apart because they are written at wildly different rates and the
     * wear churn must not endanger the one structure whose loss strands
     * every user at once. */
    nvdbUser_nvdbConfig,        /* layout in force, staged layout, status   */
    nvdbUser_nvdbWear,          /* erase counts — reserved, see §4.5        */

    /* Firmware update.  First three keep the addresses the bootloader was
     * built against; the BL has no knowledge of nvDb (§1.6). */
    nvdbUser_bootStatus,
    nvdbUser_fwuStored,
    nvdbUser_fwuGolden,
    nvdbUser_imageMeta,

    nvdbUser_crashLog,

    nvdbUser_modbusLutA,
    nvdbUser_modbusLutB,
    nvdbUser_modbusSelector,

    nvdbUser_wgTime,
    nvdbUser_wgCfg,

    nvdbUser_mqttCfg,           /* broker + topic prefix — RAM-only today   */
    nvdbUser_triceUdpCfg,       /* Trice UDP destination — RAM-only today   */

    nvdbUser_last               /* sentinel — a count, never stored         */
} eNvDbUser;

/* ==========================================================================
 * Results
 * ==========================================================================
 * PERSISTED in the status record (§2.6): never renumbered, append only.
 */
typedef enum {
    nvdbRes_undefined = 0,
    nvdbRes_ok,
    nvdbRes_notInit,            /* called before NvDb_Init()                */
    nvdbRes_badUser,            /* unknown id, or not permitted for the call */
    nvdbRes_outOfBounds,        /* offset_bytes + len_bytes exceeds the area */
    nvdbRes_noOperation,        /* len_bytes == 0 — nothing was asked for    */
    nvdbRes_refused,            /* layout rejected; the previous one stands  */
    nvdbRes_flash,              /* the medium failed                        */
    nvdbRes_last
} eNvDbRes;

/* ==========================================================================
 * The completion callback
 * ==========================================================================
 * A delete is deferred, so the only way a user can know its bytes are
 * actually gone is to be told.  NULL for a user that does not care, which is
 * most of them.
 *
 * Runs in the collector's lowest-priority context and MUST NOT BLOCK.  It is
 * invoked outside nvDb's lock, so calling back into nvDb from it is
 * permitted.  When NvDb_Delete had to do the erase inline (mark list full),
 * the callback fires BEFORE that call returns — a user that keeps state in it
 * must tolerate re-entry.
 *
 * It confirms an erase HAPPENED; it never promises one WILL.  Marks do not
 * survive a reset, so a reboot before the collector arrives means the
 * callback simply never fires.
 *
 * It reports the range that became erased, which is why it carries one: a
 * delete that a later write cut in half is collected in pieces and reported
 * in pieces.  A user that wants one completion should not write into a range
 * it has just deleted and not yet heard about.
 */
typedef void (*fNvDbEraseDone)(eNvDbUser user, uint32_t offset_bytes,
                               uint32_t len_bytes, eNvDbRes result);

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

eNvDbRes NvDb_Init       (void);

/* ==========================================================================
 * Access
 * ========================================================================== */

eNvDbRes NvDb_Read       (eNvDbUser user,       void *buff,
                          uint32_t offset_bytes, uint32_t len_bytes);
eNvDbRes NvDb_Write      (eNvDbUser user, const void *buff,
                          uint32_t offset_bytes, uint32_t len_bytes);
eNvDbRes NvDb_Delete     (eNvDbUser user,
                          uint32_t offset_bytes, uint32_t len_bytes,
                          fNvDbEraseDone onDone);
eNvDbRes NvDb_Wipe       (eNvDbUser user, fNvDbEraseDone onDone);
eNvDbRes NvDb_GetSize    (eNvDbUser user, uint32_t *size_bytes);

/* A user that has no space has size_bytes == 0 and every access fails
 * nvdbRes_outOfBounds.  Absence and emptiness are the same state; there is no
 * "unprovisioned" flag and none is needed. */

/* ==========================================================================
 * Naming — for the operator surface, not for a user
 * ========================================================================== */

/* The JSON key a layout uses for `user`, or NULL for an unknown id.  Storage
 * is static; the caller must not free it. */
const char *NvDb_UserName(eNvDbUser user);

/* The id a JSON key names, or nvdbUser_undefined if no user is called that. */
eNvDbUser NvDb_UserByName(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* NVDB_H_ */
