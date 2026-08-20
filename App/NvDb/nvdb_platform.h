/**
 * @file    nvdb_platform.h
 * @brief   The nvDb port: the RTOS half of the store.
 *
 * The nvDb core is RTOS-free (Shared/NvDb) so the host tests can drive it
 * with no board.  Everything it needs from FreeRTOS arrives through
 * nvdb_port.h, and this file is the only implementation of that on the
 * device — the same shape App/Net/wg_platform.c has for WireGuard.
 *
 * It owns two things: the priority-inheriting mutex that serializes every
 * nvDb operation, and the lowest-priority task that erases deleted space in
 * the background so that erases stay off the write path.
 */
#ifndef NVDB_PLATFORM_H_
#define NVDB_PLATFORM_H_

#include "nvdb.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create the lock and the collector task, then bring nvDb up.
 *
 * Call from defaultTask AFTER W25Q128_Init(): NvDb_Init() reads the medium.
 * A user that initialises earlier sees nvdbRes_notInit — users initialise
 * after nvDb, not around it.
 */
eNvDbRes NvDbPlatform_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* NVDB_PLATFORM_H_ */
