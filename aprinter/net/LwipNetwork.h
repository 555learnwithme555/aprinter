/*
 * Copyright (c) 2015 Ambroz Bizjak
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef APRINTER_LWIP_NETWORK_H
#define APRINTER_LWIP_NETWORK_H

//#define APRINTER_DEBUG_NETWORK

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include <lwip/init.h>
#include <lwip/timers.h>
#include <lwip/netif.h>
#include <lwip/ip_addr.h>
#include <lwip/err.h>
#include <lwip/pbuf.h>
#include <lwip/snmp.h>
#include <lwip/stats.h>
#include <lwip/dhcp.h>
#include <lwip/tcp.h>
#include <netif/etharp.h>

#include <aprinter/meta/WrapFunction.h>
#include <aprinter/meta/TypeListUtils.h>
#include <aprinter/meta/MemberType.h>
#include <aprinter/meta/MinMax.h>
#include <aprinter/base/Object.h>
#include <aprinter/base/Callback.h>
#include <aprinter/base/Assert.h>
#include <aprinter/hal/common/EthernetCommon.h>
#include <aprinter/printer/Console.h>

#include <aprinter/BeginNamespace.h>

template <typename Context, typename ParentObject, typename EthernetService>
class LwipNetwork {
public:
    struct Object;
    class TcpConnection;
    
private:
    struct EthernetActivateHandler;
    struct EthernetLinkHandler;
    struct EthernetReceiveHandler;
    class EthernetSendBuffer;
    class EthernetRecvBuffer;
    using TheEthernetClientParams = EthernetClientParams<EthernetActivateHandler, EthernetLinkHandler, EthernetReceiveHandler, EthernetSendBuffer, EthernetRecvBuffer>;
    using TheEthernet = typename EthernetService::template Ethernet<Context, Object, TheEthernetClientParams>;
    
    using TimeoutsFastEvent = typename Context::EventLoop::template FastEventSpec<LwipNetwork>;
    
    APRINTER_DEFINE_MEMBER_TYPE(MemberType_EventLoopFastEvents, EventLoopFastEvents)
    
public:
    struct ActivateParams {
        uint8_t const *mac_addr;
    };
    
    static void init (Context c)
    {
        auto *o = Object::self(c);
        
        TheEthernet::init(c);
        Context::EventLoop::template initFastEvent<TimeoutsFastEvent>(c, LwipNetwork::timeouts_event_handler);
        o->net_activated = false;
        o->eth_activated = false;
        lwip_init();
        Context::EventLoop::template triggerFastEvent<TimeoutsFastEvent>(c);
    }
    
    // Note, deinit doesn't really work due to lwIP.
    static void deinit (Context c)
    {
        auto *o = Object::self(c);
        
        Context::EventLoop::template resetFastEvent<TimeoutsFastEvent>(c);
        TheEthernet::deinit(c);
    }
    
    static void activate (Context c, ActivateParams params)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(!o->net_activated)
        AMBRO_ASSERT(!o->eth_activated)
        
        o->net_activated = true;
        init_netif(c, params);
        TheEthernet::activate(c, o->netif.hwaddr);
    }
    
    static void deactivate (Context c)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(o->net_activated)
        
        o->net_activated = false;
        o->eth_activated = false;
        deinit_netif(c);
        TheEthernet::reset(c);
    }
    
    static bool isActivated (Context c)
    {
        auto *o = Object::self(c);
        return o->net_activated;
    }
    
    static uint8_t const * getMacAddress (Context c)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(o->net_activated)
        
        return o->netif.hwaddr;
    }
    
    class TcpListener {
        friend class TcpConnection;
        
    public:
        // The user is supposed to call TcpConnection::acceptConnection from within
        // AcceptHandler. You must return true if and only if you have accepted the
        // connection and not closed it.
        // WARNING: Do not call any other network functions from this callback,
        // especially don't close the listener. Though the following is permitted:
        // - Closing the resulting connection (deinit/reset) from within this callback
        //   after having accepted it, as long as you indicate this by returning false.
        // - Closing other connections (perhaps in order to make space for the new one).
        using AcceptHandler = Callback<bool(Context)>;
        
        void init (Context c, AcceptHandler accept_handler)
        {
            m_accept_handler = accept_handler;
            m_pcb = nullptr;
            m_accepted_pcb = nullptr;
        }
        
        void deinit (Context c)
        {
            reset_internal(c);
        }
        
        void reset (Context c)
        {
            reset_internal(c);
        }
        
        bool startListening (Context c, uint16_t port)
        {
            AMBRO_ASSERT(!m_pcb)
            
            do {
                m_pcb = tcp_new();
                if (!m_pcb) {
                    goto fail;
                }
                
                ip_addr_t the_addr;
                ip_addr_set_any(0, &the_addr);
                
                auto err = tcp_bind(m_pcb, &the_addr, port);
                if (err != ERR_OK) {
                    goto fail;
                }
                
                struct tcp_pcb *listen_pcb = tcp_listen(m_pcb);
                if (!listen_pcb) {
                    goto fail;
                }
                m_pcb = listen_pcb;
                
                tcp_arg(m_pcb, this);
                tcp_accept(m_pcb, &TcpListener::pcb_accept_handler_wrapper);
                
                return true;
            } while (false);
            
        fail:
            reset_internal(c);
            return false;
        }
        
    private:
        void reset_internal (Context c)
        {
            AMBRO_ASSERT(!m_accepted_pcb)
            
            if (m_pcb) {
                tcp_arg(m_pcb, nullptr);
                tcp_accept(m_pcb, nullptr);
                auto err = tcp_close(m_pcb);
                AMBRO_ASSERT(err == ERR_OK)
                m_pcb = nullptr;
            }
        }
        
        static err_t pcb_accept_handler_wrapper (void *arg, struct tcp_pcb *newpcb, err_t err)
        {
            TcpListener *obj = (TcpListener *)arg;
            return obj->pcb_accept_handler(newpcb, err);
        }
        
        err_t pcb_accept_handler (struct tcp_pcb *newpcb, err_t err)
        {
            Context c;
            AMBRO_ASSERT(m_pcb)
            AMBRO_ASSERT(newpcb)
            AMBRO_ASSERT(err == ERR_OK)
            AMBRO_ASSERT(!m_accepted_pcb)
            
            tcp_accepted(m_pcb);
            
            m_accepted_pcb = newpcb;
            bool accept_res = m_accept_handler(c);
            
            if (m_accepted_pcb) {
                m_accepted_pcb = nullptr;
                return ERR_BUF;
            }
            
            return accept_res ? ERR_OK : ERR_ABRT;
        }
        
        AcceptHandler m_accept_handler;
        struct tcp_pcb *m_pcb;
        struct tcp_pcb *m_accepted_pcb;
    };
    
    class TcpConnection {
        enum class State : uint8_t {IDLE, RUNNING, ERRORED};
        
    public:
        using ErrorHandler = Callback<void(Context, bool remote_closed)>;
        
        // The user is supposed to call TcpConnection::copyReceivedData from within
        // RecvHandler, one or more times, with the sum of 'length' parameters
        // equal to the 'length' in the callback (or less if not all data is needed).
        // WARNING: Do not call any other network functions from this callback.
        // It is specifically prohibited to close (deinit/reset) this connection.
        using RecvHandler = Callback<void(Context, size_t length)>;
        
        using SendHandler = Callback<void(Context)>;
        
        void init (Context c, ErrorHandler error_handler, RecvHandler recv_handler, SendHandler send_handler)
        {
            m_closed_event.init(c, APRINTER_CB_OBJFUNC_T(&TcpConnection::closed_event_handler, this));
            m_error_handler = error_handler;
            m_recv_handler = recv_handler;
            m_send_handler = send_handler;
            m_state = State::IDLE;
            m_pcb = nullptr;
            m_received_pbuf = nullptr;
        }
        
        void deinit (Context c)
        {
            reset_internal(c);
            m_closed_event.deinit(c);
        }
        
        void reset (Context c)
        {
            reset_internal(c);
        }
        
        void acceptConnection (Context c, TcpListener *listener)
        {
            AMBRO_ASSERT(m_state == State::IDLE)
            AMBRO_ASSERT(!m_pcb)
            AMBRO_ASSERT(!m_received_pbuf)
            AMBRO_ASSERT(listener->m_accepted_pcb)
            
            m_pcb = listener->m_accepted_pcb;
            listener->m_accepted_pcb = nullptr;
            
            tcp_arg(m_pcb, this);
            tcp_err(m_pcb, &TcpConnection::pcb_err_handler_wrapper);
            tcp_recv(m_pcb, &TcpConnection::pcb_recv_handler_wrapper);
            tcp_sent(m_pcb, &TcpConnection::pcb_sent_handler_wrapper);
            
            m_state = State::RUNNING;
            m_recv_remote_closed = false;
            m_recv_pending = 0;
        }
        
        void copyReceivedData (Context c, char *buffer, size_t length)
        {
            AMBRO_ASSERT(m_state == State::RUNNING)
            AMBRO_ASSERT(m_pcb)
            AMBRO_ASSERT(m_received_pbuf)
            
            while (length > 0) {
                AMBRO_ASSERT(m_received_offset <= m_received_pbuf->len)
                size_t rem_bytes_in_pbuf = m_received_pbuf->len - m_received_offset;
                if (rem_bytes_in_pbuf == 0) {
                    AMBRO_ASSERT(m_received_pbuf->next)
                    m_received_pbuf = m_received_pbuf->next;
                    m_received_offset = 0;
                    continue;
                }
                
                size_t bytes_to_take = MinValue(length, rem_bytes_in_pbuf);
                
                memcpy(buffer, m_received_pbuf->payload + m_received_offset, bytes_to_take);
                buffer += bytes_to_take;
                length -= bytes_to_take;
                
                m_received_offset += bytes_to_take;
            }
        }
        
        void acceptReceivedData (Context c, size_t amount)
        {
            AMBRO_ASSERT(m_state == State::RUNNING || m_state == State::ERRORED)
            AMBRO_ASSERT(amount <= m_recv_pending)
            
            m_recv_pending -= amount;
            
            if (m_state == State::RUNNING) {
                tcp_recved(m_pcb, amount);
            }
        }
        
    private:
        void reset_internal (Context c)
        {
            AMBRO_ASSERT(!m_received_pbuf)
            
            if (m_pcb) {
                remove_pcb_callbacks(m_pcb);
                auto err = tcp_close(m_pcb);
                if (err != ERR_OK) {
                    tcp_abort(m_pcb);
                }
                m_pcb = nullptr;
            }
            m_closed_event.unset(c);
            m_state = State::IDLE;
        }
        
        static void pcb_err_handler_wrapper (void *arg, err_t err)
        {
            TcpConnection *obj = (TcpConnection *)arg;
            return obj->pcb_err_handler(err);
        }
        
        void pcb_err_handler (err_t err)
        {
            Context c;
            AMBRO_ASSERT(m_state == State::RUNNING)
            AMBRO_ASSERT(m_pcb)
            
            remove_pcb_callbacks(m_pcb);
            m_pcb = nullptr;
            m_state = State::ERRORED;
            
            if (!m_closed_event.isSet(c)) {
                m_closed_event.prependNowNotAlready(c);
            }
        }
        
        static err_t pcb_recv_handler_wrapper (void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
        {
            TcpConnection *obj = (TcpConnection *)arg;
            return obj->pcb_recv_handler(tpcb, p, err);
        }
        
        err_t pcb_recv_handler (struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
        {
            Context c;
            AMBRO_ASSERT(m_state == State::RUNNING)
            AMBRO_ASSERT(m_pcb)
            AMBRO_ASSERT(!m_received_pbuf)
            AMBRO_ASSERT(tpcb == m_pcb)
            AMBRO_ASSERT(err == ERR_OK)
            
            if (!p) {
                if (!m_recv_remote_closed) {
                    m_recv_remote_closed = true;
                    m_closed_event.prependNowNotAlready(c);
                }
            } else {
                if (!m_recv_remote_closed && p->tot_len > 0) {
                    m_recv_pending += p->tot_len;
                    m_received_pbuf = p;
                    m_received_offset = 0;
                    
                    m_recv_handler(c, p->tot_len);
                    
                    m_received_pbuf = nullptr;
                }
                
                pbuf_free(p);
            }
            
            return ERR_OK;
        }
        
        static err_t pcb_sent_handler_wrapper (void *arg, struct tcp_pcb *tpcb, uint16_t len)
        {
            TcpConnection *obj = (TcpConnection *)arg;
            return obj->pcb_sent_handler(tpcb, len);
        }
        
        err_t pcb_sent_handler (struct tcp_pcb *tpcb, uint16_t len)
        {
            //
            return ERR_OK;
        }
        
        void closed_event_handler (Context c)
        {
            switch (m_state) {
                case State::ERRORED: {
                    AMBRO_ASSERT(!m_pcb)
                    
                    return m_error_handler(c, false);
                } break;
                
                case State::RUNNING: {
                    AMBRO_ASSERT(m_pcb)
                    AMBRO_ASSERT(m_recv_remote_closed)
                    
                    return m_error_handler(c, true);
                } break;
                
                default: AMBRO_ASSERT(false);
            }
        }
        
        static void remove_pcb_callbacks (struct tcp_pcb *pcb)
        {
            tcp_arg(pcb, nullptr);
            tcp_err(pcb, nullptr);
            tcp_recv(pcb, nullptr);
            tcp_sent(pcb, nullptr);
        }
        
        typename Context::EventLoop::QueuedEvent m_closed_event;
        ErrorHandler m_error_handler;
        RecvHandler m_recv_handler;
        SendHandler m_send_handler;
        State m_state;
        bool m_recv_remote_closed;
        struct tcp_pcb *m_pcb;
        struct pbuf *m_received_pbuf;
        size_t m_received_offset;
        size_t m_recv_pending;
    };
    
private:
    static void init_netif (Context c, ActivateParams params)
    {
        auto *o = Object::self(c);
        
        ip_addr_t the_ipaddr;
        ip_addr_t the_netmask;
        ip_addr_t the_gw;
        ip_addr_set_zero(&the_ipaddr);
        ip_addr_set_zero(&the_netmask);
        ip_addr_set_zero(&the_gw);
        
        memset(&o->netif, 0, sizeof(o->netif));
        
        netif_add(&o->netif, &the_ipaddr, &the_netmask, &the_gw, &params, &LwipNetwork::netif_if_init, ethernet_input);
        netif_set_up(&o->netif);
        netif_set_default(&o->netif);
        dhcp_start(&o->netif);
    }
    
    static void deinit_netif (Context c)
    {
        auto *o = Object::self(c);
        
        dhcp_stop(&o->netif);
        netif_remove(&o->netif);
    }
    
    static err_t netif_if_init (struct netif *netif)
    {
        ActivateParams const *params = (ActivateParams const *)netif->state;
        
        netif->hostname = (char *)"aprinter";
        
        uint32_t link_speed_for_mib2 = UINT32_C(100000000);
        MIB2_INIT_NETIF(netif, snmp_ifType_ethernet_csmacd, link_speed_for_mib2);
        
        netif->name[0] = 'e';
        netif->name[1] = 'n';
        
        netif->output = etharp_output;
        netif->linkoutput = &LwipNetwork::netif_link_output;
        
        netif->hwaddr_len = ETHARP_HWADDR_LEN;
        memcpy(netif->hwaddr, params->mac_addr, ETHARP_HWADDR_LEN);
        
        netif->mtu = 1500;
        netif->flags = NETIF_FLAG_BROADCAST|NETIF_FLAG_ETHARP;
        netif->state = nullptr;
        
        return ERR_OK;
    }
    
    static void timeouts_event_handler (Context c)
    {
        auto *o = Object::self(c);
        
        Context::EventLoop::template triggerFastEvent<TimeoutsFastEvent>(c);
        sys_check_timeouts();
    }
    
    static void ethernet_activate_handler (Context c, bool error)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(o->net_activated)
        AMBRO_ASSERT(!o->eth_activated)
        
        if (error) {
            APRINTER_CONSOLE_MSG("//EthActivateErr");
            return;
        }
        o->eth_activated = true;
    }
    struct EthernetActivateHandler : public AMBRO_WFUNC_TD(&LwipNetwork::ethernet_activate_handler) {};
    
    static void ethernet_link_handler (Context c, bool link_status)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(o->eth_activated)
        
        if (link_status) {
            APRINTER_CONSOLE_MSG("//EthLinkUp");
        } else {
            APRINTER_CONSOLE_MSG("//EthLinkDown");
        }
        
        if (link_status) {
            netif_set_link_up(&o->netif);
        } else {
            netif_set_link_down(&o->netif);
        }
    }
    struct EthernetLinkHandler : public AMBRO_WFUNC_TD(&LwipNetwork::ethernet_link_handler) {};
    
    static err_t netif_link_output (struct netif *netif, struct pbuf *p)
    {
        Context c;
        auto *o = Object::self(c);
        AMBRO_ASSERT(o->net_activated)
        
#if ETH_PAD_SIZE
        pbuf_header(p, -ETH_PAD_SIZE);
#endif
        err_t ret = ERR_BUF;
        
        debug_print_pbuf(c, "Tx", p);
        
        if (!o->eth_activated) {
            goto out;
        }
        
        EthernetSendBuffer send_buf;
        send_buf.m_total_len = p->tot_len;
        send_buf.m_current_pbuf = p;
        
        if (!TheEthernet::sendFrame(c, &send_buf)) {
            goto out;
        }
        
        LINK_STATS_INC(link.xmit);
        ret = ERR_OK;
        
    out:
#if ETH_PAD_SIZE
        pbuf_header(p, ETH_PAD_SIZE);
#endif
        return ret;
    }
    
    class EthernetSendBuffer {
        friend LwipNetwork;
        
    public:
        size_t getTotalLength ()
        {
            return m_total_len;
        }
        
        size_t getChunkLength ()
        {
            AMBRO_ASSERT(m_current_pbuf)
            return m_current_pbuf->len;
        }
        
        char const * getChunkPtr ()
        {
            AMBRO_ASSERT(m_current_pbuf)
            return (char const *)m_current_pbuf->payload;
        }
        
        bool nextChunk ()
        {
            AMBRO_ASSERT(m_current_pbuf)
            m_current_pbuf = m_current_pbuf->next;
            return m_current_pbuf != nullptr;
        }
        
    private:
        size_t m_total_len;
        struct pbuf *m_current_pbuf;
    };
    
    static void ethernet_receive_handler (Context c)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(o->eth_activated)
        
        while (true) {
            EthernetRecvBuffer recv_buf;
            recv_buf.m_first_pbuf = nullptr;
            recv_buf.m_current_pbuf = nullptr;
            recv_buf.m_valid = false;
            
            if (!TheEthernet::recvFrame(c, &recv_buf)) {
                if (recv_buf.m_first_pbuf) {
                    pbuf_free(recv_buf.m_first_pbuf);
                }
                return;
            }
            
            if (!recv_buf.m_valid) {
                if (recv_buf.m_first_pbuf) {
                    pbuf_free(recv_buf.m_first_pbuf);
                }
                LINK_STATS_INC(link.memerr);
                LINK_STATS_INC(link.drop);
                continue;
            }
            
            AMBRO_ASSERT(recv_buf.m_first_pbuf)
            AMBRO_ASSERT(!recv_buf.m_current_pbuf)
            struct pbuf *p = recv_buf.m_first_pbuf;
            
            debug_print_pbuf(c, "Rx", p);
            
#if ETH_PAD_SIZE
            pbuf_header(p, ETH_PAD_SIZE);
#endif
            LINK_STATS_INC(link.recv);
            
            if (o->netif.input(p, &o->netif) != ERR_OK) {
                pbuf_free(p);
            }
        }
    }
    struct EthernetReceiveHandler : public AMBRO_WFUNC_TD(&LwipNetwork::ethernet_receive_handler) {};
    
    class EthernetRecvBuffer {
        friend LwipNetwork;
        
    public:
        bool allocate (size_t length)
        {
            AMBRO_ASSERT(!m_first_pbuf)
            
            struct pbuf *p = pbuf_alloc(PBUF_RAW, ETH_PAD_SIZE + length, PBUF_POOL);
            if (!p) {
                return false;
            }
#if ETH_PAD_SIZE
            pbuf_header(p, -ETH_PAD_SIZE);
#endif
            m_first_pbuf = p;
            m_current_pbuf = p;
            return true;
        }
        
        size_t getChunkLength ()
        {
            AMBRO_ASSERT(m_current_pbuf)
            return m_current_pbuf->len;
        }
        
        char * getChunkPtr ()
        {
            AMBRO_ASSERT(m_current_pbuf)
            return (char *)m_current_pbuf->payload;
        }
        
        bool nextChunk ()
        {
            AMBRO_ASSERT(m_current_pbuf)
            m_current_pbuf = m_current_pbuf->next;
            return m_current_pbuf != nullptr;
        }
        
        void setValid ()
        {
            m_valid = true;
        }
        
    private:
        struct pbuf *m_first_pbuf;
        struct pbuf *m_current_pbuf;
        bool m_valid;
    };
    
    static void debug_print_pbuf (Context c, char const *event, struct pbuf *p)
    {
#ifdef APRINTER_DEBUG_NETWORK
        auto *out = Context::Printer::get_msg_output(c);
        out->reply_append_str(c, "//");
        out->reply_append_str(c, event);
        out->reply_append_str(c, " tot_len=");
        out->reply_append_uint32(c, p->tot_len);
        out->reply_append_str(c, " data=");
        for (struct pbuf *q = p; q; q = q->next) {
            for (size_t i = 0; i < q->len; i++) {
                char s[4];
                sprintf(s, " %02" PRIX8, ((uint8_t *)q->payload)[i]);
                out->reply_append_str(c, s);
            }
        }
        out->reply_append_ch(c, '\n');
        out->reply_poke(c);
#endif
    }
    
public:
    using EventLoopFastEvents = JoinTypeLists<
        MakeTypeList<TimeoutsFastEvent>,
        ObjCollect<MakeTypeList<LwipNetwork>, MemberType_EventLoopFastEvents, true>
    >;
    
    using GetEthernet = TheEthernet;
    
    struct Object : public ObjBase<LwipNetwork, ParentObject, MakeTypeList<
        TheEthernet
    >> {
        bool net_activated;
        bool eth_activated;
        struct netif netif;
    };
};

#include <aprinter/EndNamespace.h>

#endif
