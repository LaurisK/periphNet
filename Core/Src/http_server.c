/**
 ******************************************************************************
 * @file    http_server.c
 * @brief   Minimal HTTP server implementation for PeriphNet Milestone 1
 ******************************************************************************
 * @attention
 *
 * Simple HTTP server using lwIP raw TCP API
 * Serves "Hello World v1.0.0" page with system information
 *
 ******************************************************************************
 */

#include "http_server.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"
#include "string.h"
#include "stdio.h"
#include "main.h"
#include "w25q128.h"
#include "bl_app_contract.h"
#include "update_manager.h"
#include "FreeRTOS.h"  /* For pvPortMalloc/vPortFree */

/* External network interface (defined in lwip.c) */
extern struct netif gnetif;

/* Flash test result (set by application init) */
static char flash_test_result[256] = "Not tested";
static bool flash_test_ok = false;
static char jedec_info[128] = "Not read";
static char uid_info[128] = "Not read";
static char erase_info[128] = "Not tested";
static char init_info[64] = "Not tested";

/* Bootloader API test result */
static char bl_api_test[128] = "Not tested";

/* HTTP response headers */
static const char http_200_header[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char http_404_header[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/html\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<html><body><h1>404 Not Found</h1></body></html>";

/**
 * @brief  Generate HTML page with system information
 * @param  buf: Buffer to write HTML to
 * @param  buflen: Size of buffer
 * @retval Number of bytes written
 */
static int http_generate_html(char *buf, size_t buflen)
{
    int len = 0;
    uint32_t uptime_sec = HAL_GetTick() / 1000;
    uint32_t uptime_min = uptime_sec / 60;
    uint32_t uptime_hr = uptime_min / 60;

    /* Get IP address */
    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d",
             ip4_addr1(&gnetif.ip_addr),
             ip4_addr2(&gnetif.ip_addr),
             ip4_addr3(&gnetif.ip_addr),
             ip4_addr4(&gnetif.ip_addr));

    /* Get MAC address */
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             gnetif.hwaddr[0], gnetif.hwaddr[1], gnetif.hwaddr[2],
             gnetif.hwaddr[3], gnetif.hwaddr[4], gnetif.hwaddr[5]);

    /* Build HTML page */
    len = snprintf(buf, buflen,
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "    <title>PeriphNet - Hello World</title>\n"
        "    <style>\n"
        "        body { font-family: Arial, sans-serif; margin: 40px; background: #f5f5f5; }\n"
        "        .container { background: white; padding: 30px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); max-width: 600px; }\n"
        "        h1 { color: #2c3e50; border-bottom: 3px solid #3498db; padding-bottom: 10px; }\n"
        "        .info { margin: 20px 0; }\n"
        "        .info-item { margin: 10px 0; padding: 10px; background: #ecf0f1; border-radius: 4px; }\n"
        "        .label { font-weight: bold; color: #34495e; }\n"
        "        .value { color: #16a085; font-family: monospace; }\n"
        "        .footer { margin-top: 30px; padding-top: 20px; border-top: 1px solid #bdc3c7; color: #7f8c8d; font-size: 0.9em; }\n"
        "    </style>\n"
        "</head>\n"
        "<body>\n"
        "    <div class=\"container\">\n"
        "        <h1>Hello World v1.0.0</h1>\n"
        "        <p>PeriphNet - STM32F407VET6 Industrial Firmware</p>\n"
        "        \n"
        "        <div class=\"info\">\n"
        "            <div class=\"info-item\">\n"
        "                <span class=\"label\">Device:</span> \n"
        "                <span class=\"value\">STM32F407VET6</span>\n"
        "            </div>\n"
        "            <div class=\"info-item\">\n"
        "                <span class=\"label\">IP Address:</span> \n"
        "                <span class=\"value\">%s</span>\n"
        "            </div>\n"
        "            <div class=\"info-item\">\n"
        "                <span class=\"label\">MAC Address:</span> \n"
        "                <span class=\"value\">%s</span>\n"
        "            </div>\n"
        "            <div class=\"info-item\">\n"
        "                <span class=\"label\">Uptime:</span> \n"
        "                <span class=\"value\">%luh %lum %lus</span>\n"
        "            </div>\n"
"            <div class=\"info-item\">\n"
         "                <span class=\"label\">Firmware:</span> \n"
         "                <span class=\"value\">v1.0.0 (Milestone 2)</span>\n"
         "            </div>\n"
         "            <div class=\"info-item\">\n"
         "                <span class=\"label\">External Flash:</span> \n"
         "                <span class=\"value\">%s</span>\n"
         "            </div>\n"
        "            <div class=\"info-item\">\n"
        "                <span class=\"label\">Stack:</span> \n"
        "                <span class=\"value\">FreeRTOS + lwIP</span>\n"
        "            </div>\n"
        "        </div>\n"
        "        \n"
        "        <div class=\"footer\">\n"
        "            <p>Milestone 1: Ethernet + HTTP Server - COMPLETE</p>\n"
        "            <p>Next: Bootloader implementation</p>\n"
        "        </div>\n"
        "    </div>\n"
        "</body>\n"
"</html>",
         ip_str, mac_str, uptime_hr, uptime_min % 60, uptime_sec % 60, flash_test_result);

    return len;
}

/**
 * @brief  TCP error callback - handle connection errors
 * @param  arg: User argument (not used)
 * @param  err: Error code
 * @retval None
 */
static void http_err_callback(void *arg, err_t err)
{
    /* Connection aborted - PCB already freed by lwIP, nothing to do */
    (void)arg;
    (void)err;
}

/**
 * @brief  TCP receive callback - handle HTTP request
 * @param  arg: User argument (not used)
 * @param  pcb: TCP protocol control block
 * @param  p: Received packet buffer
 * @param  err: Error status
 * @retval ERR_OK
 */
static err_t http_recv_callback(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    char *request;
    char *response_buf;  /* Dynamically allocated response buffer */
    int html_len;
    err_t ret_err;

    /* Client closed connection */
    if (p == NULL) {
        tcp_close(pcb);
        return ERR_OK;
    }

    /* Allocate response buffer from FreeRTOS heap */
    response_buf = (char *)pvPortMalloc(2048);
    if (response_buf == NULL) {
        /* Out of memory - close connection */
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        tcp_close(pcb);
        return ERR_MEM;
    }

    /* Get request data and check size */
    request = (char *)p->payload;
    if (p->len > 512) {  /* Reject oversized requests */
        vPortFree(response_buf);
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        tcp_close(pcb);
        return ERR_OK;
    }

    /* Simple parsing - check if it's a GET request for root */
    if (strncmp(request, "GET / ", 6) == 0 || strncmp(request, "GET /index", 10) == 0) {
        /* Build HTML with flash test result */
        const char *status_class = flash_test_ok ? "pass" : "fail";
        const char *status_text = flash_test_ok ? "PASS" : "FAIL";

        html_len = snprintf(response_buf, 2048,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "\r\n"
            "<html><body><h1>PeriphNet - Phase 4 - Update Mechanism</h1>"
            "<p>Status: <b>%s</b></p>"
            "<p>Result: %s</p>"
            "<hr><h3>Flash Tests:</h3>"
            "<p>1. Init: %s</p>"
            "<p>2. JEDEC: %s</p>"
            "<p>3. UID: %s</p>"
            "<p>4. Erase: %s</p>"
            "<hr><h3>Bootloader API Test:</h3>"
            "<p>5. BL API: %s</p>"
            "<hr><h3>Phase 4 - Update Test:</h3>"
            "<p><a href='/trigger_update'>Trigger Firmware Update (Test)</a></p>"
            "</body></html>",
            status_text, flash_test_result,
            init_info, jedec_info, uid_info, erase_info, bl_api_test);

        /* Safety check */
        if (html_len >= 2048) {
            html_len = 2047;
        }

        /* Send response */
        ret_err = tcp_write(pcb, response_buf, html_len, TCP_WRITE_FLAG_COPY);
        if (ret_err == ERR_OK) {
            tcp_output(pcb);
            /* For simple HTTP, close after sending - lwIP will queue properly */
            tcp_recved(pcb, p->tot_len);
            pbuf_free(p);
            tcp_close(pcb);
            vPortFree(response_buf);  /* Free AFTER close */
            return ERR_OK;
        } else {
            /* Write failed, clean up and close */
            tcp_recved(pcb, p->tot_len);
            pbuf_free(p);
            tcp_abort(pcb);
            vPortFree(response_buf);  /* Free AFTER abort */
            return ERR_ABRT;
        }
    } else if (strncmp(request, "GET /trigger_update", 19) == 0) {
        /* Phase 4: Trigger firmware update test endpoint */

        /* Request a firmware update (with dummy data for now) */
        uint32_t dummy_size = 245760;      /* 240KB dummy firmware */
        uint32_t dummy_crc = 0x12345678;   /* Dummy CRC */
        uint32_t dummy_version = 0x010001; /* v1.0.1 */

        int update_result = update_status_request(dummy_size, dummy_crc, dummy_version);

        const char *result_msg;
        if (update_result == 0) {
            result_msg = "<p style='color:green;'><b>SUCCESS:</b> Firmware update requested!</p>"
                        "<p>Update status written to external flash.</p>"
                        "<p><b>Next step:</b> Reboot device to trigger bootloader update process.</p>"
                        "<p><a href='/'>Back to Home</a></p>";
        } else {
            result_msg = "<p style='color:red;'><b>FAILED:</b> Could not write update request to external flash.</p>"
                        "<p><a href='/'>Back to Home</a></p>";
        }

        html_len = snprintf(response_buf, 2048,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "\r\n"
            "<html><body>"
            "<h1>PeriphNet - Trigger Firmware Update</h1>"
            "%s"
            "<hr>"
            "<p><b>NOTE:</b> This is a Phase 4 stub - no actual firmware uploaded yet.</p>"
            "<p>Bootloader will see update_requested=1 on next boot and run verification/installation stubs.</p>"
            "</body></html>",
            result_msg);

        /* Send response */
        ret_err = tcp_write(pcb, response_buf, html_len, TCP_WRITE_FLAG_COPY);
        if (ret_err == ERR_OK) {
            tcp_output(pcb);
            tcp_recved(pcb, p->tot_len);
            pbuf_free(p);
            tcp_close(pcb);
            vPortFree(response_buf);  /* Free AFTER close */
            return ERR_OK;
        } else {
            tcp_recved(pcb, p->tot_len);
            pbuf_free(p);
            tcp_abort(pcb);
            vPortFree(response_buf);  /* Free AFTER abort */
            return ERR_ABRT;
        }
    } else {
        /* 404 Not Found */
        ret_err = tcp_write(pcb, http_404_header, strlen(http_404_header), TCP_WRITE_FLAG_COPY);
        if (ret_err == ERR_OK) {
            tcp_output(pcb);
            /* For simple HTTP, close after sending */
            tcp_recved(pcb, p->tot_len);
            pbuf_free(p);
            tcp_close(pcb);
            vPortFree(response_buf);  /* Free AFTER close */
            return ERR_OK;
        } else {
            /* Write failed, clean up and abort */
            tcp_recved(pcb, p->tot_len);
            pbuf_free(p);
            tcp_abort(pcb);
            vPortFree(response_buf);  /* Free AFTER abort */
            return ERR_ABRT;
        }
    }
}

/**
 * @brief  TCP accept callback - new client connected
 * @param  arg: User argument (not used)
 * @param  newpcb: New connection PCB
 * @param  err: Error status
 * @retval ERR_OK
 */
static err_t http_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    if (err != ERR_OK || newpcb == NULL) {
        return ERR_VAL;
    }

    /* Set callbacks */
    tcp_recv(newpcb, http_recv_callback);
    tcp_err(newpcb, http_err_callback);

    return ERR_OK;
}

/**
 * @brief  Initialize HTTP server
 * @param  None
 * @retval None
 */
void http_server_init(void)
{
    struct tcp_pcb *pcb;

    /* Create new TCP PCB */
    pcb = tcp_new();

    if (pcb != NULL) {
        err_t err;

        /* Bind to port 80 */
        err = tcp_bind(pcb, IP_ADDR_ANY, HTTP_SERVER_PORT);

        if (err == ERR_OK) {
            /* Start listening */
            pcb = tcp_listen(pcb);

            /* Set accept callback */
            tcp_accept(pcb, http_accept_callback);
        } else {
            /* Binding failed, deallocate PCB */
            memp_free(MEMP_TCP_PCB, pcb);
        }
    }
}

/**
 * @brief  Test external flash - read pattern written by bootloader
 * @retval None
 */
void http_server_test_flash(void)
{
    W25Q128_ID_t flash_id;
    uint8_t uid_from_flash[12];
    uint8_t uid_current[12];
    HAL_StatusTypeDef spi_status;
    W25Q128_Status_t w25_status;

    /* Reset status strings */
    strcpy(init_info, "Testing...");
    strcpy(jedec_info, "Not tested");
    strcpy(uid_info, "Not tested");
    strcpy(erase_info, "Not tested");
    strcpy(flash_test_result, "In Progress");
    flash_test_ok = false;

    /* Step 0: Initialize W25Q128 flash driver */
    if (W25Q128_Init() != W25Q128_OK) {
        strcpy(init_info, "FAIL - W25Q128_Init() failed");
        strcpy(flash_test_result, "STEP 0 FAILED - Flash initialization failed");
        return;
    }

    /* Step 1a: Check SPI peripheral state */
    extern SPI_HandleTypeDef hspi2;
    if (hspi2.State == HAL_SPI_STATE_RESET) {
        snprintf(init_info, sizeof(init_info), "FAIL - SPI not initialized (state=%d)", hspi2.State);
        strcpy(flash_test_result, "STEP 1a FAILED - SPI peripheral not initialized");
        return;
    }

    /* Step 1b: Test CS pin control */
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_3, GPIO_PIN_SET);
    HAL_Delay(5);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_3, GPIO_PIN_RESET);
    HAL_Delay(5);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_3, GPIO_PIN_SET);

    /* Step 1c: Try wake-up command */
    uint8_t wakeup_cmd = 0xAB;
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_3, GPIO_PIN_RESET);
    spi_status = HAL_SPI_Transmit(&hspi2, &wakeup_cmd, 1, 100);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_3, GPIO_PIN_SET);

    if (spi_status != HAL_OK) {
        snprintf(init_info, sizeof(init_info), "FAIL - Wake-up SPI transmit error (status=%d)", spi_status);
        strcpy(flash_test_result, "STEP 1c FAILED - Wake-up command failed");
        return;
    }
    HAL_Delay(10);

    /* Step 1d: Try reading JEDEC ID manually */
    uint8_t jedec_cmd = 0x9F;
    uint8_t jedec_data[3] = {0};

    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_3, GPIO_PIN_RESET);
    spi_status = HAL_SPI_Transmit(&hspi2, &jedec_cmd, 1, 100);
    if (spi_status != HAL_OK) {
        HAL_GPIO_WritePin(GPIOE, GPIO_PIN_3, GPIO_PIN_SET);
        snprintf(init_info, sizeof(init_info), "FAIL - JEDEC cmd transmit error (status=%d)", spi_status);
        strcpy(flash_test_result, "STEP 1d FAILED - JEDEC command transmit failed");
        return;
    }

    spi_status = HAL_SPI_Receive(&hspi2, jedec_data, 3, 100);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_3, GPIO_PIN_SET);

    if (spi_status != HAL_OK) {
        snprintf(init_info, sizeof(init_info), "FAIL - JEDEC receive error (status=%d)", spi_status);
        strcpy(flash_test_result, "STEP 1d FAILED - JEDEC data receive failed");
        return;
    }

    /* Step 1e: Verify JEDEC data */
    if (jedec_data[0] == 0xFF && jedec_data[1] == 0xFF && jedec_data[2] == 0xFF) {
        snprintf(init_info, sizeof(init_info), "FAIL - All 0xFF (no SPI response)");
        strcpy(flash_test_result, "STEP 1e FAILED - Flash not responding (all 0xFF)");
        return;
    }

    if (jedec_data[0] == 0x00 && jedec_data[1] == 0x00 && jedec_data[2] == 0x00) {
        snprintf(init_info, sizeof(init_info), "FAIL - All 0x00 (SPI bus issue)");
        strcpy(flash_test_result, "STEP 1e FAILED - SPI bus stuck at 0x00");
        return;
    }

    /* Got valid data - store it */
    flash_id.manufacturer_id = jedec_data[0];
    flash_id.memory_type = jedec_data[1];
    flash_id.capacity = jedec_data[2];

    snprintf(init_info, sizeof(init_info), "OK - SPI state=%d", hspi2.State);

    /* Step 2: Verify JEDEC ID (accept both W25Q64 and W25Q128) */
    if (flash_id.manufacturer_id != 0xEF || flash_id.memory_type != 0x40) {
        snprintf(jedec_info, sizeof(jedec_info),
                "WRONG - Manuf=0x%02X Type=0x%02X Cap=0x%02X (Expected: 0xEF/0x40/0x17or0x18)",
                flash_id.manufacturer_id, flash_id.memory_type, flash_id.capacity);
        strcpy(flash_test_result, "STEP 2 FAILED - Wrong flash chip detected");
        return;
    }

    const char *chip_name = (flash_id.capacity == 0x18) ? "W25Q128 (16MB)" :
                            (flash_id.capacity == 0x17) ? "W25Q64 (8MB)" : "Unknown";
    snprintf(jedec_info, sizeof(jedec_info),
            "OK - 0x%02X/0x%02X/0x%02X (Winbond %s)",
            flash_id.manufacturer_id, flash_id.memory_type, flash_id.capacity, chip_name);

    /* Step 3: Read current STM32 UID */
    uid_current[0] = *(uint8_t*)(0x1FFF7A10);
    uid_current[1] = *(uint8_t*)(0x1FFF7A10 + 1);
    uid_current[2] = *(uint8_t*)(0x1FFF7A10 + 2);
    uid_current[3] = *(uint8_t*)(0x1FFF7A10 + 3);
    uid_current[4] = *(uint8_t*)(0x1FFF7A10 + 4);
    uid_current[5] = *(uint8_t*)(0x1FFF7A10 + 5);
    uid_current[6] = *(uint8_t*)(0x1FFF7A10 + 6);
    uid_current[7] = *(uint8_t*)(0x1FFF7A10 + 7);
    uid_current[8] = *(uint8_t*)(0x1FFF7A10 + 8);
    uid_current[9] = *(uint8_t*)(0x1FFF7A10 + 9);
    uid_current[10] = *(uint8_t*)(0x1FFF7A10 + 10);
    uid_current[11] = *(uint8_t*)(0x1FFF7A10 + 11);

    /* Step 4: Try writing and reading back UID to test flash write */
    uint32_t test_addr = EXT_FLASH_FWU_STATUS_ADDR + 256;  /* Use offset 256 to avoid bootloader data */

    /* Erase test sector */
    if (W25Q128_EraseSector(test_addr) != W25Q128_OK) {
        strcpy(uid_info, "FAIL - Cannot erase test sector");
        strcpy(flash_test_result, "STEP 4a FAILED - Erase failed");
        return;
    }

    /* Write UID to flash */
    if (W25Q128_WritePage(test_addr, uid_current, 12) != W25Q128_OK) {
        strcpy(uid_info, "FAIL - Cannot write UID to flash");
        strcpy(flash_test_result, "STEP 4b FAILED - Write failed");
        return;
    }

    /* Read back UID */
    if (W25Q128_Read(test_addr, uid_from_flash, 12) != W25Q128_OK) {
        strcpy(uid_info, "FAIL - Cannot read back UID");
        strcpy(flash_test_result, "STEP 4c FAILED - Read after write failed");
        return;
    }

    /* Verify write/read cycle worked */
    if (memcmp(uid_current, uid_from_flash, 12) != 0) {
        snprintf(uid_info, sizeof(uid_info),
                "WRITE TEST FAIL - Wrote: %02X%02X... Read: %02X%02X...",
                uid_current[0], uid_current[1],
                uid_from_flash[0], uid_from_flash[1]);
        strcpy(flash_test_result, "STEP 4d FAILED - Write/read verify failed");
        return;
    }

    snprintf(uid_info, sizeof(uid_info),
            "OK - Write/Read test passed: %02X%02X%02X%02X...",
            uid_from_flash[0], uid_from_flash[1], uid_from_flash[2], uid_from_flash[3]);

    /* Step 6: Test erase function */
    if (W25Q128_EraseSector(EXT_FLASH_FWU_STATUS_ADDR + 4096) != W25Q128_OK) {
        strcpy(erase_info, "FAILED - Cannot erase sector");
        strcpy(flash_test_result, "STEP 6 FAILED - Cannot erase sector");
        return;
    }

    /* Step 7: Verify sector is actually erased (all 0xFF) */
    uint8_t erased_data[16];
    if (W25Q128_Read(EXT_FLASH_FWU_STATUS_ADDR + 4096, erased_data, 16) != W25Q128_OK) {
        strcpy(erase_info, "FAILED - Cannot verify erase");
        strcpy(flash_test_result, "STEP 7 FAILED - Cannot read back erased data");
        return;
    }

    uint8_t all_ff = 1;
    for (int i = 0; i < 16; i++) {
        if (erased_data[i] != 0xFF) {
            all_ff = 0;
            break;
        }
    }

    if (!all_ff) {
        snprintf(erase_info, sizeof(erase_info),
                "FAILED - Erase verify: %02X %02X %02X %02X... (Expected: FF FF FF FF...)",
                erased_data[0], erased_data[1], erased_data[2], erased_data[3]);
        strcpy(flash_test_result, "STEP 7 FAILED - Erase verification failed");
        return;
    }

    strcpy(erase_info, "OK - Sector erased (verified 0xFF)");

    /* Step 5: Test bootloader API */
    const sBootloaderApi *bl_api = (const sBootloaderApi*)BL_API_TABLE_ADDR;

    if (bl_api->magic != BL_API_MAGIC) {
        snprintf(bl_api_test, sizeof(bl_api_test),
                "FAIL - Wrong magic 0x%08lX (expected 0x%08lX)",
                bl_api->magic, BL_API_MAGIC);
        strcpy(flash_test_result, "STEP 5 FAILED - BL API magic invalid");
        return;
    }

    /* Test get_bootloader_version */
    uint32_t bl_major = 0, bl_minor = 0, bl_patch = 0;
    bl_api->get_bootloader_version(&bl_major, &bl_minor, &bl_patch);

    /* Test calculate_crc32 (stub returns 0xDEADBEEF) */
    uint32_t test_crc = bl_api->calculate_crc32(0x08000000, 1024, false);

    /* Test verify_internal_app (stub returns BL_OK) */
    int verify_result = bl_api->verify_internal_app();

    snprintf(bl_api_test, sizeof(bl_api_test),
            "OK - BL v%lu.%lu.%lu, CRC=0x%08lX, Verify=%d",
            bl_major, bl_minor, bl_patch, test_crc, verify_result);

    strcpy(flash_test_result, "ALL TESTS PASSED");
    flash_test_ok = true;
}
