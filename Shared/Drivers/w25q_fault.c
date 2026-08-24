/**
 * @file    w25q_fault.c
 * @brief   Fault-context SPI2 + W25Q64 path — see w25q_fault.h for the rule.
 *
 * Nothing here waits on anything an interrupt has to deliver.  The only
 * clock is the DWT cycle counter, which the faulting core advances itself;
 * every loop in the file is bounded by a budget expressed in it, and the
 * fallback when that counter is unavailable is a plain decrementing counter.
 *
 * The peripheral is driven through its registers on purpose.  A handle, its
 * lock and its state machine are shared mutable data that the interrupted
 * task owned a moment ago and that the fault may have corrupted; rebuilding
 * SPI2 from compile-time constants needs none of it, and works even if the
 * peripheral was never brought up.
 *
 * The build enforces the absence of the tick, the delay, the peripheral
 * library and the RTOS in this one file at configure time (CMakeLists.txt) —
 * a convention here would decay exactly like the one this file replaces.
 * That is also why the comments say "the peripheral library" where they
 * would otherwise name it: the guard greps for the names.
 */

#include "w25q_fault.h"
#include "stm32f4xx.h"

/* --------------------------------------------------------------------------
 * Board constants
 *
 * Taken from MX_SPI2_Init() and the CS pin assignment, deliberately COPIED
 * rather than read out of the peripheral handle: the handle lives in RAM the
 * fault may have wrecked, and reading it would reintroduce the dependency
 * this module exists to remove.
 * -------------------------------------------------------------------------- */

#define W25QF_CS_PORT           GPIOE
#define W25QF_CS_PIN            3U

/* Master, 2-line, 8-bit, CPOL 0 / CPHA 0, MSB first, software NSS with SSI
 * set, BR = /2 (PCLK1 42 MHz -> 21 MHz), CRC off.  SPE is added separately
 * so the configuration can be written while the peripheral is disabled. */
#define W25QF_CR1_CFG           (SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI)
#define W25QF_CR2_CFG           (0UL)

/* Commands — only the four this path needs. */
#define W25QF_CMD_WRITE_ENABLE      0x06U
#define W25QF_CMD_READ_STATUS_1     0x05U
#define W25QF_CMD_PAGE_PROGRAM      0x02U
#define W25QF_CMD_SECTOR_ERASE      0x20U

#define W25QF_STATUS_BUSY           0x01U

/* Fallback loop: how many cycles one W25qFault_BudgetExpired() call plus the
 * flag poll around it is assumed to cost.  Under-estimating over-waits, which
 * is the safe direction — the requirement is termination, not accuracy. */
#define W25QF_FALLBACK_CYCLES_PER_ITER  8UL

/* --------------------------------------------------------------------------
 * Private data
 * -------------------------------------------------------------------------- */

/** 1 when the cycle counter was observed to advance in W25qFault_Begin().
 *  Not in CCM and not initialised anywhere else: a fault can arrive before
 *  any of this module has run, and Begin() is the only thing that sets it. */
static uint8_t s_useCyc;

/* --------------------------------------------------------------------------
 * Time base
 * -------------------------------------------------------------------------- */

/**
 * @brief Enable the DWT cycle counter and decide whether it can be trusted.
 *
 * Idempotent.  CYCCNT is deliberately NOT zeroed — the system monitor derives
 * the RTOS run-time statistics from the same counter, and this module only
 * ever takes differences.
 *
 * The counter is checked by reading it twice around a short spin rather than
 * by testing for zero: a counter stuck at any value fails the same way, and
 * a running counter passing through zero does not.
 */
static void cyc_enable(void)
{
    volatile uint32_t spin;
    uint32_t          first;
    uint32_t          second;

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;

    first = DWT->CYCCNT;
    for (spin = 0U; spin < 8U; spin++) {
    }
    second = DWT->CYCCNT;

    s_useCyc = (first != second) ? 1U : 0U;
}

void W25qFault_BudgetStart(sW25qFaultBudget *budget, uint32_t cycles)
{
    if (budget == NULL) {
        return;
    }

    budget->useCyc = s_useCyc;

    if (s_useCyc != 0U) {
        budget->deadline  = DWT->CYCCNT + cycles;
        budget->itersLeft = 0U;
    } else {
        budget->deadline  = 0U;
        budget->itersLeft = cycles / W25QF_FALLBACK_CYCLES_PER_ITER;
        if (budget->itersLeft == 0U) {
            budget->itersLeft = 1U;
        }
    }
}

bool W25qFault_BudgetExpired(sW25qFaultBudget *budget)
{
    if (budget == NULL) {
        return true;                    /* no budget is an expired budget */
    }

    if (budget->useCyc != 0U) {
        /* Signed difference of unsigned counters: correct across the ~25.6 s
         * wrap at 168 MHz for any budget shorter than half that, which every
         * budget in this module is by two orders of magnitude. */
        return ((int32_t)(DWT->CYCCNT - budget->deadline) >= 0);
    }

    if (budget->itersLeft == 0U) {
        return true;
    }
    budget->itersLeft--;
    return false;
}

/* --------------------------------------------------------------------------
 * Bus primitives
 * -------------------------------------------------------------------------- */

/**
 * @brief Hold CS high for at least the chip's deselect time.
 *
 * A fixed spin, not a wait: it depends on nothing and cannot fail.  ~16
 * iterations at 168 MHz comfortably clears the 50 ns tSHSL the part asks for,
 * and the loop is here because back-to-back register writes would not.
 */
static inline void cs_settle(void)
{
    for (volatile uint32_t i = 0U; i < 16U; i++) {
    }
}

static inline void cs_low(void)
{
    W25QF_CS_PORT->BSRR = (1UL << (W25QF_CS_PIN + 16U));
}

/**
 * @brief Wait for the shift register to drain, then release CS.
 *
 * Bounded: if the peripheral never reports itself idle, CS goes high anyway.
 * A truncated command is a far better outcome than a handler that never
 * returns, and the chip rejects an incomplete command on its own.
 */
static void cs_high(void)
{
    sW25qFaultBudget budget;

    W25qFault_BudgetStart(&budget, W25Q_FAULT_XFER_BUDGET_CYCLES);
    while ((SPI2->SR & SPI_SR_BSY) != 0U) {
        if (W25qFault_BudgetExpired(&budget)) {
            break;
        }
    }

    W25QF_CS_PORT->BSRR = (1UL << W25QF_CS_PIN);
    cs_settle();
}

/**
 * @brief Exchange one byte, full duplex.
 * @param out Byte to shift out.
 * @param in  Receives the byte shifted in; may be NULL.
 * @return w25qf_ok, or w25qf_busTimeout if a flag never appeared.
 *
 * Both waits carry the per-transfer budget.  A peripheral with its clock
 * gated off reads back as all-zero, so a fault before the bus was ever
 * brought up lands here and times out in a millisecond.
 */
static eW25qFaultRes xfer(uint8_t out, uint8_t *in)
{
    sW25qFaultBudget budget;
    uint8_t          rx;

    W25qFault_BudgetStart(&budget, W25Q_FAULT_XFER_BUDGET_CYCLES);
    while ((SPI2->SR & SPI_SR_TXE) == 0U) {
        if (W25qFault_BudgetExpired(&budget)) {
            return w25qf_busTimeout;
        }
    }

    /* 8-bit access to DR: a 16-bit write would shift out two bytes. */
    *(volatile uint8_t *)&SPI2->DR = out;

    W25qFault_BudgetStart(&budget, W25Q_FAULT_XFER_BUDGET_CYCLES);
    while ((SPI2->SR & SPI_SR_RXNE) == 0U) {
        if (W25qFault_BudgetExpired(&budget)) {
            return w25qf_busTimeout;
        }
    }

    rx = *(volatile uint8_t *)&SPI2->DR;
    if (in != NULL) {
        *in = rx;
    }

    return w25qf_ok;
}

/** Shift out a one-byte command followed by a 24-bit address. */
static eW25qFaultRes send_cmd_addr(uint8_t cmd, uint32_t addr)
{
    eW25qFaultRes res;

    cs_low();

    res = xfer(cmd, NULL);
    if (res == w25qf_ok) {
        res = xfer((uint8_t)((addr >> 16) & 0xFFU), NULL);
    }
    if (res == w25qf_ok) {
        res = xfer((uint8_t)((addr >> 8) & 0xFFU), NULL);
    }
    if (res == w25qf_ok) {
        res = xfer((uint8_t)(addr & 0xFFU), NULL);
    }

    if (res != w25qf_ok) {
        cs_high();
    }
    return res;
}

/**
 * @brief Poll status register 1 until BUSY clears.
 * @return w25qf_ok, w25qf_busTimeout if the bus died, w25qf_busyTimeout if
 *         the chip was still busy when the budget ran out.
 *
 * Waiting here is the whole point: the fault may have interrupted a program
 * or erase cycle that the MCU cannot abort, and the chip will not accept a
 * new command until it finishes.  A reset command (66h/99h) would end it
 * sooner and leave the interrupted page or sector indeterminate — the
 * recorder would be corrupting somebody else's data to save its own, so it
 * waits or gives up instead.
 */
static eW25qFaultRes wait_ready(void)
{
    sW25qFaultBudget budget;

    W25qFault_BudgetStart(&budget, W25Q_FAULT_BUSY_BUDGET_CYCLES);

    for (;;) {
        eW25qFaultRes res;
        uint8_t       status = 0xFFU;

        cs_low();
        res = xfer(W25QF_CMD_READ_STATUS_1, NULL);
        if (res == w25qf_ok) {
            res = xfer(0xFFU, &status);
        }
        cs_high();

        if (res != w25qf_ok) {
            return res;
        }
        if ((status & W25QF_STATUS_BUSY) == 0U) {
            return w25qf_ok;
        }
        if (W25qFault_BudgetExpired(&budget)) {
            return w25qf_busyTimeout;
        }
    }
}

static eW25qFaultRes write_enable(void)
{
    eW25qFaultRes res;

    cs_low();
    res = xfer(W25QF_CMD_WRITE_ENABLE, NULL);
    cs_high();

    return res;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

eW25qFaultRes W25qFault_Begin(void)
{
    volatile uint32_t drain;

    cyc_enable();

    /* 1. CS high — abort whatever command was in flight. */
    W25QF_CS_PORT->BSRR = (1UL << W25QF_CS_PIN);
    cs_settle();

    /* 2. Disable the peripheral so its configuration can be rewritten. */
    SPI2->CR1 &= ~SPI_CR1_SPE;

    /* 3. Drain a stale receive byte and clear a possible overrun.  Reading
     *    DR then SR is the documented OVR clear sequence; unconditional and
     *    with no flag poll, so it cannot wait on anything. */
    drain = SPI2->DR;
    drain = SPI2->SR;
    drain = SPI2->DR;
    (void)drain;

    /* 4. Configuration from constants, never from the peripheral handle. */
    SPI2->CR2 = W25QF_CR2_CFG;
    SPI2->CR1 = W25QF_CR1_CFG;

    /* 5. Enable. */
    SPI2->CR1 = W25QF_CR1_CFG | SPI_CR1_SPE;

    /* 6. Let the chip finish whatever it was doing. */
    return wait_ready();
}

/**
 * @brief Issue a sector erase and return with the chip still busy.
 *
 * Internal: nothing outside this file may leave work in flight on the chip.
 * It is split out because W25qFault_EraseSector is exactly this plus the
 * wait, and because putting the chip deliberately busy is what the acceptance
 * test of docs/task_fault_context_flash.md §5.3 needed — that test rig has
 * been removed, so re-exposing this is what a rerun would take.
 */
static eW25qFaultRes start_erase_sector(uint32_t addr)
{
    eW25qFaultRes res;

    if (addr >= W25Q_FAULT_FLASH_SIZE) {
        return w25qf_badArg;
    }

    res = wait_ready();
    if (res != w25qf_ok) {
        return res;
    }

    res = write_enable();
    if (res != w25qf_ok) {
        return res;
    }

    res = send_cmd_addr(W25QF_CMD_SECTOR_ERASE, addr);
    if (res != w25qf_ok) {
        return res;
    }
    cs_high();

    /* Deliberately no wait_ready() here — the caller does the waiting. */
    return w25qf_ok;
}

eW25qFaultRes W25qFault_EraseSector(uint32_t addr)
{
    eW25qFaultRes res = start_erase_sector(addr);

    if (res != w25qf_ok) {
        return res;
    }

    return wait_ready();
}

eW25qFaultRes W25qFault_WritePage(uint32_t addr, const uint8_t *data,
                                  uint32_t len_bytes)
{
    eW25qFaultRes res;

    if ((data == NULL) || (len_bytes == 0U) ||
        (len_bytes > W25Q_FAULT_PAGE_SIZE) ||
        ((addr % W25Q_FAULT_PAGE_SIZE) != 0U) ||
        ((addr + len_bytes) > W25Q_FAULT_FLASH_SIZE)) {
        return w25qf_badArg;
    }

    res = wait_ready();
    if (res != w25qf_ok) {
        return res;
    }

    res = write_enable();
    if (res != w25qf_ok) {
        return res;
    }

    res = send_cmd_addr(W25QF_CMD_PAGE_PROGRAM, addr);
    if (res != w25qf_ok) {
        return res;
    }

    /* Bounded by construction: len_bytes is at most one page and every byte
     * carries the per-transfer budget. */
    for (uint32_t i = 0U; i < len_bytes; i++) {
        res = xfer(data[i], NULL);
        if (res != w25qf_ok) {
            cs_high();
            return res;
        }
    }
    cs_high();

    return wait_ready();
}
