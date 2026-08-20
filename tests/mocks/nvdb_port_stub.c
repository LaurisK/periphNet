/* Host-side nvDb port.  There is no RTOS here, so the lock is a counter that
 * lets a test assert nvDb never re-enters it, and the collector is not a task
 * at all — the test calls NvDb_CollectStep() itself, which is what makes the
 * collector's behaviour deterministic to assert against. */

#include "nvdb_port.h"

int nvdb_stub_lockDepth;
int nvdb_stub_lockMax;
int nvdb_stub_notifyCnt;
int nvdb_stub_kickCnt;

void NvDbPort_Lock(void)
{
    nvdb_stub_lockDepth++;
    if (nvdb_stub_lockDepth > nvdb_stub_lockMax) {
        nvdb_stub_lockMax = nvdb_stub_lockDepth;
    }
}

void NvDbPort_Unlock(void)
{
    nvdb_stub_lockDepth--;
}

void NvDbPort_CollectorNotify(void)
{
    nvdb_stub_notifyCnt++;
}

void NvDbPort_Kick(void)
{
    nvdb_stub_kickCnt++;
}
