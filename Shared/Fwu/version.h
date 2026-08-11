#ifndef VERSION_H
#define VERSION_H

#include "dfu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Format version as string: "Pv1.2.3" or "Pv1.2.3_ABCD" (with hwId).
 *
 * @param ver   Version to format.
 * @param buf   Output buffer.
 * @param size  Buffer size.
 */
void ver_toString(const sFwVer *ver, char *buf, uint8_t size);

/**
 * Check if incoming FW is compatible with current FW.
 *
 * Validates: device type, hardware ID, version ordering.
 * Local-target builds skip version ordering (allow any update).
 *
 * @param current   Currently running version.
 * @param incoming  Candidate version.
 * @return fwuRes_ok if compatible, error code otherwise.
 */
eFwuRes ver_checkCompatibility(const sFwVer *current, const sFwVer *incoming);

/**
 * Compare two versions.
 * @return  >0 if a is newer, <0 if b is newer, 0 if equal.
 */
int ver_compare(const sFwVer *a, const sFwVer *b);

#ifdef __cplusplus
}
#endif

#endif /* VERSION_H */
