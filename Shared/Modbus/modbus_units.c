#include "modbus_units.h"

#include <string.h>

/* HA device/state class strings match the retired hardcoded s_sensors[]
 * table in mqtt_bridge.c so discovery output stays identical for the
 * built-in Solis config. "%" carries no device_class in the generic model —
 * a percentage point is not necessarily a battery. */
static const sMbUnitInfo s_units[] = {
    { "",    MB_UNIT_NONE,    NULL,           NULL,          "measurement" },
    { "V",   MB_UNIT_V,       "V",            "voltage",     "measurement" },
    { "A",   MB_UNIT_A,       "A",            "current",     "measurement" },
    { "W",   MB_UNIT_W,       "W",            "power",       "measurement" },
    { "VA",  MB_UNIT_VA,      "VA",           "apparent_power", "measurement" },
    { "var", MB_UNIT_VAR,     "var",          "reactive_power", "measurement" },
    { "Hz",  MB_UNIT_HZ,      "Hz",           "frequency",   "measurement" },
    { "Wh",  MB_UNIT_WH,      "Wh",           "energy",      "total_increasing" },
    { "kWh", MB_UNIT_KWH,     "kWh",          "energy",      "total_increasing" },
    { "varh", MB_UNIT_VARH,   "varh",         NULL,          "total_increasing" },
    { "VAh", MB_UNIT_VAH,     "VAh",          NULL,          "total_increasing" },
    { "%",   MB_UNIT_PERCENT, "%",            NULL,          "measurement" },
    { "Ah",  MB_UNIT_AH,      "Ah",           NULL,          "measurement" },
    { "C",   MB_UNIT_CELSIUS, "\xc2\xb0""C",  "temperature", "measurement" },
    { "min", MB_UNIT_MINUTE,  "min",          "duration",    "measurement" },
    { "s",   MB_UNIT_SECOND,  "s",            "duration",    "measurement" },
};

#define UNIT_TABLE_LEN  (sizeof(s_units) / sizeof(s_units[0]))

const sMbUnitInfo *MbUnits_FromString(const char *str)
{
    if (!str) {
        return NULL;
    }
    for (unsigned i = 0; i < UNIT_TABLE_LEN; i++) {
        if (strcmp(s_units[i].str, str) == 0) {
            return &s_units[i];
        }
    }
    return NULL;
}

const sMbUnitInfo *MbUnits_FromCode(uint8_t code)
{
    for (unsigned i = 0; i < UNIT_TABLE_LEN; i++) {
        if (s_units[i].code == code) {
            return &s_units[i];
        }
    }
    return NULL;
}
