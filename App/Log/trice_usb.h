/**
 * @file    trice_usb.h
 * @brief   Trice USB CDC transport — sends TCOBS-encoded trice data over USB VCP
 *
 * Usage: call Trice_UsbInit() early (before UDP init). The trice tool receives
 * data with:
 *   trice log -p COMx -args "/dev/ttyACMx" -i ./til.json -li ./li.json
 */

#ifndef TRICE_USB_H_
#define TRICE_USB_H_

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Initialize trice USB CDC transport
 *
 * Registers USB CDC as the trice auxiliary output. If called before
 * Trice_UdpInit(), UDP init will chain itself into the auxiliary callback.
 */
void Trice_UsbInit(void);

/**
 * @brief Send trice data over USB CDC
 *
 * Non-blocking — drops data if CDC TX is busy or host not connected.
 */
void Trice_UsbWrite(const uint8_t *data, size_t len);

#endif /* TRICE_USB_H_ */
