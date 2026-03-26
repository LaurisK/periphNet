/**
 * @file    trice_usb.c
 * @brief   Trice USB CDC transport — sends TCOBS-encoded trice data over VCP
 */

#include "App/Log/trice_usb.h"
#include "trice.h"
#include "usbd_cdc_if.h"

void Trice_UsbWrite(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }
    CDC_Transmit_FS((uint8_t *)data, (uint16_t)len);
}

void Trice_UsbInit(void)
{
    extern Write8AuxiliaryFn_t UserNonBlockingDeferredWrite8AuxiliaryFn;
    UserNonBlockingDeferredWrite8AuxiliaryFn = Trice_UsbWrite;
}
