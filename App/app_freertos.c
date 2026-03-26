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
#include "App/Http/http_server.h"
#include "App/Http/image_transfer.h"
#include "App/Log/trice_udp.h"
#include "App/Log/trice_usb.h"
#include "boot_status.h"
#include "cmsis_os.h"
#include "main.h"
#include "usart.h"
#include "w25q128.h"
#include "lwip/netif.h"
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

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10U));
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

    /* Start trice USB CDC output (available immediately, no network needed) */
    Trice_UsbInit();

    /* One-time startup logging */
    System_Init();
    System_LogResetCause();

    TRice("PeriphNet started. Heap=%u\n", xPortGetFreeHeapSize());

    /* External flash init + read JEDEC ID */
    if (W25Q128_Init() == W25Q128_OK) {
        W25Q128_ID_t id;
        W25Q128_ReadID(&id);
        TRice("Flash OK: mfr=0x%02X type=0x%02X cap=0x%02X\n",
              id.manufacturer_id, id.memory_type, id.capacity);

        /* Confirm boot to bootloader (clears confirmed flag in boot status).
         * Done early, before HTTP server starts, to avoid SPI bus contention. */
        if (BootStatus_ConfirmApp() == 0) {
            TRice("Boot confirmed\n");
        } else {
            TRice("Boot confirm FAILED (ext flash write)\n");
        }
    } else {
        TRice("Flash INIT FAILED\n");
    }

    /* Wait for DHCP lease (poll gnetif, up to 30 s) */
    extern struct netif gnetif;
    TRice("Waiting for link + DHCP...\n");
    for (uint32_t t = 0; t < 300U; t++) {
        vTaskDelay(pdMS_TO_TICKS(100U));
        KickIwdg();
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

    /* Start HTTP server (upload/download/status) */
    http_server_init();
    TRice("HTTP server started on port 80\n");

    /* Start trice UDP broadcast (port 17001) */
    Trice_UdpInit();
    TRice("Trice UDP started on port %u\n", TRICE_UDP_PORT);

    uint32_t btnDebounce[3] = {0U, 0U, 0U};
    uint32_t heartbeatTick  = 0U;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100U));

        /* Feed watchdog every 100 ms */
        KickIwdg();

        /* Reboot for firmware install if requested via HTTP */
        if (image_transfer_reboot_pending()) {
            TRice("Rebooting for firmware install...\n");
            vTaskDelay(pdMS_TO_TICKS(2000U));  /* let Trice flush + TCP close */
            NVIC_SystemReset();
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
