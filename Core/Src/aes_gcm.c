/*
 * AES-128-GCM authenticated decryption following NIST SP 800-38D.
 *
 * GCM uses AES in CTR mode for confidentiality and GHASH for
 * authentication.  Only the encrypt direction of AES is needed.
 */

#include "aes_gcm.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * GF(2^128) multiplication for GHASH
 *
 * Reduction polynomial: x^128 + x^7 + x^2 + x + 1  (R = 0xE1000...0)
 * -------------------------------------------------------------------------- */

static void ghash_mult(uint8_t result[16], const uint8_t X[16], const uint8_t H[16])
{
    uint8_t V[16];
    uint8_t Z[16];
    memcpy(V, H, 16);
    memset(Z, 0, 16);

    for (int i = 0; i < 128; i++) {
        /* If bit i of X is set, Z ^= V */
        if (X[i / 8] & (0x80 >> (i & 7))) {
            for (int j = 0; j < 16; j++) {
                Z[j] ^= V[j];
            }
        }

        /* V = V >> 1 (in GF), with conditional XOR of R */
        uint8_t lsb = V[15] & 1;
        for (int j = 15; j > 0; j--) {
            V[j] = (V[j] >> 1) | (V[j-1] << 7);
        }
        V[0] >>= 1;
        if (lsb) {
            V[0] ^= 0xE1;  /* R = 0xE1 << 120 */
        }
    }

    memcpy(result, Z, 16);
}

/* --------------------------------------------------------------------------
 * GHASH: H is the hash subkey (AES(K, 0^128))
 * -------------------------------------------------------------------------- */

static void ghash_update(uint8_t tag[16], const uint8_t H[16],
                         const uint8_t *data, uint32_t len)
{
    while (len > 0) {
        uint32_t chunk = (len >= 16) ? 16 : len;
        for (uint32_t i = 0; i < chunk; i++) {
            tag[i] ^= data[i];
        }
        ghash_mult(tag, tag, H);
        data += chunk;
        len  -= chunk;
    }
}

/* --------------------------------------------------------------------------
 * Increment counter (rightmost 32 bits, big-endian)
 * -------------------------------------------------------------------------- */

static void inc32(uint8_t ctr[16])
{
    for (int i = 15; i >= 12; i--) {
        if (++ctr[i] != 0) break;
    }
}

/* --------------------------------------------------------------------------
 * AES-128-GCM decryption
 * -------------------------------------------------------------------------- */

bool aes_gcm_decrypt(const uint8_t key[AES128_KEY_SIZE],
                     uint8_t *data, uint32_t size,
                     const uint8_t *aad, uint32_t aad_len)
{
    if (size < AES_GCM_IV_SIZE + AES_GCM_TAG_SIZE) {
        return false;
    }

    uint32_t ct_len = size - AES_GCM_IV_SIZE - AES_GCM_TAG_SIZE;
    const uint8_t *iv  = data;
    uint8_t *ct        = data + AES_GCM_IV_SIZE;
    const uint8_t *tag = data + AES_GCM_IV_SIZE + ct_len;

    sAes128Ctx aes;
    aes128_init(&aes, key);

    /* Hash subkey H = AES(K, 0^128) */
    uint8_t H[16] = {0};
    aes128_encrypt_block(&aes, H);

    /* Initial counter J0: IV || 0x00000001 */
    uint8_t J0[16];
    memcpy(J0, iv, AES_GCM_IV_SIZE);
    J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;

    /* Compute GHASH over AAD + ciphertext BEFORE decryption */
    uint8_t S[16] = {0};

    if (aad && aad_len > 0) {
        ghash_update(S, H, aad, aad_len);
        /* Pad AAD to 16-byte boundary */
        uint32_t pad = (16 - (aad_len & 15)) & 15;
        if (pad > 0) {
            uint8_t zeros[16] = {0};
            ghash_update(S, H, zeros, pad);
        }
    }

    ghash_update(S, H, ct, ct_len);
    /* Pad ciphertext to 16-byte boundary */
    {
        uint32_t pad = (16 - (ct_len & 15)) & 15;
        if (pad > 0) {
            uint8_t zeros[16] = {0};
            ghash_update(S, H, zeros, pad);
        }
    }

    /* Append lengths block: [aad_len_bits:8][ct_len_bits:8] big-endian */
    {
        uint8_t lenblock[16] = {0};
        uint64_t aad_bits = (uint64_t)(aad_len) * 8;
        uint64_t ct_bits  = (uint64_t)(ct_len) * 8;
        lenblock[4]  = (uint8_t)(aad_bits >> 24);
        lenblock[5]  = (uint8_t)(aad_bits >> 16);
        lenblock[6]  = (uint8_t)(aad_bits >> 8);
        lenblock[7]  = (uint8_t)(aad_bits);
        lenblock[12] = (uint8_t)(ct_bits >> 24);
        lenblock[13] = (uint8_t)(ct_bits >> 16);
        lenblock[14] = (uint8_t)(ct_bits >> 8);
        lenblock[15] = (uint8_t)(ct_bits);
        ghash_update(S, H, lenblock, 16);
    }

    /* Compute expected tag T = GCTR(J0, S) */
    uint8_t T[16];
    memcpy(T, J0, 16);
    aes128_encrypt_block(&aes, T);
    for (int i = 0; i < 16; i++) {
        T[i] ^= S[i];
    }

    /* Verify tag (constant-time comparison) */
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) {
        diff |= T[i] ^ tag[i];
    }
    if (diff != 0) {
        return false;
    }

    /* Decrypt ciphertext in-place using GCTR with J0+1 as starting counter */
    uint8_t ctr[16];
    memcpy(ctr, J0, 16);
    inc32(ctr);

    uint32_t offset = 0;
    while (offset < ct_len) {
        uint8_t keystream[16];
        memcpy(keystream, ctr, 16);
        aes128_encrypt_block(&aes, keystream);

        uint32_t chunk = ct_len - offset;
        if (chunk > 16) chunk = 16;

        for (uint32_t i = 0; i < chunk; i++) {
            ct[offset + i] ^= keystream[i];
        }

        inc32(ctr);
        offset += chunk;
    }

    /* Move plaintext to beginning of data buffer (overwrite IV) */
    memmove(data, ct, ct_len);

    return true;
}
