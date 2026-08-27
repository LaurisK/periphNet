/**
 * @file    pack_cfg.h
 * @brief   The uploaded pack configuration — streaming parse, re-serialise,
 *          and the record shape (docs/design_battery_pack.md §12, §14).
 *
 * LIBC ONLY, DELIBERATELY.  The parse and the serialise are pure text work
 * over a caller-supplied byte source, on the App/Net/wg_conf.c pattern — no
 * whole-document buffer, no nvDb, no RTOS — so tests/ compiles this file and
 * the §12 accept/reject matrix is a host test rather than an HTTP session.
 * PERSISTENCE LIVES IN pack.c: this file produces and consumes an sPackCfg in
 * RAM and never learns what a flash area is.
 *
 * ONE PARSER, ONE PASS, ONE RESULT STRUCT.  Pack_ConfigVerify and
 * Pack_ConfigApply differ only in whether they commit what this file
 * produced, so there is never a second validator that can disagree with the
 * first.  docs/modbus.md §5.3 is the precedent and the reason.
 *
 * BOUNDARY: internal to App/Pack.  Nothing outside the module includes it.
 *
 * STATUS: IMPLEMENTED and host-tested (tests/test_pack_cfg.c), including the
 * reject matrix and an ASan/UBSan fuzz of the parser.
 */

#ifndef PACK_CFG_H_
#define PACK_CFG_H_

/* Includes -----------------------------------------------------------------*/

#include "App/Pack/pack.h"
#include "App/Pack/pack_type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported types -----------------------------------------------------------*/

/** The configuration record's version.  Bumping it invalidates every stored
 *  record through NvRecord_Load, which collapses absent, truncated,
 *  wrong-version and corrupt into one answer — "nothing valid stored" — so an
 *  old image reading a new record becomes UNPROVISIONED and says so rather
 *  than misreading a pack's binding (§14). */
#define PACK_CFG_VERSION        1u

/** The magic the nvDb record carries, for App/nv_record.h. */
#define PACK_CFG_MAGIC          0x5041434Bu  /* "PACK" */

/** `name` is the identity: unique, [A-Za-z0-9_-], at most this many chars
 *  plus the NUL.  Persisted per-pack state keys on it, so reordering the
 *  array cannot reattach one pack's history to another. */
#define PACK_CFG_NAME_MAX_CHARS (PACK_NAME_LEN - 1u)

/** Per-type defaults, applied when the document omits them (§12). */
/* The widest current a `commands` domain may name, in AMPS.  It exists to
 * keep the amps -> milliamps multiply inside int32_t; the per-type `ceiling`
 * is what actually narrows a domain to something the hardware could accept. */
#define PACK_CFG_MAX_AMPS         2000000

#define PACK_CFG_JK_STALE_MS         15000u  /* three missed 5 s laps        */
#define PACK_CFG_JK_CELL_STALE_MS    60000u  /* four missed 15 s cell laps   */
#define PACK_CFG_PYLON_STALE_MS       5000u  /* five missed 1 Hz frames      */
#define PACK_CFG_PYLON_CELL_STALE_MS 60000u

/** One configured instance, as parsed.  Sized so PACK_MAX of them plus a
 *  header fit the ~550 B §14 budgets for nvdbUser_packCfg. */
typedef struct {
    uint32_t      nameplate_mAh;
    uint32_t      staleAfter_ms;
    uint32_t      cmdAllow;             /* PACK_CMD_BIT set the operator
                                           permits.  Absent `commands` means
                                           EVERY command the type confirms,
                                           which is cmdAllow = 0xFFFFFFFF    */
    sPackCmdBound bounds[packCmd_last]; /* operator-narrowed domains         */
    char          name[PACK_NAME_LEN];
    char          bind[PACK_BIND_LEN];  /* OPAQUE to the core; the TYPE
                                           parses it (§12)                   */
    uint8_t       boundCount;
    uint8_t       typeId;               /* ePackTypeId                       */
    uint8_t       chemistry;            /* ePackChemistry                    */
    uint8_t       cellCount;
} sPackCfgEntry;

/** The whole document, bounded and RAM-resident by design — deliberately NOT
 *  a record stream in the Modbus style, because a register config is
 *  unbounded and must be walked from flash whereas this is eight tiny
 *  records (§14). */
typedef struct {
    sPackCfgEntry pack[PACK_MAX];
    uint16_t      version;
    uint8_t       count;
} sPackCfg;

/** The ceiling a `commands` block is checked against at PARSE time.  §12: "a
 *  bound WIDER than the type's is a 422 at parse time, not a silent clamp."
 *  A per-instance bind narrows further at run time; this is the static
 *  blueprint limit the parser can see without a transport. */
typedef struct {
    const sPackCmdBound *bounds;
    uint32_t             cmdsMax;   /* PACK_CMD_BIT set the type can offer   */
    uint8_t              count;
} sPackTypeLimits;

/* Exported functions -------------------------------------------------------*/

/**
 * @brief  Parse one configuration document.
 *
 * WHEN IMPLEMENTED it must stream from @p src in arbitrary chunk sizes (the
 * HTTP body cursor decides them, not this parser) and reject, naming the
 * offending pack index and key in @p res:
 *   - any UNKNOWN KEY, at document or pack level — the useful failure mode,
 *     and what makes a stale config fail by name rather than half-apply;
 *   - a missing `name`, `type` or `bind`;
 *   - a `name` longer than PACK_CFG_NAME_MAX_CHARS, empty, or carrying a
 *     character outside [A-Za-z0-9_-];
 *   - a DUPLICATE `name` — name is the identity, so two of them is not a
 *     configuration, it is two configurations;
 *   - a `type` no blueprint claims;
 *   - a `bind` token that is empty, longer than PACK_BIND_LEN - 1, or carries
 *     a control character.  THE CORE CHECKS NOTHING ELSE ABOUT IT: bind is
 *     opaque and the TYPE parses it (§12), so "rs485:99" parses here and
 *     fails at bind time with packWhy_noBinding, which is the honest place;
 *   - a `commands` bound WIDER than the type's — a 422, never a silent clamp.
 *     This is where the answer to "who may disconnect this battery" belongs;
 *   - more than PACK_MAX packs;
 *   - `cells` above PACK_CELLS_MAX;
 *   - a `chemistry` outside ePackChemistry.
 *
 * @param  src - streaming byte source; returns bytes read, 0 at end, < 0 on
 *               a transport error
 * @param  srcCtx - passed to @p src
 * @param  out - the parsed document; touched only on success
 * @param  res - filled on success AND on failure
 * @retval packErr_ok, packErr_badArg.  res->ok carries the same verdict.
 * @note   The caller's task (http).  MAY BLOCK inside @p src.
 */
int PackCfg_Parse(fPackByteSource src, void *srcCtx, sPackCfg *out,
                  sPackCfgResult *res);

/**
 * @brief  Re-serialise a parsed configuration as the §12 JSON.
 *
 * Data-faithful, not byte-identical: the document that comes back out must
 * parse to an identical sPackCfg, which is what the round-trip test asserts.
 *
 * @param  cfg - the configuration
 * @param  sink - streaming byte sink; returns < 0 to abort
 * @param  ctx - passed to @p sink
 * @retval packErr_ok, packErr_badArg, packErr_transport when the sink refused
 * @note   The caller's task (http).  MAY BLOCK inside @p sink.
 */
int PackCfg_Serialize(const sPackCfg *cfg, fPackByteSink sink, void *ctx);

/**
 * @brief  The static blueprint limits a `commands` block is checked against.
 *
 * WHEN IMPLEMENTED this is a table in pack_cfg.c, not a call into a
 * registered sPackType: the parser runs on the HTTP task against a document
 * that may name a type this board has no instance of, and a 422 must not
 * depend on what happens to be bound.
 *
 * @param  typeId - ePackTypeId
 * @retval the limits, or NULL when no such type
 * @note   Pure.  Any task.
 */
const sPackTypeLimits *PackCfg_TypeLimits(uint8_t typeId);

/**
 * @brief  Map the config key ("jkbms", "pylontech") to an ePackTypeId.
 * @param  name - NUL-terminated config key
 * @param  out - the id, written only on success
 * @retval packErr_ok, packErr_notFound, packErr_badArg
 * @note   Pure.  Any task.
 */
int PackCfg_TypeIdFromName(const char *name, uint8_t *out);

/**
 * @brief  The config key for an ePackTypeId — the serialiser's half.
 * @param  typeId - ePackTypeId
 * @retval the key, or NULL when no such type
 * @note   Pure.  Any task.
 */
const char *PackCfg_TypeName(uint8_t typeId);

/**
 * @brief  Map "lfp" / "liion" / "lto" to an ePackChemistry.
 * @param  name - NUL-terminated config value
 * @param  out - the chemistry, written only on success
 * @retval packErr_ok, packErr_notFound, packErr_badArg
 * @note   Pure.  Any task.
 */
int PackCfg_ChemFromName(const char *name, uint8_t *out);

/**
 * @brief  The config value for an ePackChemistry.
 * @param  chem - ePackChemistry
 * @retval the value, or NULL when no such chemistry
 * @note   Pure.  Any task.
 */
const char *PackCfg_ChemName(uint8_t chem);

/**
 * @brief  Map a `commands` key ("chargeEnable", ...) to an ePackCmdId.
 * @param  name - NUL-terminated config key
 * @param  out - the id, written only on success
 * @retval packErr_ok, packErr_notFound, packErr_badArg
 * @note   Pure.  Any task.
 */
int PackCfg_CmdIdFromName(const char *name, ePackCmdId *out);

/**
 * @brief  The `commands` key for an ePackCmdId.
 * @param  cmd - ePackCmdId
 * @retval the key, or NULL when no such command
 * @note   Pure.  Any task.
 */
const char *PackCfg_CmdName(ePackCmdId cmd);

#ifdef __cplusplus
}
#endif

#endif /* PACK_CFG_H_ */
