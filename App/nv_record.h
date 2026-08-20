/**
 * @file    nv_record.h
 * @brief   A CRC'd, versioned record in one nvDb area — the user's half of
 *          the bargain.
 *
 * nvDb does not validate, protect or repair anything: each user is
 * responsible for the validity of its own data, and a user that stores
 * garbage stores it durably.  This is the small amount of ceremony that
 * obligation actually costs, in one place instead of once per module.
 *
 * A record is a struct whose FIRST member is an sNvRecordHdr.  The CRC comes
 * first and covers every byte after it, so the payload is inside the check
 * rather than beside it.  Absent, truncated, wrong-version and corrupt all
 * collapse to the same answer — "nothing valid stored" — which is the only
 * distinction a caller with a built-in default actually needs.
 *
 * Header-only and inline: this is user-side policy, and putting it anywhere
 * near nvDb would teach the store what a version is.
 */
#ifndef NV_RECORD_H
#define NV_RECORD_H

#include "image_mgmt.h"
#include "nvdb.h"

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t crc32;         /* over every byte after this field           */
    uint32_t magic;         /* whose record this is                       */
    uint16_t version;       /* the payload's layout                       */
    uint16_t size_bytes;    /* whole record, so a grown struct is caught  */
} sNvRecordHdr;

/**
 * @brief Read a record and say whether it is worth believing.
 * @param  user - the nvDb user holding it
 * @param  magic - the magic the record must carry
 * @param  version - the version it must carry
 * @param  rec - destination, beginning with an sNvRecordHdr
 * @param  size_bytes - sizeof(*rec)
 * @retval 0 if a valid record was loaded, -1 otherwise
 */
static inline int NvRecord_Load(eNvDbUser user, uint32_t magic,
                                uint16_t version, void *rec,
                                uint32_t size_bytes)
{
    sNvRecordHdr *hdr = (sNvRecordHdr *)rec;

    if (NvDb_Read(user, rec, 0u, size_bytes) != nvdbRes_ok) {
        return -1;
    }
    if (hdr->magic != magic || hdr->version != version ||
        hdr->size_bytes != (uint16_t)size_bytes) {
        return -1;
    }
    if (hdr->crc32 != ImgMgmt_Crc32((const uint8_t *)rec + sizeof(uint32_t),
                                    size_bytes - (uint32_t)sizeof(uint32_t))) {
        return -1;
    }
    return 0;
}

/**
 * @brief Stamp a record and put it on the medium.
 * @param  user - the nvDb user to hold it
 * @param  magic - the magic to stamp
 * @param  version - the version to stamp
 * @param  rec - the record, beginning with an sNvRecordHdr
 * @param  size_bytes - sizeof(*rec)
 * @retval 0 on success, -1 if the medium refused
 * @note Returns when the bytes are on the medium: nvDb has no mirror and no
 *       commit, so there is no second call to forget.
 */
static inline int NvRecord_Save(eNvDbUser user, uint32_t magic,
                                uint16_t version, void *rec,
                                uint32_t size_bytes)
{
    sNvRecordHdr *hdr = (sNvRecordHdr *)rec;

    hdr->magic      = magic;
    hdr->version    = version;
    hdr->size_bytes = (uint16_t)size_bytes;
    hdr->crc32      = ImgMgmt_Crc32((const uint8_t *)rec + sizeof(uint32_t),
                                    size_bytes - (uint32_t)sizeof(uint32_t));

    return (NvDb_Write(user, rec, 0u, size_bytes) == nvdbRes_ok) ? 0 : -1;
}

/**
 * @brief Forget a record: unreadable now, reclaimed shortly.
 * @param  user - the nvDb user holding it
 * @retval 0 on success, -1 if the medium refused
 * @note The magic is cleared synchronously — clearing bits needs no erase —
 *       and the wipe that follows is what makes the next write cheap.  nvDb's
 *       delete alone would be eventual, and "forgotten" has to be true when
 *       this returns.
 */
static inline int NvRecord_Forget(eNvDbUser user)
{
    uint32_t dead = 0u;

    if (NvDb_Write(user, &dead, offsetof(sNvRecordHdr, magic),
                   sizeof(dead)) != nvdbRes_ok) {
        return -1;
    }
    return (NvDb_Wipe(user, NULL) == nvdbRes_ok) ? 0 : -1;
}

#ifdef __cplusplus
}
#endif

#endif /* NV_RECORD_H */
