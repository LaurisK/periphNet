/*
 * DEVELOPMENT KEYS — publicly known placeholders, safe to commit.
 * They match the defaults baked into tools/dfu_image_tool.py so a fresh
 * checkout builds working FWU artifacts out of the box.
 *
 * For production: copy this file to secrets_prod.c (gitignored), replace
 * both keys, and export DFU_AES_KEY / DFU_HMAC_KEY for the build tool.
 * CMake compiles secrets_prod.c instead of this file when it exists.
 */

#include "secrets.h"

/* FIPS-197 test vector key — intentionally recognizable as non-secret */
const uint8_t GLB_blKey[AES128_KEY_SIZE] = {
    0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
    0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
};

/* ASCII "PNHMAC-DEV-KEY-0123456789abcdefg" */
const uint8_t GLB_hmacKey[HMAC_KEY_SIZE] = {
    0x50, 0x4e, 0x48, 0x4d, 0x41, 0x43, 0x2d, 0x44,
    0x45, 0x56, 0x2d, 0x4b, 0x45, 0x59, 0x2d, 0x30,
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67
};
