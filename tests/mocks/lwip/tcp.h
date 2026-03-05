/**
 * Mock lwIP tcp.h for host-native unit tests.
 * Provides minimal type definitions and function declarations.
 */
#ifndef MOCK_LWIP_TCP_H
#define MOCK_LWIP_TCP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* lwIP basic types */
typedef uint8_t  u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;
typedef int8_t   s8_t;
typedef int16_t  s16_t;
typedef int32_t  s32_t;
typedef s8_t     err_t;

/* Error codes */
#define ERR_OK     0
#define ERR_MEM   -1
#define ERR_BUF   -2
#define ERR_TIMEOUT -3
#define ERR_RTE   -4
#define ERR_INPROGRESS -5
#define ERR_VAL   -6
#define ERR_WOULDBLOCK -7
#define ERR_USE   -8
#define ERR_ALREADY -9
#define ERR_ISCONN -10
#define ERR_CONN  -11
#define ERR_IF    -12
#define ERR_ABRT  -13
#define ERR_RST   -14
#define ERR_CLSD  -15
#define ERR_ARG   -16

/* TCP write flags */
#define TCP_WRITE_FLAG_COPY  0x01
#define TCP_WRITE_FLAG_MORE  0x02

/* Packet buffer */
struct pbuf {
    struct pbuf *next;
    void   *payload;
    u16_t   tot_len;
    u16_t   len;
    u8_t    type_internal;
    u8_t    ref;
};

/* Forward declaration for callbacks */
struct tcp_pcb;

typedef err_t (*tcp_recv_fn)(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
typedef err_t (*tcp_sent_fn)(void *arg, struct tcp_pcb *pcb, u16_t len);
typedef void  (*tcp_err_fn)(void *arg, err_t err);
typedef err_t (*tcp_accept_fn)(void *arg, struct tcp_pcb *newpcb, err_t err);

/* TCP PCB - minimal mock */
struct tcp_pcb {
    void       *callback_arg;
    tcp_sent_fn sent;
    u16_t       snd_buf;
};

/* tcp_sndbuf macro */
#define tcp_sndbuf(pcb) ((pcb)->snd_buf)

/* TCP API declarations (implemented in mock_support.c) */
err_t tcp_write(struct tcp_pcb *pcb, const void *data, u16_t len, u8_t flags);
void  tcp_recved(struct tcp_pcb *pcb, u16_t len);
err_t tcp_close(struct tcp_pcb *pcb);
void  tcp_abort(struct tcp_pcb *pcb);
err_t tcp_output(struct tcp_pcb *pcb);
void  tcp_arg(struct tcp_pcb *pcb, void *arg);
void  tcp_sent(struct tcp_pcb *pcb, tcp_sent_fn sent);

/* pbuf mock */
void  pbuf_free(struct pbuf *p);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_LWIP_TCP_H */
