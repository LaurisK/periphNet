/**
 * @file    wg_conf.c
 * @brief   WireGuard `.conf` parser — see wg_conf.h.
 */

#include "App/Net/wg_conf.h"

#include <string.h>

/* --------------------------------------------------------------------------
 * Base64
 * -------------------------------------------------------------------------- */

static const char b64_tbl[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') { return (int)(c - 'A'); }
    if (c >= 'a' && c <= 'z') { return (int)(c - 'a') + 26; }
    if (c >= '0' && c <= '9') { return (int)(c - '0') + 52; }
    if (c == '+') { return 62; }
    if (c == '/') { return 63; }
    return -1;
}

int WgConf_Base64Decode(const char *s, uint8_t *out, uint32_t outLen)
{
    uint32_t enc;
    uint32_t o = 0u;
    uint32_t i;

    if (s == NULL || out == NULL || outLen == 0u) {
        return -1;
    }

    enc = ((outLen + 2u) / 3u) * 4u;
    if (strlen(s) != (size_t)enc) {
        return -2;
    }

    for (i = 0u; i < enc; i += 4u) {
        int      v[4];
        uint32_t triple;
        int      k;

        for (k = 0; k < 4; k++) {
            char c = s[i + (uint32_t)k];
            if (c == '=') {
                /* Padding is legal only as the tail of the final quad. */
                if ((i + 4u) != enc || k < 2) {
                    return -3;
                }
                v[k] = -1;
            } else {
                v[k] = b64_val(c);
                if (v[k] < 0) {
                    return -3;
                }
            }
        }

        /* Padding fills the tail: a pad in position 2 forces one in 3 too,
         * otherwise "..a=U" would decode as if the '=' were not there. */
        if (v[2] < 0 && v[3] >= 0) {
            return -3;
        }

        triple = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12);
        if (v[2] >= 0) { triple |= (uint32_t)v[2] << 6; }
        if (v[3] >= 0) { triple |= (uint32_t)v[3]; }

        if (o < outLen) { out[o++] = (uint8_t)(triple >> 16); }
        if (v[2] >= 0 && o < outLen) { out[o++] = (uint8_t)(triple >> 8); }
        if (v[3] >= 0 && o < outLen) { out[o++] = (uint8_t)triple; }
    }

    return (o == outLen) ? 0 : -4;
}

int WgConf_Base64Encode(const uint8_t *in, uint32_t inLen,
                        char *out, uint32_t outSz)
{
    uint32_t need;
    uint32_t o = 0u;
    uint32_t i;

    if (in == NULL || out == NULL) {
        return -1;
    }

    need = ((inLen + 2u) / 3u) * 4u + 1u;
    if (outSz < need) {
        return -1;
    }

    for (i = 0u; i < inLen; i += 3u) {
        uint32_t b0 = in[i];
        uint32_t b1 = ((i + 1u) < inLen) ? in[i + 1u] : 0u;
        uint32_t b2 = ((i + 2u) < inLen) ? in[i + 2u] : 0u;
        uint32_t t  = (b0 << 16) | (b1 << 8) | b2;

        out[o++] = b64_tbl[(t >> 18) & 0x3Fu];
        out[o++] = b64_tbl[(t >> 12) & 0x3Fu];
        out[o++] = ((i + 1u) < inLen) ? b64_tbl[(t >> 6) & 0x3Fu] : '=';
        out[o++] = ((i + 2u) < inLen) ? b64_tbl[t & 0x3Fu] : '=';
    }

    out[o] = '\0';
    return 0;
}

/* --------------------------------------------------------------------------
 * Small text helpers
 * -------------------------------------------------------------------------- */

static int is_space(char c)
{
    return (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f');
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive whole-string compare. */
static int eq_ci(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (lower(*a) != lower(*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

/* Trim in place, returning the first non-space character. */
static char *trim(char *s)
{
    char *end;

    while (*s != '\0' && is_space(*s)) {
        s++;
    }
    if (*s == '\0') {
        return s;
    }
    end = s + strlen(s) - 1u;
    while (end > s && is_space(*end)) {
        *end-- = '\0';
    }
    return s;
}

/* Parse "a.b.c.d", stopping at the first character that cannot belong to it.
 * Returns 0 on success and points @p end just past the address. */
static int parse_ipv4(const char *s, uint8_t ip[4], const char **end)
{
    int i;

    for (i = 0; i < 4; i++) {
        uint32_t v      = 0u;
        int      digits = 0;

        if (i > 0) {
            if (*s != '.') {
                return -1;
            }
            s++;
        }
        while (*s >= '0' && *s <= '9') {
            v = (v * 10u) + (uint32_t)(*s - '0');
            if (v > 255u) {
                return -1;
            }
            s++;
            digits++;
            if (digits > 3) {
                return -1;
            }
        }
        if (digits == 0) {
            return -1;
        }
        ip[i] = (uint8_t)v;
    }

    if (end != NULL) {
        *end = s;
    }
    return 0;
}

static int prefix_to_mask(uint32_t prefix, uint8_t mask[4])
{
    uint32_t bits;
    int      i;

    if (prefix > 32u) {
        return -1;
    }
    bits = (prefix == 0u) ? 0u : (0xFFFFFFFFu << (32u - prefix));
    for (i = 0; i < 4; i++) {
        mask[i] = (uint8_t)((bits >> (24 - (8 * i))) & 0xFFu);
    }
    return 0;
}

/* Parse "a.b.c.d" or "a.b.c.d/p".  A bare address is a host route (/32),
 * which is what wg-quick assumes too. */
static int parse_cidr(const char *s, uint8_t ip[4], uint8_t mask[4])
{
    const char *p = NULL;
    uint32_t    prefix = 32u;

    if (parse_ipv4(s, ip, &p) != 0) {
        return -1;
    }
    if (*p == '/') {
        int digits = 0;
        p++;
        prefix = 0u;
        while (*p >= '0' && *p <= '9') {
            prefix = (prefix * 10u) + (uint32_t)(*p - '0');
            p++;
            digits++;
            if (digits > 2 || prefix > 32u) {
                return -1;
            }
        }
        if (digits == 0) {
            return -1;
        }
    }
    if (*p != '\0') {
        return -1;
    }
    return prefix_to_mask(prefix, mask);
}

/* --------------------------------------------------------------------------
 * Parser
 * -------------------------------------------------------------------------- */

void WgConf_Begin(sWgConfParser *p)
{
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof(*p));
}

/* First error wins — later ones are usually consequences of it. */
static void fail(sWgConfParser *p, const char *msg)
{
    if (p->err == NULL) {
        p->err     = msg;
        p->errLine = p->lineNo;
    }
}

static void handle_interface(sWgConfParser *p, const char *key, char *val)
{
    if (eq_ci(key, "PrivateKey")) {
        if (WgConf_Base64Decode(val, p->conf.privateKey,
                                WG_CONF_KEY_SIZE) != 0) {
            fail(p, "PrivateKey is not a 32-byte base64 key");
            return;
        }
        p->conf.fields |= (uint16_t)wgConfField_privateKey;
    } else if (eq_ci(key, "Address")) {
        /* Only the first address of a comma list is used; a second one would
         * need a second netif address, which lwIP does not do. */
        char *comma = strchr(val, ',');
        if (comma != NULL) {
            *comma = '\0';
            val    = trim(val);
        }
        if (parse_cidr(val, p->conf.tunnelIp, p->conf.tunnelMask) != 0) {
            fail(p, "Address is not a valid IPv4 CIDR");
            return;
        }
        p->conf.fields |= (uint16_t)wgConfField_address;
    } else if (eq_ci(key, "PresharedKey")) {
        fail(p, "PresharedKey is not supported");
    }
    /* MTU / DNS / ListenPort / Table / SaveConfig / PreUp / PostUp: ignored
     * on purpose — they are wg-quick host plumbing with no board analogue. */
}

static void handle_peer(sWgConfParser *p, const char *key, char *val)
{
    if (eq_ci(key, "PublicKey")) {
        if (WgConf_Base64Decode(val, p->conf.peerPublicKey,
                                WG_CONF_KEY_SIZE) != 0) {
            fail(p, "PublicKey is not a 32-byte base64 key");
            return;
        }
        p->conf.fields |= (uint16_t)wgConfField_peerPublicKey;
    } else if (eq_ci(key, "Endpoint")) {
        const char *rest = NULL;
        uint32_t    port = 0u;
        int         digits = 0;

        if (parse_ipv4(val, p->conf.endpointIp, &rest) != 0) {
            fail(p, "Endpoint must be a literal IPv4 address (no hostnames)");
            return;
        }
        if (*rest != ':') {
            fail(p, "Endpoint is missing :port");
            return;
        }
        rest++;
        while (*rest >= '0' && *rest <= '9') {
            port = (port * 10u) + (uint32_t)(*rest - '0');
            rest++;
            digits++;
            if (port > 65535u) {
                break;
            }
        }
        if (digits == 0 || port == 0u || port > 65535u || *rest != '\0') {
            fail(p, "Endpoint port is out of range");
            return;
        }
        p->conf.endpointPort = (uint16_t)port;
        p->conf.fields |= (uint16_t)wgConfField_endpoint;
    } else if (eq_ci(key, "AllowedIPs")) {
        char *cur = val;

        p->conf.allowedCount = 0u;
        while (*cur != '\0') {
            char *comma = strchr(cur, ',');
            char *item;

            if (comma != NULL) {
                *comma = '\0';
            }
            item = trim(cur);
            if (*item != '\0') {
                if (p->conf.allowedCount >= WG_CONF_MAX_ALLOWED) {
                    fail(p, "more than 2 AllowedIPs ranges");
                    return;
                }
                if (parse_cidr(item,
                        p->conf.allowed[p->conf.allowedCount].ip,
                        p->conf.allowed[p->conf.allowedCount].mask) != 0) {
                    fail(p, "AllowedIPs entry is not a valid IPv4 CIDR");
                    return;
                }
                p->conf.allowedCount++;
            }
            if (comma == NULL) {
                break;
            }
            cur = comma + 1;
        }
        if (p->conf.allowedCount == 0u) {
            fail(p, "AllowedIPs is empty");
            return;
        }
        p->conf.fields |= (uint16_t)wgConfField_allowedIps;
    } else if (eq_ci(key, "PersistentKeepalive")) {
        uint32_t v      = 0u;
        int      digits = 0;
        const char *s   = val;

        while (*s >= '0' && *s <= '9') {
            v = (v * 10u) + (uint32_t)(*s - '0');
            s++;
            digits++;
            if (v > 65535u) {
                break;
            }
        }
        if (digits == 0 || v > 65535u || *s != '\0') {
            fail(p, "PersistentKeepalive is not a number of seconds");
            return;
        }
        p->conf.keepAlive_sec = (uint16_t)v;
        p->conf.fields |= (uint16_t)wgConfField_keepAlive;
    } else if (eq_ci(key, "PresharedKey")) {
        fail(p, "PresharedKey is not supported");
    }
}

static void process_line(sWgConfParser *p)
{
    char *s;
    char *hash;
    char *eq;
    char *key;
    char *val;

    p->lineNo++;
    p->line[p->lineLen] = '\0';

    if (p->overflow) {
        fail(p, "line too long");
        p->overflow = 0u;
        p->lineLen  = 0u;
        return;
    }

    /* Comments run to end of line; '#' cannot appear inside any field we
     * care about (base64 does not use it). */
    hash = strchr(p->line, '#');
    if (hash != NULL) {
        *hash = '\0';
    }

    s = trim(p->line);
    if (*s == '\0') {
        p->lineLen = 0u;
        return;
    }

    if (*s == '[') {
        char *close = strchr(s, ']');
        if (close == NULL) {
            fail(p, "unterminated section header");
            p->lineLen = 0u;
            return;
        }
        *close = '\0';
        s      = trim(s + 1);
        if (eq_ci(s, "Interface")) {
            p->section = 1u;
        } else if (eq_ci(s, "Peer")) {
            p->section = 2u;
        } else {
            fail(p, "unknown section (expected [Interface] or [Peer])");
        }
        p->lineLen = 0u;
        return;
    }

    eq = strchr(s, '=');
    if (eq == NULL) {
        fail(p, "expected 'Key = value'");
        p->lineLen = 0u;
        return;
    }
    *eq = '\0';
    key = trim(s);
    val = trim(eq + 1);

    if (p->section == 1u) {
        handle_interface(p, key, val);
    } else if (p->section == 2u) {
        handle_peer(p, key, val);
    } else {
        fail(p, "setting outside any section");
    }

    p->lineLen = 0u;
}

void WgConf_Feed(sWgConfParser *p, const uint8_t *data, uint32_t len)
{
    uint32_t i;

    if (p == NULL || data == NULL) {
        return;
    }

    for (i = 0u; i < len; i++) {
        char c = (char)data[i];

        if (c == '\n') {
            process_line(p);
        } else if (p->lineLen < (uint16_t)(WG_CONF_LINE_MAX - 1u)) {
            p->line[p->lineLen++] = c;
        } else {
            /* Keep consuming to the newline so the rest of the file still
             * parses; the line itself is reported as an error. */
            p->overflow = 1u;
        }
    }
}

int WgConf_Finish(sWgConfParser *p, const char **err)
{
    static const char *missing = "missing required field "
        "(need PrivateKey, Address, PublicKey, Endpoint, AllowedIPs)";
    const uint16_t required =
        (uint16_t)wgConfField_privateKey  |
        (uint16_t)wgConfField_address     |
        (uint16_t)wgConfField_peerPublicKey |
        (uint16_t)wgConfField_endpoint    |
        (uint16_t)wgConfField_allowedIps;

    if (p == NULL) {
        return -1;
    }

    /* A file whose last line has no newline still has content pending. */
    if (p->lineLen > 0u || p->overflow) {
        process_line(p);
    }

    if (p->err != NULL) {
        if (err != NULL) {
            *err = p->err;
        }
        return -1;
    }

    if ((p->conf.fields & required) != required) {
        if (err != NULL) {
            *err = missing;
        }
        return -1;
    }

    return 0;
}

uint16_t WgConf_ErrorLine(const sWgConfParser *p)
{
    return (p != NULL) ? p->errLine : 0u;
}

const sWgConf *WgConf_Result(const sWgConfParser *p)
{
    return (p != NULL) ? &p->conf : NULL;
}
