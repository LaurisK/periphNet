/**
 * @file    web_ui.h
 * @brief   The web UI as stored data, not as image content.
 *
 * The page used to be a 15.6 KB string literal in http_server.c.  It had grown
 * 4.1 KB -> 15.6 KB since March and more than doubled in the five weeks to
 * 2026-09-07, inside an image that is 99.65 % full, so every new card was
 * spending flash that the CAN frame source and the persisted bus roles need.
 * On the medium it costs none.
 *
 * The stored object is the `.pnui` blob `dfu_image_tool.py ui` produces, kept
 * VERBATIM: a 16-byte header followed by the gzip stream.  The board never
 * decompresses it -- it streams the payload out under `Content-Encoding: gzip`,
 * so the bytes on the medium are the bytes the browser receives, and the one
 * CRC32 in the header covers upload, flash write and every read back out.
 *
 * The image keeps a small fallback page, so `GET /` always answers.  A board
 * with this area empty is fully operable -- every /api/ route lives in the
 * image -- it just cannot show you the dashboard until the page is uploaded.
 */
#ifndef APP_HTTP_WEB_UI_H_
#define APP_HTTP_WEB_UI_H_

#include <stdbool.h>
#include <stdint.h>

/** Largest blob the area accepts.  The nvDb user is 64 KB; this leaves the
 *  header room and refuses an upload that could not fit before writing any of
 *  it, rather than discovering it at the last chunk. */
#define WEBUI_BLOB_MAX      (0xF000u)

/** Read the stored header and verify the payload against its CRC32.
 *  Call once, after NvDbPlatform_Init().  A failure here is not an error
 *  condition: it means "no page stored", and the fallback answers instead. */
void     WebUi_Init(void);

/** True when a verified page is stored and WebUi_Read() will serve it. */
bool     WebUi_Present(void);

/** Compressed payload length, i.e. the Content-Length to send. 0 if absent. */
uint32_t WebUi_PayloadSize(void);

/** CRC32 of the stored payload, for the status endpoint. 0 if absent. */
uint32_t WebUi_Crc32(void);

/** Copy `len` payload bytes from `offset` into `buff`.
 *  @retval true on success; false if absent or out of range. */
bool     WebUi_Read(uint32_t offset, void *buff, uint32_t len);

/* ---- streaming upload -------------------------------------------------- *
 * The header is written LAST, after the payload is back-read and its CRC32
 * checked, so an upload that dies partway leaves the area without a valid
 * header and the board keeps answering with the fallback.  There is no
 * half-installed state to detect and none to recover from. */

/** Reserve the area for an upload of exactly `total_bytes` (header included).
 *  @retval NULL on success, else a short reason for the 409 body. */
const char *WebUi_UploadBegin(uint32_t total_bytes);

/** Feed the next `len` bytes in order. @retval false on a medium failure. */
bool        WebUi_UploadWrite(const void *data, uint32_t len);

/** Verify what landed and commit it. @retval NULL on success, else a reason. */
const char *WebUi_UploadFinish(void);

/** Abandon an upload in progress; the stored page stays whatever it was. */
void        WebUi_UploadAbort(void);

/** Erase the stored page; the board falls back to the built-in one. */
bool        WebUi_Erase(void);

#endif /* APP_HTTP_WEB_UI_H_ */
