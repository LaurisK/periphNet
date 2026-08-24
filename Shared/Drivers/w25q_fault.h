/**
 * @file    w25q_fault.h
 * @brief   Fault-context SPI2 + W25Q64 path — depends on no interrupt.
 *
 * The ordinary driver (w25q128.c) waits on the HAL time base, which is a
 * TIM6 interrupt at NVIC priority 15.  In a fault handler (priority -1) and
 * in the TIM14 software-watchdog callback (priority 15) that interrupt cannot
 * preempt, so the tick is a constant and every deadline expressed in it is
 * permanently in the future.  The crash recorder therefore had waits that
 * could not terminate, and a recorder that hangs destroys the evidence it
 * exists to preserve (docs/task_fault_context_flash.md §1).
 *
 * This module is the answer, and it obeys one rule:
 *
 *   A fault-context I/O path may depend on nothing that an interrupt has to
 *   deliver.  Not a tick, not a DMA completion, not a scheduler, not a
 *   callback.  Every wait is bounded by something the faulting core advances
 *   itself, and every path terminates — writing nothing is an acceptable
 *   outcome, never returning is not.
 *
 * Consequences, all deliberate:
 *
 *   - Register-level.  No peripheral handle, no HAL state machine, no lock.
 *     Those are shared mutable state the interrupted task owned a moment ago;
 *     the fault path must not read it, repair it, or care.  SPI2 is rebuilt
 *     from compile-time constants instead.
 *   - The time base is the DWT cycle counter, a core counter that advances in
 *     every context and needs no interrupt.  Budgets are cycles, not
 *     milliseconds.
 *   - No read, no ID check, no chip erase, no power-down.  The crash log is
 *     the only user, and every entry point here is code that has to be
 *     correct without ever being exercised in anger.
 *
 * NOT callable from a task.  Tasks keep w25q128.h, its mutex and the HAL —
 * ticks work there and nothing about that path is broken.
 */
#ifndef W25Q_FAULT_H_
#define W25Q_FAULT_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Geometry — the parts of it this path needs, so a caller need not pull in
 * the ordinary driver's header for two constants.
 * -------------------------------------------------------------------------- */

#define W25Q_FAULT_PAGE_SIZE            256U
#define W25Q_FAULT_SECTOR_SIZE          0x1000U
#define W25Q_FAULT_FLASH_SIZE           0x1000000U

/* --------------------------------------------------------------------------
 * Budgets
 *
 * Cycles, because that is what the time base counts.  Sized from the nominal
 * core clock: the requirement is termination, not accuracy, so a board
 * running at another frequency gets a proportionally wrong — but still
 * finite — budget.
 * -------------------------------------------------------------------------- */

#define W25Q_FAULT_CORE_HZ              168000000UL
#define W25Q_FAULT_CYCLES_PER_MS        (W25Q_FAULT_CORE_HZ / 1000UL)

/** Chip-BUSY poll.  A sector erase is typically 45 ms and specified to
 *  400 ms; the fault may have interrupted one that the MCU cannot abort, so
 *  waiting is correct.  Giving up is also correct — the board must reach its
 *  reset.
 *
 *  Overridable from the build so acceptance test §5.4 can shorten it below an
 *  erase time and watch the give-up path actually give up.  Nothing else may
 *  override it: a board that ships with a budget under 400 ms would discard
 *  records for the one case this module was written for. */
#ifndef W25Q_FAULT_BUSY_BUDGET_CYCLES
#define W25Q_FAULT_BUSY_BUDGET_CYCLES   (600UL * W25Q_FAULT_CYCLES_PER_MS)
#endif

/** One TXE/RXNE/BSY flag.  A 21 MHz byte is 0.4 us; anything approaching a
 *  millisecond means the peripheral is wedged and more waiting will not
 *  help. */
#define W25Q_FAULT_XFER_BUDGET_CYCLES   (1UL * W25Q_FAULT_CYCLES_PER_MS)

/** Suggested budget for a whole crash-log save: one interrupted erase plus
 *  our own erase plus the page programs, with margin.  Per-call budgets alone
 *  do not bound a sequence of calls, which is what this is for. */
#define W25Q_FAULT_SAVE_BUDGET_CYCLES   (1500UL * W25Q_FAULT_CYCLES_PER_MS)

/* --------------------------------------------------------------------------
 * Types
 * -------------------------------------------------------------------------- */

typedef enum {
    w25qf_undefined = 0,
    w25qf_ok,
    w25qf_busyTimeout,    /*!< the chip never left BUSY within its budget   */
    w25qf_busTimeout,     /*!< SPI2 never produced TXE/RXNE within budget   */
    w25qf_badArg,
    w25qf_last
} eW25qFaultRes;

/**
 * @brief A deadline on whichever time base is available.
 *
 * Opaque in practice — the caller only starts one and asks whether it has
 * expired.  It is exposed so a caller can bound a SEQUENCE of calls on the
 * same base the module waits on, without a second notion of time appearing
 * in fault context.
 */
typedef struct {
    uint32_t deadline;      /*!< cycle-counter deadline when useCyc          */
    uint32_t itersLeft;     /*!< fallback loop counter otherwise             */
    uint8_t  useCyc;        /*!< 1 = cycle counter, 0 = plain loop counter   */
} sW25qFaultBudget;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/**
 * @brief Bring SPI2 and the chip to a known state from whatever the fault
 *        interrupted.
 *
 * Idempotent, and safe to call when the peripheral was never initialised —
 * that ends in ::w25qf_busTimeout rather than a lockup.  Enables the cycle
 * counter (two register writes, harmless if already running, and required
 * because the bootloader never enables it), forces CS high, disables SPI2,
 * drains a stale receive byte and a possible overrun, rewrites CR1/CR2 from
 * constants, re-enables it, and waits for the chip to leave BUSY.
 *
 * @return w25qf_ok, w25qf_busyTimeout or w25qf_busTimeout.
 */
eW25qFaultRes W25qFault_Begin(void);

/**
 * @brief Erase the 4 KB sector containing @p addr.
 * @param addr Any address within the target sector.
 * @return w25qf_ok, w25qf_badArg, w25qf_busTimeout or w25qf_busyTimeout.
 */
eW25qFaultRes W25qFault_EraseSector(uint32_t addr);

/**
 * @brief Program up to one 256-byte page.
 * @param addr      Destination, must be page-aligned and already erased.
 * @param data      Bytes to program.
 * @param len_bytes 1..256.
 * @return w25qf_ok, w25qf_badArg, w25qf_busTimeout or w25qf_busyTimeout.
 */
eW25qFaultRes W25qFault_WritePage(uint32_t addr, const uint8_t *data,
                                  uint32_t len_bytes);

/**
 * @brief Start a budget of @p cycles from now.
 *
 * Uses the cycle counter when it is running, a plain decrementing counter
 * otherwise; ::W25qFault_Begin decides which and must have run first.
 */
void W25qFault_BudgetStart(sW25qFaultBudget *budget, uint32_t cycles);

/**
 * @brief Has @p budget run out?
 *
 * In the fallback mode this call is itself the clock, so it must be reached
 * from the loop it bounds — which is exactly how a budget is used.
 */
bool W25qFault_BudgetExpired(sW25qFaultBudget *budget);

#ifdef __cplusplus
}
#endif

#endif /* W25Q_FAULT_H_ */
