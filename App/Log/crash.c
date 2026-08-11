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
#include "bl_app_contract.h"
#include "image_mgmt.h"
#include "w25q128.h"
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
    uint32_t sp;      /*!< PSP value at fault time   */
    uint32_t control;
    uint32_t primask;
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
static void captureRegs(sCrashRegs *regs)
{
    uint32_t *frame;

    __asm volatile ("MRS %0, PSP\n"     : "=r"(frame)         );
    __asm volatile ("MRS %0, CONTROL\n" : "=r"(regs->control) );
    __asm volatile ("MRS %0, PRIMASK\n" : "=r"(regs->primask) );

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
 * Called from exception context. SPI driver uses polling, so it works
 * even when interrupts are disabled. We force SPI handle state to READY
 * in case a transfer was in progress when the fault occurred.
 * -------------------------------------------------------------------------- */

static void flash_force_ready(void)
{
    extern SPI_HandleTypeDef W25Q128_SPI_HANDLE;

    /* Deassert CS in case a transaction was in progress */
    HAL_GPIO_WritePin(W25Q128_CS_GPIO_PORT, W25Q128_CS_GPIO_PIN, GPIO_PIN_SET);

    /* Force SPI handle to READY so HAL polling functions accept new transfers */
    W25Q128_SPI_HANDLE.State = HAL_SPI_STATE_READY;
    __HAL_UNLOCK(&W25Q128_SPI_HANDLE);
}

static void saveToFlash(eCrashType type, const sCrashRegs *regs)
{
    static sCrashLog log;  /* static to avoid stack overflow in exception ctx */

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

    /* Fault status */
    log.cfsr  = SCB->CFSR;
    log.hfsr  = SCB->HFSR;
    log.mmfar = SCB->MMFAR;
    log.bfar  = SCB->BFAR;

    /* Backtrace */
    backtrace_frame_t frame = {
        .pc = regs->pc, .lr = regs->lr,
        .sp = regs->sp, .fp = regs->sp
    };
    backtrace_t bt[CRASH_LOG_MAX_BT_DEPTH];
    int depth = _backtrace_unwind(bt, CRASH_LOG_MAX_BT_DEPTH, &frame);
    log.bt_depth = (uint8_t)(depth > 0 ? depth : 0);
    for (int i = 0; i < log.bt_depth; i++) {
        log.bt_addr[i] = (uint32_t)bt[i].address;
    }

    /* Task name (current task is the offender for these types) */
    if (type == crashType_swWatchdog || type == crashType_stackOverflow ||
        type == crashType_assert) {
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

    /* Write to external flash */
    flash_force_ready();
    W25Q128_EraseSector(EXT_FLASH_CRASH_LOG_ADDR);
    W25Q128_WaitReady(W25Q128_ERASE_TIMEOUT_MS);

    /* Write in 256-byte pages */
    const uint8_t *src = (const uint8_t *)&log;
    uint32_t remaining = sizeof(sCrashLog);
    uint32_t addr = EXT_FLASH_CRASH_LOG_ADDR;
    while (remaining > 0) {
        uint32_t chunk = (remaining > W25Q128_PAGE_SIZE) ? W25Q128_PAGE_SIZE : remaining;
        W25Q128_WritePage(addr, src, chunk);
        W25Q128_WaitReady(W25Q128_TIMEOUT_MS);
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

    /* Backtrace for the faulting context (PC + LR from exception frame;
       FP chain walk not available here since R7 was not saved) */
    printBacktrace(regs.pc, regs.lr, regs.sp, regs.sp);
    flushTrice();

    /* Save to external flash before printing task list (which takes longer) */
    saveToFlash(type, &regs);

    /* Print all tasks with individual backtraces */
    printTaskList();
    flushTrice();
}

bool Crash_ReadFromFlash(sCrashLog *log)
{
    if (!log) return false;

    if (W25Q128_Read(EXT_FLASH_CRASH_LOG_ADDR,
                     (uint8_t *)log, sizeof(sCrashLog)) != w25q_ok) {
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
    W25Q128_EraseSector(EXT_FLASH_CRASH_LOG_ADDR);
}
