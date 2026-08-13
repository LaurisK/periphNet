/**
 * @file    modbus_blocks.c
 * @brief   Read-block derivation — see modbus_blocks.h.
 */

#include "modbus_blocks.h"

#include <stddef.h>

int MbBlocks_Find(const sModbusCapabilityRecord *cap,
                  const sModbusBlockRecord *blocks, uint8_t blockCount,
                  uint16_t addr)
{
    uint8_t stride = MbRecords_Stride(cap);

    for (uint8_t i = 0; i < blockCount; i++) {
        uint16_t base = blocks[i].base;

        if (addr < base) {
            continue;
        }
        /* The block's span in ADDRESS units is regs * stride: getting this
         * domain backwards is the silent failure §3.3 names. */
        if ((uint16_t)((addr - base) / stride) < blocks[i].regs) {
            return (int)i;
        }
    }
    return -1;
}

int MbBlocks_RegIndex(const sModbusCapabilityRecord *cap,
                      const sModbusReadBlock *blk, uint16_t ptAddr)
{
    uint8_t stride = MbRecords_Stride(cap);

    if (ptAddr < blk->addr) {
        return -1;
    }
    uint16_t idx = (uint16_t)((ptAddr - blk->addr) / stride);
    return (idx < blk->regs) ? (int)idx : -1;
}

/* Insertion sort by (fc, addr).  A time table is tens of entries, the sort
 * runs once when a plan goes live, and insertion sort needs no scratch — which
 * matters more here than the asymptotics. */
static void sort_spans(sModbusPointSpan *sel, uint16_t n)
{
    for (uint16_t i = 1; i < n; i++) {
        sModbusPointSpan key = sel[i];
        uint16_t         j   = i;

        while (j > 0u &&
               ((sel[j - 1].fc > key.fc) ||
                (sel[j - 1].fc == key.fc && sel[j - 1].addr > key.addr))) {
            sel[j] = sel[j - 1];
            j--;
        }
        sel[j] = key;
    }
}

int MbBlocks_Derive(const sModbusCapabilityRecord *cap,
                    const sModbusBlockRecord *blocks, uint8_t blockCount,
                    sModbusPointSpan *sel, uint16_t selCount,
                    sModbusReadBlock *out, uint16_t maxOut)
{
    uint8_t  stride  = MbRecords_Stride(cap);
    uint16_t maxRegs = MbRecords_MaxReadRegs(cap);
    uint16_t count   = 0;
    int      curDecl = -1;       /* declared block the open read sits in */
    uint16_t curEnd  = 0;        /* wire address one past the open read  */

    if (cap == NULL || sel == NULL || out == NULL ||
        (selCount > 0u && blocks == NULL)) {
        return mbBlocks_errBadArg;
    }
    if (selCount == 0u) {
        return 0;
    }

    sort_spans(sel, selCount);

    for (uint16_t i = 0; i < selCount; i++) {
        const sModbusPointSpan *p = &sel[i];
        int      decl = MbBlocks_Find(cap, blocks, blockCount, p->addr);
        uint16_t end;

        if (decl < 0) {
            return mbBlocks_errNoBlock;
        }
        if (p->regs == 0u) {
            return mbBlocks_errBadArg;
        }

        /* A point must END inside its block: `regs` is the read-span limit the
         * slave enforces, measured from the block base. */
        {
            uint16_t offRegs = (uint16_t)((p->addr - blocks[decl].base) / stride);
            if ((uint32_t)offRegs + p->regs > blocks[decl].regs) {
                return mbBlocks_errPastBlock;
            }
        }

        end = (uint16_t)(p->addr + (uint16_t)(p->regs * stride));

        if (count > 0u && decl == curDecl && p->fc == out[count - 1].fc) {
            /* Same declared block and same function code: extend, unless the
             * quantity would exceed what the slave accepts in one request. */
            uint16_t newEnd = (end > curEnd) ? end : curEnd;
            uint16_t regs   = (uint16_t)((newEnd - out[count - 1].addr) / stride);

            if (regs <= maxRegs) {
                out[count - 1].regs = regs;
                curEnd = newEnd;
                continue;
            }
        }

        if (count >= maxOut) {
            return mbBlocks_errTooMany;
        }
        out[count].addr = p->addr;
        out[count].regs = p->regs;
        out[count].fc   = p->fc;
        count++;
        curDecl = decl;
        curEnd  = end;
    }

    return (int)count;
}
