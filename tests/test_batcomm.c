/**
 * Unit tests for App/BatComm — the pure frame encoder and the configuration
 * parser (docs/design_battery_comm.md §4, §5).
 *
 * THE CENTRAL TEST IS AN IMITATION TEST.  `frame_reproduces_the_capture`
 * feeds the encoder the numbers the PowerBrick at zaliakalnis was carrying
 * when it was recorded and asserts the twelve frames it produces BYTE FOR
 * BYTE against the payloads that were on that wire
 * (reference_dyness_can_capture_2026-09-05.md §2).  That is the only evidence
 * available for a dialect nobody can re-measure without standing in front of
 * the cabinet, and it is what a host test is for: the alternative is
 * discovering a swapped field on a live inverter with a live battery.
 *
 * -Werror=switch is scoped onto batcomm_cfg.c for this target too, so a new
 * enumerator without a name fails HERE — where a developer looks first —
 * rather than after a cross-compile.
 */

#include "test_util.h"

#include "App/BatComm/batcomm.h"
#include "App/BatComm/batcomm_cfg.h"
#include "App/BatComm/batcomm_frame.h"

#include <string.h>

/* ============================================================================
 * Byte source / sink over plain memory
 * ============================================================================ */

typedef struct {
    const char *s;
    uint32_t    pos;
    uint32_t    len;
} sMemSrc;

static int mem_src(void *ctx, uint8_t *buf, uint32_t maxLen)
{
    sMemSrc *m = (sMemSrc *)ctx;
    uint32_t n = m->len - m->pos;

    if (n > maxLen) {
        n = maxLen;
    }
    if (n > 0u) {
        (void)memcpy(buf, &m->s[m->pos], n);
        m->pos += n;
    }
    return (int)n;
}

typedef struct {
    char     buf[1024];
    uint32_t len;
} sMemSink;

static int mem_sink(void *ctx, const char *data, uint32_t len)
{
    sMemSink *k = (sMemSink *)ctx;

    if ((k->len + len) >= sizeof(k->buf)) {
        return -1;
    }
    (void)memcpy(&k->buf[k->len], data, len);
    k->len += len;
    k->buf[k->len] = '\0';
    return (int)len;
}

static int parse(const char *doc, sBatCommCfg *cfg, sBatCommCfgResult *res)
{
    sMemSrc m = { doc, 0u, (uint32_t)strlen(doc) };

    return BatCommCfg_Parse(mem_src, &m, cfg, res);
}

/* ============================================================================
 * The capture, as an input
 *
 * The numbers the recorded device was carrying, expressed in this module's
 * units.  THE SCALES ARE THE STANDARD PYLONTECH ONES (0.1 A per current
 * count, 1 Ah per capacity count), settled by the modules' nameplate: 560 Ah,
 * which is the observation reference_dyness_can_capture_2026-09-05.md §4.2
 * names as the thing that decides it, and which overturns that document's
 * §3.2 argument for 0.01 A.
 * ============================================================================ */

static void capture_input(sBatCommIn *in)
{
    (void)memset(in, 0, sizeof(*in));
    in->voltage_mV            = 53010u;     /* 0x356: 5301 counts @ 0.01 V   */
    in->current_mA            = -40400;     /* 0x356: -404 counts @ 0.1 A    */
    in->chargeLimit_mA        = 280000u;    /* 0x351: 2800 counts            */
    in->dischargeLimit_mA     = 397600u;    /* 0x351: 3976 counts            */
    in->chargeVoltLimit_mV    = 56500u;     /* 0x351: 565 counts @ 0.1 V     */
    in->dischargeVoltLimit_mV = 48000u;     /* 0x351: 480 counts             */
    in->remaining_mAh         = 386000u;    /* 0x35F: 386 counts @ 1 Ah      */
    in->capacity_mAh          = 560000u;    /* 0x70D: 560 counts             */
    in->soc_pm                = 690u;       /* 0x355: 69 %                   */
    in->soh_pm                = 1000u;      /* 0x355: 100 %                  */
    in->tempMax_dC            = 227;        /* 0x356: 22.7 C                 */
    in->modules               = 2u;         /* 0x359 byte 4                  */
    in->chargeAllowed         = 1u;
    in->dischargeAllowed      = 1u;
    in->fields = (uint16_t)batField_voltage | (uint16_t)batField_current |
                 (uint16_t)batField_soc | (uint16_t)batField_soh |
                 (uint16_t)batField_chargeLimit |
                 (uint16_t)batField_dischargeLimit |
                 (uint16_t)batField_chargeVoltLimit |
                 (uint16_t)batField_dischargeVoltLimit |
                 (uint16_t)batField_temperature |
                 (uint16_t)batField_capacity |
                 (uint16_t)batField_switches;
}

static void default_tune(sBatCommTune *t)
{
    sBatCommCfg cfg;

    BatCommCfg_Defaults(&cfg);
    *t = cfg.tune;
}

/** Build one slot and check identifier, DLC and all eight payload bytes. */
static void expect_frame(uint8_t slot, uint32_t id, const char *hex)
{
    sBatCommIn    in;
    sBatCommTune  t;
    sBatCommFrame f;
    uint8_t       want[8];

    capture_input(&in);
    default_tune(&t);

    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, slot, &f) == 1);
    TEST_ASSERT(f.id == id);
    /* DLC 8 EVERYWHERE — the captured device pads every frame, and a short
     * DLC is the more likely thing for a picky receiver to reject. */
    TEST_ASSERT(f.dlc == 8u);
    TEST_ASSERT(hex2bin(hex, want, sizeof(want)) == 8u);
    TEST_ASSERT_MEM_EQ(f.data, want, 8u);
}

/* ============================================================================
 * The imitation
 * ============================================================================ */

static void test_frame_reproduces_the_capture(void)
{
    expect_frame(0u,  0x30Fu, "0000000000000000");
    expect_frame(1u,  0x351u, "3502F00A880FE001");
    expect_frame(2u,  0x355u, "4500640000000000");
    expect_frame(3u,  0x356u, "B5146CFEE3000000");
    expect_frame(4u,  0x359u, "0000000002000000");
    expect_frame(5u,  0x35Au, "0000000000000000");
    expect_frame(6u,  0x35Cu, "C000000000000000");
    expect_frame(7u,  0x35Eu, "44594E4553532D4C");
    expect_frame(8u,  0x35Fu, "2BA4EB0E82010000");
    expect_frame(9u,  0x370u, "44594E4553532D4C");
    expect_frame(10u, 0x371u, "2042415454455259");
    expect_frame(11u, 0x70Du, "640005001E013002");
}

static void test_frame_cycle_tail_is_empty(void)
{
    sBatCommIn    in;
    sBatCommTune  t;
    sBatCommFrame f;
    uint8_t       slot;

    capture_input(&in);
    default_tune(&t);

    /* Twelve frames at 50 ms spacing inside a 1 s window — NOT a burst, and
     * not a frame in every slot.  The empty tail is part of the imitation. */
    TEST_ASSERT(BatFrame_SlotCount(batProto_dynessLv) == 12u);
    for (slot = 12u; slot < 20u; slot++) {
        TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, slot, &f) == 0);
    }
}

static void test_frame_unknown_protocol_emits_nothing(void)
{
    sBatCommIn    in;
    sBatCommTune  t;
    sBatCommFrame f;

    capture_input(&in);
    default_tune(&t);

    TEST_ASSERT(BatFrame_SlotCount(batProto_none) == 0u);
    TEST_ASSERT(BatFrame_Build(batProto_none, &t, &in, 1u, &f) == 0);
}

/* ============================================================================
 * pylon_lv — the standard Pylontech set (design_can_bms_frame_source.md §4.1)
 *
 * NOT an imitation test: nothing was captured off a wire for this dialect, so
 * what is asserted is the standard layout, and — the part that can actually
 * regress — that it agrees with dyness_lv on every frame the two share.
 * ============================================================================ */

static void expect_pylon_frame(uint8_t slot, uint32_t id, const char *hex)
{
    sBatCommIn    in;
    sBatCommTune  t;
    sBatCommFrame f;
    uint8_t       want[8];

    capture_input(&in);
    default_tune(&t);

    TEST_ASSERT(BatFrame_Build(batProto_pylonLv, &t, &in, slot, &f) == 1);
    TEST_ASSERT(f.id == id);
    TEST_ASSERT(f.dlc == 8u);
    TEST_ASSERT(hex2bin(hex, want, sizeof(want)) == 8u);
    TEST_ASSERT_MEM_EQ(f.data, want, 8u);
}

static void test_pylon_frames_match_the_standard_layout(void)
{
    expect_pylon_frame(0u, 0x351u, "3502F00A880FE001");
    expect_pylon_frame(1u, 0x355u, "4500640000000000");
    expect_pylon_frame(2u, 0x356u, "B5146CFEE3000000");
    /* modules = 2, then the standard's 'P','N' that DYNESS does not send. */
    expect_pylon_frame(3u, 0x359u, "0000000002504E00");
    expect_pylon_frame(4u, 0x35Cu, "C000000000000000");
    /* "PYLON   " */
    expect_pylon_frame(5u, 0x35Eu, "50594C4F4E202020");
}

static void test_pylon_cycle_is_six_frames_then_empty(void)
{
    sBatCommIn    in;
    sBatCommTune  t;
    sBatCommFrame f;
    uint8_t       slot;

    capture_input(&in);
    default_tune(&t);

    TEST_ASSERT(BatFrame_SlotCount(batProto_pylonLv) == 6u);
    for (slot = 6u; slot < 20u; slot++) {
        TEST_ASSERT(BatFrame_Build(batProto_pylonLv, &t, &in, slot, &f) == 0);
    }
}

/** A Pylontech-model inverter has never been shown a DYNESS identifier, so
 *  none may leak into this dialect — and each of the six identifiers must
 *  appear exactly once per cycle. */
static void test_pylon_emits_only_its_six_identifiers(void)
{
    static const uint32_t want[6] = { 0x351u, 0x355u, 0x356u, 0x359u, 0x35Cu,
                                      0x35Eu };
    sBatCommIn    in;
    sBatCommTune  t;
    sBatCommFrame f;
    uint8_t       slot;

    capture_input(&in);
    default_tune(&t);

    for (slot = 0u; slot < 6u; slot++) {
        TEST_ASSERT(BatFrame_Build(batProto_pylonLv, &t, &in, slot, &f) == 1);
        TEST_ASSERT(f.id == want[slot]);
    }
}

/** The five shared frames are ONE encoder, and this is what holds it so:
 *  with a deliberately awkward input — discharging, an alarm raised, a
 *  forbidden charge direction, one module — both dialects must agree on
 *  0x351/0x355/0x356/0x35C to the byte, and on 0x359 everywhere but 'P','N'. */
static void test_pylon_agrees_with_dyness_on_the_shared_frames(void)
{
    static const uint8_t dynSlot[4] = { 1u, 2u, 3u, 6u };
    static const uint8_t pylSlot[4] = { 0u, 1u, 2u, 4u };
    sBatCommIn    in;
    sBatCommTune  t;
    sBatCommFrame d;
    sBatCommFrame p;
    int           i;

    capture_input(&in);
    default_tune(&t);
    in.current_mA       = -123456;
    in.alarms           = (uint32_t)packAlarm_cellUnderVoltage |
                          (uint32_t)packAlarm_overTemperature;
    in.chargeAllowed    = 0u;
    in.modules          = 1u;

    for (i = 0; i < 4; i++) {
        TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, dynSlot[i],
                                   &d) == 1);
        TEST_ASSERT(BatFrame_Build(batProto_pylonLv, &t, &in, pylSlot[i],
                                   &p) == 1);
        TEST_ASSERT(d.id == p.id);
        TEST_ASSERT(d.dlc == p.dlc);
        TEST_ASSERT_MEM_EQ(d.data, p.data, 8u);
    }

    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 4u, &d) == 1);
    TEST_ASSERT(BatFrame_Build(batProto_pylonLv, &t, &in, 3u, &p) == 1);
    TEST_ASSERT(d.id == p.id);
    TEST_ASSERT_MEM_EQ(d.data, p.data, 5u);         /* alarms + module count */
    TEST_ASSERT(d.data[5] == 0u);
    TEST_ASSERT(p.data[5] == (uint8_t)'P');
    TEST_ASSERT(p.data[6] == (uint8_t)'N');
    TEST_ASSERT(p.data[7] == 0u);
}

/** batcomm.h contract 3 holds for this dialect too, and withholding 0x351
 *  must not take the other five frames with it. */
static void test_pylon_withholds_limits_without_voltage_limits(void)
{
    sBatCommIn    in;
    sBatCommTune  t;
    sBatCommFrame f;

    capture_input(&in);
    default_tune(&t);

    in.fields &= (uint16_t)~(uint16_t)batField_chargeVoltLimit;
    TEST_ASSERT(BatFrame_Build(batProto_pylonLv, &t, &in, 0u, &f) == 0);
    TEST_ASSERT(BatFrame_Build(batProto_pylonLv, &t, &in, 1u, &f) == 1);
    TEST_ASSERT(BatFrame_Build(batProto_pylonLv, &t, &in, 5u, &f) == 1);
}

/* ============================================================================
 * Found on hardware 2026-10-04: booting straight into `bms` put an all-zero
 * 0x351 on the wire, because a pack advertises a capability from bind while
 * the register behind it arrives much later.  Two guards, both pinned here.
 * ============================================================================ */

/** A voltage limit of ZERO is invalid whatever the validity bit says — in both
 *  dialects, and for either limit.  Withheld, not zeroed. */
static void test_zero_voltage_limit_withholds_0x351(void)
{
    sBatCommIn    in;
    sBatCommTune  t;
    sBatCommFrame f;

    default_tune(&t);

    capture_input(&in);
    in.chargeVoltLimit_mV = 0u;
    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 1u, &f) == 0);
    TEST_ASSERT(BatFrame_Build(batProto_pylonLv,  &t, &in, 0u, &f) == 0);

    capture_input(&in);
    in.dischargeVoltLimit_mV = 0u;
    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 1u, &f) == 0);
    TEST_ASSERT(BatFrame_Build(batProto_pylonLv,  &t, &in, 0u, &f) == 0);

    /* ...and the other frames still go, as contract 3 says. */
    TEST_ASSERT(BatFrame_Build(batProto_pylonLv,  &t, &in, 1u, &f) == 1);
    TEST_ASSERT(BatFrame_Build(batProto_pylonLv,  &t, &in, 2u, &f) == 1);

    /* A genuine ZERO CURRENT limit is NOT invalid: "0 A" is the BMS saying
     * stop, and must reach the inverter. */
    capture_input(&in);
    in.chargeLimit_mA = 0u;
    TEST_ASSERT(BatFrame_Build(batProto_pylonLv,  &t, &in, 0u, &f) == 1);
    TEST_ASSERT(f.data[2] == 0u);
    TEST_ASSERT(f.data[3] == 0u);
}

static void ready_pack(sPackState *p)
{
    uint32_t g;

    (void)memset(p, 0, sizeof(*p));
    p->caps = (uint32_t)packCap_capacityAh | (uint32_t)packCap_soh |
              (uint32_t)packCap_temperatures | (uint32_t)packCap_currentLimits |
              (uint32_t)packCap_voltageLimits | (uint32_t)packCap_switchState;
    for (g = 0u; g < (uint32_t)packGrp_last; g++) {
        p->age_ms[g] = 100u;                /* delivered a moment ago        */
    }
}

/** `PACK_AGE_NEVER` is "not read yet", not "read as zero".  Every group the
 *  pack ADVERTISES must have been delivered before a frame is built from it —
 *  and a group it does not advertise must not hold the stream hostage. */
static void test_pack_ready_waits_for_every_advertised_group(void)
{
    sPackState p;

    TEST_ASSERT(BatFrame_PackReady(NULL) == 0);

    ready_pack(&p);
    TEST_ASSERT(BatFrame_PackReady(&p) == 1);

    ready_pack(&p);
    p.age_ms[packGrp_electrical] = PACK_AGE_NEVER;
    TEST_ASSERT(BatFrame_PackReady(&p) == 0);

    ready_pack(&p);
    p.age_ms[packGrp_charge] = PACK_AGE_NEVER;          /* SOC would read 0 % */
    TEST_ASSERT(BatFrame_PackReady(&p) == 0);

    ready_pack(&p);
    p.age_ms[packGrp_temperature] = PACK_AGE_NEVER;     /* would read 0.0 C   */
    TEST_ASSERT(BatFrame_PackReady(&p) == 0);

    ready_pack(&p);
    p.age_ms[packGrp_limits] = PACK_AGE_NEVER;          /* the hardware case  */
    TEST_ASSERT(BatFrame_PackReady(&p) == 0);

    ready_pack(&p);
    p.age_ms[packGrp_switches] = PACK_AGE_NEVER;
    TEST_ASSERT(BatFrame_PackReady(&p) == 0);

    /* NOT advertised -> not waited for. */
    ready_pack(&p);
    p.caps &= ~(uint32_t)packCap_temperatures;
    p.age_ms[packGrp_temperature] = PACK_AGE_NEVER;
    TEST_ASSERT(BatFrame_PackReady(&p) == 1);

    ready_pack(&p);
    p.caps &= ~((uint32_t)packCap_currentLimits | (uint32_t)packCap_voltageLimits);
    p.age_ms[packGrp_limits] = PACK_AGE_NEVER;
    TEST_ASSERT(BatFrame_PackReady(&p) == 1);

    /* Either limit capability alone is enough to require the group. */
    ready_pack(&p);
    p.caps &= ~(uint32_t)packCap_currentLimits;
    p.age_ms[packGrp_limits] = PACK_AGE_NEVER;
    TEST_ASSERT(BatFrame_PackReady(&p) == 0);
}

/* ============================================================================
 * The contracts
 * ============================================================================ */

/** batcomm.h contract 3: a missing voltage limit WITHHOLDS 0x351.  A zero CVL
 *  is not "no limit", it is "stop charging". */
static void test_frame_withholds_limits_without_voltage_limits(void)
{
    sBatCommIn    in;
    sBatCommTune  t;
    sBatCommFrame f;

    capture_input(&in);
    default_tune(&t);

    in.fields &= (uint16_t)~(uint16_t)batField_chargeVoltLimit;
    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 1u, &f) == 0);

    capture_input(&in);
    in.fields &= (uint16_t)~(uint16_t)batField_dischargeVoltLimit;
    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 1u, &f) == 0);

    /* Every OTHER frame still goes: the pack is talking, it simply cannot
     * state its voltage limits. */
    capture_input(&in);
    in.fields &= (uint16_t)~(uint16_t)batField_chargeVoltLimit;
    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 3u, &f) == 1);
    TEST_ASSERT(f.id == 0x356u);
}

/** Permission and limit agree, in the channel measured to be obeyed. */
static void test_frame_forbidden_direction_publishes_zero(void)
{
    sBatCommIn    in;
    sBatCommTune  t;
    sBatCommFrame f;

    capture_input(&in);
    default_tune(&t);
    in.chargeAllowed = 0u;

    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 1u, &f) == 1);
    TEST_ASSERT(f.data[2] == 0u);       /* charge current == 0               */
    TEST_ASSERT(f.data[3] == 0u);
    TEST_ASSERT(f.data[4] == 0x88u);    /* discharge untouched               */
    TEST_ASSERT(f.data[5] == 0x0Fu);
    /* The CVL is NOT zeroed with it: the pack still has a voltage limit, and
     * zeroing it would say something the pack did not. */
    TEST_ASSERT(f.data[0] == 0x35u);
    TEST_ASSERT(f.data[1] == 0x02u);

    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 6u, &f) == 1);
    TEST_ASSERT(f.data[0] == 0x40u);    /* 0x35C: discharge only             */
}

/** A limit whose validity bit is clear reads zero and MEANS NOTHING, so it is
 *  emitted as zero rather than as whatever happened to be in the struct. */
static void test_frame_invalid_limit_field_publishes_zero(void)
{
    sBatCommIn    in;
    sBatCommTune  t;
    sBatCommFrame f;

    capture_input(&in);
    default_tune(&t);
    in.fields &= (uint16_t)~(uint16_t)batField_dischargeLimit;

    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 1u, &f) == 1);
    TEST_ASSERT(f.data[4] == 0u);
    TEST_ASSERT(f.data[5] == 0u);
}

/** batcomm.h contract 1.  Negative means DISCHARGE on this wire, matching
 *  pack.h and both devices measured in the field, and contradicting the
 *  generic Pylontech document. */
static void test_frame_sign_is_negative_while_discharging(void)
{
    sBatCommIn    in;
    sBatCommTune  t;
    sBatCommFrame f;
    int16_t       i;

    capture_input(&in);
    default_tune(&t);

    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 3u, &f) == 1);
    i = (int16_t)((uint16_t)f.data[2] | ((uint16_t)f.data[3] << 8));
    TEST_ASSERT(i == -404);

    /* Charging is positive, with no other change. */
    in.current_mA = 40400;
    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 3u, &f) == 1);
    i = (int16_t)((uint16_t)f.data[2] | ((uint16_t)f.data[3] << 8));
    TEST_ASSERT(i == 404);
}

/** The one bit that would settle the unmeasured half of the sign question,
 *  and it lives HERE rather than in the cluster (design §7.2). */
static void test_frame_invert_current_flips_only_the_measurement(void)
{
    sBatCommIn    in;
    sBatCommTune  t;
    sBatCommFrame f;
    int16_t       i;

    capture_input(&in);
    default_tune(&t);
    t.invertCurrent = 1u;

    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 3u, &f) == 1);
    i = (int16_t)((uint16_t)f.data[2] | ((uint16_t)f.data[3] << 8));
    TEST_ASSERT(i == 404);

    /* THE LIMITS ARE UNSIGNED MAGNITUDES and must not move with it: a charge
     * limit arriving negative would be read as a very large positive by a
     * u16 receiver. */
    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 1u, &f) == 1);
    TEST_ASSERT(f.data[2] == 0xF0u);
    TEST_ASSERT(f.data[3] == 0x0Au);
}

/** SATURATE, NEVER WRAP.  A wrapped limit is the one arithmetic failure a
 *  receiver cannot detect. */
static void test_frame_saturates_rather_than_wraps(void)
{
    sBatCommIn    in;
    sBatCommTune  t;
    sBatCommFrame f;
    int16_t       i;

    capture_input(&in);
    default_tune(&t);
    in.chargeLimit_mA = 100000000u;         /* 100 kA -> 1e6 counts          */
    in.current_mA     = -1000000000;        /* -1 MA  -> -1e7 counts         */

    /* 0x351 SATURATES AT 32767, NOT 65535: three of its four fields are typed
     * i16 by the generic protocol, and a 40000-count limit read as i16 is a
     * large NEGATIVE current — the wrapping failure by another route. */
    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 1u, &f) == 1);
    TEST_ASSERT(f.data[2] == 0xFFu);
    TEST_ASSERT(f.data[3] == 0x7Fu);

    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 3u, &f) == 1);
    i = (int16_t)((uint16_t)f.data[2] | ((uint16_t)f.data[3] << 8));
    TEST_ASSERT(i == -32768);
}

/** SOC is TRUNCATED, not rounded: a rounded-up SOC is an over-report of the
 *  field the inverter's own charge decision reads. */
static void test_frame_soc_truncates(void)
{
    sBatCommIn    in;
    sBatCommTune  t;
    sBatCommFrame f;

    capture_input(&in);
    default_tune(&t);
    in.soc_pm = 699u;                       /* 69.9 %                        */

    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 2u, &f) == 1);
    TEST_ASSERT(f.data[0] == 69u);
    TEST_ASSERT(f.data[1] == 0u);
}

/** The 0x359 mapping, and the knob that turns it off — the bit layout is the
 *  generic one and is UNVERIFIED for this vendor. */
static void test_frame_alarms_map_and_can_be_suppressed(void)
{
    sBatCommIn    in;
    sBatCommTune  t;
    sBatCommFrame f;

    capture_input(&in);
    default_tune(&t);
    in.alarms = (uint32_t)packAlarm_cellOverVoltage |
                (uint32_t)packAlarm_overTemperature |
                (uint32_t)packAlarm_chargeOverCurrent |
                (uint32_t)packAlarm_cellImbalance;

    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 4u, &f) == 1);
    TEST_ASSERT(f.data[0] == (0x02u | 0x08u));
    TEST_ASSERT(f.data[1] == 0x01u);
    TEST_ASSERT(f.data[2] == 0u);
    TEST_ASSERT(f.data[3] == 0x08u);
    TEST_ASSERT(f.data[4] == 2u);           /* the module count survives      */
    /* Bytes 5-6 are the generic protocol's 'P','N' and the captured device
     * does NOT send them. */
    TEST_ASSERT(f.data[5] == 0u);
    TEST_ASSERT(f.data[6] == 0u);

    t.emitAlarms = 0u;
    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 4u, &f) == 1);
    TEST_ASSERT(f.data[0] == 0u);
    TEST_ASSERT(f.data[1] == 0u);
    TEST_ASSERT(f.data[3] == 0u);
    TEST_ASSERT(f.data[4] == 2u);
}

static void test_frame_module_count_floors_at_one(void)
{
    sBatCommIn    in;
    sBatCommTune  t;
    sBatCommFrame f;

    capture_input(&in);
    default_tune(&t);
    in.modules = 0u;

    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 4u, &f) == 1);
    TEST_ASSERT(f.data[4] == 1u);
}

/** The scale is a KNOB precisely because the argument it replaced was sound
 *  and lost to a single observation: settling it the other way must not need
 *  a firmware cycle on a board reached only through a tunnel. */
static void test_frame_scales_are_configuration(void)
{
    sBatCommIn    in;
    sBatCommTune  t;
    sBatCommFrame f;
    int16_t       i;

    capture_input(&in);
    default_tune(&t);
    t.currentScale_mA   = 10u;              /* 0.01 A per count              */
    t.capacityScale_mAh = 100u;             /* 0.1 Ah per count              */

    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 3u, &f) == 1);
    i = (int16_t)((uint16_t)f.data[2] | ((uint16_t)f.data[3] << 8));
    TEST_ASSERT(i == -4040);

    TEST_ASSERT(BatFrame_Build(batProto_dynessLv, &t, &in, 8u, &f) == 1);
    TEST_ASSERT(((uint16_t)f.data[4] | ((uint16_t)f.data[5] << 8)) == 3860u);
}

/* ============================================================================
 * Configuration
 * ============================================================================ */

/* The honest day-one zaliakalnis document: speak the dialect the Solis at
 * that site already accepts, on the cell the battery is NOT on, from the
 * cluster. */
static const char ZALIAKALNIS[] =
    "{\"version\":1,\"protocol\":\"dyness_lv\",\"peripheral\":\"can2\","
    "\"source\":\"cluster\"}";

static void test_cfg_accepts_the_zaliakalnis_document(void)
{
    sBatCommCfg       cfg;
    sBatCommCfgResult res;

    TEST_ASSERT(parse(ZALIAKALNIS, &cfg, &res) == batErr_ok);
    TEST_ASSERT(res.ok == 1);
    TEST_ASSERT(cfg.proto == (uint8_t)batProto_dynessLv);
    TEST_ASSERT(cfg.source == (uint8_t)batSrc_cluster);
    TEST_ASSERT(cfg.inverterBus == 1u);         /* can2                      */
    /* DEFAULT ENABLED: a document that says what to speak and where must not
     * silently do nothing. */
    TEST_ASSERT(cfg.enabled == 1u);
    TEST_ASSERT(cfg.fallback == (uint8_t)batFallback_bridge);
    TEST_ASSERT(cfg.batteryBusIsInput == 0u);
}

/* The sodas document: the Solis there is set to PYLON_LV (43009 = 1), so the
 * profile is matched to the inverter — never the inverter to the profile. */
static const char SODAS[] =
    "{\"version\":1,\"protocol\":\"pylon_lv\",\"peripheral\":\"can1\","
    "\"source\":\"pack\",\"pack\":\"sodas15\",\"enabled\":true,"
    "\"fallback\":\"bridge\",\"batteryBusIsInput\":false}";

static void test_cfg_accepts_the_sodas_document(void)
{
    sBatCommCfg       cfg;
    sBatCommCfgResult res;
    eBatCommProto     p;

    TEST_ASSERT(parse(SODAS, &cfg, &res) == batErr_ok);
    TEST_ASSERT(cfg.proto == (uint8_t)batProto_pylonLv);
    TEST_ASSERT(cfg.source == (uint8_t)batSrc_pack);
    TEST_ASSERT(strcmp(cfg.packName, "sodas15") == 0);
    TEST_ASSERT(cfg.inverterBus == 0u);             /* can1                  */
    TEST_ASSERT(cfg.enabled == 1u);
    TEST_ASSERT(cfg.fallback == (uint8_t)batFallback_bridge);
    TEST_ASSERT(cfg.batteryBusIsInput == 0u);

    TEST_ASSERT(strcmp(BatComm_ProtoName(batProto_pylonLv), "pylon_lv") == 0);
    TEST_ASSERT(BatComm_ProtoFromName("pylon_lv", &p) == batErr_ok);
    TEST_ASSERT(p == batProto_pylonLv);
}

static void test_cfg_defaults_are_applied_when_keys_are_absent(void)
{
    sBatCommCfg cfg;

    TEST_ASSERT(parse(ZALIAKALNIS, &cfg, NULL) == batErr_ok);
    TEST_ASSERT(cfg.tune.slot_ms           == BATCOMM_DFLT_SLOT_MS);
    TEST_ASSERT(cfg.tune.slots             == BATCOMM_DFLT_SLOTS);
    TEST_ASSERT(cfg.tune.currentScale_mA   == BATCOMM_DFLT_CURRENT_SCALE_MA);
    TEST_ASSERT(cfg.tune.capacityScale_mAh == BATCOMM_DFLT_CAPACITY_SCALE_MAH);
    TEST_ASSERT(cfg.tune.staleTrip         == BATCOMM_DFLT_STALE_TRIP);
    /* UNMEASURED, so off: both devices actually measured agree with pack.h. */
    TEST_ASSERT(cfg.tune.invertCurrent     == 0u);
    TEST_ASSERT(cfg.tune.emitAlarms        == 1u);
}

static void test_cfg_pack_source_needs_a_name(void)
{
    sBatCommCfg       cfg;
    sBatCommCfgResult res;

    TEST_ASSERT(parse("{\"protocol\":\"dyness_lv\",\"peripheral\":\"can1\","
                      "\"source\":\"pack\"}", &cfg, &res) == batErr_badArg);
    TEST_ASSERT(res.ok == 0);
    TEST_ASSERT(strcmp(res.field, "pack") == 0);

    TEST_ASSERT(parse("{\"protocol\":\"dyness_lv\",\"peripheral\":\"can1\","
                      "\"source\":\"pack\",\"pack\":\"bms1\"}",
                      &cfg, &res) == batErr_ok);
    TEST_ASSERT(cfg.source == (uint8_t)batSrc_pack);
    TEST_ASSERT(strcmp(cfg.packName, "bms1") == 0);
}

/** A named pack with source "cluster" is a CONTRADICTION, not a harmless
 *  leftover: it is what an operator writes when they edited one line of two. */
static void test_cfg_rejects_a_pack_name_on_a_cluster_source(void)
{
    sBatCommCfg       cfg;
    sBatCommCfgResult res;

    TEST_ASSERT(parse("{\"protocol\":\"dyness_lv\",\"peripheral\":\"can1\","
                      "\"source\":\"cluster\",\"pack\":\"bms1\"}",
                      &cfg, &res) == batErr_badArg);
    TEST_ASSERT(strcmp(res.field, "pack") == 0);
}

static void test_cfg_reject_matrix(void)
{
    sBatCommCfg       cfg;
    sBatCommCfgResult res;
    uint32_t          i;

    static const struct {
        const char *doc;
        const char *field;
    } cases[] = {
        /* the three required statements */
        { "{\"peripheral\":\"can1\",\"source\":\"cluster\"}",   "protocol" },
        { "{\"protocol\":\"dyness_lv\",\"peripheral\":\"can1\"}", "source" },
        /* a dialect this image does not carry must FAIL, not fall back.
         * pylon_hv is a real, different Pylontech dialect — which is exactly
         * the kind of near-miss name that must not be quietly accepted as
         * pylon_lv. */
        { "{\"protocol\":\"pylon_hv\",\"peripheral\":\"can1\","
          "\"source\":\"cluster\"}",                            "protocol" },
        /* there is no third cell */
        { "{\"protocol\":\"dyness_lv\",\"peripheral\":\"can3\","
          "\"source\":\"cluster\"}",                            "peripheral" },
        { "{\"protocol\":\"dyness_lv\",\"peripheral\":\"can1\","
          "\"source\":\"both\"}",                               "source" },
        { "{\"protocol\":\"dyness_lv\",\"peripheral\":\"can1\","
          "\"source\":\"cluster\",\"fallback\":\"maybe\"}",     "fallback" },
        /* the bridge itself refuses a source period below 50 ms */
        { "{\"protocol\":\"dyness_lv\",\"peripheral\":\"can1\","
          "\"source\":\"cluster\",\"tune\":{\"slot_ms\":10}}",  "slot_ms" },
        { "{\"protocol\":\"dyness_lv\",\"peripheral\":\"can1\","
          "\"source\":\"cluster\",\"tune\":{\"slots\":64}}",    "slots" },
        /* a cycle too short for the profile would silently drop the frames
         * that fell off the end */
        { "{\"protocol\":\"dyness_lv\",\"peripheral\":\"can1\","
          "\"source\":\"cluster\",\"tune\":{\"slots\":8}}",     "slots" },
        /* ...and the limit is the PROFILE's, not a constant: pylon_lv has
         * six frames, so five slots is too short and eight is not */
        { "{\"protocol\":\"pylon_lv\",\"peripheral\":\"can1\","
          "\"source\":\"cluster\",\"tune\":{\"slots\":5}}",     "slots" },
        { "{\"protocol\":\"dyness_lv\",\"peripheral\":\"can1\","
          "\"source\":\"cluster\","
          "\"tune\":{\"currentScale_mA\":0}}",         "currentScale_mA" },
        { "{\"protocol\":\"dyness_lv\",\"peripheral\":\"can1\","
          "\"source\":\"cluster\",\"tune\":{\"staleTrip\":0}}", "staleTrip" },
        /* UNKNOWN KEYS ARE REJECTED so a stale document fails by name
         * instead of losing the setting it thought it was making */
        { "{\"protocol\":\"dyness_lv\",\"peripheral\":\"can1\","
          "\"source\":\"cluster\",\"derate_pm\":800}",          "derate_pm" },
        { "{\"protocol\":\"dyness_lv\",\"peripheral\":\"can1\","
          "\"source\":\"cluster\",\"tune\":{\"riseRate\":1}}",  "riseRate" },
        /* a newer document is REFUSED, never reinterpreted through this
         * image's field meanings — a scale here multiplies a current */
        { "{\"version\":99,\"protocol\":\"dyness_lv\","
          "\"peripheral\":\"can1\",\"source\":\"cluster\"}",    "version" },
        { "{\"protocol\":\"dyness_lv\",\"peripheral\":\"can1\","
          "\"source\":\"pack\",\"pack\":\"has space\"}",        "pack" },
    };

    for (i = 0u; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        TEST_ASSERT(parse(cases[i].doc, &cfg, &res) == batErr_badArg);
        TEST_ASSERT(res.ok == 0);
        if (strcmp(res.field, cases[i].field) != 0) {
            printf("  case %u: field \"%s\", expected \"%s\"\n",
                   (unsigned)i, res.field, cases[i].field);
            TEST_ASSERT(0);
        }
    }
}

static void test_cfg_export_round_trips(void)
{
    sBatCommCfg a, b;
    sMemSink    sink;

    TEST_ASSERT(parse("{\"protocol\":\"dyness_lv\",\"peripheral\":\"can2\","
                      "\"source\":\"pack\",\"pack\":\"jk_a\","
                      "\"enabled\":false,\"fallback\":\"silent\","
                      "\"batteryBusIsInput\":true,"
                      "\"tune\":{\"slot_ms\":100,\"slots\":14,"
                      "\"currentScale_mA\":10,\"capacityScale_mAh\":100,"
                      "\"staleTrip\":5,\"invertCurrent\":1,"
                      "\"emitAlarms\":0}}", &a, NULL) == batErr_ok);

    sink.len = 0u;
    sink.buf[0] = '\0';
    TEST_ASSERT(BatCommCfg_Serialize(&a, mem_sink, &sink) == batErr_ok);
    TEST_ASSERT(parse(sink.buf, &b, NULL) == batErr_ok);

    /* Data-faithful, not byte-identical — so the fields are compared, not the
     * documents. */
    TEST_ASSERT(a.proto == b.proto);
    TEST_ASSERT(a.source == b.source);
    TEST_ASSERT(a.inverterBus == b.inverterBus);
    TEST_ASSERT(a.enabled == b.enabled);
    TEST_ASSERT(a.fallback == b.fallback);
    TEST_ASSERT(a.batteryBusIsInput == b.batteryBusIsInput);
    TEST_ASSERT(strcmp(a.packName, b.packName) == 0);
    TEST_ASSERT(memcmp(&a.tune, &b.tune, sizeof(a.tune)) == 0);
}

/** An unprovisioned board must not be able to reach a half-configuration by
 *  uploading an empty object. */
static void test_cfg_rejects_an_empty_document(void)
{
    sBatCommCfg       cfg;
    sBatCommCfgResult res;

    TEST_ASSERT(parse("{}", &cfg, &res) != batErr_ok);
}

/* ============================================================================
 * The name accessors
 * ============================================================================ */

static void test_names_cover_every_enumerator(void)
{
    int i;

    for (i = 0; i < (int)batProto_last; i++) {
        TEST_ASSERT(strcmp(BatComm_ProtoName((eBatCommProto)i), "?") != 0);
    }
    for (i = 0; i < (int)batSrc_last; i++) {
        TEST_ASSERT(strcmp(BatComm_SourceName((eBatCommSource)i), "?") != 0);
    }
    for (i = 0; i < (int)batFallback_last; i++) {
        TEST_ASSERT(strcmp(BatComm_FallbackName((eBatCommFallback)i),
                           "?") != 0);
    }
    for (i = 0; i < (int)batState_last; i++) {
        TEST_ASSERT(strcmp(BatComm_StateName((eBatCommState)i), "?") != 0);
    }
    for (i = 0; i < (int)batWhy_last; i++) {
        TEST_ASSERT(strcmp(BatComm_WhyName((eBatCommWhy)i), "?") != 0);
    }
    /* Out of range answers "?", never NULL: both adapters pass the result
     * straight into a %s. */
    TEST_ASSERT(strcmp(BatComm_StateName((eBatCommState)99), "?") == 0);
    TEST_ASSERT(strcmp(BatComm_WhyName((eBatCommWhy)99), "?") == 0);
}

/** Every name that reaches JSON must carry no quote or backslash, or the
 *  reply it lands in stops being parseable. */
static void test_names_are_json_safe(void)
{
    int i;

    for (i = 0; i < (int)batWhy_last; i++) {
        const char *s = BatComm_WhyName((eBatCommWhy)i);

        TEST_ASSERT(strchr(s, '"') == NULL);
        TEST_ASSERT(strchr(s, '\\') == NULL);
    }
    for (i = 0; i < (int)batState_last; i++) {
        const char *s = BatComm_StateName((eBatCommState)i);

        TEST_ASSERT(strchr(s, '"') == NULL);
        TEST_ASSERT(strchr(s, '\\') == NULL);
    }
}

int main(void)
{
    RUN_TEST(test_frame_reproduces_the_capture);
    RUN_TEST(test_frame_cycle_tail_is_empty);
    RUN_TEST(test_frame_unknown_protocol_emits_nothing);
    RUN_TEST(test_pylon_frames_match_the_standard_layout);
    RUN_TEST(test_pylon_cycle_is_six_frames_then_empty);
    RUN_TEST(test_pylon_emits_only_its_six_identifiers);
    RUN_TEST(test_pylon_agrees_with_dyness_on_the_shared_frames);
    RUN_TEST(test_pylon_withholds_limits_without_voltage_limits);
    RUN_TEST(test_zero_voltage_limit_withholds_0x351);
    RUN_TEST(test_pack_ready_waits_for_every_advertised_group);
    RUN_TEST(test_frame_withholds_limits_without_voltage_limits);
    RUN_TEST(test_frame_forbidden_direction_publishes_zero);
    RUN_TEST(test_frame_invalid_limit_field_publishes_zero);
    RUN_TEST(test_frame_sign_is_negative_while_discharging);
    RUN_TEST(test_frame_invert_current_flips_only_the_measurement);
    RUN_TEST(test_frame_saturates_rather_than_wraps);
    RUN_TEST(test_frame_soc_truncates);
    RUN_TEST(test_frame_alarms_map_and_can_be_suppressed);
    RUN_TEST(test_frame_module_count_floors_at_one);
    RUN_TEST(test_frame_scales_are_configuration);

    RUN_TEST(test_cfg_accepts_the_zaliakalnis_document);
    RUN_TEST(test_cfg_accepts_the_sodas_document);
    RUN_TEST(test_cfg_defaults_are_applied_when_keys_are_absent);
    RUN_TEST(test_cfg_pack_source_needs_a_name);
    RUN_TEST(test_cfg_rejects_a_pack_name_on_a_cluster_source);
    RUN_TEST(test_cfg_reject_matrix);
    RUN_TEST(test_cfg_export_round_trips);
    RUN_TEST(test_cfg_rejects_an_empty_document);

    RUN_TEST(test_names_cover_every_enumerator);
    RUN_TEST(test_names_are_json_safe);

    printf("\n%s\n", test_failures ? "FAILURES" : "all tests passed");
    return test_failures ? 1 : 0;
}
