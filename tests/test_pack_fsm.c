/**
 * Unit tests for App/Pack/pack_fsm.c — condition, staleness, per-group ages,
 * confidence, capability gating and command validation
 * (docs/design_battery_pack.md §10, §11.1, §13; test plan in
 * docs/task_battery_pack_tests.md).
 *
 * EVERY TEST IN THIS FILE IS EXPECTED TO FAIL until pack_fsm.c is
 * implemented.  That is the point: the file exists so the contract is
 * executable before the code is, and a stub that returned success would be
 * indistinguishable from a working module.
 *
 * WHAT IS DELIBERATELY ABSENT: consumer-sensed disconnection (§4).  Only
 * something that sees SEVERAL packs can sense a breaker, so there is nothing
 * in this module to assert about it, and a single-pack site cannot detect one
 * at all.  See the test plan.
 */

#include "test_util.h"

#include "App/Pack/pack.h"
#include "App/Pack/pack_fsm.h"
#include "App/Pack/pack_type.h"

#include <string.h>

/* The JK's budgets, so the numbers below mean something on a real bus. */
#define STALE_MS        15000u
#define CELL_STALE_MS   60000u

/* ============================================================================
 * Area 0 — the shapes the persisted and cross-module contracts depend on
 *
 * sPackState/sPackCells sizes are budgeted in §15, and the enums are
 * append-only because they are persisted (ePackTypeId, ePackChemistry) or
 * cross onto the HTTP surface (ePackErr).  A renumber here is silent and
 * expensive, which is exactly what a test is for.
 * ============================================================================ */

static void test_layout_is_as_budgeted(void)
{
    TEST_ASSERT(sizeof(sPackState) == 132u);
    TEST_ASSERT(sizeof(sPackCells) == 80u);

    /* Persisted in the pack configuration: never renumbered, append only. */
    TEST_ASSERT((int)packType_jkBms     == 0);
    TEST_ASSERT((int)packType_pylontech == 1);
    TEST_ASSERT((int)packType_last      == 2);
    TEST_ASSERT((int)packChem_lfp   == 0);
    TEST_ASSERT((int)packChem_liIon == 1);
    TEST_ASSERT((int)packChem_lto   == 2);

    /* Crosses into consumers and onto HTTP: append-only, no _undefined = 0. */
    TEST_ASSERT((int)packErr_ok             ==  0);
    TEST_ASSERT((int)packErr_unknownOutcome == -8);
    TEST_ASSERT((int)packErr_transport      == -12);

    /* PACK_AGE_NEVER must not collide with a plausible age. */
    TEST_ASSERT(PACK_AGE_NEVER == 0xFFFFFFFFu);

    /* A command is one id, a capability set is a mask, and PACK_CMD_BIT is
     * the only bridge between them. */
    TEST_ASSERT(PACK_CMD_BIT(packCmd_chargeEnable) == 1u);
    TEST_ASSERT(PACK_CMD_BIT(packCmd_dischargeLimit) == 16u);
    TEST_ASSERT((int)packCmd_last == 5);
    TEST_ASSERT((int)packGrp_last == 8);

    /* §13: the module's internal event ids are exactly the eight listed, and
     * the count is what func.c sizes the range by. */
    TEST_ASSERT(PACK_EVT_COUNT == 8u);
}

/* ============================================================================
 * Area 1 — condition transitions and the wall clock (§13)
 * ============================================================================ */

static void test_starts_absent_never_seen(void)
{
    sPackFsm fsm;

    PackFsm_Init(&fsm, STALE_MS, CELL_STALE_MS, 0u);

    TEST_ASSERT(fsm.cond == (uint8_t)packCond_absent);

    /* PACK_AGE_NEVER, not 0: "configured, has never answered" and "answered
     * this instant" are different facts and this is the ONE mechanism that
     * separates them. */
    TEST_ASSERT(PackFsm_AgeMs(&fsm, packGrp_electrical, 0u) == PACK_AGE_NEVER);
    TEST_ASSERT(PackFsm_AgeMs(&fsm, packGrp_electrical, 999999u) ==
                PACK_AGE_NEVER);
    TEST_ASSERT(PackFsm_AgeMs(&fsm, packGrp_cells, 0u) == PACK_AGE_NEVER);
}

static void test_absent_to_online_to_stale(void)
{
    sPackFsm       fsm;
    ePackCondition from = packCond_last;
    ePackCondition to   = packCond_last;

    PackFsm_Init(&fsm, STALE_MS, CELL_STALE_MS, 0u);

    /* A publish is EVIDENCE, not a transition: recording it must not move
     * the condition by itself. */
    (void)PackFsm_NotePublish(&fsm, PACK_GRP_BIT(packGrp_electrical), 1000u);
    TEST_ASSERT(fsm.cond == (uint8_t)packCond_absent);

    /* The tick is what decides.  absent -> online. */
    TEST_ASSERT(PackFsm_Evaluate(&fsm, 1000u, &from, &to) == 1);
    TEST_ASSERT(from == packCond_absent);
    TEST_ASSERT(to   == packCond_online);
    TEST_ASSERT(fsm.cond == (uint8_t)packCond_online);

    /* Still online at exactly the budget: the budget is INCLUSIVE, so a 5 s
     * lap against a 15 s budget has no boundary surprise. */
    TEST_ASSERT(PackFsm_Evaluate(&fsm, 1000u + STALE_MS, NULL, NULL) == 0);
    TEST_ASSERT(fsm.cond == (uint8_t)packCond_online);

    /* One millisecond past it: online -> stale. */
    TEST_ASSERT(PackFsm_Evaluate(&fsm, 1000u + STALE_MS + 1u, &from, &to) == 1);
    TEST_ASSERT(from == packCond_online);
    TEST_ASSERT(to   == packCond_stale);

    /* And it does not re-fire; a transition is an edge. */
    TEST_ASSERT(PackFsm_Evaluate(&fsm, 1000u + STALE_MS + 5000u, NULL, NULL)
                == 0);
}

/* THE test for §8/§13: SILENCE IS NOT AN EVENT.  A pack that stops answering
 * posts nothing, so an event-only task could never fire the timeout it exists
 * to enforce.  Time passing must change the AGE but never the CONDITION until
 * the tick runs. */
static void test_silence_produces_no_event_only_the_tick_fires(void)
{
    sPackFsm fsm;

    PackFsm_Init(&fsm, STALE_MS, CELL_STALE_MS, 0u);
    (void)PackFsm_NotePublish(&fsm, PACK_GRP_BIT(packGrp_electrical), 1000u);
    (void)PackFsm_Evaluate(&fsm, 1000u, NULL, NULL);
    TEST_ASSERT(fsm.cond == (uint8_t)packCond_online);

    /* An hour of silence.  Reading the age is a pure query and must not
     * mutate anything — so the condition is STILL online. */
    TEST_ASSERT(PackFsm_AgeMs(&fsm, packGrp_electrical, 1000u + 3600000u)
                == 3600000u);
    TEST_ASSERT(fsm.cond == (uint8_t)packCond_online);

    /* GroupsStale is a query too, and it already knows the truth — which is
     * why the condition transition genuinely needs its own decision point. */
    TEST_ASSERT((PackFsm_GroupsStale(&fsm, 1000u + 3600000u) &
                 PACK_GRP_BIT(packGrp_electrical)) != 0u);
    TEST_ASSERT(fsm.cond == (uint8_t)packCond_online);

    /* Only now. */
    TEST_ASSERT(PackFsm_Evaluate(&fsm, 1000u + 3600000u, NULL, NULL) == 1);
    TEST_ASSERT(fsm.cond == (uint8_t)packCond_stale);
}

/* "A pack NEVER returns to packCond_absent once it has answered" — absent
 * means "has never answered", and rewriting history would lose the very
 * distinction ePackAbsentReason exists to keep. */
static void test_stale_never_decays_back_to_absent(void)
{
    sPackFsm fsm;

    PackFsm_Init(&fsm, STALE_MS, CELL_STALE_MS, 0u);
    (void)PackFsm_NotePublish(&fsm, PACK_GRP_BIT(packGrp_electrical), 1000u);
    (void)PackFsm_Evaluate(&fsm, 1000u, NULL, NULL);
    (void)PackFsm_Evaluate(&fsm, 1000u + STALE_MS + 1u, NULL, NULL);
    TEST_ASSERT(fsm.cond == (uint8_t)packCond_stale);

    (void)PackFsm_Evaluate(&fsm, 1000u + (1000u * 3600u), NULL, NULL);
    TEST_ASSERT(fsm.cond == (uint8_t)packCond_stale);

    /* And it recovers on the next answer. */
    (void)PackFsm_NotePublish(&fsm, PACK_GRP_BIT(packGrp_electrical),
                              1000u + (1000u * 3600u));
    TEST_ASSERT(PackFsm_Evaluate(&fsm, 1000u + (1000u * 3600u), NULL, NULL)
                == 1);
    TEST_ASSERT(fsm.cond == (uint8_t)packCond_online);
}

/* A pack that answers with an exception is ALIVE.  NoteLiveness feeds
 * condition without faking a value — that is the whole reason it is not a
 * publish (§11.1). */
static void test_liveness_without_a_measurement(void)
{
    sPackFsm fsm;

    PackFsm_Init(&fsm, STALE_MS, CELL_STALE_MS, 0u);

    PackFsm_NoteLiveness(&fsm, 1, 500u);
    TEST_ASSERT(PackFsm_Evaluate(&fsm, 500u, NULL, NULL) == 1);
    TEST_ASSERT(fsm.cond == (uint8_t)packCond_online);

    /* But no VALUE group became fresh: the charge group is still untouched. */
    TEST_ASSERT(PackFsm_AgeMs(&fsm, packGrp_charge, 500u) == PACK_AGE_NEVER);

    /* A failed exchange records nothing — it is not evidence of a time, and
     * the wall clock already covers it. */
    PackFsm_NoteLiveness(&fsm, 0, 500u + STALE_MS);
    TEST_ASSERT(PackFsm_AgeMs(&fsm, packGrp_electrical, 500u + STALE_MS)
                == STALE_MS);
}

/* ============================================================================
 * Area 2 — per-group ages (§10.4, §11.1)
 * ============================================================================ */

static void test_age_zero_is_not_never(void)
{
    sPackFsm fsm;

    PackFsm_Init(&fsm, STALE_MS, CELL_STALE_MS, 0u);
    (void)PackFsm_NotePublish(&fsm, PACK_GRP_BIT(packGrp_electrical), 7000u);

    /* Delivered this instant. */
    TEST_ASSERT(PackFsm_AgeMs(&fsm, packGrp_electrical, 7000u) == 0u);
    TEST_ASSERT(PackFsm_AgeMs(&fsm, packGrp_electrical, 7000u) !=
                PACK_AGE_NEVER);
    TEST_ASSERT(PackFsm_AgeMs(&fsm, packGrp_electrical, 7250u) == 250u);
}

/* "The groups it did not name keep their previous age — which is what makes a
 * partial delivery VISIBLE instead of silent."  A Pylontech pack that got
 * four of its six frames must not have the other two's ages advanced. */
static void test_partial_publish_leaves_unnamed_groups_untouched(void)
{
    sPackFsm fsm;
    uint32_t firstSeen;

    PackFsm_Init(&fsm, STALE_MS, CELL_STALE_MS, 0u);

    firstSeen = PackFsm_NotePublish(&fsm,
                                    PACK_GRP_BIT(packGrp_electrical) |
                                    PACK_GRP_BIT(packGrp_charge), 1000u);
    /* Both are seen for the FIRST time here. */
    TEST_ASSERT(firstSeen == (PACK_GRP_BIT(packGrp_electrical) |
                              PACK_GRP_BIT(packGrp_charge)));

    /* A later, PARTIAL commit naming only the electrical group. */
    firstSeen = PackFsm_NotePublish(&fsm, PACK_GRP_BIT(packGrp_electrical),
                                    9000u);
    TEST_ASSERT(firstSeen == 0u);          /* nothing new, only a refresh   */

    TEST_ASSERT(PackFsm_AgeMs(&fsm, packGrp_electrical, 9000u) == 0u);
    TEST_ASSERT(PackFsm_AgeMs(&fsm, packGrp_charge,     9000u) == 8000u);

    /* Groups never named at all stay NEVER — they do not inherit a sibling's
     * freshness. */
    TEST_ASSERT(PackFsm_AgeMs(&fsm, packGrp_temperature, 9000u) ==
                PACK_AGE_NEVER);
    TEST_ASSERT(PackFsm_AgeMs(&fsm, packGrp_cells, 9000u) == PACK_AGE_NEVER);

    /* A no-op publish is a no-op. */
    TEST_ASSERT(PackFsm_NotePublish(&fsm, 0u, 12000u) == 0u);
    TEST_ASSERT(PackFsm_AgeMs(&fsm, packGrp_electrical, 12000u) == 3000u);
}

/* The cell group has ITS OWN CLOCK: on a JK it runs 4-5 s behind the
 * electrical group and is judged against cellStaleAfter_ms, not staleAfter_ms.
 * A pack whose cells lag by 20 s is perfectly healthy. */
static void test_cell_group_has_its_own_budget(void)
{
    sPackFsm fsm;

    PackFsm_Init(&fsm, STALE_MS, CELL_STALE_MS, 0u);
    (void)PackFsm_NotePublish(&fsm,
                              PACK_GRP_BIT(packGrp_electrical) |
                              PACK_GRP_BIT(packGrp_cells), 1000u);

    /* 20 s later: past the 15 s electrical budget, well inside the 60 s cell
     * budget.  The electrical group is stale, the cell group is not. */
    {
        uint32_t stale = PackFsm_GroupsStale(&fsm, 1000u + 20000u);
        TEST_ASSERT((stale & PACK_GRP_BIT(packGrp_electrical)) != 0u);
        TEST_ASSERT((stale & PACK_GRP_BIT(packGrp_cells)) == 0u);
    }

    /* 70 s later: both. */
    {
        uint32_t stale = PackFsm_GroupsStale(&fsm, 1000u + 70000u);
        TEST_ASSERT((stale & PACK_GRP_BIT(packGrp_electrical)) != 0u);
        TEST_ASSERT((stale & PACK_GRP_BIT(packGrp_cells)) != 0u);
    }
}

static void test_never_delivered_counts_as_stale(void)
{
    sPackFsm fsm;
    uint32_t stale;

    PackFsm_Init(&fsm, STALE_MS, CELL_STALE_MS, 0u);

    stale = PackFsm_GroupsStale(&fsm, 0u);
    TEST_ASSERT((stale & PACK_GRP_BIT(packGrp_electrical)) != 0u);
    TEST_ASSERT((stale & PACK_GRP_BIT(packGrp_charge)) != 0u);
    TEST_ASSERT((stale & PACK_GRP_BIT(packGrp_cells)) != 0u);
    TEST_ASSERT((stale & PACK_GRP_BIT(packGrp_vendorInfo)) != 0u);
}

/* ============================================================================
 * Area 3 — confidence (§10.6)
 *
 * "A pack going quiet loses confidence BEFORE it goes stale, and a type can
 * never claim more than its freshness earns."
 * ============================================================================ */

static void test_confidence_cap_decays_before_stale(void)
{
    sPackFsm fsm;
    uint16_t atQuarter;
    uint16_t atHalf;

    PackFsm_Init(&fsm, STALE_MS, CELL_STALE_MS, 0u);

    /* Never delivered: believe nothing. */
    TEST_ASSERT(PackFsm_ConfidenceCap_pm(&fsm, 0u) == 0u);

    (void)PackFsm_NotePublish(&fsm, PACK_GRP_BIT(packGrp_electrical), 1000u);

    /* Fresh: full. */
    TEST_ASSERT(PackFsm_ConfidenceCap_pm(&fsm, 1000u) == PACK_CONF_FULL_PM);

    /* Still full at exactly staleAfter/4. */
    atQuarter = PackFsm_ConfidenceCap_pm(&fsm,
                                         1000u + (STALE_MS / PACK_CONF_FULL_DIV));
    TEST_ASSERT(atQuarter == PACK_CONF_FULL_PM);

    /* Past the knee it falls, strictly monotonically, and it is ALREADY
     * below full while the pack is still online — that is the point. */
    atHalf = PackFsm_ConfidenceCap_pm(&fsm, 1000u + (STALE_MS / 2u));
    TEST_ASSERT(atHalf < PACK_CONF_FULL_PM);
    TEST_ASSERT(atHalf > 0u);

    /* At and past the budget: nothing. */
    TEST_ASSERT(PackFsm_ConfidenceCap_pm(&fsm, 1000u + STALE_MS) == 0u);
    TEST_ASSERT(PackFsm_ConfidenceCap_pm(&fsm, 1000u + STALE_MS + 9999u) == 0u);
}

/* ============================================================================
 * Area 4 — capability gating (§10.2)
 *
 * "A field group listed against a capability is meaningful only when that
 * capability is set; otherwise it reads zero and MEANS NOTHING."
 * ============================================================================ */

static void test_group_capability_mapping(void)
{
    /* The electrical group is what every battery has, and it is the liveness
     * group — so it is unconditional. */
    TEST_ASSERT(PackFsm_GroupCapMask(packGrp_electrical) == 0u);
    TEST_ASSERT(PackFsm_GroupIsMeaningful(0u, packGrp_electrical) == 1);

    TEST_ASSERT((PackFsm_GroupCapMask(packGrp_charge) &
                 (uint32_t)packCap_capacityAh) != 0u);
    TEST_ASSERT((PackFsm_GroupCapMask(packGrp_temperature) &
                 (uint32_t)packCap_temperatures) != 0u);
    TEST_ASSERT((PackFsm_GroupCapMask(packGrp_limits) &
                 (uint32_t)packCap_currentLimits) != 0u);
    TEST_ASSERT((PackFsm_GroupCapMask(packGrp_switches) &
                 (uint32_t)packCap_switchState) != 0u);
    TEST_ASSERT((PackFsm_GroupCapMask(packGrp_cells) &
                 (uint32_t)packCap_cellSummary) != 0u);
}

/* The Pylontech case, straight out of §5: capacity is ABSENT from the
 * implemented frame set, so an instance advertises no packCap_capacityAh and
 * remaining_mAh/capacity_mAh/soc must read as meaningless — which is
 * precisely why nameplate_mAh comes from configuration instead. */
static void test_pylontech_shaped_caps_gate_the_charge_group(void)
{
    uint32_t pylonCaps = (uint32_t)packCap_soh |
                         (uint32_t)packCap_temperatures |
                         (uint32_t)packCap_currentLimits |
                         (uint32_t)packCap_switchState |
                         (uint32_t)packCap_cellSummary;

    TEST_ASSERT(PackFsm_GroupIsMeaningful(pylonCaps, packGrp_charge) == 0);
    TEST_ASSERT(PackFsm_GroupIsMeaningful(pylonCaps, packGrp_temperature) == 1);
    TEST_ASSERT(PackFsm_GroupIsMeaningful(pylonCaps, packGrp_limits) == 1);
    TEST_ASSERT(PackFsm_GroupIsMeaningful(pylonCaps, packGrp_electrical) == 1);

    /* No cell DETAIL: the summary is meaningful, the per-cell array is not —
     * which is what Pack_GetCells answers packErr_notSupported to. */
    TEST_ASSERT(PackFsm_GroupIsMeaningful(pylonCaps, packGrp_cells) == 1);
    TEST_ASSERT((pylonCaps & (uint32_t)packCap_cellDetail) == 0u);
}

/* ============================================================================
 * Area 5 — the absent reason (§10.5)
 *
 * Without it, "unreachable", "no device matches that address" and "this
 * firmware has no such type" all render identically as `absent`, and they are
 * three different call-outs plus a configuration error.
 * ============================================================================ */

static void test_absent_reason_no_type(void)
{
    sPackFsm fsm;

    PackFsm_Init(&fsm, STALE_MS, CELL_STALE_MS, 0u);
    PackFsm_SetBindReason(&fsm, packWhy_noType);

    (void)PackFsm_Evaluate(&fsm, 1000u, NULL, NULL);
    TEST_ASSERT(fsm.cond == (uint8_t)packCond_absent);
    TEST_ASSERT(fsm.why  == (uint8_t)packWhy_noType);
}

static void test_absent_reason_no_binding(void)
{
    sPackFsm fsm;

    PackFsm_Init(&fsm, STALE_MS, CELL_STALE_MS, 0u);
    /* bindKey resolved to zero devices, or to several. */
    PackFsm_SetBindReason(&fsm, packWhy_noBinding);

    (void)PackFsm_Evaluate(&fsm, 1000u, NULL, NULL);
    TEST_ASSERT(fsm.why == (uint8_t)packWhy_noBinding);
}

/* "Bound, but no live plan covers it" is a THIRD thing, and §8 records that a
 * config swap can leave a subscribed device with no armed timers while
 * `polled: true` still reads true — so this reason is not hypothetical. */
static void test_absent_reason_not_polled(void)
{
    sPackFsm fsm;

    PackFsm_Init(&fsm, STALE_MS, CELL_STALE_MS, 0u);
    PackFsm_SetBindReason(&fsm, packWhy_notPolled);

    (void)PackFsm_Evaluate(&fsm, 1000u, NULL, NULL);
    TEST_ASSERT(fsm.why == (uint8_t)packWhy_notPolled);
}

/* A sound binding and a silent pack: noReply, and it survives the trip
 * through online and back out to stale. */
static void test_absent_reason_no_reply_and_none_when_online(void)
{
    sPackFsm fsm;

    PackFsm_Init(&fsm, STALE_MS, CELL_STALE_MS, 0u);
    PackFsm_SetBindReason(&fsm, packWhy_none);   /* the binding is fine */

    (void)PackFsm_Evaluate(&fsm, 1000u, NULL, NULL);
    TEST_ASSERT(fsm.cond == (uint8_t)packCond_absent);
    TEST_ASSERT(fsm.why  == (uint8_t)packWhy_noReply);

    (void)PackFsm_NotePublish(&fsm, PACK_GRP_BIT(packGrp_electrical), 2000u);
    (void)PackFsm_Evaluate(&fsm, 2000u, NULL, NULL);
    TEST_ASSERT(fsm.cond == (uint8_t)packCond_online);
    TEST_ASSERT(fsm.why  == (uint8_t)packWhy_none);

    (void)PackFsm_Evaluate(&fsm, 2000u + STALE_MS + 1u, NULL, NULL);
    TEST_ASSERT(fsm.cond == (uint8_t)packCond_stale);
    TEST_ASSERT(fsm.why  == (uint8_t)packWhy_noReply);
}

/* ============================================================================
 * Area 6 — command validation: the eight refusal paths of §10.9
 * ============================================================================ */

/* An operator-narrowed set: chargeEnable and chargeLimit only.  Note
 * dischargeEnable is deliberately NOT advertised — "it is where the answer to
 * 'who may disconnect this battery' belongs". */
static const sPackCmdBound BOUNDS[] = {
    { 0,      1, packCmd_chargeEnable  },   /* boolean, bounds-checked {0,1} */
    { 0, 200000, packCmd_chargeLimit   },   /* mA                            */
};

static void ctx_init(sPackCmdCtx *ctx)
{
    (void)memset(ctx, 0, sizeof(*ctx));
    ctx->cmds        = PACK_CMD_BIT(packCmd_chargeEnable) |
                       PACK_CMD_BIT(packCmd_chargeLimit);
    ctx->bounds      = BOUNDS;
    ctx->boundCount  = (uint8_t)(sizeof(BOUNDS) / sizeof(BOUNDS[0]));
    ctx->cond        = (uint8_t)packCond_online;
    ctx->inFlight    = 0u;
    ctx->provisioned = 1u;
}

static void test_command_accepted_when_everything_holds(void)
{
    sPackCmdCtx  ctx;
    sPackCommand cmd = { 1, packCmd_chargeEnable, "cli" };

    ctx_init(&ctx);
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) == packErr_ok);

    /* THERE IS NO CLAMPING, so the exact endpoints must pass rather than be
     * nudged: min and max are inclusive. */
    cmd.cmd   = packCmd_chargeLimit;
    cmd.value = 0;
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) == packErr_ok);
    cmd.value = 200000;
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) == packErr_ok);
}

static void test_command_rejects_bad_arguments(void)
{
    sPackCmdCtx  ctx;
    sPackCommand cmd = { 1, packCmd_chargeEnable, "cli" };

    ctx_init(&ctx);

    TEST_ASSERT(PackFsm_ValidateCommand(NULL, &cmd, 5000u) == packErr_badArg);
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, NULL, 5000u) == packErr_badArg);

    /* An id off the end of the dense range. */
    cmd.cmd = packCmd_last;
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) == packErr_badArg);
    cmd.cmd = (ePackCmdId)99;
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) == packErr_badArg);
}

/* "A COMMAND IS ONE ID (so a multi-bit value cannot pass validation), A
 * CAPABILITY SET IS A MASK.  PACK_CMD_BIT is the only bridge."  Passing a
 * MASK where a dense id belongs must be refused by the id range check.
 *
 * CAVEAT recorded in the test plan: this only holds for masks that land
 * outside 0..packCmd_last-1.  The two-bit mask 0b011 IS the legal id 3
 * (packCmd_chargeLimit), so the id range is what does the work, not the bit
 * count. */
static void test_a_capability_mask_cannot_pass_as_a_command_id(void)
{
    sPackCmdCtx  ctx;
    sPackCommand cmd = { 1, packCmd_chargeEnable, "cli" };

    ctx_init(&ctx);

    cmd.cmd = (ePackCmdId)(PACK_CMD_BIT(packCmd_chargeEnable) |
                           PACK_CMD_BIT(packCmd_chargeLimit));   /* 9 */
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) == packErr_badArg);

    cmd.cmd = (ePackCmdId)(PACK_CMD_BIT(packCmd_dischargeLimit));  /* 16 */
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) == packErr_badArg);
}

/* "timeout_ms is 1..PACK_CMD_TIMEOUT_MAX_MS; 0 IS REJECTED — an unbounded
 * deadline is the same as no guarantee." */
static void test_command_rejects_zero_and_oversized_timeout(void)
{
    sPackCmdCtx  ctx;
    sPackCommand cmd = { 1, packCmd_chargeEnable, "cli" };

    ctx_init(&ctx);

    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 0u) == packErr_badArg);
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 1u) == packErr_ok);
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, PACK_CMD_TIMEOUT_MAX_MS)
                == packErr_ok);
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd,
                                        PACK_CMD_TIMEOUT_MAX_MS + 1u)
                == packErr_badArg);
}

/* Rule 1: refused unless advertised, BEFORE anything reaches a wire. */
static void test_command_refused_unless_advertised(void)
{
    sPackCmdCtx  ctx;
    sPackCommand cmd = { 0, packCmd_dischargeEnable, "http" };

    ctx_init(&ctx);
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) ==
                packErr_notSupported);

    /* A read-only pack advertises nothing at all. */
    ctx.cmds = 0u;
    cmd.cmd  = packCmd_chargeEnable;
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) ==
                packErr_notSupported);
}

/* §16 item 10's failure, caught: a command advertised with no bound behind it
 * is not really supported. */
static void test_command_advertised_without_a_bound_is_not_supported(void)
{
    sPackCmdCtx  ctx;
    sPackCommand cmd = { 1, packCmd_balanceEnable, "cli" };

    ctx_init(&ctx);
    ctx.cmds |= PACK_CMD_BIT(packCmd_balanceEnable);   /* but no BOUNDS entry */

    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) ==
                packErr_notSupported);
    TEST_ASSERT(PackFsm_FindBound(BOUNDS, ctx.boundCount, packCmd_balanceEnable)
                == NULL);
    TEST_ASSERT(PackFsm_FindBound(BOUNDS, ctx.boundCount, packCmd_chargeLimit)
                != NULL);
}

/* Rule 2: refused unless in range.  THERE IS NO CLAMPING. */
static void test_command_bounds_are_checked_not_clamped(void)
{
    sPackCmdCtx  ctx;
    sPackCommand cmd = { 0, packCmd_chargeLimit, "cluster" };

    ctx_init(&ctx);

    cmd.value = -1;
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) ==
                packErr_outOfRange);
    cmd.value = 200001;
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) ==
                packErr_outOfRange);
}

/* Booleans are bounds-checked {0, 1} like anything else, so a stray 2 for
 * chargeEnable is refused by the same rule rather than by a special case. */
static void test_booleans_are_bounds_checked(void)
{
    sPackCmdCtx  ctx;
    sPackCommand cmd = { 0, packCmd_chargeEnable, "cli" };

    ctx_init(&ctx);

    cmd.value = 0;
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) == packErr_ok);
    cmd.value = 1;
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) == packErr_ok);
    cmd.value = 2;
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) ==
                packErr_outOfRange);
    cmd.value = -1;
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) ==
                packErr_outOfRange);
}

/* Rule 3: commanding an absent or stale pack is a PROGRAMMING ERROR, not a
 * retry. */
static void test_command_refused_when_not_online(void)
{
    sPackCmdCtx  ctx;
    sPackCommand cmd = { 1, packCmd_chargeEnable, "cluster" };

    ctx_init(&ctx);

    ctx.cond = (uint8_t)packCond_stale;
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) ==
                packErr_notOnline);

    ctx.cond = (uint8_t)packCond_absent;
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) ==
                packErr_notOnline);
}

/* Rule 4: one command in flight per pack.  "Queueing would make 'what state
 * is this pack in' unanswerable." */
static void test_command_refused_when_one_is_in_flight(void)
{
    sPackCmdCtx  ctx;
    sPackCommand cmd = { 1, packCmd_chargeEnable, "cli" };

    ctx_init(&ctx);
    ctx.inFlight = 1u;
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) == packErr_busy);
}

static void test_command_refused_when_unprovisioned(void)
{
    sPackCmdCtx  ctx;
    sPackCommand cmd = { 1, packCmd_chargeEnable, "cli" };

    ctx_init(&ctx);
    ctx.provisioned = 0u;
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) ==
                packErr_unprovisioned);
}

/* The refusal ORDER is part of the contract: a caller must learn the most
 * specific reason, so "not advertised" beats "not online" and both beat
 * "busy". */
static void test_refusal_order_is_most_specific_first(void)
{
    sPackCmdCtx  ctx;
    sPackCommand cmd = { 1, packCmd_dischargeEnable, "cli" };

    ctx_init(&ctx);
    ctx.cond     = (uint8_t)packCond_stale;
    ctx.inFlight = 1u;

    /* Unadvertised, offline AND busy: the unadvertised answer wins. */
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) ==
                packErr_notSupported);

    /* Advertised but offline and busy: offline wins. */
    cmd.cmd = packCmd_chargeEnable;
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) ==
                packErr_notOnline);

    /* Online and busy: busy. */
    ctx.cond = (uint8_t)packCond_online;
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 5000u) == packErr_busy);

    /* A bad argument beats everything, because it is a caller bug. */
    TEST_ASSERT(PackFsm_ValidateCommand(&ctx, &cmd, 0u) == packErr_badArg);
}

/* ============================================================================
 * Area 7 — the in-flight command slot: unknownOutcome and late completions
 * (§10.9 rules 5-8, §11.1)
 * ============================================================================ */

static void test_claim_and_normal_completion(void)
{
    sPackCmdSlot slot;
    sPackCommand cmd = { 1, packCmd_chargeEnable, "cli" };
    uint32_t     seq = 0u;
    ePackErr     deliver = packErr_badArg;

    (void)memset(&slot, 0, sizeof(slot));

    TEST_ASSERT(PackFsm_CmdClaim(&slot, &cmd, 5000u, 1000u, &seq) ==
                packErr_ok);
    TEST_ASSERT(slot.inFlight == 1u);
    TEST_ASSERT(slot.deadline_ms == 6000u);
    TEST_ASSERT(slot.value == 1);
    TEST_ASSERT(slot.cmd == (uint8_t)packCmd_chargeEnable);

    /* A second claim is refused — one in flight per pack. */
    TEST_ASSERT(PackFsm_CmdClaim(&slot, &cmd, 5000u, 1100u, NULL) ==
                packErr_busy);

    /* The type answers in time: live, delivered, slot released. */
    TEST_ASSERT(PackFsm_CmdComplete(&slot, seq, packErr_ok, &deliver) == 1);
    TEST_ASSERT(deliver == packErr_ok);
    TEST_ASSERT(slot.inFlight == 0u);
}

/* "The last four are produced by a TYPE": a pack that answered and rejected
 * the write is packErr_refused, a wire failure is packErr_transport, and both
 * travel through unchanged.  The core does not reinterpret them. */
static void test_type_results_travel_through_unchanged(void)
{
    sPackCmdSlot slot;
    sPackCommand cmd = { 1, packCmd_chargeEnable, "cli" };
    uint32_t     seq = 0u;
    ePackErr     deliver = packErr_badArg;

    (void)memset(&slot, 0, sizeof(slot));
    (void)PackFsm_CmdClaim(&slot, &cmd, 5000u, 1000u, &seq);
    TEST_ASSERT(PackFsm_CmdComplete(&slot, seq, packErr_refused, &deliver) == 1);
    TEST_ASSERT(deliver == packErr_refused);

    (void)memset(&slot, 0, sizeof(slot));
    (void)PackFsm_CmdClaim(&slot, &cmd, 5000u, 1000u, &seq);
    TEST_ASSERT(PackFsm_CmdComplete(&slot, seq, packErr_transport, &deliver)
                == 1);
    TEST_ASSERT(deliver == packErr_transport);
}

/* Rule 5, the safety-critical one: "A command outstanding when the pack goes
 * stale completes packErr_unknownOutcome — NEVER 'assume it obeyed'." */
static void test_stale_mid_command_is_unknown_outcome(void)
{
    sPackCmdSlot slot;
    sPackCommand cmd = { 0, packCmd_chargeEnable, "cluster" };
    uint32_t     seq = 0u;
    ePackErr     result = packErr_ok;

    (void)memset(&slot, 0, sizeof(slot));
    TEST_ASSERT(PackFsm_CmdClaim(&slot, &cmd, 30000u, 1000u, &seq) ==
                packErr_ok);

    /* Still online and inside the deadline: nothing to expire. */
    TEST_ASSERT(PackFsm_CmdExpire(&slot, packCond_online, 0, 2000u, &result)
                == 0);
    TEST_ASSERT(slot.inFlight == 1u);

    /* The pack goes stale WELL BEFORE the command's own deadline.  The
     * outcome is unknown, not a timeout — the distinction is the whole
     * reason packErr_unknownOutcome exists. */
    TEST_ASSERT(PackFsm_CmdExpire(&slot, packCond_stale, 0, 5000u, &result)
                == 1);
    TEST_ASSERT(result == packErr_unknownOutcome);
    TEST_ASSERT(slot.inFlight == 0u);
}

/* Rule 8: a configuration change — this module's or the Modbus module's —
 * completes it packErr_unknownOutcome.  The Modbus module completes its own
 * outstanding requests against the OLD generation, which is exactly why this
 * module cannot wait for that answer and must decide for itself. */
static void test_config_change_mid_command_is_unknown_outcome(void)
{
    sPackCmdSlot slot;
    sPackCommand cmd = { 1, packCmd_chargeLimit, "http" };
    uint32_t     seq = 0u;
    ePackErr     result = packErr_ok;

    (void)memset(&slot, 0, sizeof(slot));
    (void)PackFsm_CmdClaim(&slot, &cmd, 30000u, 1000u, &seq);

    /* Online, inside the deadline — and the binding just vanished. */
    TEST_ASSERT(PackFsm_CmdExpire(&slot, packCond_online, 1, 2000u, &result)
                == 1);
    TEST_ASSERT(result == packErr_unknownOutcome);
    TEST_ASSERT(slot.inFlight == 0u);
}

/* A type that simply never answers gets packErr_timeout, not unknownOutcome:
 * the pack was online throughout, so the outcome is "it did not complete",
 * which is a different fact. */
static void test_deadline_expiry_is_a_timeout(void)
{
    sPackCmdSlot slot;
    sPackCommand cmd = { 1, packCmd_chargeEnable, "cli" };
    uint32_t     seq = 0u;
    ePackErr     result = packErr_ok;

    (void)memset(&slot, 0, sizeof(slot));
    (void)PackFsm_CmdClaim(&slot, &cmd, 5000u, 1000u, &seq);

    TEST_ASSERT(PackFsm_CmdExpire(&slot, packCond_online, 0, 6000u, &result)
                == 0);                          /* at the deadline, not past */
    TEST_ASSERT(PackFsm_CmdExpire(&slot, packCond_online, 0, 6001u, &result)
                == 1);
    TEST_ASSERT(result == packErr_timeout);
    TEST_ASSERT(slot.inFlight == 0u);

    /* Nothing to expire twice — `done` fires exactly once (rule 6). */
    TEST_ASSERT(PackFsm_CmdExpire(&slot, packCond_online, 0, 60000u, &result)
                == 0);
}

/* §11.1: "A completion for a command the core has ALREADY FINISHED is
 * DISCARDED AND COUNTED.  A late answer must never be written into a decision
 * the consumer has already been told about." */
static void test_late_completion_is_discarded(void)
{
    sPackCmdSlot slot;
    sPackCommand cmd = { 1, packCmd_chargeEnable, "cli" };
    uint32_t     seq = 0u;
    ePackErr     result = packErr_ok;
    ePackErr     deliver = packErr_badArg;

    (void)memset(&slot, 0, sizeof(slot));
    (void)PackFsm_CmdClaim(&slot, &cmd, 5000u, 1000u, &seq);

    /* The core gives up first. */
    TEST_ASSERT(PackFsm_CmdExpire(&slot, packCond_stale, 0, 3000u, &result)
                == 1);
    TEST_ASSERT(result == packErr_unknownOutcome);

    /* The type answers afterwards, carrying the seq it was claimed with.
     * LATE: discarded, and the slot is not resurrected. */
    TEST_ASSERT(PackFsm_CmdComplete(&slot, seq, packErr_ok, &deliver) == 0);
    TEST_ASSERT(deliver == packErr_badArg);     /* untouched */
    TEST_ASSERT(slot.inFlight == 0u);

    /* And a completion for a slot that was never claimed is late too. */
    (void)memset(&slot, 0, sizeof(slot));
    TEST_ASSERT(PackFsm_CmdComplete(&slot, 0u, packErr_ok, &deliver) == 0);
}

/* The generation is what makes a late answer recognisable rather than merely
 * unlikely: a SECOND command claimed after the first was abandoned must not
 * be completed by the first one's straggling answer. */
static void test_a_stale_generation_cannot_complete_the_next_command(void)
{
    sPackCmdSlot slot;
    sPackCommand cmd = { 1, packCmd_chargeEnable, "cli" };
    uint32_t     seqA = 0u;
    uint32_t     seqB = 0u;
    ePackErr     result = packErr_ok;
    ePackErr     deliver = packErr_badArg;

    (void)memset(&slot, 0, sizeof(slot));

    (void)PackFsm_CmdClaim(&slot, &cmd, 5000u, 1000u, &seqA);
    (void)PackFsm_CmdExpire(&slot, packCond_stale, 0, 3000u, &result);

    /* The pack comes back and a new command is issued. */
    cmd.value = 0;
    TEST_ASSERT(PackFsm_CmdClaim(&slot, &cmd, 5000u, 9000u, &seqB) ==
                packErr_ok);
    TEST_ASSERT(seqB != seqA);

    /* The FIRST command's answer finally arrives.  It must not land on the
     * second one. */
    TEST_ASSERT(PackFsm_CmdComplete(&slot, seqA, packErr_ok, &deliver) == 0);
    TEST_ASSERT(slot.inFlight == 1u);           /* B is still outstanding    */

    /* B completes normally. */
    TEST_ASSERT(PackFsm_CmdComplete(&slot, seqB, packErr_ok, &deliver) == 1);
    TEST_ASSERT(deliver == packErr_ok);
}

int main(void)
{
    printf("=== pack_fsm tests ===\n");

    RUN_TEST(test_layout_is_as_budgeted);

    RUN_TEST(test_starts_absent_never_seen);
    RUN_TEST(test_absent_to_online_to_stale);
    RUN_TEST(test_silence_produces_no_event_only_the_tick_fires);
    RUN_TEST(test_stale_never_decays_back_to_absent);
    RUN_TEST(test_liveness_without_a_measurement);

    RUN_TEST(test_age_zero_is_not_never);
    RUN_TEST(test_partial_publish_leaves_unnamed_groups_untouched);
    RUN_TEST(test_cell_group_has_its_own_budget);
    RUN_TEST(test_never_delivered_counts_as_stale);

    RUN_TEST(test_confidence_cap_decays_before_stale);

    RUN_TEST(test_group_capability_mapping);
    RUN_TEST(test_pylontech_shaped_caps_gate_the_charge_group);

    RUN_TEST(test_absent_reason_no_type);
    RUN_TEST(test_absent_reason_no_binding);
    RUN_TEST(test_absent_reason_not_polled);
    RUN_TEST(test_absent_reason_no_reply_and_none_when_online);

    RUN_TEST(test_command_accepted_when_everything_holds);
    RUN_TEST(test_command_rejects_bad_arguments);
    RUN_TEST(test_a_capability_mask_cannot_pass_as_a_command_id);
    RUN_TEST(test_command_rejects_zero_and_oversized_timeout);
    RUN_TEST(test_command_refused_unless_advertised);
    RUN_TEST(test_command_advertised_without_a_bound_is_not_supported);
    RUN_TEST(test_command_bounds_are_checked_not_clamped);
    RUN_TEST(test_booleans_are_bounds_checked);
    RUN_TEST(test_command_refused_when_not_online);
    RUN_TEST(test_command_refused_when_one_is_in_flight);
    RUN_TEST(test_command_refused_when_unprovisioned);
    RUN_TEST(test_refusal_order_is_most_specific_first);

    RUN_TEST(test_claim_and_normal_completion);
    RUN_TEST(test_type_results_travel_through_unchanged);
    RUN_TEST(test_stale_mid_command_is_unknown_outcome);
    RUN_TEST(test_config_change_mid_command_is_unknown_outcome);
    RUN_TEST(test_deadline_expiry_is_a_timeout);
    RUN_TEST(test_late_completion_is_discarded);
    RUN_TEST(test_a_stale_generation_cannot_complete_the_next_command);

    printf("%s (%d failures)\n", test_failures ? "FAILED" : "PASSED",
           test_failures);
    return test_failures ? 1 : 0;
}
