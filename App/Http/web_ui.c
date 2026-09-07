/**
 * @file    web_ui.c
 * @brief   See web_ui.h.
 */

#include "App/Http/web_ui.h"

#include "nvdb.h"
#include "image_mgmt.h"
#include "trice.h"

#include <string.h>

/* The container dfu_image_tool.py `ui` writes.  Matched by bytes, not by a
 * packed uint32, so no endianness assumption is made about the file. */
typedef struct {
    char     magic[4];          /* "PNUI"                                   */
    uint16_t version;
    uint16_t reserved;
    uint32_t payload_size;      /* gzip bytes following this header         */
    uint32_t payload_crc32;     /* over those bytes                         */
} __attribute__((packed)) sWebUiHdr;

#define WEBUI_HDR_SIZE      16u
#define WEBUI_FORMAT_VER    1u

/* Staging for the CRC verify.  Plain .bss, NOT CCM: the flash driver silently
 * drops to polled transfers for any buffer DMA cannot reach, and CCM is
 * exactly that.  256 B rather than something roomier because main SRAM is the
 * second-tightest region on this part -- 21 SPI reads for a 5 KB page is not
 * a cost anybody can measure, and half a kilobyte of .bss is.
 *
 * Kept SEPARATE from the serve path's buffer on purpose: WebUi_Init() runs in
 * defaultTask while a page is served from the http task, so one shared buffer
 * would be a race for the sake of 256 bytes. */
static uint8_t s_chunk[256];

static struct {
    bool     present;           /* header valid AND payload CRC verified    */
    uint32_t payload_size;
    uint32_t payload_crc32;
    /* upload in progress */
    bool     uploading;
    uint32_t expect_total;      /* header + payload, as announced           */
    uint32_t received;
    sWebUiHdr pending;          /* the header, held back until it verifies  */
} s_ui;

_Static_assert(sizeof(sWebUiHdr) == WEBUI_HDR_SIZE, "pnui header is 16 bytes");

/* -------------------------------------------------------------------------- */

/** CRC32 the stored payload and compare it against `hdr`. */
static bool payloadVerifies(const sWebUiHdr *hdr)
{
    uint32_t state = ImgMgmt_Crc32Init();
    uint32_t off   = 0u;

    while (off < hdr->payload_size) {
        uint32_t n = hdr->payload_size - off;

        if (n > sizeof(s_chunk)) {
            n = sizeof(s_chunk);
        }
        if (NvDb_Read(nvdbUser_webUi, s_chunk,
                      WEBUI_HDR_SIZE + off, n) != nvdbRes_ok) {
            return false;
        }
        state = ImgMgmt_Crc32Update(state, s_chunk, n);
        off  += n;
    }

    return ImgMgmt_Crc32Final(state) == hdr->payload_crc32;
}

static bool headerLooksSane(const sWebUiHdr *hdr)
{
    return (memcmp(hdr->magic, "PNUI", 4) == 0) &&
           (hdr->version == WEBUI_FORMAT_VER)   &&
           (hdr->payload_size > 0u)             &&
           (hdr->payload_size <= (WEBUI_BLOB_MAX - WEBUI_HDR_SIZE));
}

/* -------------------------------------------------------------------------- */

void WebUi_Init(void)
{
    sWebUiHdr hdr;

    memset(&s_ui, 0, sizeof(s_ui));

    if (NvDb_Read(nvdbUser_webUi, &hdr, 0u, sizeof(hdr)) != nvdbRes_ok) {
        return;                         /* no area, or no space in it       */
    }
    if (!headerLooksSane(&hdr)) {
        return;                         /* erased or never written          */
    }

    /* Verified at boot, once, rather than per page load: a browser must never
     * be the thing that discovers a bad flash write, and 5 KB of SPI is a
     * couple of milliseconds. */
    if (!payloadVerifies(&hdr)) {
        TRice("WEBUI: stored page fails CRC, serving fallback\n");
        return;
    }

    s_ui.present       = true;
    s_ui.payload_size  = hdr.payload_size;
    s_ui.payload_crc32 = hdr.payload_crc32;
    TRice("WEBUI: page ok, %u B gzip\n", (unsigned)hdr.payload_size);
}

bool     WebUi_Present(void)     { return s_ui.present; }
uint32_t WebUi_PayloadSize(void) { return s_ui.present ? s_ui.payload_size  : 0u; }
uint32_t WebUi_Crc32(void)       { return s_ui.present ? s_ui.payload_crc32 : 0u; }

bool WebUi_Read(uint32_t offset, void *buff, uint32_t len)
{
    if (!s_ui.present || (len == 0u) ||
        (offset > s_ui.payload_size) ||
        ((s_ui.payload_size - offset) < len)) {
        return false;
    }

    return NvDb_Read(nvdbUser_webUi, buff,
                     WEBUI_HDR_SIZE + offset, len) == nvdbRes_ok;
}

/* -------------------------------------------------------------------------- */

const char *WebUi_UploadBegin(uint32_t total_bytes)
{
    if (s_ui.uploading) {
        return "an upload is already in progress";
    }
    if (total_bytes <= WEBUI_HDR_SIZE) {
        return "body is too short to be a .pnui blob";
    }
    if (total_bytes > WEBUI_BLOB_MAX) {
        return "blob exceeds the stored-page area";
    }

    /* Wipe first, so every write below lands in erased space and needs no
     * read-modify-write.  It also makes the OLD page unreadable immediately,
     * which is the honest state: from here until commit there is no page. */
    if (NvDb_Wipe(nvdbUser_webUi, NULL) != nvdbRes_ok) {
        return "could not clear the stored-page area";
    }

    s_ui.present      = false;
    s_ui.payload_size = 0u;
    s_ui.uploading    = true;
    s_ui.expect_total = total_bytes;
    s_ui.received     = 0u;
    memset(&s_ui.pending, 0, sizeof(s_ui.pending));

    return NULL;
}

bool WebUi_UploadWrite(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    if (!s_ui.uploading || (len == 0u)) {
        return false;
    }
    if ((s_ui.received + len) > s_ui.expect_total) {
        return false;
    }

    /* The first 16 bytes are the header.  They are held in RAM and written
     * last: an interrupted upload must not leave a header describing a
     * payload that is not there. */
    while ((len > 0u) && (s_ui.received < WEBUI_HDR_SIZE)) {
        ((uint8_t *)&s_ui.pending)[s_ui.received] = *p;
        s_ui.received++;
        p++;
        len--;
    }
    if (len == 0u) {
        return true;
    }

    if (NvDb_Write(nvdbUser_webUi, p, s_ui.received, len) != nvdbRes_ok) {
        return false;
    }
    s_ui.received += len;

    return true;
}

const char *WebUi_UploadFinish(void)
{
    if (!s_ui.uploading) {
        return "no upload in progress";
    }
    s_ui.uploading = false;

    if (s_ui.received != s_ui.expect_total) {
        return "body was shorter than Content-Length";
    }
    if (!headerLooksSane(&s_ui.pending)) {
        return "not a .pnui blob (bad magic, version or size)";
    }
    if (s_ui.pending.payload_size != (s_ui.received - WEBUI_HDR_SIZE)) {
        return "header payload_size disagrees with the body length";
    }

    /* Read the payload back off the medium rather than trusting the bytes we
     * were handed: this is the check that catches a bad flash write, which is
     * the failure the operator cannot see and the browser would. */
    if (!payloadVerifies(&s_ui.pending)) {
        return "stored payload does not match its CRC32";
    }

    /* THE COMMIT.  Everything above is reversible by doing nothing. */
    if (NvDb_Write(nvdbUser_webUi, &s_ui.pending, 0u,
                   sizeof(s_ui.pending)) != nvdbRes_ok) {
        return "could not write the header";
    }

    s_ui.present       = true;
    s_ui.payload_size  = s_ui.pending.payload_size;
    s_ui.payload_crc32 = s_ui.pending.payload_crc32;
    TRice("WEBUI: page updated, %u B gzip\n", (unsigned)s_ui.payload_size);

    return NULL;
}

void WebUi_UploadAbort(void)
{
    s_ui.uploading = false;
    s_ui.received  = 0u;
    /* s_ui.present stays false: Begin() already wiped the old page, and
     * claiming one is stored when the area is erased would be a lie. */
}

bool WebUi_Erase(void)
{
    s_ui.present      = false;
    s_ui.payload_size = 0u;
    s_ui.uploading    = false;

    return NvDb_Wipe(nvdbUser_webUi, NULL) == nvdbRes_ok;
}
