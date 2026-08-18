/**
 * @file    app_freertos.c
 * @brief   Application FreeRTOS tasks
 *
 * Tasks created here:
 *  - defaultTask  (512 words, osPriorityNormal)
 *      – Initialises lwIP
 *      – Logs reset cause on first run
 *      – Heartbeat (1 s) via Trice
 *      – Feeds watchdog every 100 ms
 *      – Polls three buttons to trigger deliberate faults (debug/test)
 *
 *  - triceTask  (256 words, osPriorityNormal+1)
 *      – Calls TriceTransfer() every 10 ms when USART3 TX DMA is free
 */

#include "App/app_freertos.h"
#include "App/system.h"
#include "App/Cmd/cmd_parser.h"
#include "App/Http/http_server.h"
#include "App/Modbus/modbus.h"
#include "App/Rs485/rs485_port.h"
#include "App/Test/modbus_test_port.h"
#include "App/Fwu/fwu_control.h"
#include "App/Log/trice_udp.h"
#include "App/Log/trice_usb.h"
#include "App/Mon/sysmon.h"
#include "App/Net/wg_link.h"
#include "App/Net/wg_platform.h"
#include "App/Net/wg_time.h"
#include "boot_status.h"
#include "cmsis_os.h"
#include "main.h"
#include "usart.h"
#include "w25q128.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/dhcp.h"
#include "trice.h"

/* --------------------------------------------------------------------------
 * Task attributes
 * -------------------------------------------------------------------------- */

static const osThreadAttr_t s_triceAttr = {
    .name       = "trice",
    .stack_size = 256U * 4U,
    .priority   = (osPriority_t)(osPriorityNormal + 1),
};

/* --------------------------------------------------------------------------
 * triceTask
 * -------------------------------------------------------------------------- */

static void triceTask(void *arg)
{
    (void)arg;

    int8_t monId = SysMon_TaskRegister(256U, 1000U);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10U));
        SysMon_TaskCheckin(monId);
        if (MX_USART3_Ready()) {
            TriceTransfer();
        }
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void App_FreertosInit(void)
{
    /* Before any task exists: SysMon_TaskRegister() is a no-op until this
     * runs, and a task only registers once, from inside its own body. */
    SysMon_Init();

    osThreadNew(triceTask, NULL, &s_triceAttr);
}

/* --------------------------------------------------------------------------
 * Default task body (called from CubeMX's StartDefaultTask after MX_LWIP_Init)
 * -------------------------------------------------------------------------- */

/* Defined in gpio.c USER CODE */
extern void App_GPIO_InitButtons(void);

void App_DefaultTaskEntry(void)
{
    /* Reconfigure buttons as polled input with pull-up (active-low) */
    App_GPIO_InitButtons();

    /* LED1 boot indicator — rapid blink for ~5 s */
    uint32_t ledFastCtr = 50U;

    /* Start trice USB CDC output (available immediately, no network needed) */
    Trice_UsbInit();

    /* Start command parser (USB CDC + UART1 input) */
    Cmd_Init();

    /* One-time startup logging */
    System_Init();
    System_LogResetCause();

    TRice("PeriphNet started. Heap=%u\n", xPortGetFreeHeapSize());

    /* External flash init + read JEDEC ID */
    if (W25Q128_Init() == w25q_ok) {
        W25Q128_ID_t id;
        W25Q128_ReadID(&id);
        TRice("Flash OK: mfr=0x%02X type=0x%02X cap=0x%02X\n",
              id.manufacturer_id, id.memory_type, id.capacity);

        /* NOTE: the app deliberately does NOT self-confirm.  An outside
         * actor must POST /api/fwu/confirm after checking the device
         * is healthy; otherwise the bootloader rolls back to the golden
         * image after BOOT_ATTEMPTS_MAX unconfirmed boots. */
        if (BootStatus_IsUnconfirmed()) {
            TRice("Boot UNCONFIRMED: awaiting /api/fwu/confirm\n");
        }
    } else {
        TRice("Flash INIT FAILED\n");
    }

    /* Monotonic time base for the WireGuard handshake timestamp.  Must run
     * after flash init and before the tunnel starts — the hub rejects a
     * timestamp that is not newer than the last one it saw from us, so this
     * is what makes the tunnel survive a reboot. */
    if (WgTime_Init() == 0) {
        TRice("WG time base: %u s (flash-backed)\n", WgTime_Now());
    } else {
        TRice("WG time base: %u s, NO FLASH BACKING (tunnel may need the "
              "hub's peer state cleared after a reset)\n", WgTime_Now());
    }

    /* Wait for DHCP lease (poll gnetif, up to 30 s) */
    extern struct netif gnetif;
    TRice("Waiting for link + DHCP...\n");
    for (uint32_t t = 0; t < 300U; t++) {
        vTaskDelay(pdMS_TO_TICKS(100U));
        KickIwdg();

        /* Rapid LED blink during boot */
        if (ledFastCtr > 0U) {
            ledFastCtr--;
            HAL_GPIO_TogglePin(gpio_led1_GPIO_Port, gpio_led1_Pin);
        }

        if ((t % 50U) == 49U) {
            TRice("  t=%us flags=0x%02X link=%u ip=0x%08X\n",
                  (t + 1U) / 10U, gnetif.flags,
                  netif_is_link_up(&gnetif), gnetif.ip_addr.addr);
        }
        if (gnetif.ip_addr.addr != 0U) {
            uint32_t ip = gnetif.ip_addr.addr;
            TRice("DHCP OK: %d.%d.%d.%d\n",
                  (ip >>  0) & 0xFF, (ip >>  8) & 0xFF,
                  (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
            break;
        }
    }
    if (gnetif.ip_addr.addr == 0U) {
        TRice("DHCP timeout – no IP (flags=0x%02X)\n", gnetif.flags);
    }

    /* Register the Modbus peripherals.  The module knows nothing about any of
     * them: a driver claims a port slot, and a device names the port it lives
     * on — which is config (docs/modbus.md §5.1, §3.4).  Registration is
     * independent of Modbus_Init and may follow it; a device on a slot with no
     * driver is simply not polled.
     *
     * The test peripheral is registered unconditionally because whether a
     * board HAS one is a CONFIGURATION question, not a build question: one
     * image serves a bench board and a real one. */
    (void)MbRtu_Register();
    (void)ModbusTestPort_Register();

    /* Set it and forget it (docs/modbus.md §4.2): the store comes up, invalid
     * regions are erased, and the engine runs.  Timers are NOT started here —
     * they come and go with subscriptions — so a board that boots with a valid
     * config and no subscribers puts nothing on the wire.  It needs
     * W25Q128_Init, which ran above. */
    if (Modbus_Init() != 0) {
        TRice("Modbus: init failed\n");
    }

    /* Hand the Ethernet netif back to the ETH DMA's checksum offload.
     *
     * lwipopts.h now compiles software checksum generation IN, because the
     * WireGuard netif has no hardware behind it and was emitting inner packets
     * with garbage checksums — every peer's ip_rcv() dropped them before the
     * FORWARD chain, so the tunnel handshook perfectly and carried no data.
     * netif_add() gives every netif NETIF_CHECKSUM_ENABLE_ALL, which is what
     * the WireGuard netif wants; here we clear the flags on the ETH netif so it
     * behaves exactly as before and the DMA keeps doing the work. */
    LOCK_TCPIP_CORE();
    if (netif_default != NULL) {
        NETIF_SET_CHECKSUM_CTRL(netif_default, 0);
    }
    UNLOCK_TCPIP_CORE();

    /* Start HTTP server (upload/download/status) */
    http_server_init();
    TRice("HTTP server started on port 80\n");

    /* Start trice UDP broadcast (port 17001) */
    Trice_UdpInit();
    TRice("Trice UDP started on port %u\n", TRICE_UDP_PORT);

    /* WireGuard tunnel to the hub.  Started here rather than in MX_LWIP_Init()
     * so WgTime_Init() has already run.  A missing link or an unreachable hub
     * is not an error: the handshake retries on lwIP timers and nothing else
     * on the board depends on it. */
    {
        int wgRc = WgLink_Start(NULL);
        int hwSeeded = 0;
        WgPlatform_GetRngStatus(&hwSeeded, NULL);
        if (wgRc != 0) {
            TRice("WG: start failed (%d)\n", wgRc);
        }
        if (!hwSeeded) {
            /* Only meaningful once something has drawn randomness; the
             * handshake above does. */
            TRice("WG: WARNING entropy not fully from hardware RNG\n");
        }
    }

    uint32_t btnDebounce[3]  = {0U, 0U, 0U};
    uint32_t heartbeatTick   = 0U;
    uint32_t ledPulseCounter = 0U;
    uint32_t wgTick          = 0U;
    int      wgWasUp         = -1;

    /* The loop runs at 100 ms, but golden promotion and the pre-reboot flush
     * both hold it for seconds while kicking the IWDG themselves — hence a
     * deadline well clear of those rather than of the loop period. */
    int8_t monId = SysMon_TaskRegister(1024U, 10000U);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100U));

        /* Feed watchdog every 100 ms */
        KickIwdg();

        /* Housekeeping for the system monitor.  It samples once a second and
         * returns immediately in between.  It runs HERE, in the same task
         * that kicks the watchdog, so the one task it cannot report on is the
         * one the IWDG and TIM14 already cover. */
        SysMon_TaskCheckin(monId);
        SysMon_Poll();

        /* ---- LED1 status indication ---- */
        if (ledFastCtr > 0U) {
            ledFastCtr--;
            HAL_GPIO_TogglePin(gpio_led1_GPIO_Port, gpio_led1_Pin);
        } else {
            ledPulseCounter = (ledPulseCounter + 1U) % 14U;
            if (ledPulseCounter == 0U) {
                HAL_GPIO_WritePin(gpio_led1_GPIO_Port, gpio_led1_Pin,
                                  GPIO_PIN_SET);
            } else if (ledPulseCounter == 2U) {
                HAL_GPIO_WritePin(gpio_led1_GPIO_Port, gpio_led1_Pin,
                                  GPIO_PIN_RESET);
            }
        }

        /* Reboot for firmware install if requested via HTTP */
        if (FwuCtl_RebootPending()) {
            TRice("Rebooting for firmware install...\n");
            vTaskDelay(pdMS_TO_TICKS(2000U));  /* let Trice flush + TCP close */
            NVIC_SystemReset();
        }

        /* Copy staged blob → golden after a confirm (a few seconds of
         * SPI traffic; runs here so tcpip_thread stays responsive) */
        if (FwuCtl_PromotePending()) {
            TRice("Promoting stored blob to golden...\n");
            FwuCtl_RunPromotion();
            TRice("Golden promotion done\n");
        }

        /* ---- WireGuard housekeeping (every 5 s) ----
         * Persisting the time base touches SPI flash, so it runs here in a
         * task, never in lwIP context.  WgTime_Tick() itself rate-limits to
         * one write per WG_TIME_PERSIST_S. */
        wgTick++;
        if (wgTick >= 50U) {
            wgTick = 0U;
            WgTime_Tick();

            if (WgLink_IsRunning()) {
                int up = WgLink_IsUp();
                if (up != wgWasUp) {
                    wgWasUp = up;
                    if (up) {
                        TRice("WG: tunnel UP (session established)\n");
                    } else {
                        TRice("WG: tunnel DOWN (handshaking)\n");
                    }
                }
            }
        }

        /* Heartbeat every 1 s (10 × 100 ms) */
        heartbeatTick++;
        if (heartbeatTick >= 10U) {
            heartbeatTick = 0U;
            TRice("Heartbeat: tick=%u heap=%u\n",
                  HAL_GetTick(), xPortGetFreeHeapSize());
        }

        /* ---- Button-triggered deliberate faults (debug/test) ---- */

        /* BTN1: HardFault via null-pointer write */
        if (HAL_GPIO_ReadPin(gpio_button1_GPIO_Port, gpio_button1_Pin)
                == GPIO_PIN_RESET) {
            if (btnDebounce[0]++ == 0U) {
                TRice("BTN1: triggering HardFault (null write)\n");
                *(volatile uint32_t *)0x00000000U = 0xDEADBEEFU;
            }
        } else {
            btnDebounce[0] = 0U;
        }

        /* BTN2: UsageFault via integer divide-by-zero
           (requires SCB->CCR |= SCB_CCR_DIV_0_TRP_Msk, set in main) */
        if (HAL_GPIO_ReadPin(gpio_button2_GPIO_Port, gpio_button2_Pin)
                == GPIO_PIN_RESET) {
            if (btnDebounce[1]++ == 0U) {
                TRice("BTN2: triggering UsageFault (div-by-zero)\n");
                volatile int z = 0;
                volatile int r = 1 / z;
                (void)r;
            }
        } else {
            btnDebounce[1] = 0U;
        }

        /* BTN3: BusFault via invalid SRAM write (beyond 128 KB boundary) */
        if (HAL_GPIO_ReadPin(gpio_button3_GPIO_Port, gpio_button3_Pin)
                == GPIO_PIN_RESET) {
            if (btnDebounce[2]++ == 0U) {
                TRice("BTN3: triggering BusFault (invalid SRAM)\n");
                *(volatile uint32_t *)0x20100000U = 0xDEADBEEFU;
            }
        } else {
            btnDebounce[2] = 0U;
        }
    }
}
