#ifndef NVDB_TEST_UTIL_H
#define NVDB_TEST_UTIL_H

#include "nvdb.h"
#include "nvdb_exceptions.h"
#include "nvdb_internal.h"
#include "nvdb_port.h"
#include "w25q128_mock.h"

#include <string.h>

/* Where the layout this image ships puts everything on a board that had
 * nothing on it.  Spelled out rather than derived, so a change to the packer
 * that quietly moves a user has to be acknowledged here. */
#define NVDBT_BOOTSTATUS_ADDR   0x000000u
#define NVDBT_FWUSTORED_ADDR    0x001000u
#define NVDBT_FWUGOLDEN_ADDR    0x07B000u
#define NVDBT_IMAGEMETA_ADDR    0x0F5000u
#define NVDBT_CRASHLOG_ADDR     0x0F8000u
#define NVDBT_LUTA_ADDR         0x0F9000u
#define NVDBT_LUTB_ADDR         0x0FD000u
#define NVDBT_SEL_ADDR          0x101000u
#define NVDBT_WGTIME_ADDR       0x102000u
#define NVDBT_WGCFG_ADDR        0x103000u
/* The two users that are new in this layout go above nvDb's own pinned
 * block, which is the first free space there is. */
#define NVDBT_MQTT_ADDR         0x109000u
#define NVDBT_TRICE_ADDR        0x10A000u

/* A board out of the factory: erased medium, counters zeroed, nvDb brought
 * up from nothing. */
static inline eNvDbRes nvdbt_freshBoot(void)
{
    mock_flash_reset();
    return NvDb_Init();
}

/* A reset: the medium keeps whatever is on it, nvDb comes up again.  This is
 * also how a test observes that marks do not survive a reboot. */
static inline eNvDbRes nvdbt_reboot(void)
{
    mock_flash_eraseCnt = 0;
    mock_flash_writeCnt = 0;
    return NvDb_Init();
}

/* Run the collector to a standstill, the way a lowest-priority task would. */
static inline uint32_t nvdbt_collectAll(void)
{
    uint32_t steps = 0;
    while (NvDb_CollectStep()) {
        steps++;
        if (steps > 8192u) {
            break;              /* a runaway collector is a test failure     */
        }
    }
    return steps;
}

/* Keep visiting the collector while it has nothing to do, which is what the
 * platform task does once a second.  Deferred housekeeping — the wear
 * write-back — happens on one of these, not on the first. */
static inline void nvdbt_idle(uint32_t visits)
{
    uint32_t i;
    for (i = 0; i < visits; i++) {
        (void)NvDb_CollectStep();
    }
}

static inline int nvdbt_allBytes(uint32_t addr, uint32_t len, uint8_t val)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        if (mock_flash[addr + i] != val) {
            return 0;
        }
    }
    return 1;
}

#endif /* NVDB_TEST_UTIL_H */
