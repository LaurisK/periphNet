/**
 * @file    wg_cfg.c
 * @brief   Persistent WireGuard configuration — see wg_cfg.h.
 */

#include "App/Net/wg_cfg.h"

#include <string.h>

#include "Shared/Drivers/w25q128.h"
#include "Shared/Fwu/bl_app_contract.h"
#include "Shared/Fwu/image_mgmt.h"
#include "trice.h"

/* --------------------------------------------------------------------------
 * Record formats
 * -------------------------------------------------------------------------- */

#define WG_CFG_MAGIC      0x46434757u   /* "WGCF" little-endian */
#define WG_CFG_VERSION    2u

/* v1 — network fields only, keys were still baked into the image.  Kept so a
 * board provisioned before v2 does not silently lose its tunnel address. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint8_t  tunnelIp[4];
    uint8_t  tunnelMask[4];
    uint8_t  endpointIp[4];
    uint16_t endpointPort;
    uint16_t keepAlive_sec;
    uint8_t  reserved[32];
    uint32_t crc32;
} __attribute__((packed)) sWgCfgRecordV1;

/* v2 — adds identity.  Rewritten whole on every change, so a plain magic+CRC
 * record is enough; no NOR bit-clear trickery like sBootStatus, which has to
 * mutate flags without an erase. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint8_t  tunnelIp[4];
    uint8_t  tunnelMask[4];
    uint8_t  endpointIp[4];
    uint16_t endpointPort;
    uint16_t keepAlive_sec;
    uint8_t  privateKey[WG_CONF_KEY_SIZE];
    uint8_t  peerPublicKey[WG_CONF_KEY_SIZE];
    uint8_t  hasPrivateKey;
    uint8_t  hasPeerKey;
    uint8_t  allowedCount;
    uint8_t  pad0;
    uint8_t  allowedIp[WG_CONF_MAX_ALLOWED][4];
    uint8_t  allowedMask[WG_CONF_MAX_ALLOWED][4];
    uint8_t  reserved[16];
    uint32_t crc32;
} __attribute__((packed)) sWgCfgRecordV2;

/* Big enough for either layout; both start with the same magic/version/size
 * prefix, so one read serves both. */
typedef union {
    sWgCfgRecordV1 v1;
    sWgCfgRecordV2 v2;
    uint8_t        raw[sizeof(sWgCfgRecordV2)];
} uWgCfgRecord;

static uint32_t record_crc(const void *rec, uint32_t size)
{
    /* CRC covers everything before the trailing crc32 field. */
    return ImgMgmt_Crc32((const uint8_t *)rec, size - 4u);
}

/* Read and validate whichever version is stored.  Returns the version (1 or
 * 2), or 0 if nothing valid is there. */
static uint16_t record_read(uWgCfgRecord *rec)
{
    uint32_t stored_crc;

    if (W25Q128_Read(EXT_FLASH_WG_CFG_ADDR, rec->raw,
                     (uint32_t)sizeof(rec->raw)) != w25q_ok) {
        return 0u;
    }
    if (rec->v1.magic != WG_CFG_MAGIC) {
        return 0u;
    }

    if (rec->v1.version == 1u &&
        rec->v1.size == (uint16_t)sizeof(sWgCfgRecordV1)) {
        stored_crc = rec->v1.crc32;
        return (stored_crc == record_crc(rec, sizeof(sWgCfgRecordV1)))
                   ? 1u : 0u;
    }

    if (rec->v2.version == 2u &&
        rec->v2.size == (uint16_t)sizeof(sWgCfgRecordV2)) {
        stored_crc = rec->v2.crc32;
        return (stored_crc == record_crc(rec, sizeof(sWgCfgRecordV2)))
                   ? 2u : 0u;
    }

    return 0u;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int WgCfg_Load(sWgLinkCfg *cfg)
{
    uWgCfgRecord rec;
    uint16_t     ver;

    if (cfg == NULL) {
        return -1;
    }

    ver = record_read(&rec);
    if (ver == 0u) {
        return -2;
    }

    /* The v1 prefix is byte-identical in v2, so the network fields can be
     * taken from either without a branch. */
    memcpy(cfg->tunnelIp,   rec.v1.tunnelIp,   sizeof(cfg->tunnelIp));
    memcpy(cfg->tunnelMask, rec.v1.tunnelMask, sizeof(cfg->tunnelMask));
    memcpy(cfg->endpointIp, rec.v1.endpointIp, sizeof(cfg->endpointIp));
    cfg->endpointPort = rec.v1.endpointPort;
    cfg->keepAlive_sec = rec.v1.keepAlive_sec;

    if (ver == 1u) {
        /* Pre-key record: keys stay as the caller had them, and the allowed
         * range is synthesised the way the old code derived it — from the
         * tunnel address and mask — so behaviour is unchanged. */
        cfg->allowedCount = 1u;
        cfg->allowed[0].ip[0] = cfg->tunnelIp[0] & cfg->tunnelMask[0];
        cfg->allowed[0].ip[1] = cfg->tunnelIp[1] & cfg->tunnelMask[1];
        cfg->allowed[0].ip[2] = cfg->tunnelIp[2] & cfg->tunnelMask[2];
        cfg->allowed[0].ip[3] = cfg->tunnelIp[3] & cfg->tunnelMask[3];
        memcpy(cfg->allowed[0].mask, cfg->tunnelMask, 4u);
        return 0;
    }

    if (rec.v2.hasPrivateKey) {
        memcpy(cfg->privateKey, rec.v2.privateKey, WG_CONF_KEY_SIZE);
        cfg->hasPrivateKey = 1u;
    }
    if (rec.v2.hasPeerKey) {
        memcpy(cfg->peerPublicKey, rec.v2.peerPublicKey, WG_CONF_KEY_SIZE);
        cfg->hasPeerKey = 1u;
    }

    if (rec.v2.allowedCount > 0u &&
        rec.v2.allowedCount <= (uint8_t)WG_CONF_MAX_ALLOWED) {
        uint8_t i;
        cfg->allowedCount = rec.v2.allowedCount;
        for (i = 0u; i < cfg->allowedCount; i++) {
            memcpy(cfg->allowed[i].ip,   rec.v2.allowedIp[i],   4u);
            memcpy(cfg->allowed[i].mask, rec.v2.allowedMask[i], 4u);
        }
    }

    return 0;
}

int WgCfg_Save(const sWgLinkCfg *cfg)
{
    sWgCfgRecordV2 rec;
    uint8_t        i;

    if (cfg == NULL) {
        return -1;
    }

    memset(&rec, 0, sizeof(rec));
    rec.magic   = WG_CFG_MAGIC;
    rec.version = WG_CFG_VERSION;
    rec.size    = (uint16_t)sizeof(rec);
    memcpy(rec.tunnelIp,   cfg->tunnelIp,   sizeof(rec.tunnelIp));
    memcpy(rec.tunnelMask, cfg->tunnelMask, sizeof(rec.tunnelMask));
    memcpy(rec.endpointIp, cfg->endpointIp, sizeof(rec.endpointIp));
    rec.endpointPort  = cfg->endpointPort;
    rec.keepAlive_sec = cfg->keepAlive_sec;

    if (cfg->hasPrivateKey) {
        memcpy(rec.privateKey, cfg->privateKey, WG_CONF_KEY_SIZE);
        rec.hasPrivateKey = 1u;
    }
    if (cfg->hasPeerKey) {
        memcpy(rec.peerPublicKey, cfg->peerPublicKey, WG_CONF_KEY_SIZE);
        rec.hasPeerKey = 1u;
    }

    rec.allowedCount = (cfg->allowedCount > (uint8_t)WG_CONF_MAX_ALLOWED)
                           ? (uint8_t)WG_CONF_MAX_ALLOWED : cfg->allowedCount;
    for (i = 0u; i < rec.allowedCount; i++) {
        memcpy(rec.allowedIp[i],   cfg->allowed[i].ip,   4u);
        memcpy(rec.allowedMask[i], cfg->allowed[i].mask, 4u);
    }

    rec.crc32 = record_crc(&rec, (uint32_t)sizeof(rec));

    if (W25Q128_EraseSector(EXT_FLASH_WG_CFG_ADDR) != w25q_ok) {
        TRice("WG cfg: erase failed\n");
        return -2;
    }
    /* sizeof(rec) is below the 256 B page size, so one write suffices. */
    if (W25Q128_WritePage(EXT_FLASH_WG_CFG_ADDR, (const uint8_t *)&rec,
                          (uint32_t)sizeof(rec)) != w25q_ok) {
        TRice("WG cfg: write failed\n");
        return -3;
    }

    return 0;
}

int WgCfg_Clear(void)
{
    if (W25Q128_EraseSector(EXT_FLASH_WG_CFG_ADDR) != w25q_ok) {
        return -1;
    }
    return 0;
}

int WgCfg_IsStored(void)
{
    uWgCfgRecord rec;

    return (record_read(&rec) != 0u) ? 1 : 0;
}

uint16_t WgCfg_StoredVersion(void)
{
    uWgCfgRecord rec;

    return record_read(&rec);
}
