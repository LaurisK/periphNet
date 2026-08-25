/**
 * @file    modbus_units.h
 * @brief   Point unit table: JSON string <-> stored byte code <-> HA classes.
 *
 * The stored byte is the DLMS/COSEM physical-unit enumeration from
 * IEC 62056-6-2 (transcribed from the Gurux DLMS sources, which mirror the
 * Blue Book table — GuruxDLMS.c enums.h and Gurux.DLMS.Net Unit.cs agree on
 * every value used here). One documented deviation: DLMS has no distinct
 * kWh code (COSEM expresses kWh as Wh plus a scaler, but this design's
 * scalePow10 places the decimal point, it does not convert units), so kWh
 * gets a PRIVATE code in the range IEC leaves reserved — required so config
 * export and HA discovery can tell kWh from Wh.
 *
 * One table serves all three directions (compiler, export, HA discovery),
 * so a unit string round-trips by construction.
 */
#ifndef MODBUS_UNITS_H_
#define MODBUS_UNITS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IEC 62056-6-2 codes (subset used by the compiler) */
#define MB_UNIT_NONE        255u   /* DLMS "no unit"                        */
#define MB_UNIT_MINUTE      6u
#define MB_UNIT_SECOND      7u
#define MB_UNIT_CELSIUS     9u
#define MB_UNIT_W           27u    /* active power                          */
#define MB_UNIT_VA          28u    /* apparent power                        */
#define MB_UNIT_VAR         29u    /* reactive power                        */
#define MB_UNIT_WH          30u    /* active energy                         */
#define MB_UNIT_VAH         31u    /* apparent energy                       */
#define MB_UNIT_VARH        32u    /* reactive energy                       */
#define MB_UNIT_A           33u
#define MB_UNIT_V           35u
#define MB_UNIT_OHM         38u    /* resistance (DLMS "Resistance")        */
#define MB_UNIT_HZ          44u
#define MB_UNIT_PERCENT     56u
#define MB_UNIT_AH          57u    /* ampere-hours                          */

/* Private codes (IEC 62056-6-2 reserves 73..252) */
#define MB_UNIT_KWH         130u   /* kilowatt-hours, see header comment    */

typedef struct {
    const char *str;            /* JSON unit string ("V", "kWh", "C", "")   */
    uint8_t     code;           /* stored byte code                         */
    const char *haUnit;         /* HA unit_of_measurement; NULL = omit      */
    const char *haDeviceClass;  /* HA device_class; NULL = omit             */
    const char *haStateClass;   /* HA state_class                           */
} sMbUnitInfo;

/* NULL if the string / code is unknown. Empty string = "no unit". */
const sMbUnitInfo *MbUnits_FromString(const char *str);
const sMbUnitInfo *MbUnits_FromCode(uint8_t code);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_UNITS_H_ */
