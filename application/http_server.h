#ifndef __HTTP_SERVER_H__
#define __HTTP_SERVER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "lwip/err.h"
#include "lwip/tcp.h"

#define HTTP_SERVER_PORT 80

void http_server_init(void);
void http_server_test_flash(void);

#ifdef __cplusplus
}
#endif

#endif /* __HTTP_SERVER_H__ */
