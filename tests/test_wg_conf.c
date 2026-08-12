/**
 * Unit tests for App/Net/wg_conf.c — the WireGuard `.conf` parser.
 *
 * The accept cases use a real WGDashboard-issued file (keys replaced), since
 * that is the exact text an operator uploads.  The reject cases cover the
 * failures that would otherwise produce a tunnel which handshakes but carries
 * no traffic, which is the failure mode this parser exists to make loud.
 */

#include "test_util.h"
#include "wg_conf.h"

#include <string.h>

/* A key of known bytes: 32 x 0xA5 encodes to this. */
#define KEY_A5_B64 "paWlpaWlpaWlpaWlpaWlpaWlpaWlpaWlpaWlpaWlpaU="
/* 32 x 0x01 */
#define KEY_01_B64 "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE="

static const char conf_ok[] =
    "[Interface]\n"
    "PrivateKey = " KEY_A5_B64 "\n"
    "Address = 10.77.0.65/32\n"
    "MTU = 1420\n"
    "DNS = 1.1.1.1\n"
    "\n"
    "[Peer]\n"
    "PublicKey = " KEY_01_B64 "\n"
    "AllowedIPs = 10.77.0.0/24\n"
    "Endpoint = 85.206.57.75:51820\n"
    "PersistentKeepalive = 21\n";

/* Parse a whole buffer in one go. */
static int parse_all(const char *text, sWgConfParser *p, const char **err)
{
    WgConf_Begin(p);
    WgConf_Feed(p, (const uint8_t *)text, (uint32_t)strlen(text));
    return WgConf_Finish(p, err);
}

static void test_base64_roundtrip(void)
{
    uint8_t key[32];
    char    b64[64];
    uint8_t back[32];
    int     i;

    for (i = 0; i < 32; i++) {
        key[i] = (uint8_t)(i * 7 + 3);
    }
    TEST_ASSERT(WgConf_Base64Encode(key, 32, b64, sizeof(b64)) == 0);
    TEST_ASSERT(strlen(b64) == 44);
    TEST_ASSERT(WgConf_Base64Decode(b64, back, 32) == 0);
    TEST_ASSERT_MEM_EQ(key, back, 32);

    /* Known vector, so a byte-order slip cannot pass by symmetry alone. */
    memset(key, 0xA5, 32);
    TEST_ASSERT(WgConf_Base64Encode(key, 32, b64, sizeof(b64)) == 0);
    TEST_ASSERT(strcmp(b64, KEY_A5_B64) == 0);

    /* Rejects: wrong length, bad alphabet, misplaced padding.  The last one
     * is right-length and would decode to 32 bytes if the '=' were simply
     * skipped, so it catches a lenient decoder rather than a lazy one. */
    TEST_ASSERT(WgConf_Base64Decode("AAAA", back, 32) != 0);
    TEST_ASSERT(WgConf_Base64Decode(
        "!aWlpaWlpaWlpaWlpaWlpaWlpaWlpaWlpaWlpaWlpaU=", back, 32) != 0);
    TEST_ASSERT(WgConf_Base64Decode(
        "paWlpaWlpaWlpaWlpaWlpaWlpaWlpaWlpaWlpaWlpa=U", back, 32) != 0);
    TEST_ASSERT(WgConf_Base64Encode(key, 32, b64, 10) != 0);
}

static void test_parses_dashboard_file(void)
{
    sWgConfParser  p;
    const char    *err = NULL;
    const sWgConf *c;
    uint8_t        expect[32];

    TEST_ASSERT(parse_all(conf_ok, &p, &err) == 0);
    c = WgConf_Result(&p);

    memset(expect, 0xA5, sizeof(expect));
    TEST_ASSERT_MEM_EQ(c->privateKey, expect, 32);
    memset(expect, 0x01, sizeof(expect));
    TEST_ASSERT_MEM_EQ(c->peerPublicKey, expect, 32);

    TEST_ASSERT(c->tunnelIp[0] == 10 && c->tunnelIp[1] == 77 &&
                c->tunnelIp[2] == 0  && c->tunnelIp[3] == 65);
    /* /32 must be preserved verbatim — the routes come from AllowedIPs. */
    TEST_ASSERT(c->tunnelMask[0] == 255 && c->tunnelMask[3] == 255);

    TEST_ASSERT(c->endpointIp[0] == 85 && c->endpointIp[3] == 75);
    TEST_ASSERT(c->endpointPort == 51820);
    TEST_ASSERT(c->keepAlive_sec == 21);

    TEST_ASSERT(c->allowedCount == 1);
    TEST_ASSERT(c->allowed[0].ip[1] == 77 && c->allowed[0].ip[3] == 0);
    TEST_ASSERT(c->allowed[0].mask[2] == 255 && c->allowed[0].mask[3] == 0);
}

/* The interface address and the peer's allowed range are different things;
 * a /32 address alongside a /24 allowed range is the normal case, and
 * conflating them is precisely the bug this parser removes. */
static void test_address_and_allowed_are_independent(void)
{
    sWgConfParser  p;
    const sWgConf *c;

    TEST_ASSERT(parse_all(conf_ok, &p, NULL) == 0);
    c = WgConf_Result(&p);
    TEST_ASSERT(c->tunnelMask[3] == 255);   /* /32 on the interface */
    TEST_ASSERT(c->allowed[0].mask[3] == 0); /* /24 route           */
}

static void test_two_allowed_ranges(void)
{
    static const char text[] =
        "[Interface]\n"
        "PrivateKey = " KEY_A5_B64 "\n"
        "Address = 10.77.0.65/32\n"
        "[Peer]\n"
        "PublicKey = " KEY_01_B64 "\n"
        "AllowedIPs = 10.77.0.0/24, 192.168.0.0/24\n"
        "Endpoint = 85.206.57.75:51820\n";
    sWgConfParser  p;
    const sWgConf *c;

    TEST_ASSERT(parse_all(text, &p, NULL) == 0);
    c = WgConf_Result(&p);
    TEST_ASSERT(c->allowedCount == 2);
    TEST_ASSERT(c->allowed[1].ip[0] == 192 && c->allowed[1].ip[1] == 168);
    TEST_ASSERT(c->allowed[1].mask[2] == 255 && c->allowed[1].mask[3] == 0);
    /* Absent PersistentKeepalive means off, matching WireGuard itself. */
    TEST_ASSERT(c->keepAlive_sec == 0);
}

static void test_tolerates_formatting(void)
{
    static const char text[] =
        "\r\n"
        "# a comment\r\n"
        "  [interface]  \r\n"
        "  privatekey=" KEY_A5_B64 "   # trailing comment\r\n"
        "Address   =    10.77.0.65/32\r\n"
        "\r\n"
        "[PEER]\r\n"
        "PublicKey =" KEY_01_B64 "\r\n"
        "AllowedIPs = 10.77.0.0/24\r\n"
        "Endpoint = 85.206.57.75:51820";   /* no trailing newline */
    sWgConfParser p;

    TEST_ASSERT(parse_all(text, &p, NULL) == 0);
    TEST_ASSERT(WgConf_Result(&p)->endpointPort == 51820);
}

/* Any split point must give the same result — the upload path feeds it
 * whatever TCP segmentation happens to deliver. */
static void test_chunked_feed_matches(void)
{
    uint32_t chunk;

    for (chunk = 1u; chunk <= 7u; chunk++) {
        sWgConfParser p;
        uint32_t      i;
        uint32_t      len = (uint32_t)strlen(conf_ok);

        WgConf_Begin(&p);
        for (i = 0u; i < len; i += chunk) {
            uint32_t n = (len - i < chunk) ? (len - i) : chunk;
            WgConf_Feed(&p, (const uint8_t *)conf_ok + i, n);
        }
        TEST_ASSERT(WgConf_Finish(&p, NULL) == 0);
        TEST_ASSERT(WgConf_Result(&p)->endpointPort == 51820);
        TEST_ASSERT(WgConf_Result(&p)->allowedCount == 1);
    }
}

static void expect_reject(const char *text, const char *what)
{
    sWgConfParser p;
    const char   *err = NULL;

    if (parse_all(text, &p, &err) == 0) {
        printf("FAIL: expected reject (%s)\n", what);
        test_failures++;
    } else if (err == NULL) {
        printf("FAIL: reject without a message (%s)\n", what);
        test_failures++;
    }
}

static void test_rejects(void)
{
    expect_reject(
        "[Interface]\nPrivateKey = " KEY_A5_B64 "\nAddress = 10.77.0.65/32\n"
        "[Peer]\nPublicKey = " KEY_01_B64 "\nAllowedIPs = 10.77.0.0/24\n",
        "missing Endpoint");

    expect_reject(
        "[Interface]\nAddress = 10.77.0.65/32\n"
        "[Peer]\nPublicKey = " KEY_01_B64 "\nAllowedIPs = 10.77.0.0/24\n"
        "Endpoint = 85.206.57.75:51820\n",
        "missing PrivateKey");

    expect_reject(
        "[Interface]\nPrivateKey = " KEY_A5_B64 "\nAddress = 10.77.0.65/32\n"
        "[Peer]\nPublicKey = " KEY_01_B64 "\nAllowedIPs = 10.77.0.0/24\n"
        "Endpoint = vpn.example.com:51820\n",
        "hostname endpoint");

    expect_reject(
        "[Interface]\nPrivateKey = " KEY_A5_B64 "\nAddress = 10.77.0.65/32\n"
        "[Peer]\nPublicKey = " KEY_01_B64 "\nAllowedIPs = 10.77.0.0/24\n"
        "PresharedKey = " KEY_01_B64 "\nEndpoint = 85.206.57.75:51820\n",
        "preshared key");

    expect_reject(
        "[Interface]\nPrivateKey = tooshort=\nAddress = 10.77.0.65/32\n"
        "[Peer]\nPublicKey = " KEY_01_B64 "\nAllowedIPs = 10.77.0.0/24\n"
        "Endpoint = 85.206.57.75:51820\n",
        "malformed private key");

    expect_reject(
        "[Interface]\nPrivateKey = " KEY_A5_B64 "\nAddress = 10.77.0.65/33\n"
        "[Peer]\nPublicKey = " KEY_01_B64 "\nAllowedIPs = 10.77.0.0/24\n"
        "Endpoint = 85.206.57.75:51820\n",
        "prefix > 32");

    expect_reject(
        "[Interface]\nPrivateKey = " KEY_A5_B64 "\nAddress = 10.77.0.999/32\n"
        "[Peer]\nPublicKey = " KEY_01_B64 "\nAllowedIPs = 10.77.0.0/24\n"
        "Endpoint = 85.206.57.75:51820\n",
        "octet > 255");

    expect_reject(
        "[Interface]\nPrivateKey = " KEY_A5_B64 "\nAddress = 10.77.0.65/32\n"
        "[Peer]\nPublicKey = " KEY_01_B64 "\n"
        "AllowedIPs = 10.77.0.0/24, 192.168.0.0/24, 172.16.0.0/16\n"
        "Endpoint = 85.206.57.75:51820\n",
        "three allowed ranges");

    expect_reject(
        "PrivateKey = " KEY_A5_B64 "\n",
        "setting before any section");

    expect_reject(
        "[Interface]\nPrivateKey " KEY_A5_B64 "\n",
        "missing '='");

    /* A .pnfw blob must not be mistaken for a config even if it reaches the
     * parser — the binary has no newline-delimited 'Key = value' structure. */
    expect_reject("PNFW\x01\x02\x03\x04 binary junk\n", "binary blob");
}

static void test_error_reports_line(void)
{
    sWgConfParser p;
    const char   *err = NULL;

    TEST_ASSERT(parse_all(
        "[Interface]\n"
        "PrivateKey = " KEY_A5_B64 "\n"
        "Address = nonsense\n", &p, &err) != 0);
    TEST_ASSERT(err != NULL);
    TEST_ASSERT(WgConf_ErrorLine(&p) == 3);
}

int main(void)
{
    printf("=== wg_conf tests ===\n");
    RUN_TEST(test_base64_roundtrip);
    RUN_TEST(test_parses_dashboard_file);
    RUN_TEST(test_address_and_allowed_are_independent);
    RUN_TEST(test_two_allowed_ranges);
    RUN_TEST(test_tolerates_formatting);
    RUN_TEST(test_chunked_feed_matches);
    RUN_TEST(test_rejects);
    RUN_TEST(test_error_reports_line);

    printf("%s (%d failures)\n", test_failures ? "FAILED" : "PASSED",
           test_failures);
    return test_failures ? 1 : 0;
}
