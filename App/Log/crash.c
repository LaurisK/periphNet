/**
 * @file    crash.c
 * @brief   Crash diagnostics and fault logging – STM32F407 / Cortex-M4F
 *
 * Adapted from the Zhaga/Lusety crash handler pattern for Cortex-M4F:
 *  – Fault registers include CFSR / HFSR / MMFAR / BFAR
 *  – FreeRTOS task context layout for CM4F (R4-R11, LR, then exception frame)
 *  – FPU-awareness: checks EXC_RETURN bit 4 to adjust saved-register offsets
 *  – DMA flush uses DMA1_Stream3 NDTR polling (USART3 TX stream on STM32F4)
 *  – Crash log persisted to external SPI flash for later HTTP retrieval
 */

#include "App/Log/crash.h"
#include "App/Log/trice_consumer.h"
#include "image_mgmt.h"
#include "nvdb.h"
#include "nvdb_exceptions.h"
#include "w25q_fault.h"
#include "trice.h"
#include "usart.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx_hal.h"
#include "Middlewares/Third_Party/backtrace/backtrace.h"
#include <string.h>
#include <stddef.h>

/* --------------------------------------------------------------------------
 * Private types
 * -------------------------------------------------------------------------- */

/** CPU register snapshot captured from the PSP exception frame */
typedef struct {
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;
    uint32_t lr;
    uint32_t pc;
    uint32_t psr;
    uint32_t sp;      /*!< stack pointer at fault time                  */
    uint32_t control;
    uint32_t primask;
    uint32_t fp;      /*!< R7 of the faulting context = frame pointer    */
    uint8_t  frameOnMsp;
} sCrashRegs;

/* --------------------------------------------------------------------------
 * Private data
 * -------------------------------------------------------------------------- */

static const char * const s_crashTypeStr[] = {
    "HARDFAULT",
    "NON MASKABLE IRQ",
    "BUS FAULT",
    "USAGE FAULT",
    "MEM MANAGE",
    "SW WATCHDOG TRIGGERED",
    "STACK OVERFLOW",
    "ASSERT FAILED"
};

static const char * const s_taskStateStr[] = {
    "Run", "Rdy", "Blk", "Sus", "Del"
};

/* --------------------------------------------------------------------------
 * Private functions
 * -------------------------------------------------------------------------- */

/**
 * @brief Poll until DMA1_Stream3 (USART3 TX) finishes and reset UART state.
 *
 * In exception / interrupt context the DMA-complete IRQ cannot fire, so the
 * HAL UART state machine never transitions back to READY.  We poll the NDTR
 * register directly and then force the gState to READY so the next
 * TriceTransfer call can initiate a new transfer.
 */
static void flushTrice(void)
{
    extern UART_HandleTypeDef huart3;

    /* Wait for any in-progress DMA transfer to finish */
    uint32_t timeout = 200000UL;
    while ((DMA1_Stream3->NDTR != 0U) && (timeout-- > 0U)) {}

    /* Force UART state to READY so HAL_UART_Transmit_DMA accepts a new buf */
    if (huart3.gState != HAL_UART_STATE_READY) {
        huart3.gState = HAL_UART_STATE_READY;
    }

    TriceConsumer_ResetAll();

    TriceTransfer();

    /* Wait for the newly started transfer */
    timeout = 200000UL;
    while ((DMA1_Stream3->NDTR != 0U) && (timeout-- > 0U)) {}
}

/**
 * @brief Print PC/LR/FP backtrace entries.
 */
static void printBacktrace(uint32_t pc, uint32_t lr, uint32_t sp, uint32_t fp)
{
    backtrace_frame_t frame;
    backtrace_t       bt[8];

    frame.pc = pc;
    frame.lr = lr;
    frame.sp = sp;
    frame.fp = fp;

    int depth = _backtrace_unwind(bt, 8, &frame);

    trice(">>> Backtrace:\n");
    for (int i = 0; i < depth; i++) {
        trice("\t  #%d 0x%08X\n", i, (uint32_t)bt[i].address);
    }
}

/**
 * @brief Iterate FreeRTOS tasks and print state + per-task backtrace.
 *
 * Accesses pxTopOfStack (offset 0 in TCB) to reconstruct the Cortex-M4F
 * saved context.  Handles optional FPU context (EXC_RETURN bit 4).
 *
 * CM4F portSAVE_CONTEXT layout (FPU NOT used, top[8] bit4 == 1):
 *   top[0..7]  = R4-R11 (software save)
 *   top[8]     = LR (EXC_RETURN)
 *   top[9]     = R0  ─┐
 *   top[10]    = R1   │  hardware exception frame
 *   top[11]    = R2   │
 *   top[12]    = R3   │
 *   top[13]    = R12  │
 *   top[14]    = LR   │ (task's LR at context switch)
 *   top[15]    = PC   │
 *   top[16]    = xPSR─┘
 *
 * If FPU WAS used (top[8] bit4 == 0) there are 16 extra s16-s31 words
 * BEFORE the R4-R11 block → add fpu_offset = 16 to all indices above.
 */
static void printTaskList(void)
{
    static TaskStatus_t tasks[16];
    UBaseType_t n = uxTaskGetSystemState(tasks, 16, NULL);

    trice("========== Tasks (%u) ==============\n", (uint32_t)n);

    for (UBaseType_t i = 0; i < n; i++) {
        const char *st = (tasks[i].eCurrentState < eInvalid)
            ? s_taskStateStr[tasks[i].eCurrentState] : "???";

        triceS("\t%s", tasks[i].pcTaskName);
        trice(" [%c%c%c] freeStack=%u words\n",
              st[0], st[1], st[2],
              (uint32_t)tasks[i].usStackHighWaterMark);

        /* pxTopOfStack is the FIRST member of the TCB (guaranteed by FreeRTOS) */
        volatile StackType_t *top =
            *((volatile StackType_t *volatile *)tasks[i].xHandle);

        /* EXC_RETURN at top[8]; bit 4 == 0 means FPU context was saved */
        uint32_t exc_return = (uint32_t)top[8];
        uint32_t fpu_offset = ((exc_return & 0x10U) == 0U) ? 16U : 0U;

        uint32_t r7_fp   = (uint32_t)top[3U  + fpu_offset]; /* R7 = frame ptr  */
        uint32_t task_lr = (uint32_t)top[14U + fpu_offset]; /* saved LR        */
        uint32_t task_pc = (uint32_t)top[15U + fpu_offset]; /* saved PC        */
        uint32_t task_sp = (uint32_t)(top + 17U + fpu_offset); /* SP after frame */

        printBacktrace(task_pc, task_lr, task_sp, r7_fp);
        flushTrice();
    }

    trice("--- End ---\n");
}

/**
 * @brief Capture registers from the PSP exception frame.
 *
 * Valid when the fault originated in Thread mode (FreeRTOS task), which is
 * the expected case for all button-triggered and watchdog faults.
 */
/* Memory map of this part, used to tell a plausible code/stack address from
 * noise.  Flash spans bootloader + application; task stacks live in the CCM
 * heap, which is why CCM must be accepted as "stack RAM" here — on the Zhaga
 * M0+ original only main SRAM existed and the check was one range. */
#define CRASH_FLASH_BEGIN   0x08000000UL
#define CRASH_FLASH_END     0x08080000UL
#define CRASH_SRAM_BEGIN    0x20000000UL
#define CRASH_SRAM_END      0x20020000UL
#define CRASH_CCM_BEGIN     0x10000000UL
#define CRASH_CCM_END       0x10010000UL
#define SCAN_WORDS    128U

/* Latched by Crash_CaptureEntry() on exception entry.  volatile: written from
 * assembly, read by C, and nothing must hoist the reads. */
typedef struct {
    uint32_t r4_r11[8];
    uint32_t msp;
    uint32_t psp;
    uint32_t caller_r7;   /*!< faulting context's R7 = its frame pointer */
    uint32_t valid;       /*!< CRASH_ENTRY_MAGIC when the shim ran        */
} sCrashEntryRegs;

/* Only the five fault handlers run the shim.  The software-watchdog, assert
 * and stack-overflow paths call Crash_GenerateReport() directly from ordinary
 * C, where there is no exception frame to latch — so the struct must announce
 * whether it holds anything, or those three reports would decode a stale or
 * zeroed snapshot as if it were real. */
#define CRASH_ENTRY_MAGIC   0xC5A1BEEFUL

volatile sCrashEntryRegs g_crashEntry __attribute__((used));

__attribute__((naked, used)) void Crash_CaptureEntry(void)
{
    /* R7 is the one register that is already GONE by the time this runs: the
     * calling handler's prologue does `push {r7, lr}` then `add r7, sp, #0`,
     * so r7 now holds the handler's own frame pointer.  The faulting context's
     * R7 is the word the prologue pushed, i.e. *MSP — and it is worth
     * recovering because R7 is a real frame pointer here: the build sets
     * -fno-omit-frame-pointer (CMakeLists.txt), which both guarantees that
     * prologue shape and makes the FP chain walkable in the first place.
     * captureRegs() range-checks the result before trusting it.
     *
     * Clobbers r0-r2 only, which AAPCS already lets a callee destroy. */
    __asm volatile (
        "ldr   r0, =g_crashEntry   \n"
        "stm   r0, {r4-r11}        \n"   /* 32-bit STM: Cortex-M4 only       */
        "mrs   r1, msp             \n"
        "str   r1, [r0, #32]       \n"
        "mrs   r2, psp             \n"
        "str   r2, [r0, #36]       \n"
        "ldr   r2, [r1]            \n"   /* *MSP = R7 pushed by the prologue */
        "str   r2, [r0, #40]       \n"
        "ldr   r2, =0xC5A1BEEF     \n"
        "str   r2, [r0, #44]       \n"
        "bx    lr                  \n"
    );
}

static int addr_in_flash(uint32_t a)
{
    return (a >= CRASH_FLASH_BEGIN) && (a < CRASH_FLASH_END);
}

static int sp_in_ram(uint32_t a)
{
    if ((a & 3U) != 0U) {
        return 0;
    }
    return ((a >= CRASH_SRAM_BEGIN) && (a < CRASH_SRAM_END)) ||
           ((a >= CRASH_CCM_BEGIN)  && (a < CRASH_CCM_END));
}

/* Does this look like a live exception frame?  A stacked PC in flash and a
 * stacked xPSR with a sane Thumb bit is enough to pick PSP from MSP without
 * EXC_RETURN, which a called function cannot see. */
static int frame_looks_valid(uint32_t sp)
{
    const uint32_t *f = (const uint32_t *)sp;

    if (!sp_in_ram(sp)) {
        return 0;
    }
    return addr_in_flash(f[6] & ~1UL) && ((f[7] & (1UL << 24)) != 0U);
}

/* Cortex-M0+ cannot unwind, so the Zhaga handler scans each stack for words
 * that LOOK like return addresses and lets the host symbolise them.  That idea
 * is worth borrowing here for a reason unrelated to the core: a frame-pointer
 * walk trusts the frame chain, and the fault this exists to diagnose is memory
 * corruption — exactly the case where the chain cannot be trusted.  A scan
 * depends on nothing but the stack bytes.
 *
 * False positives are expected and are the caller's problem to filter; a
 * missing frame is worse than a spurious one here. */
static uint8_t scan_stack(const char *name, uint32_t sp, uint32_t skipWords,
                          uint32_t *out, uint8_t outMax)
{
    const uint32_t *p;
    const uint32_t *end;
    uint8_t         n = 0U;

    if (!sp_in_ram(sp)) {
        return 0U;
    }
    p   = (const uint32_t *)sp + skipWords;
    end = p + SCAN_WORDS;

    if (name != NULL) {
        TRiceS("err:[%s]\n", (char *)name);
    }
    for (; p < end; p++) {
        uint32_t v;

        if (!sp_in_ram((uint32_t)p)) {
            break;                      /* ran off the end of RAM */
        }
        v = *p;
        if (((v & 1U) != 0U) && addr_in_flash(v & ~1UL)) {
            if (name != NULL) {
                trice("err:  LR 0x%08X\n", v);
            }
            if ((out != NULL) && (n < outMax)) {
                out[n] = v;
            }
            n = (uint8_t)((n < 255U) ? (n + 1U) : n);
        }
    }
    return (n > outMax) ? outMax : n;
}

/* Every stack in the system, the faulting one first.  This is the part that
 * survives corruption of the FreeRTOS lists only partially: uxTaskGetSystemState
 * walks those same lists, so if it returns nothing the scan below is limited to
 * MSP/PSP — which is itself a signal worth seeing. */
static void scanAllStacks(int frameOnMsp)
{
    static TaskStatus_t tasks[CRASH_LOG_MAX_TASKS];  /* static: exception ctx */
    UBaseType_t         n;
    TaskHandle_t        current;

    trice("err:=== LR scan (candidates, filter host-side) ===\n");

    if (frameOnMsp) {
        (void)scan_stack("faulting (MSP)", g_crashEntry.msp, 8U, NULL, 0U);
        (void)scan_stack("thread (PSP)",   g_crashEntry.psp, 0U, NULL, 0U);
    } else {
        (void)scan_stack("faulting task (PSP)", g_crashEntry.psp, 8U, NULL, 0U);
        (void)scan_stack("IRQ (MSP)",           g_crashEntry.msp, 0U, NULL, 0U);
    }
    flushTrice();

    n       = uxTaskGetSystemState(tasks, CRASH_LOG_MAX_TASKS, NULL);
    current = xTaskGetCurrentTaskHandle();

    for (UBaseType_t i = 0; i < n; i++) {
        uint32_t task_sp;

        if (tasks[i].xHandle == current) {
            continue;                   /* covered by the PSP scan above */
        }
        /* pxTopOfStack is the first member of the TCB. */
        task_sp = (uint32_t)(*(uint32_t *volatile *)tasks[i].xHandle);
        (void)scan_stack(tasks[i].pcTaskName, task_sp, 0U, NULL, 0U);
        flushTrice();
    }

    trice("err:=== end LR scan ===\n");
}

static void captureRegs(sCrashRegs *regs)
{
    uint32_t *frame;

    __asm volatile ("MRS %0, CONTROL\n" : "=r"(regs->control) );
    __asm volatile ("MRS %0, PRIMASK\n" : "=r"(regs->primask) );

    /* No shim ran: this is a watchdog/assert/stack-overflow report, not a
     * fault.  There is no exception frame, so read the stacks live and make no
     * claim about a frame pointer or callee-saved registers. */
    if (g_crashEntry.valid != CRASH_ENTRY_MAGIC) {
        uint32_t livePsp;

        __asm volatile ("MRS %0, PSP\n" : "=r"(livePsp));
        memset((void *)&g_crashEntry, 0, sizeof(g_crashEntry));
        regs->sp         = livePsp;
        regs->fp         = livePsp;
        regs->frameOnMsp = 0U;
        regs->r0 = regs->r1 = regs->r2 = regs->r3 = 0U;
        regs->r12 = regs->lr = regs->pc = regs->psr = 0U;
        return;
    }

    /* This used to read PSP unconditionally, which silently decoded garbage
     * for a fault taken in handler mode — where the frame is on MSP.  Pick the
     * stack whose frame validates instead; PSP first, since a task fault is
     * the common case and FreeRTOS runs tasks on PSP. */
    if (frame_looks_valid(g_crashEntry.psp)) {
        frame            = (uint32_t *)g_crashEntry.psp;
        regs->frameOnMsp = 0U;
    } else if (frame_looks_valid(g_crashEntry.msp)) {
        frame            = (uint32_t *)g_crashEntry.msp;
        regs->frameOnMsp = 1U;
    } else {
        frame            = (uint32_t *)g_crashEntry.psp;   /* best effort */
        regs->frameOnMsp = 0U;
    }

    /* Frame pointer of the faulting context.  Only trusted if it points into
     * a stack; anything else means the prologue was not what we assumed, and a
     * WRONG fp is worse than none — it walks the unwinder into noise. */
    regs->fp  = sp_in_ram(g_crashEntry.caller_r7) ? g_crashEntry.caller_r7
                                                  : regs->sp;

    regs->sp  = (uint32_t)frame;
    regs->r0  = frame[0];
    regs->r1  = frame[1];
    regs->r2  = frame[2];
    regs->r3  = frame[3];
    regs->r12 = frame[4];
    regs->lr  = frame[5];
    regs->pc  = frame[6];
    regs->psr = frame[7];
}

/**
 * @brief Print stacked registers and Cortex-M4 fault status registers.
 */
static void printRegisters(eCrashType type, const sCrashRegs *regs)
{
    uint32_t idx = (uint32_t)type;
    uint32_t strCount = sizeof(s_crashTypeStr) / sizeof(s_crashTypeStr[0]);
    const char *hdr = (idx < strCount) ? s_crashTypeStr[idx] : "UNKNOWN FAULT";

    TRiceS("err:========== %s ==========\n", hdr);
    trice("err:Stacked Registers:\n");
    trice("\t R0  = 0x%08X\n\t R1  = 0x%08X\n", regs->r0,  regs->r1);
    trice("\t R2  = 0x%08X\n\t R3  = 0x%08X\n", regs->r2,  regs->r3);
    trice("\t R12 = 0x%08X\n\t LR  = 0x%08X\n", regs->r12, regs->lr);
    trice("\t PC  = 0x%08X\n\t PSR = 0x%08X\n", regs->pc,  regs->psr);
    trice("err:Control:\n");
    trice("\t SP      = 0x%08X (PSP)\n\t CONTROL = 0x%08X\n", regs->sp, regs->control);
    trice("\t PRIMASK = 0x%08X\n", regs->primask);

    /* Cortex-M4 fault status registers */
    trice("err:Fault Status:\n");
    trice("\t CFSR = 0x%08X\n\t HFSR = 0x%08X\n", (uint32_t)SCB->CFSR, (uint32_t)SCB->HFSR);
    trice("\t MMFAR= 0x%08X\n\t BFAR = 0x%08X\n", (uint32_t)SCB->MMFAR, (uint32_t)SCB->BFAR);

    trice("err:=========================================\n");
}

/* --------------------------------------------------------------------------
 * Flash storage — save crash log to external SPI flash
 *
 * Called from exception context, where the ordinary driver cannot be used:
 * every wait on its path is bounded by the peripheral-library tick, and that
 * tick is incremented by a TIM6 interrupt which a fault handler (priority -1)
 * and the TIM14 software watchdog (priority 15, so equal-priority) both
 * prevent from running.  Those deadlines therefore never expire, and a
 * recorder that hangs destroys the evidence it exists to preserve.
 *
 * w25q_fault.c is the path that has no such dependency: register-level,
 * budgeted in CPU cycles, and terminating in every case.  Writing nothing is
 * an acceptable outcome here; not returning is not.
 * -------------------------------------------------------------------------- */

static void saveToFlash(eCrashType type, const sCrashRegs *regs)
{
    static sCrashLog log;  /* static to avoid stack overflow in exception ctx */
    sW25qFaultBudget saveBudget;

    /* Feed the independent watchdog EXACTLY once, here.  In fault context
     * nothing else will, and a fault arriving 16 s into the window would
     * otherwise leave the recorder no time at all; one refresh buys a
     * deterministic full window.  Refusing to refresh again is what still
     * guarantees the board resets if this path is wrong about something.
     *
     * The key register directly, not KickIwdg(): that also touches TIM14 and
     * the watchdog-gap statistics, neither of which belongs here. */
    IWDG->KR = 0xAAAAU;

    memset(&log, 0, sizeof(log));
    log.magic      = CRASH_LOG_MAGIC;
    log.tick       = HAL_GetTick();
    log.crash_type = (uint8_t)type;

    /* Registers */
    log.r0      = regs->r0;
    log.r1      = regs->r1;
    log.r2      = regs->r2;
    log.r3      = regs->r3;
    log.r12     = regs->r12;
    log.lr      = regs->lr;
    log.pc      = regs->pc;
    log.psr     = regs->psr;
    log.sp      = regs->sp;
    log.control = regs->control;
    log.primask = regs->primask;

    for (int i = 0; i < 8; i++) {
        log.r4_r11[i] = g_crashEntry.r4_r11[i];
    }
    log.frame_on_msp = regs->frameOnMsp;

    /* A call path that survives a broken frame chain — the faulting stack only,
     * because that is the one worth the flash and the other stacks go to Trice. */
    log.scan_count = scan_stack(NULL,
                                regs->frameOnMsp ? g_crashEntry.msp
                                                 : g_crashEntry.psp,
                                8U, log.scan_lr, CRASH_LOG_MAX_SCAN);

    /* Fault status */
    log.cfsr  = SCB->CFSR;
    log.hfsr  = SCB->HFSR;
    log.mmfar = SCB->MMFAR;
    log.bfar  = SCB->BFAR;

    /* Backtrace */
    backtrace_frame_t frame = {
        .pc = regs->pc, .lr = regs->lr,
        .sp = regs->sp, .fp = regs->fp
    };
    backtrace_t bt[CRASH_LOG_MAX_BT_DEPTH];
    int depth = _backtrace_unwind(bt, CRASH_LOG_MAX_BT_DEPTH, &frame);
    log.bt_depth = (uint8_t)(depth > 0 ? depth : 0);
    for (int i = 0; i < log.bt_depth; i++) {
        log.bt_addr[i] = (uint32_t)bt[i].address;
    }

    /* Recorded for EVERY type, faults included.  It used to be limited to the
     * types where the current task is provably the offender, which left a
     * HardFault report with no task at all -- and "which task faulted" is the
     * first question asked of one.  For a fault in task context this IS the
     * offender; for a fault in ISR context it is the task that was interrupted,
     * which is still worth having.  Tell the two apart from the stacked PSR:
     * IPSR (bits 8:0 of `psr`) is nonzero when the faulting context was an
     * exception handler. */
    {
        const char *name = pcTaskGetName(NULL);
        if (name) {
            strncpy(log.task_name, name, sizeof(log.task_name) - 1);
        }
    }

    /* Task snapshots */
    TaskStatus_t tasks[CRASH_LOG_MAX_TASKS];
    UBaseType_t n = uxTaskGetSystemState(tasks, CRASH_LOG_MAX_TASKS, NULL);
    if (n > CRASH_LOG_MAX_TASKS) n = CRASH_LOG_MAX_TASKS;
    log.task_count = (uint8_t)n;

    for (UBaseType_t i = 0; i < n; i++) {
        sCrashTask *ct = &log.tasks[i];
        strncpy(ct->name, tasks[i].pcTaskName, sizeof(ct->name) - 1);
        ct->free_stack = (uint16_t)tasks[i].usStackHighWaterMark;
        ct->state = (uint8_t)tasks[i].eCurrentState;

        volatile StackType_t *top =
            *((volatile StackType_t *volatile *)tasks[i].xHandle);
        uint32_t exc_return = (uint32_t)top[8];
        uint32_t fpu_off = ((exc_return & 0x10U) == 0U) ? 16U : 0U;
        ct->pc = (uint32_t)top[15U + fpu_off];
        ct->lr = (uint32_t)top[14U + fpu_off];
    }

    /* CRC32 over everything except the crc32 field itself */
    log.crc32 = ImgMgmt_Crc32((const uint8_t *)&log,
                               offsetof(sCrashLog, crc32));

    /* Where to put it comes from nvDb; putting it there does not.  This runs
     * in fault context, where the RTOS may be dead and an SPI transaction may
     * have been in flight, so it goes through w25q_fault.c — the path that
     * rebuilds the bus from constants and bounds every wait in CPU cycles.
     * One of the two clients nvdb_exceptions.h exists for.
     *
     * Before nvDb has run there is nowhere to write: the flash driver has not
     * been brought up either at that point, so nothing is lost that could
     * have been saved. */
    uint32_t base = 0U;
    uint32_t area = 0U;

    if (NvDb_GetAbsoluteAddress(nvdbUser_crashLog, &base, &area) != nvdbRes_ok ||
        area < sizeof(sCrashLog)) {
        return;
    }

    if (W25qFault_Begin() != w25qf_ok) {
        return;
    }

    /* One budget for the whole save, checked between steps.  The per-call
     * budgets bound each wait but not their sum, and an erase that legitimately
     * eats its own 600 ms must not then be followed by four more waits that
     * each eat theirs. */
    W25qFault_BudgetStart(&saveBudget, W25Q_FAULT_SAVE_BUDGET_CYCLES);

    if (W25qFault_EraseSector(base) != w25qf_ok) {
        return;
    }

    /* Write in 256-byte pages */
    const uint8_t *src = (const uint8_t *)&log;
    uint32_t remaining = sizeof(sCrashLog);
    uint32_t addr = base;
    while (remaining > 0) {
        uint32_t chunk = (remaining > W25Q_FAULT_PAGE_SIZE)
                       ? W25Q_FAULT_PAGE_SIZE : remaining;

        if (W25qFault_BudgetExpired(&saveBudget)) {
            return;
        }
        if (W25qFault_WritePage(addr, src, chunk) != w25qf_ok) {
            return;
        }
        src += chunk;
        addr += chunk;
        remaining -= chunk;
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void Crash_GenerateReport(eCrashType type)
{
    sCrashRegs regs;

    captureRegs(&regs);
    flushTrice();

    if (type == crashType_swWatchdog || type == crashType_stackOverflow ||
        type == crashType_assert) {
        TRiceS("err:Fault in task: %s\n", pcTaskGetName(NULL));
    }

    printRegisters(type, &regs);
    flushTrice();

    /* The FP chain walk IS available now: Crash_CaptureEntry() latches R4-R11
     * on exception entry, so R7 of the faulting context is real.  This used to
     * pass `sp` as the frame pointer and could never produce more than pc+lr. */
    printBacktrace(regs.pc, regs.lr, regs.sp, regs.fp);
    flushTrice();

    /* Save to external flash before printing task list (which takes longer) */
    saveToFlash(type, &regs);

    /* Print all tasks with individual backtraces */
    printTaskList();
    flushTrice();

    /* Chain-independent view, last because it is the most verbose and the
     * durable record is already in flash by now. */
    scanAllStacks(regs.frameOnMsp);
    flushTrice();

    /* Consumed: a second report must not inherit this one's snapshot. */
    g_crashEntry.valid = 0U;
}

bool Crash_ReadFromFlash(sCrashLog *log)
{
    if (!log) return false;

    /* Reading happens in a task, so it goes through the front door. */
    if (NvDb_Read(nvdbUser_crashLog, log, 0U,
                  sizeof(sCrashLog)) != nvdbRes_ok) {
        return false;
    }

    if (log->magic != CRASH_LOG_MAGIC) {
        return false;
    }

    uint32_t expected = ImgMgmt_Crc32((const uint8_t *)log,
                                       offsetof(sCrashLog, crc32));
    return (log->crc32 == expected);
}

void Crash_ClearFlash(void)
{
    /* nvDb's delete is eventual, and an operator who just cleared the log
     * expects the next read to say so.  Clearing the magic is a bit-clear, so
     * it lands synchronously and costs no erase; the wipe then reclaims the
     * space in the background, which is what makes the NEXT crash write
     * cheap. */
    uint32_t magic = 0U;

    (void)NvDb_Write(nvdbUser_crashLog, &magic, offsetof(sCrashLog, magic),
                     sizeof(magic));
    (void)NvDb_Wipe(nvdbUser_crashLog, NULL);
}
