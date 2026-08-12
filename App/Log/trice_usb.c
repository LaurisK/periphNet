/**
 * @file    trice_usb.c
 * @brief   Trice USB CDC transport — sends TCOBS-encoded trice data over VCP
 *
 * ## Why this guards on the device state
 *
 * CDC_Transmit_FS() refuses with USBD_BUSY whenever the CDC handle's TxState
 * is non-zero, and TxState is cleared only by the IN-transfer completion
 * callback.  Submitting a packet while no host is listening therefore latches
 * TxState at 1 permanently: the transfer never completes, so every later write
 * is dropped and Trice goes silent over USB for the rest of the boot.
 *
 * That is exactly what happened here.  Trice_UsbInit() runs at the top of
 * App_DefaultTaskEntry, well before USB enumeration finishes, so the first
 * deferred flush hit an unconfigured device and latched the endpoint.  The
 * symptom was maximally confusing: the CLI kept working (that is the OUT
 * endpoint) while the board returned zero bytes on the IN endpoint, which
 * reads as "nothing is being logged" rather than "the sink is jammed".
 */

#include "App/Log/trice_usb.h"
#include "trice.h"
#include "usbd_cdc_if.h"
#include "usbd_cdc.h"

extern USBD_HandleTypeDef hUsbDeviceFS;

/* Tracks the unconfigured → configured edge, so a transfer left pending by a
 * previous host session can be discarded on re-enumeration. */
static uint8_t s_wasConfigured;

void Trice_UsbWrite(const uint8_t *data, size_t len)
{
    if (len == 0u) {
        return;
    }

    if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED) {
        /* No host: dropping the line is correct.  Handing it to the stack
         * would jam the endpoint for good. */
        s_wasConfigured = 0u;
        return;
    }

    if (!s_wasConfigured) {
        /* Fresh enumeration.  Anything still marked in-flight belongs to a
         * host that has since gone away and will never complete, so clear it
         * rather than inheriting a permanently busy endpoint. */
        USBD_CDC_HandleTypeDef *hcdc =
            (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
        if (hcdc != NULL) {
            hcdc->TxState = 0u;
        }
        s_wasConfigured = 1u;
    }

    /* Still ignored on purpose: a USBD_BUSY here means the host is simply
     * slower than the log stream, and a dropped log line must never stall
     * the caller. */
    (void)CDC_Transmit_FS((uint8_t *)data, (uint16_t)len);
}

void Trice_UsbInit(void)
{
    extern Write8AuxiliaryFn_t UserNonBlockingDeferredWrite8AuxiliaryFn;
    UserNonBlockingDeferredWrite8AuxiliaryFn = Trice_UsbWrite;
}
