/**
 * Unit tests for Shared/Modbus/modbus_units.c — string <-> code round trip
 * and the DLMS code values verified against the Gurux DLMS sources
 * (GuruxDLMS.c enums.h / Gurux.DLMS.Net Unit.cs, both mirror
 * IEC 62056-6-2).
 */

#include "test_util.h"
#include "modbus_units.h"

#include <stdlib.h>

static void test_dlms_codes(void)
{
    /* Values double-checked against two independent Gurux sources */
    TEST_ASSERT(MbUnits_FromString("V")->code == 35);
    TEST_ASSERT(MbUnits_FromString("A")->code == 33);
    TEST_ASSERT(MbUnits_FromString("W")->code == 27);
    TEST_ASSERT(MbUnits_FromString("VA")->code == 28);
    TEST_ASSERT(MbUnits_FromString("var")->code == 29);
    TEST_ASSERT(MbUnits_FromString("Wh")->code == 30);
    TEST_ASSERT(MbUnits_FromString("Hz")->code == 44);
    TEST_ASSERT(MbUnits_FromString("%")->code == 56);
    TEST_ASSERT(MbUnits_FromString("Ah")->code == 57);
    TEST_ASSERT(MbUnits_FromString("C")->code == 9);
    TEST_ASSERT(MbUnits_FromString("min")->code == 6);
    TEST_ASSERT(MbUnits_FromString("s")->code == 7);
    TEST_ASSERT(MbUnits_FromString("")->code == 255);

    /* kWh is a private code (no distinct DLMS value exists) */
    TEST_ASSERT(MbUnits_FromString("kWh")->code == MB_UNIT_KWH);
    TEST_ASSERT(MbUnits_FromString("kWh")->code != MbUnits_FromString("Wh")->code);
}

static void test_round_trip(void)
{
    static const char *strs[] = {
        "", "V", "A", "W", "VA", "var", "Hz", "Wh", "kWh",
        "varh", "VAh", "%", "Ah", "C", "min", "s",
    };

    for (unsigned i = 0; i < sizeof(strs) / sizeof(strs[0]); i++) {
        const sMbUnitInfo *u = MbUnits_FromString(strs[i]);
        TEST_ASSERT(u != NULL);
        const sMbUnitInfo *back = MbUnits_FromCode(u->code);
        TEST_ASSERT(back == u);   /* code must be unique per row */
    }
}

static void test_unknown_rejected(void)
{
    TEST_ASSERT(MbUnits_FromString("volts") == NULL);
    TEST_ASSERT(MbUnits_FromString("kw") == NULL);
    TEST_ASSERT(MbUnits_FromString(NULL) == NULL);
    TEST_ASSERT(MbUnits_FromCode(200) == NULL);
}

static void test_ha_classes(void)
{
    TEST_ASSERT(strcmp(MbUnits_FromString("V")->haDeviceClass, "voltage") == 0);
    TEST_ASSERT(strcmp(MbUnits_FromString("kWh")->haStateClass,
                       "total_increasing") == 0);
    TEST_ASSERT(strcmp(MbUnits_FromString("C")->haUnit, "\xc2\xb0""C") == 0);
    TEST_ASSERT(MbUnits_FromString("%")->haDeviceClass == NULL);
    TEST_ASSERT(MbUnits_FromString("")->haUnit == NULL);
}

int main(void)
{
    RUN_TEST(test_dlms_codes);
    RUN_TEST(test_round_trip);
    RUN_TEST(test_unknown_rejected);
    RUN_TEST(test_ha_classes);
    return test_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
