/**
 * @file    nvdb_exceptions.h
 * @brief   Absolute addresses — crash handler and FWU module ONLY.
 *
 * Two clients cannot call nvDb to perform their I/O:
 *
 *   - The crash handler runs in fault context, where the RTOS may be dead and
 *     an SPI transaction may have been in flight.  It resets the peripheral
 *     by hand and writes directly.
 *   - The bootloader cannot link nvDb at all — 32 KB, no RTOS — and runs
 *     before the application exists.  It does NOT call this either: the FWU
 *     module resolves through nvDb and publishes what the BL needs, in a
 *     structure the BL already understands.  The BL has no knowledge of nvDb,
 *     which is exactly what keeps the directory format free to evolve.
 *
 * This declaration is deliberately NOT in nvdb.h, so "exception modules only"
 * is enforced by what a file includes rather than by a comment.
 *
 * Everything a client does with an address after this returns is that
 * client's own implementation.  nvDb has no further part in it, and in
 * particular does not know that those bytes were touched.
 */
#ifndef NVDB_EXCEPTIONS_H_
#define NVDB_EXCEPTIONS_H_

#include "nvdb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Does not block.  Callable from ANY context, fault handlers included: it is
 * a RAM lookup once NvDb_Init() has run, with no RTOS call, no mutex and no
 * allocation.  Before Init it returns nvdbRes_notInit.
 *
 * Only users flagged for it in the build table are resolvable; anything else
 * gets nvdbRes_badUser, so the exception is a property of the layout rather
 * than of the caller's discipline. */
eNvDbRes NvDb_GetAbsoluteAddress(eNvDbUser user, uint32_t *addr_bytes,
                                                 uint32_t *size_bytes);

#ifdef __cplusplus
}
#endif

#endif /* NVDB_EXCEPTIONS_H_ */
