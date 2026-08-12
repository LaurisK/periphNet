/**
 * @file    wg_conf.h
 * @brief   WireGuard `.conf` (wg-quick INI) parser.
 *
 * Turns the file WGDashboard hands out into the parameters the link needs, so
 * a board's identity and network settings arrive as uploaded data instead of
 * being baked into the image.
 *
 * The parser is fed a byte stream and keeps only one line at a time, so an
 * upload can be parsed straight off the socket without buffering the file.
 * It depends on nothing but libc, which is what makes it host-testable.
 *
 * ## The two addresses are NOT the same thing
 *
 * A `.conf` carries two different notions that are easy to conflate:
 *
 *   [Interface] Address    — this device's own address inside the tunnel,
 *                            conventionally a /32.
 *   [Peer]      AllowedIPs — which destinations route INTO the tunnel, and
 *                            which inner source addresses the peer may claim.
 *
 * `wg-quick` treats them separately: the address goes on the interface, the
 * AllowedIPs become routes.  So must we — deriving one from the other works
 * only by accident when the mask happens to be wide enough, and fails
 * silently when it is not (the handshake still succeeds, every packet is
 * dropped).
 *
 * ## Deliberate limits
 *
 * - `PresharedKey` is rejected rather than ignored: silently dropping it would
 *   produce a tunnel that cannot handshake against a hub that expects one.
 * - `Endpoint` must be a literal IPv4 address; no resolver is wired to this
 *   path, and a hostname would fail later and less clearly.
 * - `PersistentKeepalive` defaults to 0 (off) when absent, matching WireGuard
 *   itself.  Behind NAT you almost always want it set; `wg status` reports the
 *   effective value so an accidental 0 is visible.
 */

#ifndef WG_CONF_H_
#define WG_CONF_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Peer allowed-IP ranges honoured.  The bundled port's
 *  WIREGUARD_MAX_SRC_IPS is 2, which is the real ceiling. */
#define WG_CONF_MAX_ALLOWED   2u

/** Raw key length (base64 form is 44 chars + NUL). */
#define WG_CONF_KEY_SIZE      32u

/** Longest line accepted.  A base64 key line is ~60 chars; two CIDR ranges
 *  with spaces are ~40.  160 leaves generous headroom without a big buffer. */
#define WG_CONF_LINE_MAX      160u

/** Which fields a parse actually produced. */
typedef enum {
    wgConfField_privateKey    = 0x01u,
    wgConfField_address       = 0x02u,
    wgConfField_peerPublicKey = 0x04u,
    wgConfField_endpoint      = 0x08u,
    wgConfField_allowedIps    = 0x10u,
    wgConfField_keepAlive     = 0x20u,
} eWgConfField;

/** One CIDR range, held as address + mask rather than a prefix length so it
 *  drops straight into lwIP without further conversion. */
typedef struct {
    uint8_t ip[4];
    uint8_t mask[4];
} sWgIpRange;

/** Everything a `.conf` can tell us. */
typedef struct {
    uint8_t    privateKey[WG_CONF_KEY_SIZE];     /* raw, not base64        */
    uint8_t    peerPublicKey[WG_CONF_KEY_SIZE];  /* raw, the hub's         */
    uint8_t    tunnelIp[4];                      /* [Interface] Address    */
    uint8_t    tunnelMask[4];                    /* its prefix, as a mask  */
    uint8_t    endpointIp[4];
    uint16_t   endpointPort;
    uint16_t   keepAlive_sec;
    sWgIpRange allowed[WG_CONF_MAX_ALLOWED];
    uint8_t    allowedCount;
    uint16_t   fields;                           /* eWgConfField bitmask   */
} sWgConf;

/** Parser state.  Caller owns the storage; nothing is allocated. */
typedef struct {
    char        line[WG_CONF_LINE_MAX];
    uint16_t    lineLen;
    uint16_t    lineNo;
    uint8_t     section;        /* 0 = none, 1 = [Interface], 2 = [Peer] */
    uint8_t     overflow;       /* a line exceeded the buffer            */
    const char *err;            /* first error, NULL while healthy       */
    uint16_t    errLine;
    sWgConf     conf;
} sWgConfParser;

/**
 * @brief  Reset a parser ready for a new file.
 */
void WgConf_Begin(sWgConfParser *p);

/**
 * @brief  Feed the next chunk of the file.  Any split point is safe.
 */
void WgConf_Feed(sWgConfParser *p, const uint8_t *data, uint32_t len);

/**
 * @brief  Flush the final (possibly unterminated) line and check completeness.
 *
 * @param  p    parser
 * @param  err  receives a human-readable reason on failure, untouched on
 *              success.  Points into static storage or the parser.
 * @return 0 if the file parsed and carries every required field, negative
 *         otherwise.  Required: PrivateKey, Address, PublicKey, Endpoint,
 *         AllowedIPs.
 */
int WgConf_Finish(sWgConfParser *p, const char **err);

/**
 * @brief  The line number the first error occurred on (0 if none).
 */
uint16_t WgConf_ErrorLine(const sWgConfParser *p);

/**
 * @brief  Parse result.  Only meaningful after WgConf_Finish() returned 0.
 */
const sWgConf *WgConf_Result(const sWgConfParser *p);

/* --------------------------------------------------------------------------
 * Base64 — exposed because keys cross this boundary in both directions and
 * a second implementation elsewhere would be one more thing to get wrong.
 * -------------------------------------------------------------------------- */

/**
 * @brief  Decode exactly @p outLen bytes of standard base64.
 * @return 0 on success, negative if the text is malformed or the wrong length.
 */
int WgConf_Base64Decode(const char *s, uint8_t *out, uint32_t outLen);

/**
 * @brief  Encode @p inLen bytes.  @p outSz must hold 4*ceil(inLen/3) + 1.
 * @return 0 on success, negative if the buffer is too small.
 */
int WgConf_Base64Encode(const uint8_t *in, uint32_t inLen,
                        char *out, uint32_t outSz);

#ifdef __cplusplus
}
#endif

#endif /* WG_CONF_H_ */
