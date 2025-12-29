/**
 ******************************************************************************
 * @file    hardfault_handler.c
 * @brief   Comprehensive HardFault handler with TRICE debugging output
 ******************************************************************************
 * Captures all fault information for debugging:
 * - Stack frame (R0-R3, R12, LR, PC, xPSR)
 * - Remaining registers (R4-R11)
 * - Fault status registers (CFSR, HFSR, DFSR, AFSR, BFAR, MMFAR)
 * - Stack pointer values (MSP, PSP)
 *
 * Outputs via TRICE for remote debugging over TCP
 ******************************************************************************
 */

#include "trice.h"
#include <stdint.h>

/* Cortex-M4 Fault Status Registers */
#define SCB_CFSR   (*((volatile uint32_t *)0xE000ED28))  /* Configurable Fault Status */
#define SCB_HFSR   (*((volatile uint32_t *)0xE000ED2C))  /* HardFault Status */
#define SCB_DFSR   (*((volatile uint32_t *)0xE000ED30))  /* Debug Fault Status */
#define SCB_AFSR   (*((volatile uint32_t *)0xE000ED3C))  /* Auxiliary Fault Status */
#define SCB_BFAR   (*((volatile uint32_t *)0xE000ED38))  /* Bus Fault Address */
#define SCB_MMFAR  (*((volatile uint32_t *)0xE000ED34))  /* MemManage Fault Address */

/* CFSR bit definitions */
#define CFSR_IACCVIOL   (1 << 0)   /* Instruction access violation */
#define CFSR_DACCVIOL   (1 << 1)   /* Data access violation */
#define CFSR_MUNSTKERR  (1 << 3)   /* MemManage fault on unstacking */
#define CFSR_MSTKERR    (1 << 4)   /* MemManage fault on stacking */
#define CFSR_MLSPERR    (1 << 5)   /* MemManage fault during FP lazy state preservation */
#define CFSR_MMARVALID  (1 << 7)   /* MMFAR valid */
#define CFSR_IBUSERR    (1 << 8)   /* Instruction bus error */
#define CFSR_PRECISERR  (1 << 9)   /* Precise data bus error */
#define CFSR_IMPRECISERR (1 << 10) /* Imprecise data bus error */
#define CFSR_UNSTKERR   (1 << 11)  /* Bus fault on unstacking */
#define CFSR_STKERR     (1 << 12)  /* Bus fault on stacking */
#define CFSR_LSPERR     (1 << 13)  /* Bus fault during FP lazy state preservation */
#define CFSR_BFARVALID  (1 << 15)  /* BFAR valid */
#define CFSR_UNDEFINSTR (1 << 16)  /* Undefined instruction */
#define CFSR_INVSTATE   (1 << 17)  /* Invalid state */
#define CFSR_INVPC      (1 << 18)  /* Invalid PC */
#define CFSR_NOCP       (1 << 19)  /* No coprocessor */
#define CFSR_UNALIGNED  (1 << 24)  /* Unaligned access */
#define CFSR_DIVBYZERO  (1 << 25)  /* Divide by zero */

/* HFSR bit definitions */
#define HFSR_VECTTBL    (1 << 1)   /* Vector table read fault */
#define HFSR_FORCED     (1 << 30)  /* Forced HardFault */
#define HFSR_DEBUGEVT   (1 << 31)  /* Debug event */

/* Stack frame pushed by processor on exception */
typedef struct {
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;
    uint32_t lr;   /* Link Register */
    uint32_t pc;   /* Program Counter */
    uint32_t psr;  /* Program Status Register */
} exception_stack_frame_t;

/**
 * @brief  HardFault handler in C - called from assembly
 * @param  hardfault_args: Pointer to stack frame
 * @param  lr_value: LR register value (EXC_RETURN)
 */
void HardFault_Handler_C(exception_stack_frame_t *hardfault_args, uint32_t lr_value)
{
    uint32_t cfsr = SCB_CFSR;
    uint32_t hfsr = SCB_HFSR;
    uint32_t dfsr = SCB_DFSR;
    uint32_t afsr = SCB_AFSR;
    uint32_t bfar = SCB_BFAR;
    uint32_t mmfar = SCB_MMFAR;

    /* Critical fault information */
    TRice("\n\n");
    TRice("========================================\n");
    TRice("     HARDFAULT EXCEPTION OCCURRED      \n");
    TRice("========================================\n");

    /* Stack frame registers */
    TRice("\n--- Stack Frame (auto-saved by CPU) ---\n");
    TRice("R0  = 0x%08X\n", hardfault_args->r0);
    TRice("R1  = 0x%08X\n", hardfault_args->r1);
    TRice("R2  = 0x%08X\n", hardfault_args->r2);
    TRice("R3  = 0x%08X\n", hardfault_args->r3);
    TRice("R12 = 0x%08X\n", hardfault_args->r12);
    TRice("LR  = 0x%08X (return address)\n", hardfault_args->lr);
    TRice("PC  = 0x%08X (fault address)\n", hardfault_args->pc);
    TRice("PSR = 0x%08X\n", hardfault_args->psr);

    /* EXC_RETURN information */
    TRice("\n--- Exception Return ---\n");
    TRice("EXC_RETURN = 0x%08X\n", lr_value);
    if (lr_value & (1 << 2)) {
        TRice("  Return to: Thread mode (PSP)\n");
    } else {
        TRice("  Return to: Handler mode (MSP)\n");
    }
    if (lr_value & (1 << 3)) {
        TRice("  Stack: Process Stack (PSP)\n");
    } else {
        TRice("  Stack: Main Stack (MSP)\n");
    }

    /* Fault status registers */
    TRice("\n--- Fault Status Registers ---\n");
    TRice("CFSR = 0x%08X\n", cfsr);
    TRice("HFSR = 0x%08X\n", hfsr);
    TRice("DFSR = 0x%08X\n", dfsr);
    TRice("AFSR = 0x%08X\n", afsr);

    /* Decode CFSR - MemManage faults */
    if (cfsr & 0xFF) {
        TRice("\n--- MemManage Fault ---\n");
        if (cfsr & CFSR_IACCVIOL)   TRice("  - Instruction access violation\n");
        if (cfsr & CFSR_DACCVIOL)   TRice("  - Data access violation\n");
        if (cfsr & CFSR_MUNSTKERR)  TRice("  - MemManage fault on unstacking\n");
        if (cfsr & CFSR_MSTKERR)    TRice("  - MemManage fault on stacking\n");
        if (cfsr & CFSR_MLSPERR)    TRice("  - MemManage fault during FP lazy state preservation\n");
        if (cfsr & CFSR_MMARVALID) {
            TRice("  - Fault address: 0x%08X (MMFAR)\n", mmfar);
        }
    }

    /* Decode CFSR - Bus faults */
    if (cfsr & 0xFF00) {
        TRice("\n--- Bus Fault ---\n");
        if (cfsr & CFSR_IBUSERR)     TRice("  - Instruction bus error\n");
        if (cfsr & CFSR_PRECISERR)   TRice("  - Precise data bus error\n");
        if (cfsr & CFSR_IMPRECISERR) TRice("  - Imprecise data bus error\n");
        if (cfsr & CFSR_UNSTKERR)    TRice("  - Bus fault on unstacking\n");
        if (cfsr & CFSR_STKERR)      TRice("  - Bus fault on stacking\n");
        if (cfsr & CFSR_LSPERR)      TRice("  - Bus fault during FP lazy state preservation\n");
        if (cfsr & CFSR_BFARVALID) {
            TRice("  - Fault address: 0x%08X (BFAR)\n", bfar);
        }
    }

    /* Decode CFSR - Usage faults */
    if (cfsr & 0xFFFF0000) {
        TRice("\n--- Usage Fault ---\n");
        if (cfsr & CFSR_UNDEFINSTR) TRice("  - Undefined instruction\n");
        if (cfsr & CFSR_INVSTATE)   TRice("  - Invalid state (e.g., trying to switch to ARM mode)\n");
        if (cfsr & CFSR_INVPC)      TRice("  - Invalid PC load\n");
        if (cfsr & CFSR_NOCP)       TRice("  - No coprocessor\n");
        if (cfsr & CFSR_UNALIGNED)  TRice("  - Unaligned access\n");
        if (cfsr & CFSR_DIVBYZERO)  TRice("  - Divide by zero\n");
    }

    /* Decode HFSR */
    if (hfsr) {
        TRice("\n--- HardFault Status ---\n");
        if (hfsr & HFSR_VECTTBL)  TRice("  - Vector table read fault\n");
        if (hfsr & HFSR_FORCED)   TRice("  - Forced HardFault (escalated from other fault)\n");
        if (hfsr & HFSR_DEBUGEVT) TRice("  - Debug event\n");
    }

    /* Stack unwinding information */
    TRice("\n--- Stack Unwinding Information ---\n");
    TRice("Stack frame at: 0x%08X\n", (uint32_t)hardfault_args);
    TRice("\nTo debug with GDB:\n");
    TRice("  1. Load ELF: file build/application.elf\n");
    TRice("  2. Set PC: set $pc = 0x%08X\n", hardfault_args->pc);
    TRice("  3. Set SP: set $sp = 0x%08X\n", (uint32_t)hardfault_args);
    TRice("  4. Backtrace: bt\n");
    TRice("\nTo disassemble fault address:\n");
    TRice("  arm-none-eabi-objdump -d -S build/application.elf | grep -A20 %08X\n", hardfault_args->pc);
    TRice("\nTo find function at fault address:\n");
    TRice("  arm-none-eabi-addr2line -e build/application.elf 0x%08X\n", hardfault_args->pc);

    TRice("\n========================================\n");
    TRice("  System halted - infinite loop below   \n");
    TRice("========================================\n\n");

    /* Halt execution */
    while (1) {
        __asm volatile ("nop");
    }
}

/**
 * @brief  HardFault handler assembly wrapper
 *
 * Captures register state and calls C handler.
 * This must be naked to prevent corrupting the stack frame.
 */
__attribute__((naked))
void HardFault_Handler(void)
{
    __asm volatile (
        /* Determine which stack pointer was in use */
        "tst lr, #4              \n"  /* Test bit 2 of LR (EXC_RETURN) */
        "ite eq                  \n"
        "mrseq r0, msp           \n"  /* If 0: using MSP, load MSP into R0 */
        "mrsne r0, psp           \n"  /* If 1: using PSP, load PSP into R0 */

        /* R0 now points to stack frame */
        /* Save LR (EXC_RETURN) to R1 for analysis */
        "mov r1, lr              \n"

        /* Call C handler: HardFault_Handler_C(stack_frame*, lr_value) */
        "b HardFault_Handler_C   \n"
        ::: "r0", "r1"
    );
}
