/**
 * Crypto unit tests against published vectors:
 *   SHA-256      — FIPS 180-4 examples
 *   HMAC-SHA256  — RFC 4231 test cases 1, 2, 3, 6
 *   AES-128      — FIPS 197 appendix C.1
 *   AES-128-GCM  — McGrew/Viega test cases 3 & 4 (NIST SP 800-38D set)
 */

#include "test_util.h"

#include "sha256.h"
#include "hmac_sha256.h"
#include "aes128.h"
#include "aes_gcm.h"

#include <stdlib.h>

/* ============================================================================
 * SHA-256
 * ============================================================================ */

static void check_sha256(const uint8_t *msg, uint32_t len, const char *hex_digest)
{
    sSha256Ctx ctx;
    uint8_t digest[SHA256_DIGEST_SIZE], expected[SHA256_DIGEST_SIZE];

    sha256_init(&ctx);
    sha256_update(&ctx, msg, len);
    sha256_final(&ctx, digest);

    hex2bin(hex_digest, expected, sizeof(expected));
    TEST_ASSERT_MEM_EQ(digest, expected, SHA256_DIGEST_SIZE);
}

static void test_sha256_vectors(void)
{
    check_sha256((const uint8_t *)"", 0,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_sha256((const uint8_t *)"abc", 3,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check_sha256((const uint8_t *)
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

static void test_sha256_streaming(void)
{
    /* One million 'a', fed in odd-sized chunks to exercise buffering */
    sSha256Ctx ctx;
    uint8_t digest[SHA256_DIGEST_SIZE], expected[SHA256_DIGEST_SIZE];
    uint8_t chunk[997];

    memset(chunk, 'a', sizeof(chunk));
    sha256_init(&ctx);
    uint32_t remaining = 1000000;
    while (remaining) {
        uint32_t n = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        sha256_update(&ctx, chunk, n);
        remaining -= n;
    }
    sha256_final(&ctx, digest);

    hex2bin("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
            expected, sizeof(expected));
    TEST_ASSERT_MEM_EQ(digest, expected, SHA256_DIGEST_SIZE);
}

/* ============================================================================
 * HMAC-SHA256 (RFC 4231)
 * ============================================================================ */

static void check_hmac(const uint8_t *key, uint32_t key_len,
                       const uint8_t *data, uint32_t data_len,
                       const char *hex_mac)
{
    uint8_t mac[HMAC_SHA256_SIZE], expected[HMAC_SHA256_SIZE];

    hmac_sha256(key, key_len, data, data_len, mac);
    hex2bin(hex_mac, expected, sizeof(expected));
    TEST_ASSERT_MEM_EQ(mac, expected, HMAC_SHA256_SIZE);

    /* Same vector through the streaming interface, split mid-message */
    sHmacSha256Ctx ctx;
    hmac_sha256_init(&ctx, key, key_len);
    uint32_t half = data_len / 2;
    hmac_sha256_update(&ctx, data, half);
    hmac_sha256_update(&ctx, data + half, data_len - half);
    hmac_sha256_final(&ctx, mac);
    TEST_ASSERT_MEM_EQ(mac, expected, HMAC_SHA256_SIZE);
}

static void test_hmac_rfc4231(void)
{
    uint8_t key[131], data[50];

    /* TC1 */
    memset(key, 0x0b, 20);
    check_hmac(key, 20, (const uint8_t *)"Hi There", 8,
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    /* TC2 */
    check_hmac((const uint8_t *)"Jefe", 4,
        (const uint8_t *)"what do ya want for nothing?", 28,
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    /* TC3 */
    memset(key, 0xaa, 20);
    memset(data, 0xdd, 50);
    check_hmac(key, 20, data, 50,
        "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");

    /* TC6 — key longer than block size */
    memset(key, 0xaa, 131);
    check_hmac(key, 131,
        (const uint8_t *)"Test Using Larger Than Block-Size Key - Hash Key First", 54,
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

/* ============================================================================
 * AES-128 block cipher (FIPS 197 C.1)
 * ============================================================================ */

static void test_aes128_fips197(void)
{
    uint8_t key[16], block[16], expected[16];
    sAes128Ctx ctx;

    hex2bin("000102030405060708090a0b0c0d0e0f", key, sizeof(key));
    hex2bin("00112233445566778899aabbccddeeff", block, sizeof(block));
    hex2bin("69c4e0d86a7b0430d8cdb78070b4c55a", expected, sizeof(expected));

    aes128_init(&ctx, key);
    aes128_encrypt_block(&ctx, block);
    TEST_ASSERT_MEM_EQ(block, expected, 16);
}

/* ============================================================================
 * AES-128-GCM decryption
 * ============================================================================ */

static const char *GCM_KEY = "feffe9928665731c6d6a8f9467308308";
static const char *GCM_IV  = "cafebabefacedbaddecaf888";
static const char *GCM_PT64 =
    "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
    "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255";
static const char *GCM_CT64 =
    "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
    "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985";

static void test_gcm_case3(void)
{
    /* 64-byte message, no AAD */
    uint8_t key[16], iv[12], ct[64], pt[64], expected_pt[64], tag[16];
    sAesGcmCtx ctx;

    hex2bin(GCM_KEY, key, sizeof(key));
    hex2bin(GCM_IV, iv, sizeof(iv));
    hex2bin(GCM_CT64, ct, sizeof(ct));
    hex2bin(GCM_PT64, expected_pt, sizeof(expected_pt));
    hex2bin("4d5c2af327cd64a62cf35abd2ba6fab4", tag, sizeof(tag));

    aes_gcm_dec_init(&ctx, key, iv, NULL, 0);
    aes_gcm_dec_update(&ctx, ct, pt, sizeof(ct));
    TEST_ASSERT(aes_gcm_dec_final(&ctx, tag));
    TEST_ASSERT_MEM_EQ(pt, expected_pt, sizeof(pt));
}

static void test_gcm_case4_streaming(void)
{
    /* 60-byte message (partial final block) with 20 bytes of AAD,
     * fed in 16-byte chunks to exercise the streaming path */
    uint8_t key[16], iv[12], aad[20], ct[60], pt[60], expected_pt[64], tag[16];
    sAesGcmCtx ctx;

    hex2bin(GCM_KEY, key, sizeof(key));
    hex2bin(GCM_IV, iv, sizeof(iv));
    hex2bin("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad, sizeof(aad));
    hex2bin(GCM_CT64, ct, sizeof(ct));            /* first 60 bytes match */
    hex2bin(GCM_PT64, expected_pt, sizeof(expected_pt));
    hex2bin("5bc94fbc3221a5db94fae95ae7121a47", tag, sizeof(tag));

    aes_gcm_dec_init(&ctx, key, iv, aad, sizeof(aad));
    aes_gcm_dec_update(&ctx, ct, pt, 16);
    aes_gcm_dec_update(&ctx, ct + 16, pt + 16, 16);
    aes_gcm_dec_update(&ctx, ct + 32, pt + 32, 16);
    aes_gcm_dec_update(&ctx, ct + 48, pt + 48, 12);   /* partial final */
    TEST_ASSERT(aes_gcm_dec_final(&ctx, tag));
    TEST_ASSERT_MEM_EQ(pt, expected_pt, 60);
}

static void test_gcm_reject_tampered(void)
{
    uint8_t key[16], iv[12], ct[64], pt[64], tag[16];
    sAesGcmCtx ctx;

    hex2bin(GCM_KEY, key, sizeof(key));
    hex2bin(GCM_IV, iv, sizeof(iv));
    hex2bin(GCM_CT64, ct, sizeof(ct));
    hex2bin("4d5c2af327cd64a62cf35abd2ba6fab4", tag, sizeof(tag));

    ct[17] ^= 0x01;   /* flip one ciphertext bit */
    aes_gcm_dec_init(&ctx, key, iv, NULL, 0);
    aes_gcm_dec_update(&ctx, ct, pt, sizeof(ct));
    TEST_ASSERT(!aes_gcm_dec_final(&ctx, tag));

    /* Valid ciphertext, corrupted tag */
    ct[17] ^= 0x01;
    tag[0] ^= 0x80;
    aes_gcm_dec_init(&ctx, key, iv, NULL, 0);
    aes_gcm_dec_update(&ctx, ct, pt, sizeof(ct));
    TEST_ASSERT(!aes_gcm_dec_final(&ctx, tag));
}

static void test_gcm_oneshot_blob(void)
{
    /* aes_gcm_decrypt() over the [iv][ct][tag] blob layout, AAD attached */
    uint8_t key[16], aad[20], expected_pt[64];
    uint8_t blob[12 + 60 + 16];

    hex2bin(GCM_KEY, key, sizeof(key));
    hex2bin("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad, sizeof(aad));
    hex2bin(GCM_IV, blob, 12);
    hex2bin(GCM_CT64, blob + 12, 60);
    hex2bin("5bc94fbc3221a5db94fae95ae7121a47", blob + 12 + 60, 16);
    hex2bin(GCM_PT64, expected_pt, sizeof(expected_pt));

    TEST_ASSERT(aes_gcm_decrypt(key, blob, sizeof(blob), aad, sizeof(aad)));
    /* on success the plaintext is moved to the start of the buffer */
    TEST_ASSERT_MEM_EQ(blob, expected_pt, 60);
}

int main(void)
{
    RUN_TEST(test_sha256_vectors);
    RUN_TEST(test_sha256_streaming);
    RUN_TEST(test_hmac_rfc4231);
    RUN_TEST(test_aes128_fips197);
    RUN_TEST(test_gcm_case3);
    RUN_TEST(test_gcm_case4_streaming);
    RUN_TEST(test_gcm_reject_tampered);
    RUN_TEST(test_gcm_oneshot_blob);
    return test_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
