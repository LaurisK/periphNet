/**
 * @file    modbus_trice_sink.c
 * @brief   Trice subscriber — see modbus_trice_sink.h.
 *
 * A consumer, not engine code: it reaches the module only through modbus.h,
 * exactly as App/Mqtt and App/Can do.  It lives in App/Modbus/ because it is
 * a diagnostic of this module and nothing else uses it.
 */

#include "App/Modbus/modbus_trice_sink.h"
#include "App/Modbus/modbus.h"

#include "modbus_decode.h"

#include "trice.h"

#include <stdio.h>
#include <string.h>

static int s_handle = -1;

/* Runs in the modbus task, synchronously, and must not block (§4.7).  One
 * Trice line per event is well inside that: no allocation, no copy-and-post. */
static void trice_sink_cb(const sModbusEvent *ev, void *ctx)
{
    (void)ctx;
    char buf[110];

    switch (ev->type) {
    case mbEvt_sample: {
        const sModbusPointDesc *pt = ev->u.sample.pt;
        char                    val[52];

        if (ev->u.sample.text != NULL) {
            snprintf(val, sizeof(val), "%s", ev->u.sample.text);
        } else if (pt->decodeType == mbDecode_bitfield) {
            snprintf(val, sizeof(val), "%u",
                     (unsigned)(uint16_t)ev->u.sample.value);
        } else {
            MbFormat_Scaled(val, sizeof(val), ev->u.sample.value,
                            pt->scalePow10);
        }

        snprintf(buf, sizeof(buf), "%s/%s = %s [dev %u pt %u @%us]",
                 pt->topicPrefix, pt->name, val,
                 pt->devOrd, pt->ptOrd, (unsigned)pt->period_sec);
        TRiceS("Modbus dump: %s\n", buf);
        break;
    }

    case mbEvt_pointDesc: {
        const sModbusPointDesc *pt = ev->u.desc.pt;

        if (pt == NULL) {
            TRice("Modbus catalogue: empty\n");
            break;
        }
        snprintf(buf, sizeof(buf), "%s/%s %s@%us%s",
                 pt->topicPrefix, pt->name,
                 (pt->flags & MB_PT_WRITE) ? "rw " : "r ",
                 (unsigned)pt->period_sec,
                 ev->u.desc.last ? " (last)" : "");
        TRiceS("Modbus catalogue: %s\n", buf);
        break;
    }

    case mbEvt_txn:
        /* A successful read is already visible as its samples; only the
         * outcomes that produced none are worth a line. */
        if (ev->u.txn.err != mbErr_ok) {
            TRice("Modbus dump: dev %u slave %u addr %u regs %u failed (%d)\n",
                  ev->u.txn.devOrd, ev->u.txn.slaveAddr, ev->u.txn.addr,
                  ev->u.txn.regs, (int)ev->u.txn.err);
        }
        break;

    case mbEvt_config:
        TRice("Modbus dump: config live, region %u, %u devices %u points\n",
              ev->u.config.activeRegion, ev->u.config.counts.devices,
              ev->u.config.counts.points);
        break;

    case mbEvt_released:
        TRice("Modbus dump: off\n");
        break;

    default:
        break;
    }
}

void ModbusTriceSink_Set(int enable)
{
    if (enable) {
        if (s_handle >= 0) {
            return;
        }
        s_handle = Modbus_Subscribe(MB_PLAN_ALL, mbEvt_all,
                                    trice_sink_cb, NULL);
        if (s_handle < 0) {
            TRice("Modbus dump: subscribe failed (%d)\n", s_handle);
            return;
        }
        TRice("Modbus dump: on\n");
        return;
    }

    if (s_handle < 0) {
        return;
    }
    /* The "off" line comes from the mbEvt_released callback, which IS the
     * release point — so the log says off exactly when the module has stopped
     * calling, not when the request was made. */
    Modbus_Unsubscribe(s_handle);
    s_handle = -1;
}

int ModbusTriceSink_Get(void)
{
    return (s_handle >= 0);
}
