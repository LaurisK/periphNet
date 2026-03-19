#include "trice.h"
#include "usart.h"
#include <stdint.h>

#define SCB_CFSR   (*((volatile uint32_t *)0xE000ED28))
#define SCB_HFSR   (*((volatile uint32_t *)0xE000ED2C))
#define SCB_DFSR   (*((volatile uint32_t *)0xE000ED30))
#define SCB_AFSR   (*((volatile uint32_t *)0xE000ED3C))
#define SCB_BFAR   (*((volatile uint32_t *)0xE000ED38))
#define SCB_MMFAR  (*((volatile uint32_t *)0xE000ED34))

#define CFSR_IACCVIOL   (1 << 0)
#define CFSR_DACCVIOL   (1 << 1)
#define CFSR_MUNSTKERR  (1 << 3)
#define CFSR_MSTKERR    (1 << 4)
#define CFSR_MLSPERR    (1 << 5)
#define CFSR_MMARVALID  (1 << 7)
#define CFSR_IBUSERR    (1 << 8)
#define CFSR_PRECISERR  (1 << 9)
#define CFSR_IMPRECISERR (1 << 10)
#define CFSR_UNSTKERR   (1 << 11)
#define CFSR_STKERR     (1 << 12)
#define CFSR_LSPERR     (1 << 13)
#define CFSR_BFARVALID  (1 << 15)
#define CFSR_UNDEFINSTR (1 << 16)
#define CFSR_INVSTATE   (1 << 17)
#define CFSR_INVPC      (1 << 18)
#define CFSR_NOCP       (1 << 19)
#define CFSR_UNALIGNED  (1 << 24)
#define CFSR_DIVBYZERO  (1 << 25)

#define HFSR_VECTTBL    (1 << 1)
#define HFSR_FORCED     (1 << 30)
#define HFSR_DEBUGEVT   (1 << 31)

typedef struct {
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;
    uint32_t lr;
    uint32_t pc;
    uint32_t psr;
} exception_stack_frame_t;

/**
 * @brief HardFault handler called from the naked assembly wrapper.
 * @param hardfault_args Pointer to the exception stack frame saved by the CPU.
 * @param lr_value Value of LR at exception entry (EXC_RETURN encoding).
 * @note Outputs full fault diagnostics via TRICE then halts in an infinite loop.
 */
void HardFault_Handler_C(exception_stack_frame_t *hardfault_args, uint32_t lr_value)
{
    /* Abort any in-progress UART DMA so TriceTransfer() can transmit the dump */
    HAL_UART_Abort(&huart3);

    extern volatile uint32_t g_upload_progress;

    uint32_t cfsr = SCB_CFSR;
    uint32_t hfsr = SCB_HFSR;
    uint32_t dfsr = SCB_DFSR;
    uint32_t afsr = SCB_AFSR;
    uint32_t bfar = SCB_BFAR;
    uint32_t mmfar = SCB_MMFAR;

    TRice("\n\n");
    TRice("========================================\n");
    TRice("     HARDFAULT EXCEPTION OCCURRED      \n");
    TRice("========================================\n");

    TRice("upload_progress=0x%X\n", (unsigned)g_upload_progress);
    TRice("\n--- Stack Frame (auto-saved by CPU) ---\n");
    TRice("R0  = 0x%08X\n", hardfault_args->r0);
    TRice("R1  = 0x%08X\n", hardfault_args->r1);
    TRice("R2  = 0x%08X\n", hardfault_args->r2);
    TRice("R3  = 0x%08X\n", hardfault_args->r3);
    TRice("R12 = 0x%08X\n", hardfault_args->r12);
    TRice("LR  = 0x%08X (return address)\n", hardfault_args->lr);
    TRice("PC  = 0x%08X (fault address)\n", hardfault_args->pc);
    TRice("PSR = 0x%08X\n", hardfault_args->psr);

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

    TRice("\n--- Fault Status Registers ---\n");
    TRice("CFSR = 0x%08X\n", cfsr);
    TRice("HFSR = 0x%08X\n", hfsr);
    TRice("DFSR = 0x%08X\n", dfsr);
    TRice("AFSR = 0x%08X\n", afsr);

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

    if (cfsr & 0xFFFF0000) {
        TRice("\n--- Usage Fault ---\n");
        if (cfsr & CFSR_UNDEFINSTR) TRice("  - Undefined instruction\n");
        if (cfsr & CFSR_INVSTATE)   TRice("  - Invalid state (e.g., trying to switch to ARM mode)\n");
        if (cfsr & CFSR_INVPC)      TRice("  - Invalid PC load\n");
        if (cfsr & CFSR_NOCP)       TRice("  - No coprocessor\n");
        if (cfsr & CFSR_UNALIGNED)  TRice("  - Unaligned access\n");
        if (cfsr & CFSR_DIVBYZERO)  TRice("  - Divide by zero\n");
    }

    if (hfsr) {
        TRice("\n--- HardFault Status ---\n");
        if (hfsr & HFSR_VECTTBL)  TRice("  - Vector table read fault\n");
        if (hfsr & HFSR_FORCED)   TRice("  - Forced HardFault (escalated from other fault)\n");
        if (hfsr & HFSR_DEBUGEVT) TRice("  - Debug event\n");
    }

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
    TriceTransfer();
    while (1) {
        __asm volatile ("nop");
    }
}

