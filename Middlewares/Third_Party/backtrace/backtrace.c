/**
 * @file    backtrace.c
 * @brief   Cortex-M4 frame-pointer stack unwinder
 *
 * Walks the R7 frame-pointer chain (ARM Thumb-2 GCC convention) and
 * collects return addresses.  Requires -fno-omit-frame-pointer.
 *
 * Frame layout (one entry on the stack per function call):
 *   [FP+0] = previous FP  (R7 of the calling function)
 *   [FP+4] = saved LR     (return address into the calling function)
 *
 * The unwinder starts with the explicit PC and LR from the exception frame,
 * then follows the FP chain for deeper frames.
 */

#include "backtrace.h"

/* STM32F407: internal flash 0x08000000–0x08080000 (512 KB) */
#define FLASH_START 0x08000000UL
#define FLASH_END   0x08080000UL

/* STM32F407: SRAM1+SRAM2 0x20000000–0x20020000 (128 KB) */
#define RAM_START   0x20000000UL
#define RAM_END     0x20020000UL

static int is_flash(uint32_t addr)
{
    return (addr >= FLASH_START && addr < FLASH_END);
}

static int is_ram(uint32_t addr)
{
    return (addr >= RAM_START && addr < RAM_END);
}

int _backtrace_unwind(backtrace_t *bt, int max, backtrace_frame_t *frame)
{
    int depth = 0;

    /* Entry 0: the faulting / current PC */
    if (depth < max && is_flash(frame->pc)) {
        bt[depth].address = frame->pc & ~1U;
        bt[depth].name    = "";
        depth++;
    }

    /* Entry 1: LR (return address from the faulting function) */
    uint32_t lr = frame->lr & ~1U;
    if (depth < max && is_flash(lr) && lr != bt[0].address) {
        bt[depth].address = lr;
        bt[depth].name    = "";
        depth++;
    }

    /* Entries 2+: walk R7 frame-pointer chain */
    uint32_t fp = frame->fp;
    for (int guard = 0; depth < max && guard < 16; guard++) {
        if (!is_ram(fp) || (fp & 3U) != 0U) {
            break;
        }

        uint32_t prev_fp  = *((volatile uint32_t *)fp);
        uint32_t ret_addr = *((volatile uint32_t *)(fp + 4U));

        /* Stack grows down: next FP must be above current FP */
        if (!is_ram(prev_fp) || prev_fp <= fp) {
            break;
        }
        if (!is_flash(ret_addr)) {
            break;
        }

        bt[depth].address = ret_addr & ~1U;
        bt[depth].name    = "";
        depth++;

        fp = prev_fp;
    }

    return depth;
}
