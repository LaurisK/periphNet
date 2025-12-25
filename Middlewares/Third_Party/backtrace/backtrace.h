/**
 * @file    backtrace.h
 * @brief   Cortex-M4 frame-pointer stack unwinder
 *
 * Requires -fno-omit-frame-pointer compile flag.
 * ARM Thumb-2 with GCC uses R7 as frame pointer. Each frame:
 *   [FP+0] = previous FP
 *   [FP+4] = saved LR (return address to caller)
 */

#ifndef BACKTRACE_H_
#define BACKTRACE_H_

#include <stdint.h>

/** Single backtrace entry */
typedef struct {
    uint32_t    address;  /*!< PC value (Thumb bit cleared) */
    const char *name;     /*!< Always "" – no symbol table on target */
} backtrace_t;

/** Starting frame for the unwinder */
typedef struct {
    uint32_t fp;  /*!< Frame pointer (R7) */
    uint32_t sp;  /*!< Stack pointer    */
    uint32_t lr;  /*!< Link register    */
    uint32_t pc;  /*!< Program counter  */
} backtrace_frame_t;

/**
 * @brief  Unwind the call stack from a given frame.
 *
 * Entries 0 and 1 are the fault PC and LR respectively (from the exception
 * frame). Further entries walk the FP chain if fp points to valid RAM.
 *
 * @param  bt    Output array of backtrace entries
 * @param  max   Maximum entries to fill
 * @param  frame Starting frame (PC, LR, SP, FP)
 * @return Number of valid entries filled
 */
int _backtrace_unwind(backtrace_t *bt, int max, backtrace_frame_t *frame);

#endif /* BACKTRACE_H_ */
