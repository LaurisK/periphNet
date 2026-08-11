#include "version.h"
#include <stdio.h>
#include <string.h>

void ver_toString(const sFwVer *ver, char *buf, uint8_t size)
{
    if (!ver || !buf || size == 0) {
        return;
    }

    int len = snprintf(buf, size, "%c%c%u.%u.%u",
                       ver->deviceType, ver->target,
                       ver->major, ver->minor, ver->patch);

    if (ver->hwId != 0 && len > 0 && (uint8_t)len < size) {
        snprintf(buf + len, size - (uint8_t)len, "_%04X", ver->hwId);
    }
}

int ver_compare(const sFwVer *a, const sFwVer *b)
{
    if (a->major != b->major) return (a->major > b->major) ? 1 : -1;
    if (a->minor != b->minor) return (a->minor > b->minor) ? 1 : -1;
    if (a->patch != b->patch) return (a->patch > b->patch) ? 1 : -1;
    return 0;
}

eFwuRes ver_checkCompatibility(const sFwVer *current, const sFwVer *incoming)
{
    if (!current || !incoming) {
        return fwuRes_errNoImage;
    }

    /* Device type must match */
    if (current->deviceType != incoming->deviceType) {
        return fwuRes_errVerDeviceType;
    }

    /* Hardware ID must match */
    if (current->hwId != incoming->hwId) {
        return fwuRes_errVerHwId;
    }

    /* Local builds skip version ordering — allow any update */
    if (current->target == (uint8_t)fwTarget_local) {
        return fwuRes_ok;
    }

    /* Incoming must be strictly newer */
    if (ver_compare(incoming, current) <= 0) {
        return fwuRes_errVerNotNewer;
    }

    return fwuRes_ok;
}
