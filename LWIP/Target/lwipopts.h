/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : Target/lwipopts.h
  * Description        : This file overrides LwIP stack default configuration
  *                      done in opt.h file.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion --------------------------------------*/
#ifndef __LWIPOPTS__H__
#define __LWIPOPTS__H__

#include "main.h"

/*-----------------------------------------------------------------------------*/
/* Current version of LwIP supported by CubeMx: 2.1.2 -*/
/*-----------------------------------------------------------------------------*/

/* Within 'USER CODE' section, code will be kept by default at each generation */
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

#ifdef __cplusplus
 extern "C" {
#endif

/* STM32CubeMX Specific Parameters (not defined in opt.h) ---------------------*/
/* Parameters set in STM32CubeMX LwIP Configuration GUI -*/
/*----- WITH_RTOS enabled (Since FREERTOS is set) -----*/
#define WITH_RTOS 1
/* Temporary workaround to avoid conflict on errno defined in STM32CubeIDE and lwip sys_arch.c errno */
#undef LWIP_PROVIDE_ERRNO
/*----- CHECKSUM_BY_HARDWARE disabled -----*/
#define CHECKSUM_BY_HARDWARE 0
/*-----------------------------------------------------------------------------*/

/* LwIP Stack Parameters (modified compared to initialization value in opt.h) -*/
/* Parameters set in STM32CubeMX LwIP Configuration GUI -*/
/*----- Value in opt.h for LWIP_DHCP: 0 -----*/
#define LWIP_DHCP 1
/*----- Value in opt.h for MEM_ALIGNMENT: 1 -----*/
#define MEM_ALIGNMENT 4
/*----- Default Value for MEM_SIZE: 1600 ---*/
#define MEM_SIZE 8192
/*----- Value in opt.h for MEMP_NUM_SYS_TIMEOUT: (LWIP_TCP + IP_REASSEMBLY + LWIP_ARP + (2*LWIP_DHCP) + LWIP_AUTOIP + LWIP_IGMP + LWIP_DNS + (PPP_SUPPORT*6*MEMP_NUM_PPP_PCB) + (LWIP_IPV6 ? (1 + LWIP_IPV6_REASS + LWIP_IPV6_MLD) : 0)) -*/
#define MEMP_NUM_SYS_TIMEOUT 5
/*----- Value in opt.h for LWIP_ETHERNET: LWIP_ARP || PPPOE_SUPPORT -*/
#define LWIP_ETHERNET 1
/*----- Value in opt.h for LWIP_DNS_SECURE: (LWIP_DNS_SECURE_RAND_XID | LWIP_DNS_SECURE_NO_MULTIPLE_OUTSTANDING | LWIP_DNS_SECURE_RAND_SRC_PORT) -*/
#define LWIP_DNS_SECURE 7
/*----- Value in opt.h for TCP_SND_QUEUELEN: (4*TCP_SND_BUF + (TCP_MSS - 1))/TCP_MSS -----*/
#define TCP_SND_QUEUELEN 9
/*----- Value in opt.h for TCP_SNDLOWAT: LWIP_MIN(LWIP_MAX(((TCP_SND_BUF)/2), (2 * TCP_MSS) + 1), (TCP_SND_BUF) - 1) -*/
#define TCP_SNDLOWAT 1071
/*----- Value in opt.h for TCP_SNDQUEUELOWAT: LWIP_MAX(TCP_SND_QUEUELEN)/2, 5) -*/
#define TCP_SNDQUEUELOWAT 5
/*----- Value in opt.h for TCP_WND_UPDATE_THRESHOLD: LWIP_MIN(TCP_WND/4, TCP_MSS*4) -----*/
#define TCP_WND_UPDATE_THRESHOLD 536
/*----- Value in opt.h for LWIP_NETIF_LINK_CALLBACK: 0 -----*/
#define LWIP_NETIF_LINK_CALLBACK 1
/*----- Value in opt.h for TCPIP_THREAD_STACKSIZE: 0 -----*/
#define TCPIP_THREAD_STACKSIZE 6144
/*----- Value in opt.h for TCPIP_THREAD_PRIO: 1 -----*/
#define TCPIP_THREAD_PRIO 24
/*----- Value in opt.h for TCPIP_MBOX_SIZE: 0 -----*/
#define TCPIP_MBOX_SIZE 16
/*----- Value in opt.h for SLIPIF_THREAD_STACKSIZE: 0 -----*/
#define SLIPIF_THREAD_STACKSIZE 1024
/*----- Value in opt.h for SLIPIF_THREAD_PRIO: 1 -----*/
#define SLIPIF_THREAD_PRIO 3
/*----- Value in opt.h for DEFAULT_THREAD_STACKSIZE: 0 -----*/
#define DEFAULT_THREAD_STACKSIZE 1024
/*----- Value in opt.h for DEFAULT_THREAD_PRIO: 1 -----*/
#define DEFAULT_THREAD_PRIO 3
/*----- Value in opt.h for DEFAULT_UDP_RECVMBOX_SIZE: 0 -----*/
#define DEFAULT_UDP_RECVMBOX_SIZE 6
/*----- Value in opt.h for DEFAULT_TCP_RECVMBOX_SIZE: 0 -----*/
#define DEFAULT_TCP_RECVMBOX_SIZE 6
/*----- Value in opt.h for DEFAULT_ACCEPTMBOX_SIZE: 0 -----*/
#define DEFAULT_ACCEPTMBOX_SIZE 6
/*----- Value in opt.h for RECV_BUFSIZE_DEFAULT: INT_MAX -----*/
#define RECV_BUFSIZE_DEFAULT 2000000000
/*----- Default Value for LWIP_DISABLE_TCP_SANITY_CHECKS: 0 ---*/
#define LWIP_DISABLE_TCP_SANITY_CHECKS 1
/*----- Default Value for LWIP_DISABLE_MEMP_SANITY_CHECKS: 0 ---*/
#define LWIP_DISABLE_MEMP_SANITY_CHECKS 1
/*----- Value in opt.h for LWIP_STATS: 1 -----*/
#define LWIP_STATS 0
/*----- Default Value for LWIP_CHECKSUM_CTRL_PER_NETIF: 0 ---*/
#define LWIP_CHECKSUM_CTRL_PER_NETIF 1
/*-----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */

/* Checksums: software generation is COMPILED IN, then switched off per netif.
 *
 * The CubeMX default is CHECKSUM_GEN_* = 0, because the STM32 ETH DMA inserts
 * IP/UDP/TCP/ICMP checksums in hardware.  That is true only for frames the ETH
 * peripheral actually emits.  The WireGuard netif is not the ETH peripheral:
 * its packets are checksummed by nobody, encrypted, and carried as the payload
 * of an outer UDP datagram — the hardware checksums that outer datagram and
 * never sees the inner one.  Every packet the board sent into the tunnel
 * therefore carried a garbage IP/UDP checksum and was dropped by the peer's
 * ip_rcv() before it reached the FORWARD chain, which is exactly what the hub
 * showed: visible in tcpdump, WireGuard rx counters climbing, FORWARD counter
 * frozen.
 *
 * So: generate in software (1), enable LWIP_CHECKSUM_CTRL_PER_NETIF, and clear
 * the flags again on the ETH netif only (App_FreertosInit), which keeps the
 * hardware offload for Ethernet while the WireGuard netif gets real checksums.
 * netif_add() defaults every new netif to NETIF_CHECKSUM_ENABLE_ALL, so the
 * WireGuard netif needs no code of its own.
 *
 * THE .ioc IS THE PRIMARY CARRIER OF THIS NOW, NOT THIS BLOCK.  Two CubeMX
 * LwIP parameters (Checksum tab) drive the whole thing, and they are the only
 * two in PeriphNet.ioc:
 *
 *     LWIP.CHECKSUM_BY_HARDWARE=0
 *     LWIP.LWIP_CHECKSUM_CTRL_PER_NETIF=1
 *
 * DO NOT ADD CHECKSUM_GEN_* / CHECKSUM_CHECK_* KEYS TO THE .ioc.  Setting
 * LWIP_CHECKSUM_CTRL_PER_NETIF forces all ten of them to 1 (the GUI says so:
 * "if enabled, the CHECKSUM_GEN_* and CHECKSUM_CHECK_* defines must be
 * enabled"), so a key holding 0 is a conflict CubeMX warns about, and a key
 * holding 1 is inert -- the generator only ever writes a CHECKSUM_* define
 * when its value is 0, leaving everything else to opt.h, which defaults them
 * to 1.  That is why the generated block above lists none of them and is
 * nonetheless correct.
 *
 * This override stays as a BACKSTOP, because the failure mode is silent: the
 * settings once lived in the generated region, a regeneration reverted them
 * to 0, and nothing complained -- with LWIP_CHECKSUM_CTRL_PER_NETIF undefined
 * NETIF_SET_CHECKSUM_CTRL degrades to a no-op macro, so the build stayed clean
 * and only the tunnel stopped carrying data.  Re-asserting here costs nothing
 * and means flipping CHECKSUM_BY_HARDWARE back on in the GUI cannot silently
 * reintroduce it.
 *
 * NOTE CHECKSUM_BY_HARDWARE is read by nobody in this tree -- ethernetif.c
 * sets TxConfig.ChecksumCtrl unconditionally -- so disabling it does not turn
 * off the ETH DMA offload.  It only unlocks the per-netif control above. */
#undef  LWIP_CHECKSUM_CTRL_PER_NETIF
#define LWIP_CHECKSUM_CTRL_PER_NETIF 1
#undef  CHECKSUM_GEN_IP
#define CHECKSUM_GEN_IP           1
#undef  CHECKSUM_GEN_UDP
#define CHECKSUM_GEN_UDP          1
#undef  CHECKSUM_GEN_TCP
#define CHECKSUM_GEN_TCP          1
#undef  CHECKSUM_GEN_ICMP
#define CHECKSUM_GEN_ICMP         1

/* CHECKSUM_CHECK_* held at 0, which DEPARTS from CubeMX's dependency.
 *
 * The GUI insists that enabling LWIP_CHECKSUM_CTRL_PER_NETIF enables all ten
 * CHECKSUM_* switches, and left alone they come out of opt.h as 1.  That is
 * defensible -- but it is a behaviour change on the WireGuard receive path,
 * which is the one path on this board with a history of failing silently and
 * expensively, and it buys nothing here: the ETH netif has its flags cleared
 * at runtime so the MAC keeps checking in hardware, and inner tunnel packets
 * arrive already verified by the peer that encrypted them.
 *
 * Generation is the half that was actually broken and is forced on above.
 * Checking stays exactly as the last known-good firmware had it, so the DMA
 * work does not smuggle an untested change onto the tunnel with it.  Turning
 * these on is a fine idea on its own merits -- as its own change, with a board
 * in front of you. */
#undef  CHECKSUM_CHECK_IP
#define CHECKSUM_CHECK_IP         0
#undef  CHECKSUM_CHECK_UDP
#define CHECKSUM_CHECK_UDP        0
#undef  CHECKSUM_CHECK_TCP
#define CHECKSUM_CHECK_TCP        0
#undef  CHECKSUM_CHECK_ICMP
#define CHECKSUM_CHECK_ICMP       0

/* mDNS responder — access board via periphnet.local */
#define LWIP_MDNS_RESPONDER       1
#define MDNS_MAX_SERVICES         1
#define LWIP_IGMP                 1
#define LWIP_NUM_NETIF_CLIENT_DATA (LWIP_MDNS_RESPONDER)
#define MEMP_NUM_UDP_PCB          8

/* MQTT client — HA auto-discovery payloads need larger buffers */
#define MQTT_OUTPUT_RINGBUF_SIZE  1024
#define MQTT_VAR_HEADER_BUFFER_LEN 256

/* HTTP server task uses netconn with recv/send timeouts so a dead client
 * cannot stall the (single-threaded) server task */
#define LWIP_SO_RCVTIMEO          1
#define LWIP_SO_SNDTIMEO          1

/* WireGuard runs its handshake and packet crypto on lwIP timers/input, i.e.
 * in tcpip_thread.  Measured worst-case added depth is ~1.5 KB
 * (wireguardif_process_data_message 224 -> chacha20poly1305_decrypt 360 ->
 * poly1305_blocks 960), which leaves too little margin in the CubeMX-generated
 * value above (currently 2048).  Buy the headroom rather than repeat the EthIf
 * stack-overflow crash-loop (docs/issue_idle_iwdg_crashloop.md).
 *
 * This is a FLOOR, not an override: whatever CubeMX regenerates is kept if it
 * already meets the minimum, so raising the stack in the .ioc is not silently
 * undone here.
 *
 * NOTE the extra bytes come out of the 48 KB FreeRTOS heap in CCM (this stack
 * is pvPortMalloc'd), NOT main SRAM.  Task stacks total ~25.6 KB of that 48 KB,
 * so there is room — verify against the "Heap=" value TRice'd at boot.
 * Separately, wireguardif_init() mem_calloc's a 968-byte device struct per
 * netif from the lwIP heap (MEM_SIZE below). */
#define TCPIP_THREAD_STACKSIZE_MIN 6144
#if !defined(TCPIP_THREAD_STACKSIZE) || (TCPIP_THREAD_STACKSIZE < TCPIP_THREAD_STACKSIZE_MIN)
#undef  TCPIP_THREAD_STACKSIZE
#define TCPIP_THREAD_STACKSIZE    TCPIP_THREAD_STACKSIZE_MIN
#endif

/* More heap + pool slots for concurrent HTTP connections (fits within
 * 128 KB SRAM — 20 KB heap with ~7 KB headroom). */
#undef  MEM_SIZE
#define MEM_SIZE                  20480
#undef  MEMP_NUM_TCP_PCB
#define MEMP_NUM_TCP_PCB          8
#undef  MEMP_NUM_NETCONN
#define MEMP_NUM_NETCONN          8
#undef  MEMP_NUM_TCP_SEG
#define MEMP_NUM_TCP_SEG          24
#undef  MEMP_NUM_SYS_TIMEOUT
#define MEMP_NUM_SYS_TIMEOUT      16

/* WireGuard needs an explicit route hook: its netif carries the /32 address
 * every .conf assigns, so lwIP's subnet match would never select it and the
 * tunnel would carry nothing.  The routes come from the peer's AllowedIPs
 * instead — see App/Net/wg_link.c WgLink_Ip4Route(). */
#define LWIP_HOOK_FILENAME        "lwip_hooks.h"
#define LWIP_HOOK_IP4_ROUTE(dest) WgLink_Ip4Route(dest)

/* Keep the complete index_html (~4.3 KB) in the TCP send buffer so
 * that netconn_write can finish the whole response in one go.
 * 6144 bytes = ~4× the default (1460×2), fitting 4234-byte HTML +
 * headers with ~1700 bytes headroom. */
#undef  TCP_SND_BUF
#define TCP_SND_BUF               6144
#undef  TCP_SND_QUEUELEN
#define TCP_SND_QUEUELEN          18
#undef  TCP_SNDLOWAT
#define TCP_SNDLOWAT              3072
#undef  TCP_SNDQUEUELOWAT
#define TCP_SNDQUEUELOWAT         9

/* USER CODE END 1 */

#ifdef __cplusplus
}
#endif
#endif /*__LWIPOPTS__H__ */
