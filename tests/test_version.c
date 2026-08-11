/**
 * Unit tests for Shared/Fwu/version.c — string formatting, ordering,
 * and the FWU compatibility gate (device type / hwId / strictly-newer,
 * with the local-target exemption).
 */

#include "test_util.h"

#include "version.h"

#include <stdlib.h>

static sFwVer make_ver(uint8_t target, uint16_t major, uint8_t minor,
                       uint8_t patch, uint16_t hwId)
{
    sFwVer v = {
        .deviceType = (uint8_t)fwDev_periphnet,
        .target     = target,
        .major      = major,
        .minor      = minor,
        .patch      = patch,
        .hwId       = hwId,
    };
    return v;
}

static void test_to_string(void)
{
    char buf[24];

    sFwVer v = make_ver(fwTarget_release, 1, 2, 3, 0);
    ver_toString(&v, buf, sizeof(buf));
    TEST_ASSERT(strcmp(buf, "Pv1.2.3") == 0);

    v = make_ver(fwTarget_local, 1, 2, 3, 0xABCD);
    ver_toString(&v, buf, sizeof(buf));
    TEST_ASSERT(strcmp(buf, "Pl1.2.3_ABCD") == 0);

    /* Truncation must not overflow */
    v = make_ver(fwTarget_dev, 65535, 255, 255, 0xFFFF);
    char small[8];
    ver_toString(&v, small, sizeof(small));
    TEST_ASSERT(strlen(small) < sizeof(small));
}

static void test_compare(void)
{
    sFwVer a = make_ver(fwTarget_release, 1, 2, 3, 0);
    sFwVer b = make_ver(fwTarget_release, 1, 2, 4, 0);

    TEST_ASSERT(ver_compare(&a, &a) == 0);
    TEST_ASSERT(ver_compare(&b, &a) > 0);
    TEST_ASSERT(ver_compare(&a, &b) < 0);

    b = make_ver(fwTarget_release, 2, 0, 0, 0);
    TEST_ASSERT(ver_compare(&b, &a) > 0);

    b = make_ver(fwTarget_release, 1, 3, 0, 0);
    TEST_ASSERT(ver_compare(&b, &a) > 0);
}

static void test_compat_gate(void)
{
    sFwVer cur = make_ver(fwTarget_release, 1, 2, 3, 0);

    /* Strictly newer → OK */
    sFwVer in = make_ver(fwTarget_release, 1, 2, 4, 0);
    TEST_ASSERT(ver_checkCompatibility(&cur, &in) == fwuRes_ok);

    /* Equal / older → rejected */
    in = cur;
    TEST_ASSERT(ver_checkCompatibility(&cur, &in) == fwuRes_errVerNotNewer);
    in = make_ver(fwTarget_release, 1, 2, 2, 0);
    TEST_ASSERT(ver_checkCompatibility(&cur, &in) == fwuRes_errVerNotNewer);

    /* Device type mismatch */
    in = make_ver(fwTarget_release, 9, 9, 9, 0);
    in.deviceType = 'X';
    TEST_ASSERT(ver_checkCompatibility(&cur, &in) == fwuRes_errVerDeviceType);

    /* Hardware ID mismatch */
    in = make_ver(fwTarget_release, 9, 9, 9, 7);
    TEST_ASSERT(ver_checkCompatibility(&cur, &in) == fwuRes_errVerHwId);

    /* Local-target current build accepts equal and older versions */
    sFwVer local = make_ver(fwTarget_local, 1, 2, 3, 0);
    in = make_ver(fwTarget_release, 1, 0, 0, 0);
    TEST_ASSERT(ver_checkCompatibility(&local, &in) == fwuRes_ok);
    in = local;
    TEST_ASSERT(ver_checkCompatibility(&local, &in) == fwuRes_ok);

    /* NULL args */
    TEST_ASSERT(ver_checkCompatibility(NULL, &in) == fwuRes_errNoImage);
    TEST_ASSERT(ver_checkCompatibility(&cur, NULL) == fwuRes_errNoImage);
}

int main(void)
{
    RUN_TEST(test_to_string);
    RUN_TEST(test_compare);
    RUN_TEST(test_compat_gate);
    return test_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
