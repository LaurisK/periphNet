#ifndef UPDATE_MANAGER_H
#define UPDATE_MANAGER_H

#include "bl_app_contract.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int update_status_read(sUpdateStatus *status);
int update_status_write(const sUpdateStatus *status);
int update_status_clear(void);
int update_status_request(uint32_t image_size, uint32_t image_crc32, uint32_t app_version);
int update_status_is_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* UPDATE_MANAGER_H */
