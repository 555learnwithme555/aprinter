/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved. 
 * 
 * Redistribution and use in source and binary forms, with or without modification, 
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED 
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT 
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, 
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT 
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING 
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY 
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 * 
 * Author: Adam Dunkels <adam@sics.se>
 *
 */
#ifndef LWIP_HDR_TCP_IMPL_H
#define LWIP_HDR_TCP_IMPL_H

#include "lwip/opt.h"

#if LWIP_TCP /* don't build if not configured for use in lwipopts.h */

#include "lwip/def.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/ip.h"
#include "lwip/icmp.h"
#include "lwip/err.h"
#include "lwip/ip6.h"
#include "lwip/ip6_addr.h"

LWIP_EXTERN_C_BEGIN

/* Functions for interfacing with TCP: */

/* Lower layer interface to TCP: */
void             tcp_init    (void);  /* Initialize this module. */
void             tcp_tmr     (void);  /* Must be called every
                                         TCP_TMR_INTERVAL
                                         ms. (Typically 250 ms). */
/* It is also possible to call these two functions at the right
   intervals (instead of calling tcp_tmr()). */
void             tcp_slowtmr (void);
void             tcp_fasttmr (void);

/* Call this from a netif driver (watch out for threading issues!) that has
   returned a memory error on transmit and now has free buffers to send more.
   This iterates all active pcbs that had an error and tries to call
   tcp_output, so use this with care as it might slow down the system. */
void             tcp_txnow   (void);

/* Only used by IP to pass a TCP segment to TCP: */
void             tcp_input   (struct pbuf *p, struct netif *inp);
/* Used within the TCP code only: */
struct tcp_pcb * tcp_alloc   (u8_t prio);
void             tcp_abandon (struct tcp_pcb *pcb, int reset);
err_t            tcp_send_empty_ack(struct tcp_pcb *pcb);
void             tcp_rexmit  (struct tcp_pcb *pcb);
void             tcp_rexmit_rto  (struct tcp_pcb *pcb);
void             tcp_rexmit_fast (struct tcp_pcb *pcb);
u32_t            tcp_update_rcv_ann_wnd(struct tcp_pcb *pcb);

/* Check if a PCB is a tcp_pcb_listen by looking at the state. */
#define tcp_pcb_is_listen(pcb) ((pcb)->state == LISTEN_CLOS || (pcb)->state == LISTEN)

/**
 * This is the Nagle algorithm: try to combine user data to send as few TCP
 * segments as possible. Only send if
 * - no previously transmitted data on the connection remains unacknowledged or
 * - the TF_NODELAY flag is set (nagle algorithm turned off for this pcb) or
 * - the only unsent segment is at least pcb->mss bytes long (or there is more
 *   than one unsent segment - with lwIP, this can happen although unsent->len < mss)
 * - or if we are in fast-retransmit (TF_INFR)
 */
#define tcp_do_output_nagle(tpcb) ((((tpcb)->unacked == NULL) || \
                            ((tpcb)->flags & (TF_NODELAY | TF_INFR)) || \
                            (((tpcb)->unsent != NULL) && (((tpcb)->unsent->next != NULL) || \
                              ((tpcb)->unsent->len >= (tpcb)->mss))) || \
                            ((tcp_sndbuf(tpcb) == 0) || (tcp_sndqueuelen(tpcb) >= TCP_SND_QUEUELEN)) \
                            ) ? 1 : 0)
#define tcp_output_nagle(tpcb) (tcp_do_output_nagle(tpcb) ? tcp_output(tpcb) : ERR_OK)


#define TCP_SEQ_LT(a,b)     ((s32_t)((u32_t)(a) - (u32_t)(b)) < 0)
#define TCP_SEQ_LEQ(a,b)    ((s32_t)((u32_t)(a) - (u32_t)(b)) <= 0)
#define TCP_SEQ_GT(a,b)     ((s32_t)((u32_t)(a) - (u32_t)(b)) > 0)
#define TCP_SEQ_GEQ(a,b)    ((s32_t)((u32_t)(a) - (u32_t)(b)) >= 0)
/* is b<=a<=c? */
#if 0 /* see bug #10548 */
#define TCP_SEQ_BETWEEN(a,b,c) ((c)-(b) >= (a)-(b))
#endif
#define TCP_SEQ_BETWEEN(a,b,c) (TCP_SEQ_GEQ(a,b) && TCP_SEQ_LEQ(a,c))
#define TCP_FIN 0x01U
#define TCP_SYN 0x02U
#define TCP_RST 0x04U
#define TCP_PSH 0x08U
#define TCP_ACK 0x10U
#define TCP_URG 0x20U
#define TCP_ECE 0x40U
#define TCP_CWR 0x80U

#define TCP_FLAGS 0x3fU

/* Length of the TCP header, excluding options. */
#define TCP_HLEN 20

#ifndef TCP_TMR_INTERVAL
#define TCP_TMR_INTERVAL       250  /* The TCP timer interval in milliseconds. */
#endif /* TCP_TMR_INTERVAL */

#ifndef TCP_FAST_INTERVAL
#define TCP_FAST_INTERVAL      TCP_TMR_INTERVAL /* the fine grained timeout in milliseconds */
#endif /* TCP_FAST_INTERVAL */

#ifndef TCP_SLOW_INTERVAL
#define TCP_SLOW_INTERVAL      (2*TCP_TMR_INTERVAL)  /* the coarse grained timeout in milliseconds */
#endif /* TCP_SLOW_INTERVAL */

#define TCP_FIN_WAIT_TIMEOUT 20000 /* milliseconds */
#define TCP_SYN_RCVD_TIMEOUT 20000 /* milliseconds */

#ifndef TCP_MSL
#define TCP_MSL 60000UL /* The maximum segment lifetime in milliseconds */
#endif

/* Keepalive values, compliant with RFC 1122. Don't change this unless you know what you're doing */
#ifndef  TCP_KEEPIDLE_DEFAULT
#define  TCP_KEEPIDLE_DEFAULT     7200000UL /* Default KEEPALIVE timer in milliseconds */
#endif

#ifndef  TCP_KEEPINTVL_DEFAULT
#define  TCP_KEEPINTVL_DEFAULT    75000UL   /* Default Time between KEEPALIVE probes in milliseconds */
#endif

#ifndef  TCP_KEEPCNT_DEFAULT
#define  TCP_KEEPCNT_DEFAULT      9U        /* Default Counter for KEEPALIVE probes */
#endif

#define  TCP_MAXIDLE              TCP_KEEPCNT_DEFAULT * TCP_KEEPINTVL_DEFAULT  /* Maximum KEEPALIVE probe time */

/* Fields are (of course) in network byte order.
 * Some fields are converted to host byte order in tcp_input().
 */
PACK_STRUCT_BEGIN
struct tcp_hdr {
  PACK_STRUCT_FIELD(u16_t src);
  PACK_STRUCT_FIELD(u16_t dest);
  PACK_STRUCT_FIELD(u32_t seqno);
  PACK_STRUCT_FIELD(u32_t ackno);
  PACK_STRUCT_FIELD(u16_t _hdrlen_rsvd_flags);
  PACK_STRUCT_FIELD(u16_t wnd);
  PACK_STRUCT_FIELD(u16_t chksum);
  PACK_STRUCT_FIELD(u16_t urgp);
} PACK_STRUCT_STRUCT;
PACK_STRUCT_END

#define TCPH_HDRLEN(phdr) ((u16_t)(lwip_ntohs((phdr)->_hdrlen_rsvd_flags) >> 12))
#define TCPH_FLAGS(phdr)  ((u16_t)(lwip_ntohs((phdr)->_hdrlen_rsvd_flags) & TCP_FLAGS))

#define TCPH_HDRLEN_SET(phdr, len) (phdr)->_hdrlen_rsvd_flags = lwip_htons(((len) << 12) | TCPH_FLAGS(phdr))
#define TCPH_FLAGS_SET(phdr, flags) (phdr)->_hdrlen_rsvd_flags = (((phdr)->_hdrlen_rsvd_flags & PP_HTONS(~TCP_FLAGS)) | lwip_htons(flags))
#define TCPH_HDRLEN_FLAGS_SET(phdr, len, flags) (phdr)->_hdrlen_rsvd_flags = lwip_htons(((len) << 12) | (flags))

#define TCPH_SET_FLAG(phdr, flags ) (phdr)->_hdrlen_rsvd_flags = ((phdr)->_hdrlen_rsvd_flags | lwip_htons(flags))
#define TCPH_UNSET_FLAG(phdr, flags) (phdr)->_hdrlen_rsvd_flags = ((phdr)->_hdrlen_rsvd_flags & ~lwip_htons(flags))

#define TCP_TCPLEN(seg) ((seg)->len + (((TCPH_FLAGS((seg)->tcphdr) & (TCP_FIN | TCP_SYN)) != 0) ? 1U : 0U))

/** Flags used on input processing, not on pcb->flags
*/
#define TF_CLOSED    (u8_t)0x10U   /* Connection was successfully closed. */
#define TF_GOT_FIN   (u8_t)0x20U   /* Connection was closed by the remote end. */

/* This structure represents a TCP segment on the unsent, unacked queues */
struct tcp_seg {
  struct tcp_seg *next;    /* used when putting segments on a queue */
  struct pbuf *p;          /* buffer containing data + TCP header */
  u16_t len;               /* the TCP length of this segment */
  u8_t  flags;
#define TF_SEG_OPTS_MSS         (u8_t)0x01U /* Include MSS option. */
#define TF_SEG_OPTS_TS          (u8_t)0x02U /* Include timestamp option. */
#define TF_SEG_DATA_CHECKSUMMED (u8_t)0x04U /* ALL data (not the header) is
                                               checksummed into 'chksum' */
#define TF_SEG_OPTS_WND_SCALE   (u8_t)0x08U /* Include WND SCALE option */
  struct tcp_hdr *tcphdr;  /* the TCP header */
};

#define LWIP_TCP_OPT_EOL        0
#define LWIP_TCP_OPT_NOP        1
#define LWIP_TCP_OPT_MSS        2
#define LWIP_TCP_OPT_WS         3
#define LWIP_TCP_OPT_TS         8

#define LWIP_TCP_OPT_LEN_MSS    4
#if LWIP_TCP_TIMESTAMPS
#define LWIP_TCP_OPT_LEN_TS     10
#define LWIP_TCP_OPT_LEN_TS_OUT 12 /* aligned for output (includes NOP padding) */
#else
#define LWIP_TCP_OPT_LEN_TS_OUT 0
#endif
#define LWIP_TCP_OPT_LEN_WS_OUT 0

#define LWIP_TCP_OPT_LENGTH(flags) \
  (flags & TF_SEG_OPTS_MSS       ? LWIP_TCP_OPT_LEN_MSS    : 0) + \
  (flags & TF_SEG_OPTS_TS        ? LWIP_TCP_OPT_LEN_TS_OUT : 0) + \
  (flags & TF_SEG_OPTS_WND_SCALE ? LWIP_TCP_OPT_LEN_WS_OUT : 0)

#define LWIP_TCP_MAX_OPT_LENGTH (LWIP_TCP_OPT_LENGTH(TF_SEG_OPTS_MSS|TF_SEG_OPTS_TS|TF_SEG_OPTS_WND_SCALE))

/** This returns a TCP header option for MSS in an u32_t */
#define TCP_BUILD_MSS_OPTION(mss) lwip_htonl(0x02040000 | ((mss) & 0xFFFF))

#define TCPWNDSIZE_F       U16_F
#define TCPWND_MAX         0xFFFFU
#define TCPWND_CHECK16(x)
#define TCPWND_MIN16(x)    x

/* Global variables: */
extern struct tcp_pcb *tcp_input_pcb;
extern u32_t tcp_ticks;
extern u8_t tcp_active_pcbs_changed;

/* The TCP PCB lists. */
extern struct tcp_pcb_base *tcp_bound_pcbs;
extern struct tcp_pcb_listen *tcp_listen_pcbs;
extern struct tcp_pcb *tcp_active_pcbs;  /* List of all TCP PCBs that are in a
              state in which they accept or send
              data. */
extern struct tcp_pcb *tcp_tw_pcbs;      /* List of all TCP PCBs in TIME-WAIT. */

#ifndef TCP_DEBUG_PCB_LISTS
#define TCP_DEBUG_PCB_LISTS 0
#endif

#define TCP_REG_ACTIVE(npcb)                       \
  do {                                             \
    tcp_reg((struct tcp_pcb_base **)&tcp_active_pcbs, to_tcp_pcb_base(npcb)); \
    tcp_active_pcbs_changed = 1;                   \
  } while (0)

#define TCP_RMV_ACTIVE(npcb)                       \
  do {                                             \
    tcp_rmv((struct tcp_pcb_base **)&tcp_active_pcbs, to_tcp_pcb_base(npcb)); \
    tcp_active_pcbs_changed = 1;                   \
  } while (0)

/* Internal functions: */

u8_t tcp_state_is_active(enum tcp_state state);

void tcp_reg(struct tcp_pcb_base **pcbs, struct tcp_pcb_base *npcb);
void tcp_rmv(struct tcp_pcb_base **pcbs, struct tcp_pcb_base *npcb);

void tcp_pcb_purge(struct tcp_pcb *pcb);
void tcp_pcb_free(struct tcp_pcb *pcb, u8_t send_rst, struct tcp_pcb *prev);
void tcp_move_to_time_wait(struct tcp_pcb *pcb);
void tcp_report_err(tcp_err_fn errf, void *arg, err_t err);

void tcp_segs_free(struct tcp_seg *seg);
void tcp_seg_free(struct tcp_seg *seg);

#define tcp_ack(pcb)                               \
  do {                                             \
    if((pcb)->flags & TF_ACK_DELAY) {              \
      (pcb)->flags &= ~TF_ACK_DELAY;               \
      (pcb)->flags |= TF_ACK_NOW;                  \
    }                                              \
    else {                                         \
      (pcb)->flags |= TF_ACK_DELAY;                \
    }                                              \
  } while (0)

#define tcp_ack_now(pcb)                           \
  do {                                             \
    (pcb)->flags |= TF_ACK_NOW;                    \
  } while (0)

err_t tcp_send_fin(struct tcp_pcb *pcb);
err_t tcp_enqueue_flags(struct tcp_pcb *pcb, u8_t flags);

void tcp_rexmit_seg(struct tcp_pcb *pcb, struct tcp_seg *seg);

void tcp_rst(u32_t seqno, u32_t ackno,
       const ip_addr_t *local_ip, const ip_addr_t *remote_ip,
       u16_t local_port, u16_t remote_port);

u32_t tcp_next_iss(void);

err_t tcp_keepalive(struct tcp_pcb *pcb);
err_t tcp_zero_window_probe(struct tcp_pcb *pcb);
void  tcp_trigger_input_pcb_close(void);

#if TCP_CALCULATE_EFF_SEND_MSS
u16_t tcp_eff_send_mss_impl(u16_t sendmss, const ip_addr_t *dest
#if LWIP_IPV6 || LWIP_IPV4_SRC_ROUTING
                           , const ip_addr_t *src
#endif /* LWIP_IPV6 || LWIP_IPV4_SRC_ROUTING */
#if LWIP_IPV6 && LWIP_IPV4
                           , u8_t isipv6
#endif /* LWIP_IPV6 && LWIP_IPV4 */
                           );
#if LWIP_IPV4 && LWIP_IPV6
#define tcp_eff_send_mss(sendmss, src, dest, isipv6) tcp_eff_send_mss_impl(sendmss, dest, src, isipv6)
#elif LWIP_IPV6 || LWIP_IPV4_SRC_ROUTING
#define tcp_eff_send_mss(sendmss, src, dest, isipv6) tcp_eff_send_mss_impl(sendmss, dest, src)
#else /* LWIP_IPV4 && LWIP_IPV6 */
#define tcp_eff_send_mss(sendmss, src, dest, isipv6) tcp_eff_send_mss_impl(sendmss, dest)
#endif /* LWIP_IPV4 && LWIP_IPV6 */
#endif /* TCP_CALCULATE_EFF_SEND_MSS */

#if TCP_DEBUG || TCP_INPUT_DEBUG || TCP_OUTPUT_DEBUG
void tcp_debug_print(struct tcp_hdr *tcphdr);
void tcp_debug_print_flags(u8_t flags);
void tcp_debug_print_state(enum tcp_state s);
void tcp_debug_print_pcbs(void);
s16_t tcp_pcbs_sane(void);
#else
#  define tcp_debug_print(tcphdr)
#  define tcp_debug_print_flags(flags)
#  define tcp_debug_print_state(s)
#  define tcp_debug_print_pcbs()
#  define tcp_pcbs_sane() 1
#endif /* TCP_DEBUG */

/** External function (implemented in timers.c), called when TCP detects
 * that a timer is needed (i.e. active- or time-wait-pcb found). */
void tcp_timer_needed(void);

#if LWIP_IPV4
void tcp_netif_ipv4_addr_changed(const ip4_addr_t* old_addr, const ip4_addr_t* new_addr);
#endif /* LWIP_IPV4 */

LWIP_EXTERN_C_END

#endif /* LWIP_TCP */

#endif /* LWIP_HDR_TCP_H */
