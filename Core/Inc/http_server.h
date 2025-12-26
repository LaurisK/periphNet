/**
 ******************************************************************************
 * @file    http_server.h
 * @brief   Minimal HTTP server for PeriphNet Milestone 1
 ******************************************************************************
 * @attention
 *
 * Simple HTTP server using lwIP raw TCP API
 * Serves "Hello World" page with system information
 *
 ******************************************************************************
 */

#ifndef __HTTP_SERVER_H__
#define __HTTP_SERVER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "lwip/err.h"
#include "lwip/tcp.h"

/* HTTP server configuration */
#define HTTP_SERVER_PORT 80

/* Function prototypes */
void http_server_init(void);
void http_server_test_flash(void);

#ifdef __cplusplus
}
#endif

#endif /* __HTTP_SERVER_H__ */
