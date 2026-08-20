#ifndef W25Q128_MOCK_H
#define W25Q128_MOCK_H

#include <stdint.h>

/* RAM-backed NOR flash simulation (see w25q128_mock.c).
 * The full 8 MB of the W25Q64 on the board: nvDb owns the whole address
 * space, so its relayout and blob-area tests need every byte of it.  RAM is
 * free on the host. */
#define MOCK_FLASH_SIZE  0x800000u

extern uint8_t mock_flash[MOCK_FLASH_SIZE];

/* Erase accounting.  nvDb's central promise on the write path is that a write
 * into already-erased space performs NO erase (task_nv_db.md C12), and the
 * only way to assert that is to count. */
extern uint32_t mock_flash_eraseCnt;
extern uint32_t mock_flash_writeCnt;

/* Power-cut simulation.  Set to N and the Nth erase or program from now on
 * does HALF its work and then reports failure, and every operation after it
 * fails outright — which is what a board losing its supply mid-sector looks
 * like to the code that comes up next.  0 disables it. */
extern uint32_t mock_flash_failAfter;

/* Transient-fault simulation, which is a different animal from a power cut:
 * after `glitchAfter` operations the next `glitchOps` erases/programs fail
 * CLEANLY — no work done, no latch — and everything after them succeeds. A
 * medium that times out once and then behaves is the case a power cut cannot
 * model, and it is the one where "give up and record it" and "retry later"
 * come apart. */
extern uint32_t mock_flash_glitchAfter;
extern uint32_t mock_flash_glitchOps;

/* Reset the simulated flash to fully erased (0xFF) and zero the counters. */
void mock_flash_reset(void);

/* Power comes back: the medium keeps whatever the cut left on it. */
void mock_flash_reset_power(void);

#endif /* W25Q128_MOCK_H */
