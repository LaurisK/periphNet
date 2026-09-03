/*
 * can_bus.c
 *
 *  Created on: 2026-09-03
 *      Author: Lauris
 *
 * See can_bus.h for what this owns and why it is one file.
 *
 * BIT TIMING is derived, not tabulated: one nominal bit is 14 time quanta
 * (SJW 1, BS1 10, BS2 3, sample point 11/14 = 78.6 %, which is what CiA
 * recommends and what every 500 kbit battery bus in the field uses), so the
 * prescaler is PCLK1 / (14 * bitrate).  A bitrate that does not divide
 * exactly is REFUSED rather than rounded — a bus 0.5 % off looks like it
 * works and then loses one frame in a hundred.
 *
 * THE TX PATH IS A QUEUE, NOT THREE MAILBOXES.  Forwarding happens in the
 * other bus's RX ISR, which may not block; six Pylontech frames arrive
 * back-to-back at 1 Hz and three mailboxes cannot hold them.  A full queue
 * is counted and refused, never waited on.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Can/can_bus.h"

#include "can.h"
#include "cmsis_os.h"
#include "trice.h"

#include <string.h>

/* Private defines ----------------------------------------------------------*/

/** Nominal bit = SJW(1) + BS1(10) + BS2(3) time quanta. */
#define CAN_TQ_PER_BIT          14u

#define CAN_BITRATE_MIN_BPS     10000u
#define CAN_BITRATE_MAX_BPS     1000000u

/** bxCAN has 28 filter banks shared by both cells; CAN1 owns [0, split) and
 *  CAN2 owns [split, 28).  One accept-everything bank each is all a bridge
 *  needs, but the split still has to be declared or CAN2 gets none. */
#define CAN_FILTER_BANK_SPLIT   14u

/* Private types ------------------------------------------------------------*/

typedef struct {
    sCanFrame frame[CAN_TX_QUEUE_DEPTH];
    uint8_t   head;
    uint8_t   tail;
    uint8_t   count;
    uint8_t   peak;
} sCanTxQueue;

typedef struct {
    CAN_HandleTypeDef *hal;
    sCanTxQueue        txq;
    sCanBusStats       stats;
} sCanBusCtx;

typedef struct {
    fCanRxHandler cb;
    void         *ctx;
    uint32_t      id;
    uint32_t      mask;
    uint8_t       bus;
    uint8_t       used;
} sCanSub;

/* Private variables --------------------------------------------------------*/

/* Plain .bss on purpose: CCM is the constrained region here (~92 %), and none
 * of this is hot enough to earn a place in it. */
static sCanBusCtx s_bus[canBus_last];
static sCanSub    s_sub[CAN_SUBSCRIBERS_MAX];
static fCanTap    s_tap;
static void      *s_tapCtx;

/* Private function prototypes ----------------------------------------------*/

static uint32_t EnterCritical(void);
static void     ExitCritical(uint32_t saved);
static sCanBusCtx *BusOf(eCanBus bus);
static sCanBusCtx *CtxOfHandle(const CAN_HandleTypeDef *hal);
static int      BusIndexOf(const CAN_HandleTypeDef *hal);
static int      Timing(uint32_t bitrate_bps, uint32_t *prescaler);
static int      ConfigFilter(eCanBus bus);
static void     PumpTx(sCanBusCtx *ctx);
static void     DrainFifo(CAN_HandleTypeDef *hal, uint32_t fifo);
static void     Dispatch(const sCanFrame *frame);
static void     Tap(const sCanFrame *frame, eCanDir dir);

/* Private functions --------------------------------------------------------*/

/**
 * The ONE place that knows there are two contexts.  taskENTER_CRITICAL()
 * misbehaves from an ISR on this port and taskENTER_CRITICAL_FROM_ISR() is
 * wrong outside one, so the choice is made here and nowhere else — the same
 * rule App/Pack/pack.c follows for the same reason.
 */
static uint32_t EnterCritical(void)
{
    if (0u != __get_IPSR()) {
        return taskENTER_CRITICAL_FROM_ISR();
    }
    taskENTER_CRITICAL();
    return 0u;
}

/** RE-CHECKS IPSR; it does not infer the context from @p saved. */
static void ExitCritical(uint32_t saved)
{
    if (0u != __get_IPSR()) {
        taskEXIT_CRITICAL_FROM_ISR(saved);
    } else {
        (void)saved;
        taskEXIT_CRITICAL();
    }
}

static sCanBusCtx *BusOf(eCanBus bus)
{
    if ((uint32_t)bus >= (uint32_t)canBus_last) {
        return NULL;
    }
    if (s_bus[bus].hal == NULL) {
        s_bus[bus].hal = (bus == canBus_1) ? &hcan1 : &hcan2;
    }
    return &s_bus[bus];
}

static int BusIndexOf(const CAN_HandleTypeDef *hal)
{
    if (hal->Instance == CAN1) {
        return (int)canBus_1;
    }
    if (hal->Instance == CAN2) {
        return (int)canBus_2;
    }
    return -1;
}

static sCanBusCtx *CtxOfHandle(const CAN_HandleTypeDef *hal)
{
    int idx = BusIndexOf(hal);

    if (idx < 0) {
        return NULL;
    }
    return BusOf((eCanBus)idx);
}

/**
 * @brief  Exact prescaler for a bitrate, or a refusal.
 * @param  bitrate_bps - requested rate
 * @param  prescaler - out, 1..1024
 * @retval 0 when the rate is reachable exactly, negative otherwise
 */
static int Timing(uint32_t bitrate_bps, uint32_t *prescaler)
{
    uint32_t pclk1_Hz = HAL_RCC_GetPCLK1Freq();
    uint32_t divisor  = CAN_TQ_PER_BIT * bitrate_bps;
    uint32_t presc;

    if ((bitrate_bps < CAN_BITRATE_MIN_BPS) ||
        (bitrate_bps > CAN_BITRATE_MAX_BPS)) {
        return -1;
    }
    if ((divisor == 0u) || ((pclk1_Hz % divisor) != 0u)) {
        return -2;
    }
    presc = pclk1_Hz / divisor;
    if ((presc < 1u) || (presc > 1024u)) {
        return -3;
    }
    *prescaler = presc;
    return 0;
}

/**
 * Accept EVERYTHING: 32-bit mask mode with a zero mask matches every
 * identifier, standard and extended, data and remote alike.  A bridge that
 * filters in silicon cannot report what it dropped, and "what crossed" is
 * half of what this module is for.
 */
static int ConfigFilter(eCanBus bus)
{
    sCanBusCtx *ctx = BusOf(bus);
    CAN_FilterTypeDef filter;

    memset(&filter, 0, sizeof(filter));
    filter.FilterIdHigh         = 0x0000u;
    filter.FilterIdLow          = 0x0000u;
    filter.FilterMaskIdHigh     = 0x0000u;
    filter.FilterMaskIdLow      = 0x0000u;
    filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
    filter.FilterBank           = (bus == canBus_1) ? 0u : CAN_FILTER_BANK_SPLIT;
    filter.FilterMode           = CAN_FILTERMODE_IDMASK;
    filter.FilterScale          = CAN_FILTERSCALE_32BIT;
    filter.FilterActivation     = CAN_FILTER_ENABLE;
    filter.SlaveStartFilterBank = CAN_FILTER_BANK_SPLIT;

    return (HAL_CAN_ConfigFilter(ctx->hal, &filter) == HAL_OK) ? 0 : -1;
}

/** Move whatever fits from the software queue into free mailboxes.  Called
 *  with the lock NOT held; takes it around each hand-off. */
static void PumpTx(sCanBusCtx *ctx)
{
    for (;;) {
        CAN_TxHeaderTypeDef header;
        sCanFrame           frame;
        uint32_t            mailbox;
        uint32_t            saved;

        saved = EnterCritical();
        if ((ctx->stats.running == 0u) || (ctx->txq.count == 0u) ||
            (HAL_CAN_GetTxMailboxesFreeLevel(ctx->hal) == 0u)) {
            ExitCritical(saved);
            return;
        }
        frame = ctx->txq.frame[ctx->txq.tail];
        ctx->txq.tail = (uint8_t)((ctx->txq.tail + 1u) % CAN_TX_QUEUE_DEPTH);
        ctx->txq.count--;

        header.StdId              = frame.ext ? 0u : frame.id;
        header.ExtId              = frame.ext ? frame.id : 0u;
        header.IDE                = frame.ext ? CAN_ID_EXT : CAN_ID_STD;
        header.RTR                = frame.rtr ? CAN_RTR_REMOTE : CAN_RTR_DATA;
        header.DLC                = frame.dlc;
        header.TransmitGlobalTime = DISABLE;

        if (HAL_CAN_AddTxMessage(ctx->hal, &header, frame.data,
                                 &mailbox) != HAL_OK) {
            ctx->stats.txDroppedCnt++;
        }
        ctx->stats.txQueueDepth = ctx->txq.count;
        ExitCritical(saved);
    }
}

/** Hand one frame to every subscriber whose filter it satisfies. */
static void Dispatch(const sCanFrame *frame)
{
    for (uint8_t i = 0u; i < CAN_SUBSCRIBERS_MAX; i++) {
        const sCanSub *sub = &s_sub[i];

        if ((sub->used == 0u) || (sub->bus != frame->bus)) {
            continue;
        }
        if ((frame->id & sub->mask) != (sub->id & sub->mask)) {
            continue;
        }
        sub->cb(frame, sub->ctx);
    }
}

static void Tap(const sCanFrame *frame, eCanDir dir)
{
    fCanTap tap = s_tap;

    if (tap != NULL) {
        tap(frame, dir, s_tapCtx);
    }
}

/** Empty one RX FIFO, tapping and dispatching each frame in arrival order. */
static void DrainFifo(CAN_HandleTypeDef *hal, uint32_t fifo)
{
    sCanBusCtx *ctx = CtxOfHandle(hal);
    int         idx = BusIndexOf(hal);

    if ((ctx == NULL) || (idx < 0)) {
        return;
    }

    while (HAL_CAN_GetRxFifoFillLevel(hal, fifo) > 0u) {
        CAN_RxHeaderTypeDef header;
        sCanFrame           frame;

        memset(&frame, 0, sizeof(frame));
        if (HAL_CAN_GetRxMessage(hal, fifo, &header, frame.data) != HAL_OK) {
            return;
        }
        frame.ext      = (header.IDE == CAN_ID_EXT) ? 1u : 0u;
        frame.id       = frame.ext ? header.ExtId : header.StdId;
        frame.rtr      = (header.RTR == CAN_RTR_REMOTE) ? 1u : 0u;
        frame.dlc      = (uint8_t)header.DLC;
        frame.bus      = (uint8_t)idx;
        frame.stamp_ms = HAL_GetTick();

        /* THE MESSAGE IS READ EVEN WHEN THE BUS IS NOT "RUNNING", and that is
         * not tidiness: returning without reading leaves the FIFO-pending
         * interrupt asserted, and an interrupt at priority 5 that re-fires
         * immediately never lets the task that would clear the condition run
         * again.  Drain first, judge afterwards. */
        if (ctx->stats.running == 0u) {
            continue;
        }

        ctx->stats.rxCnt++;

        /* TAP FIRST, ALWAYS.  A frame is counted before anybody decides what
         * it means, so a subscriber that throws it away, or a bridge that
         * refuses to forward it, still leaves evidence it existed. */
        Tap(&frame, canDir_rx);
        Dispatch(&frame);
    }
}

/* Exported functions -------------------------------------------------------*/

int CanBus_Start(eCanBus bus, uint32_t bitrate_bps)
{
    sCanBusCtx *ctx = BusOf(bus);
    uint32_t    prescaler = 0u;
    uint32_t    saved;

    if (ctx == NULL) {
        return -1;
    }
    if (Timing(bitrate_bps, &prescaler) != 0) {
        TRice("err:CAN%u: %u bps is not exact on this clock\n",
              (unsigned)bus + 1u, bitrate_bps);
        return -2;
    }

    (void)CanBus_Stop(bus);

    if (ctx->hal->State != HAL_CAN_STATE_RESET) {
        (void)HAL_CAN_DeInit(ctx->hal);
    }

    ctx->hal->Instance                  = (bus == canBus_1) ? CAN1 : CAN2;
    ctx->hal->Init.Prescaler            = prescaler;
    ctx->hal->Init.Mode                 = CAN_MODE_NORMAL;
    ctx->hal->Init.SyncJumpWidth        = CAN_SJW_1TQ;
    ctx->hal->Init.TimeSeg1             = CAN_BS1_10TQ;
    ctx->hal->Init.TimeSeg2             = CAN_BS2_3TQ;
    ctx->hal->Init.TimeTriggeredMode    = DISABLE;
    ctx->hal->Init.AutoBusOff           = ENABLE;
    ctx->hal->Init.AutoWakeUp           = DISABLE;
    ctx->hal->Init.AutoRetransmission   = ENABLE;
    ctx->hal->Init.ReceiveFifoLocked    = DISABLE;
    /* CHRONOLOGICAL, not by identifier.  A bridge that reorders a device's
     * own frames is not transparent, and identifier priority would do exactly
     * that to a queued burst. */
    ctx->hal->Init.TransmitFifoPriority = ENABLE;

    if (HAL_CAN_Init(ctx->hal) != HAL_OK) {
        TRice("err:CAN%u: init failed\n", (unsigned)bus + 1u);
        return -3;
    }
    if (ConfigFilter(bus) != 0) {
        TRice("err:CAN%u: filter config failed\n", (unsigned)bus + 1u);
        return -4;
    }
    /* Queue and flags first, THEN the wire: the RX interrupt is armed below
     * and a frame can arrive between the two calls. */
    saved = EnterCritical();
    ctx->txq.head           = 0u;
    ctx->txq.tail           = 0u;
    ctx->txq.count          = 0u;
    ctx->stats.txQueueDepth = 0u;
    ctx->stats.bitrate_bps  = bitrate_bps;
    ctx->stats.busOff       = 0u;
    ctx->stats.running      = 1u;
    ExitCritical(saved);

    if (HAL_CAN_Start(ctx->hal) != HAL_OK) {
        ctx->stats.running     = 0u;
        ctx->stats.bitrate_bps = 0u;
        TRice("err:CAN%u: start failed\n", (unsigned)bus + 1u);
        return -5;
    }
    if (HAL_CAN_ActivateNotification(ctx->hal,
            CAN_IT_RX_FIFO0_MSG_PENDING | CAN_IT_RX_FIFO0_OVERRUN |
            CAN_IT_TX_MAILBOX_EMPTY     | CAN_IT_BUSOFF           |
            CAN_IT_ERROR_WARNING        | CAN_IT_ERROR_PASSIVE    |
            CAN_IT_LAST_ERROR_CODE      | CAN_IT_ERROR) != HAL_OK) {
        ctx->stats.running     = 0u;
        ctx->stats.bitrate_bps = 0u;
        (void)HAL_CAN_Stop(ctx->hal);
        TRice("err:CAN%u: notifications failed\n", (unsigned)bus + 1u);
        return -6;
    }

    TRice("CAN%u up: %u bps, prescaler %u, filters open\n",
          (unsigned)bus + 1u, bitrate_bps, prescaler);
    return 0;
}

int CanBus_Stop(eCanBus bus)
{
    sCanBusCtx *ctx = BusOf(bus);
    uint32_t    saved;

    if (ctx == NULL) {
        return -1;
    }
    if (ctx->stats.running == 0u) {
        return 0;
    }

    saved = EnterCritical();
    ctx->stats.running      = 0u;
    ctx->txq.count          = 0u;
    ctx->txq.head           = 0u;
    ctx->txq.tail           = 0u;
    ctx->stats.txQueueDepth = 0u;
    ctx->stats.bitrate_bps  = 0u;
    ExitCritical(saved);

    (void)HAL_CAN_Stop(ctx->hal);
    TRice("CAN%u down\n", (unsigned)bus + 1u);
    return 0;
}

int CanBus_IsRunning(eCanBus bus)
{
    const sCanBusCtx *ctx = BusOf(bus);

    return ((ctx != NULL) && (ctx->stats.running != 0u)) ? 1 : 0;
}

int CanBus_Send(const sCanFrame *frame)
{
    sCanBusCtx *ctx;
    sCanFrame   copy;
    uint32_t    saved;
    int         res = 0;

    if (frame == NULL) {
        return -1;
    }
    ctx = BusOf((eCanBus)frame->bus);
    if (ctx == NULL) {
        return -1;
    }
    if (ctx->stats.running == 0u) {
        return -2;
    }

    copy     = *frame;
    copy.dlc = (frame->dlc > 8u) ? 8u : frame->dlc;

    saved = EnterCritical();
    if (ctx->txq.count >= CAN_TX_QUEUE_DEPTH) {
        ctx->stats.txDroppedCnt++;
        res = -3;
    } else {
        ctx->txq.frame[ctx->txq.head] = copy;
        ctx->txq.head = (uint8_t)((ctx->txq.head + 1u) % CAN_TX_QUEUE_DEPTH);
        ctx->txq.count++;
        if (ctx->txq.count > ctx->txq.peak) {
            ctx->txq.peak = ctx->txq.count;
        }
        ctx->stats.txQueueDepth = ctx->txq.count;
        ctx->stats.txQueuePeak  = ctx->txq.peak;
        ctx->stats.txAcceptedCnt++;
    }
    ExitCritical(saved);

    if (res != 0) {
        return res;
    }

    /* Accepted, so it is counted as traffic even if the wire never confirms
     * it; txDoneCnt is the number that says whether anybody was listening. */
    Tap(&copy, canDir_tx);
    PumpTx(ctx);
    return 0;
}

int CanBus_Subscribe(eCanBus bus, uint32_t id, uint32_t mask,
                     fCanRxHandler cb, void *ctx)
{
    uint32_t saved;
    int      handle = -1;

    if ((cb == NULL) || ((uint32_t)bus >= (uint32_t)canBus_last)) {
        return -1;
    }

    saved = EnterCritical();
    for (uint8_t i = 0u; i < CAN_SUBSCRIBERS_MAX; i++) {
        if (s_sub[i].used == 0u) {
            s_sub[i].cb   = cb;
            s_sub[i].ctx  = ctx;
            s_sub[i].id   = id;
            s_sub[i].mask = mask;
            s_sub[i].bus  = (uint8_t)bus;
            s_sub[i].used = 1u;
            handle = (int)i;
            break;
        }
    }
    ExitCritical(saved);

    if (handle < 0) {
        TRice("err:CAN: subscription table full\n");
    }
    return handle;
}

int CanBus_Unsubscribe(int handle)
{
    uint32_t saved;

    if ((handle < 0) || (handle >= (int)CAN_SUBSCRIBERS_MAX)) {
        return -1;
    }
    saved = EnterCritical();
    s_sub[handle].used = 0u;
    s_sub[handle].cb   = NULL;
    ExitCritical(saved);
    return 0;
}

void CanBus_SetTap(fCanTap tap, void *ctx)
{
    uint32_t saved = EnterCritical();

    s_tapCtx = ctx;
    s_tap    = tap;
    ExitCritical(saved);
}

int CanBus_GetStats(eCanBus bus, sCanBusStats *out)
{
    sCanBusCtx *ctx = BusOf(bus);
    uint32_t    saved;
    uint32_t    esr;

    if ((ctx == NULL) || (out == NULL)) {
        return -1;
    }

    saved = EnterCritical();
    *out = ctx->stats;
    ExitCritical(saved);

    esr = ctx->hal->Instance->ESR;
    out->rxErrorCnt = (uint8_t)((esr & CAN_ESR_REC_Msk) >> CAN_ESR_REC_Pos);
    out->txErrorCnt = (uint8_t)((esr & CAN_ESR_TEC_Msk) >> CAN_ESR_TEC_Pos);
    out->busOff     = ((esr & CAN_ESR_BOFF) != 0u) ? 1u : 0u;
    return 0;
}

void CanBus_ResetStats(void)
{
    uint32_t saved = EnterCritical();

    for (uint8_t b = 0u; b < (uint8_t)canBus_last; b++) {
        sCanBusCtx *ctx = BusOf((eCanBus)b);
        uint32_t    bitrate_bps = ctx->stats.bitrate_bps;
        uint8_t     running     = ctx->stats.running;

        memset(&ctx->stats, 0, sizeof(ctx->stats));
        ctx->txq.peak           = ctx->txq.count;
        ctx->stats.bitrate_bps  = bitrate_bps;
        ctx->stats.running      = running;
        ctx->stats.txQueueDepth = ctx->txq.count;
        ctx->stats.txQueuePeak  = ctx->txq.peak;
    }
    ExitCritical(saved);
}

/* --------------------------------------------------------------------------
 * HAL callbacks — THE single definition of each, for BOTH cells.  This is the
 * whole reason the dispatcher exists (docs/design_battery_pack.md §16.5).
 * -------------------------------------------------------------------------- */

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    DrainFifo(hcan, CAN_RX_FIFO0);
}

void HAL_CAN_RxFifo1MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    DrainFifo(hcan, CAN_RX_FIFO1);
}

void HAL_CAN_TxMailbox0CompleteCallback(CAN_HandleTypeDef *hcan)
{
    sCanBusCtx *ctx = CtxOfHandle(hcan);

    if (ctx != NULL) {
        ctx->stats.txDoneCnt++;
        PumpTx(ctx);
    }
}

void HAL_CAN_TxMailbox1CompleteCallback(CAN_HandleTypeDef *hcan)
{
    HAL_CAN_TxMailbox0CompleteCallback(hcan);
}

void HAL_CAN_TxMailbox2CompleteCallback(CAN_HandleTypeDef *hcan)
{
    HAL_CAN_TxMailbox0CompleteCallback(hcan);
}

void HAL_CAN_TxMailbox0AbortCallback(CAN_HandleTypeDef *hcan)
{
    sCanBusCtx *ctx = CtxOfHandle(hcan);

    if (ctx != NULL) {
        ctx->stats.txDroppedCnt++;
        PumpTx(ctx);
    }
}

void HAL_CAN_TxMailbox1AbortCallback(CAN_HandleTypeDef *hcan)
{
    HAL_CAN_TxMailbox0AbortCallback(hcan);
}

void HAL_CAN_TxMailbox2AbortCallback(CAN_HandleTypeDef *hcan)
{
    HAL_CAN_TxMailbox0AbortCallback(hcan);
}

/**
 * Errors are COUNTED AND SURVIVED, never acted on.  AutoBusOff recovers the
 * cell by itself after 128 idle sequences; a bridge that stopped forwarding
 * because one side is noisy would take the healthy side down with it.
 */
void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan)
{
    sCanBusCtx *ctx = CtxOfHandle(hcan);
    uint32_t    err;

    if (ctx == NULL) {
        return;
    }
    err = HAL_CAN_GetError(hcan);
    ctx->stats.errorCnt++;
    ctx->stats.lastError = err;

    if ((err & HAL_CAN_ERROR_BOF) != 0u) {
        ctx->stats.busOffCnt++;
    }
    if ((err & (HAL_CAN_ERROR_RX_FOV0 | HAL_CAN_ERROR_RX_FOV1)) != 0u) {
        ctx->stats.rxOverrunCnt++;
    }

    /* HAL ORs error bits into ErrorCode and never clears them by itself, so
     * without this every later error IRQ would re-count the first bus-off
     * this bus ever had. */
    (void)HAL_CAN_ResetError(hcan);
}
