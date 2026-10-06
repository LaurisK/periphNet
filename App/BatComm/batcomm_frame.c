/*
 * batcomm_frame.c
 *
 * The pure encoder (docs/design_battery_comm.md §4).  Every byte of the
 * dyness_lv set is either measured in reference_dyness_can_capture_2026-09-05.md
 * or is a documented zero; nothing is invented.  The pylon_lv set is the
 * standard Pylontech layout (docs/design_can_bms_frame_source.md §4.1) and has
 * NOT been captured off a wire — its 0x35E string and the 'P','N' bytes of
 * 0x359 are the specification's, not a measurement.
 *
 * LIBC ONLY, ZERO FILE STATICS, NO FLOAT.  See batcomm_frame.h.
 *
 * EVERY MULTI-BYTE FIELD IS LITTLE-ENDIAN and is written BYTE BY BYTE rather
 * than through a packed struct: this file is compiled by tests/ on a host
 * whose alignment and padding rules are not the target's, and a wire format
 * that depends on either is not a wire format.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/BatComm/batcomm_frame.h"

#include <string.h>

/* Private defines ----------------------------------------------------------*/

/** The 0x35F model/version prefix and the 0x70D constants, exactly as
 *  captured.  They never changed in 24 h of logging and nothing decodes them,
 *  so they are reproduced rather than synthesised — an imitation that differs
 *  in a constant is an imitation that can be told apart. */
#define BF_35F_B0   0x2Bu
#define BF_35F_B1   0xA4u
#define BF_35F_B2   0xEBu
#define BF_35F_B3   0x0Eu

#define BF_70D_W0   100u
#define BF_70D_W1   5u
#define BF_70D_W2   286u

/** 0x35C.  bit7 = charge enabled, bit6 = discharge enabled.  The capture only
 *  ever showed 0xC0, so the OTHER bits' semantics are UNVERIFIED for this
 *  vendor and this file never sets one. */
#define BF_CHG_EN   0x80u
#define BF_DSG_EN   0x40u

/* Private function prototypes ----------------------------------------------*/

static uint16_t sat_u16(uint32_t v);
static int16_t  sat_i16(int32_t v);
static uint32_t scale_or_default(uint16_t v, uint32_t dflt);
static void     put_u16(uint8_t *p, uint16_t v);
static void     put_i16(uint8_t *p, int16_t v);
static void     frame_init(sBatCommFrame *out, uint32_t id);
static int32_t  current_counts(const sBatCommTune *t, int32_t mA);
static uint16_t limit_counts(const sBatCommTune *t, uint32_t mA, int allowed);
static void     alarm_bytes(const sBatCommIn *in, uint8_t *b);
static int      build_limits(const sBatCommTune *t, const sBatCommIn *in,
                             sBatCommFrame *out);
static void     build_soc(const sBatCommIn *in, sBatCommFrame *out);
static void     build_measure(const sBatCommTune *t, const sBatCommIn *in,
                              sBatCommFrame *out);
static void     build_alarm(const sBatCommTune *t, const sBatCommIn *in,
                            int withPn, sBatCommFrame *out);
static void     build_chgctrl(const sBatCommIn *in, sBatCommFrame *out);
static int      build_dyness(const sBatCommTune *t, const sBatCommIn *in,
                             uint8_t slot, sBatCommFrame *out);
static int      build_pylon(const sBatCommTune *t, const sBatCommIn *in,
                            uint8_t slot, sBatCommFrame *out);

/* Private functions --------------------------------------------------------*/

static uint16_t sat_u16(uint32_t v)
{
    return (v > 0xFFFFu) ? (uint16_t)0xFFFFu : (uint16_t)v;
}

/** 0x351's four fields SATURATE AT 32767, not at 65535.  The generic protocol
 *  types three of them as i16 and only `DVL` as u16, and a receiver reading
 *  i16 would see a 40000-count limit as a large NEGATIVE current — which is
 *  the same class of failure as wrapping, arrived at by a different route. */
static uint16_t sat_u15(uint32_t v)
{
    return (v > 32767u) ? (uint16_t)32767u : (uint16_t)v;
}

/** SATURATE, NEVER WRAP.  A wrapped current limit is the one arithmetic
 *  failure here that a receiver cannot detect: 3000.0 A truncated to 16 bits
 *  is a small positive number that looks entirely reasonable. */
static int16_t sat_i16(int32_t v)
{
    if (v > 32767) {
        return (int16_t)32767;
    }
    if (v < -32768) {
        return (int16_t)-32768;
    }
    return (int16_t)v;
}

static uint32_t scale_or_default(uint16_t v, uint32_t dflt)
{
    return (v == 0u) ? dflt : (uint32_t)v;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_i16(uint8_t *p, int16_t v)
{
    put_u16(p, (uint16_t)v);
}

/** DLC 8 EVERYWHERE, and that is imitation rather than laziness: the captured
 *  device pads every frame to 8 and the inverter accepts it, while the
 *  generic Pylontech spec's 4/6/7/2 is the more likely thing for a picky
 *  receiver to reject.  No receiver rejects trailing zeros it would ignore. */
static void frame_init(sBatCommFrame *out, uint32_t id)
{
    (void)memset(out, 0, sizeof(*out));
    out->id  = id;
    out->dlc = 8u;
}

/** mA -> wire counts, with the sign convention applied ONCE, here.
 *  batcomm.h contract 1: + charge / - discharge goes out unchanged unless the
 *  profile says to invert. */
static int32_t current_counts(const sBatCommTune *t, int32_t mA)
{
    const int32_t scale = (int32_t)scale_or_default(t->currentScale_mA,
                                                    BATCOMM_DFLT_CURRENT_SCALE_MA);
    int32_t       v     = mA / scale;

    if (t->invertCurrent != 0u) {
        v = -v;
    }
    return v;
}

/** A limit is an UNSIGNED MAGNITUDE and stays one on the wire: the direction
 *  is the field's position in 0x351, never a sign.  A forbidden direction
 *  emits zero, because permission and limit must agree in the channel that is
 *  actually known to be obeyed (docs/design_battery_cluster.md, contract 3). */
static uint16_t limit_counts(const sBatCommTune *t, uint32_t mA, int allowed)
{
    const uint32_t scale = scale_or_default(t->currentScale_mA,
                                            BATCOMM_DFLT_CURRENT_SCALE_MA);

    if (allowed == 0) {
        return 0u;
    }
    return sat_u15(mA / scale);
}

/**
 * ePackAlarm -> the four 0x359 bytes.
 *
 * THE BIT LAYOUT IS THE GENERIC PYLONTECH ONE AND IS UNVERIFIED FOR THIS
 * VENDOR: the captured device only ever sent zeros, so no alarm bit has ever
 * been observed being produced OR consumed here
 * (reference_dyness_can_capture_2026-09-05.md §4.3).  That is why
 * `emitAlarms` exists.  The reason it defaults to ON anyway is direction: a
 * spurious alarm makes an inverter more cautious, a missing one makes it less,
 * and the limit fields — the channel measured end-to-end as obeyed — already
 * carry the same refusal.
 */
static void alarm_bytes(const sBatCommIn *in, uint8_t *b)
{
    const uint32_t a = in->alarms;

    if ((a & ((uint32_t)packAlarm_packOverVoltage |
              (uint32_t)packAlarm_cellOverVoltage)) != 0u) {
        b[0] |= 0x02u;
    }
    if ((a & ((uint32_t)packAlarm_packUnderVoltage |
              (uint32_t)packAlarm_cellUnderVoltage)) != 0u) {
        b[0] |= 0x04u;
    }
    if ((a & (uint32_t)packAlarm_overTemperature) != 0u) {
        b[0] |= 0x08u;
    }
    if ((a & (uint32_t)packAlarm_underTemperature) != 0u) {
        b[0] |= 0x10u;
    }
    if ((a & (uint32_t)packAlarm_dischargeOverCur) != 0u) {
        b[0] |= 0x80u;
    }
    if ((a & (uint32_t)packAlarm_chargeOverCurrent) != 0u) {
        b[1] |= 0x01u;
    }
    if ((a & ((uint32_t)packAlarm_internalFault |
              (uint32_t)packAlarm_protectionOpen)) != 0u) {
        b[1] |= 0x08u;
    }
    /* Imbalance has no place in the generic layout, so it is reported as the
     * warning byte's "internal error" rather than silently dropped. */
    if ((a & (uint32_t)packAlarm_cellImbalance) != 0u) {
        b[3] |= 0x08u;
    }
}

/* --- the frames the two dialects share -------------------------------------
 * 0x351, 0x355, 0x356, 0x359 and 0x35C are the same fields in the same
 * positions in dyness_lv and pylon_lv, so there is ONE encoder for each and
 * the host tests can assert that the two dialects agree on the bytes they
 * have in common rather than hoping two copies stayed in step.
 * ------------------------------------------------------------------------*/

/** 0x351.  Returns 0 when WITHHELD.
 *
 *  0x351 IS WITHHELD, NOT ZEROED, when a voltage limit is missing.  A zero CVL
 *  is an instruction to stop charging and a pack that cannot report its limits
 *  has not given one. */
static int build_limits(const sBatCommTune *t, const sBatCommIn *in,
                        sBatCommFrame *out)
{
    if (((in->fields & (uint16_t)batField_chargeVoltLimit) == 0u) ||
        ((in->fields & (uint16_t)batField_dischargeVoltLimit) == 0u)) {
        return 0;
    }
    /* A VOLTAGE LIMIT OF ZERO IS INVALID WHATEVER THE VALIDITY BIT SAYS.  No
     * battery has a 0 V limit, so a zero here is a value nobody has read yet
     * (a capability is advertised from bind, the register arrives later) —
     * found on hardware 2026-10-04, when booting straight into `bms` put an
     * all-zero 0x351 on the wire for the first seconds.  Withhold it, exactly
     * as for a missing limit. */
    if ((in->chargeVoltLimit_mV == 0u) || (in->dischargeVoltLimit_mV == 0u)) {
        return 0;
    }
    frame_init(out, BATCOMM_ID_LIMITS);
    put_u16(&out->data[0], sat_u15(in->chargeVoltLimit_mV / 100u));
    put_u16(&out->data[2],
            limit_counts(t, in->chargeLimit_mA,
                         ((in->fields & (uint16_t)batField_chargeLimit) !=
                          0u) && (in->chargeAllowed != 0u)));
    put_u16(&out->data[4],
            limit_counts(t, in->dischargeLimit_mA,
                         ((in->fields & (uint16_t)batField_dischargeLimit) !=
                          0u) && (in->dischargeAllowed != 0u)));
    put_u16(&out->data[6], sat_u15(in->dischargeVoltLimit_mV / 100u));
    return 1;
}

static void build_soc(const sBatCommIn *in, sBatCommFrame *out)
{
    frame_init(out, BATCOMM_ID_SOC);
    /* TRUNCATED, not rounded: a rounded-up SOC is an over-report, and this is
     * the field the inverter's own charge decision reads. */
    put_u16(&out->data[0],
            ((in->fields & (uint16_t)batField_soc) != 0u)
                ? sat_u16((uint32_t)in->soc_pm / 10u) : 0u);
    put_u16(&out->data[2],
            ((in->fields & (uint16_t)batField_soh) != 0u)
                ? sat_u16((uint32_t)in->soh_pm / 10u) : 0u);
}

static void build_measure(const sBatCommTune *t, const sBatCommIn *in,
                          sBatCommFrame *out)
{
    frame_init(out, BATCOMM_ID_MEASURE);
    put_i16(&out->data[0],
            ((in->fields & (uint16_t)batField_voltage) != 0u)
                ? sat_i16((int32_t)(in->voltage_mV / 10u)) : 0);
    put_i16(&out->data[2],
            ((in->fields & (uint16_t)batField_current) != 0u)
                ? sat_i16(current_counts(t, in->current_mA)) : 0);
    put_i16(&out->data[4],
            ((in->fields & (uint16_t)batField_temperature) != 0u)
                ? in->tempMax_dC : 0);
}

/** 0x359.  Byte 4 is the module count.  Bytes 5-6 are the generic protocol's
 *  'P','N' ASCII: the standard Pylontech set carries them (`withPn`), the
 *  captured DYNESS device does NOT send them. */
static void build_alarm(const sBatCommTune *t, const sBatCommIn *in,
                        int withPn, sBatCommFrame *out)
{
    frame_init(out, BATCOMM_ID_ALARM);
    if (t->emitAlarms != 0u) {
        alarm_bytes(in, &out->data[0]);
    }
    out->data[4] = (in->modules == 0u) ? 1u : in->modules;
    if (withPn != 0) {
        out->data[5] = (uint8_t)'P';
        out->data[6] = (uint8_t)'N';
    }
}

static void build_chgctrl(const sBatCommIn *in, sBatCommFrame *out)
{
    frame_init(out, BATCOMM_ID_CHGCTRL);
    if (in->chargeAllowed != 0u) {
        out->data[0] |= (uint8_t)BF_CHG_EN;
    }
    if (in->dischargeAllowed != 0u) {
        out->data[0] |= (uint8_t)BF_DSG_EN;
    }
}

/** The standard Pylontech LV set (docs/design_can_bms_frame_source.md §4.1):
 *  six frames, the cycle order the spec gives.  This is what the JK itself
 *  sends to a Solis set to PYLON_LV, and the only dialect measured accepted by
 *  the inverter at sodas.  Every frame is padded to DLC 8 like the DYNESS set,
 *  for the same reason. */
static int build_pylon(const sBatCommTune *t, const sBatCommIn *in,
                       uint8_t slot, sBatCommFrame *out)
{
    switch (slot) {
    case 0u:
        return build_limits(t, in, out);

    case 1u:
        build_soc(in, out);
        return 1;

    case 2u:
        build_measure(t, in, out);
        return 1;

    case 3u:
        build_alarm(t, in, 1, out);
        return 1;

    case 4u:
        build_chgctrl(in, out);
        return 1;

    case 5u:
        frame_init(out, BATCOMM_ID_MFGNAME);
        (void)memcpy(out->data, "PYLON   ", 8u);
        return 1;

    default:
        return 0;                       /* the empty tail of the cycle       */
    }
}

static int build_dyness(const sBatCommTune *t, const sBatCommIn *in,
                        uint8_t slot, sBatCommFrame *out)
{
    const uint32_t capScale = scale_or_default(t->capacityScale_mAh,
                                               BATCOMM_DFLT_CAPACITY_SCALE_MAH);

    switch (slot) {
    case 0u:
        frame_init(out, BATCOMM_ID_INFO0);          /* all zero, as captured */
        return 1;

    case 1u:
        return build_limits(t, in, out);

    case 2u:
        build_soc(in, out);
        return 1;

    case 3u:
        build_measure(t, in, out);
        return 1;

    case 4u:
        build_alarm(t, in, 0, out);
        return 1;

    case 5u:
        frame_init(out, BATCOMM_ID_ALARM_EXT);      /* all zero, as captured */
        return 1;

    case 6u:
        build_chgctrl(in, out);
        return 1;

    case 7u:
        frame_init(out, BATCOMM_ID_MFGNAME);
        (void)memcpy(out->data, "DYNESS-L", 8u);
        return 1;

    case 8u:
        frame_init(out, BATCOMM_ID_REMAINING);
        out->data[0] = (uint8_t)BF_35F_B0;
        out->data[1] = (uint8_t)BF_35F_B1;
        out->data[2] = (uint8_t)BF_35F_B2;
        out->data[3] = (uint8_t)BF_35F_B3;
        put_u16(&out->data[4],
                ((in->fields & (uint16_t)batField_capacity) != 0u)
                    ? sat_u16(in->remaining_mAh / capScale) : 0u);
        return 1;

    case 9u:
        frame_init(out, BATCOMM_ID_NAME0);
        (void)memcpy(out->data, "DYNESS-L", 8u);
        return 1;

    case 10u:
        frame_init(out, BATCOMM_ID_NAME1);
        (void)memcpy(out->data, " BATTERY", 8u);
        return 1;

    case 11u:
        frame_init(out, BATCOMM_ID_MODELINFO);
        put_u16(&out->data[0], (uint16_t)BF_70D_W0);
        put_u16(&out->data[2], (uint16_t)BF_70D_W1);
        put_u16(&out->data[4], (uint16_t)BF_70D_W2);
        put_u16(&out->data[6],
                ((in->fields & (uint16_t)batField_capacity) != 0u)
                    ? sat_u16(in->capacity_mAh / capScale) : 0u);
        return 1;

    default:
        return 0;                       /* the empty tail of the cycle       */
    }
}

/* Exported functions -------------------------------------------------------*/

int BatFrame_PackReady(const sPackState *p)
{
    if (p == NULL) {
        return 0;
    }
    if (p->age_ms[packGrp_electrical] == PACK_AGE_NEVER) {
        return 0;
    }
    if (((p->caps & ((uint32_t)packCap_capacityAh | (uint32_t)packCap_soh)) !=
         0u) && (p->age_ms[packGrp_charge] == PACK_AGE_NEVER)) {
        return 0;
    }
    if (((p->caps & (uint32_t)packCap_temperatures) != 0u) &&
        (p->age_ms[packGrp_temperature] == PACK_AGE_NEVER)) {
        return 0;
    }
    if (((p->caps & ((uint32_t)packCap_currentLimits |
                     (uint32_t)packCap_voltageLimits)) != 0u) &&
        (p->age_ms[packGrp_limits] == PACK_AGE_NEVER)) {
        return 0;
    }
    if (((p->caps & (uint32_t)packCap_switchState) != 0u) &&
        (p->age_ms[packGrp_switches] == PACK_AGE_NEVER)) {
        return 0;
    }
    return 1;
}

uint8_t BatFrame_SlotCount(eBatCommProto proto)
{
    if (proto == batProto_dynessLv) {
        return 12u;
    }
    if (proto == batProto_pylonLv) {
        return 6u;
    }
    return 0u;
}

int BatFrame_Build(eBatCommProto proto, const sBatCommTune *tune,
                   const sBatCommIn *in, uint8_t slot, sBatCommFrame *out)
{
    if ((tune == NULL) || (in == NULL) || (out == NULL)) {
        return 0;
    }
    if (proto == batProto_dynessLv) {
        return build_dyness(tune, in, slot, out);
    }
    if (proto == batProto_pylonLv) {
        return build_pylon(tune, in, slot, out);
    }
    return 0;
}
