#include "App/Net/wg_cfg.h"

#include <string.h>

#include "Shared/Drivers/w25q128.h"
#include "Shared/Fwu/bl_app_contract.h"
#include "Shared/Fwu/image_mgmt.h"
#include "trice.h"

/* --------------------------------------------------------------------------
 * Record format
 * -------------------------------------------------------------------------- */

#define WG_CFG_MAGIC    0x46434757u   /* "WGCF" little-endian */
#define WG_CFG_VERSION  1u

/* Rewritten whole on every change, so a plain magic+CRC record is enough —
 * no NOR bit-clear trickery like sBootStatus, which has to mutate flags
 * without an erase. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;              /* sizeof(sWgCfgRecord), for forward compat */
    uint8_t  tunnelIp[4];
    uint8_t  tunnelMask[4];
    uint8_t  endpointIp[4];
    uint16_t endpointPort;
    uint16_t keepAlive;
    uint8_t  reserved[32];      /* room for a provisioned private key */
    uint32_t crc32;             /* over everything before this field */
} __attribute__((packed)) sWgCfgRecord;

static uint32_t record_crc(const sWgCfgRecord *rec)
{
    return ImgMgmt_Crc32((const uint8_t *)rec,
                         (uint32_t)(sizeof(*rec) - sizeof(rec->crc32)));
}

static int record_read(sWgCfgRecord *rec)
{
    if (W25Q128_Read(EXT_FLASH_WG_CFG_ADDR, (uint8_t *)rec,
                     (uint32_t)sizeof(*rec)) != w25q_ok) {
        return -1;
    }
    if (rec->magic != WG_CFG_MAGIC || rec->version != WG_CFG_VERSION ||
        rec->size != (uint16_t)sizeof(*rec)) {
        return -2;
    }
    if (rec->crc32 != record_crc(rec)) {
        return -3;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int WgCfg_Load(sWgLinkCfg *cfg)
{
    sWgCfgRecord rec;

    if (cfg == NULL) {
        return -1;
    }
    if (record_read(&rec) != 0) {
        return -2;
    }

    memcpy(cfg->tunnelIp,   rec.tunnelIp,   sizeof(cfg->tunnelIp));
    memcpy(cfg->tunnelMask, rec.tunnelMask, sizeof(cfg->tunnelMask));
    memcpy(cfg->endpointIp, rec.endpointIp, sizeof(cfg->endpointIp));
    cfg->endpointPort = rec.endpointPort;
    cfg->keepAlive    = rec.keepAlive;

    return 0;
}

int WgCfg_Save(const sWgLinkCfg *cfg)
{
    sWgCfgRecord rec;

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
    rec.endpointPort = cfg->endpointPort;
    rec.keepAlive    = cfg->keepAlive;
    rec.crc32        = record_crc(&rec);

    if (W25Q128_EraseSector(EXT_FLASH_WG_CFG_ADDR) != w25q_ok) {
        TRice("WG cfg: erase failed\n");
        return -2;
    }
    /* sizeof(rec) is far below the 256 B page size, so one write suffices. */
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
    sWgCfgRecord rec;

    return (record_read(&rec) == 0) ? 1 : 0;
}
