/*
 * Copyright (c) 2016 Ambroz Bizjak
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

#ifndef APRINTER_IPSTACK_IPSTACK_H
#define APRINTER_IPSTACK_IPSTACK_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <aprinter/meta/ServiceUtils.h>
#include <aprinter/meta/MinMax.h>
#include <aprinter/base/Assert.h>
#include <aprinter/base/Preprocessor.h>
#include <aprinter/base/Hints.h>
#include <aprinter/structure/DoubleEndedList.h>

#include <aipstack/misc/Err.h>
#include <aipstack/misc/Buf.h>
#include <aipstack/misc/Chksum.h>
#include <aipstack/misc/SendRetry.h>
#include <aipstack/proto/IpAddr.h>
#include <aipstack/proto/Ip4Proto.h>
#include <aipstack/proto/Icmp4Proto.h>
#include <aipstack/ip/IpIfaceDriver.h>
#include <aipstack/ip/IpReassembly.h>

#include <aipstack/BeginNamespace.h>

struct IpIfaceIp4AddrSetting {
    bool present;
    uint8_t prefix;
    Ip4Addr addr;
};

struct IpIfaceIp4GatewaySetting {
    bool present;
    Ip4Addr addr;
};

template <typename Arg>
class IpStack {
    APRINTER_USE_TYPES1(Arg, (Params))
    APRINTER_USE_VALS(Params, (HeaderBeforeIp, IcmpTTL))
    APRINTER_USE_TYPES1(Arg, (Context, BufAllocator))
    
    using ReassemblyService = IpReassemblyService<Params::MaxReassEntrys, Params::MaxReassSize>;
    APRINTER_MAKE_INSTANCE(Reassembly, (ReassemblyService::template Compose<Context>))
    
public:
    static size_t const HeaderBeforeIp4Dgram = HeaderBeforeIp + Ip4Header::Size;
    
    // Minimum MTU is smallest IP header plus 8 bytes (for fragmentation to work).
    static size_t const MinIpIfaceMtu = Ip4Header::Size + 8;
    
    class Iface;
    
public:
    void init ()
    {
        m_reassembly.init();
        
        m_iface_list.init();
        m_proto_listeners_list.init();
        m_next_id = 0;
    }
    
    void deinit ()
    {
        AMBRO_ASSERT(m_iface_list.isEmpty())
        AMBRO_ASSERT(m_proto_listeners_list.isEmpty())
        
        m_reassembly.deinit();
    }
    
public:
    struct Ip4DgramMeta {
        Ip4Addr local_addr;
        Ip4Addr remote_addr;
        uint8_t ttl;
        uint8_t proto;
        Iface *iface;
    };
    
    IpErr sendIp4Dgram (Ip4DgramMeta const &meta, IpBufRef dgram,
                        IpSendRetry::Request *retryReq = nullptr)
    {
        // Reveal IP header.
        IpBufRef pkt;
        if (!dgram.revealHeader(Ip4Header::Size, &pkt)) {
            return IpErr::NO_HEADER_SPACE;
        }
        
        // Find an interface and address for output.
        Iface *route_iface;
        Ip4Addr route_addr;
        if (!routeIp4(meta.remote_addr, meta.iface, &route_iface, &route_addr)) {
            return IpErr::NO_IP_ROUTE;
        }
        
        // Sanity check length.
        if (AMBRO_UNLIKELY(dgram.tot_len > UINT16_MAX)) {
            return IpErr::PKT_TOO_LARGE;
        }
        
        // Check if fragmentation is needed and calculate the length of
        // the first packet.
        size_t mtu = route_iface->m_ip_mtu;
        bool more_fragments = pkt.tot_len > mtu;
        size_t pkt_send_len = more_fragments ? round_frag_length(Ip4Header::Size, mtu) : pkt.tot_len;
        
        // Generate an identification number.
        uint16_t ident = m_next_id++;
        
        // Write IP header fields.
        auto ip4_header = Ip4Header::MakeRef(pkt.getChunkPtr());
        ip4_header.set(Ip4Header::VersionIhl(),   (4<<Ip4VersionShift)|5);
        ip4_header.set(Ip4Header::DscpEcn(),      0);
        ip4_header.set(Ip4Header::TotalLen(),     pkt_send_len);
        ip4_header.set(Ip4Header::Ident(),        ident);
        ip4_header.set(Ip4Header::FlagsOffset(),  more_fragments?Ip4FlagMF:0);
        ip4_header.set(Ip4Header::TimeToLive(),   meta.ttl);
        ip4_header.set(Ip4Header::Protocol(),     meta.proto);
        ip4_header.set(Ip4Header::HeaderChksum(), 0);
        ip4_header.set(Ip4Header::SrcAddr(),      meta.local_addr);
        ip4_header.set(Ip4Header::DstAddr(),      meta.remote_addr);
        
        // Calculate the IP header checksum.
        uint16_t calc_chksum = IpChksum(ip4_header.data, Ip4Header::Size);
        ip4_header.set(Ip4Header::HeaderChksum(), calc_chksum);
        
        // Send the packet to the driver.
        IpErr err = route_iface->m_driver->sendIp4Packet(pkt.subTo(pkt_send_len), route_addr, retryReq);
        
        // If no fragmentation is needed or sending failed, this is the end.
        if (AMBRO_LIKELY(!more_fragments) || err != IpErr::SUCCESS) {
            return err;
        }
        
        // Calculate the next fragment offset and skip the sent data.
        size_t fragment_offset = pkt_send_len - Ip4Header::Size;
        dgram.skipBytes(fragment_offset);
        
        // Send remaining fragments.
        while (true) {
            // We must send fragments such that the fragment offset is a multiple of 8.
            // This is achieved by round_frag_length.
            AMBRO_ASSERT(fragment_offset % 8 == 0)
            
            // Calculate how much to send and whether we have more fragments.
            size_t rem_pkt_length = Ip4Header::Size + dgram.tot_len;
            more_fragments = rem_pkt_length > mtu;
            pkt_send_len = more_fragments ? round_frag_length(Ip4Header::Size, mtu) : rem_pkt_length;
            
            // Write the fragment-specific IP header fields.
            ip4_header.set(Ip4Header::TotalLen(),     pkt_send_len);
            ip4_header.set(Ip4Header::FlagsOffset(),  (more_fragments?Ip4FlagMF:0)|(fragment_offset/8));
            ip4_header.set(Ip4Header::HeaderChksum(), 0);
            
            // Calculate the IP header checksum.
            uint16_t calc_chksum = IpChksum(ip4_header.data, Ip4Header::Size);
            ip4_header.set(Ip4Header::HeaderChksum(), calc_chksum);
            
            // Construct a packet with header and partial data.
            IpBufNode data_node = dgram.toNode();
            IpBufNode header_node;
            IpBufRef frag_pkt = pkt.subHeaderToContinuedBy(Ip4Header::Size, &data_node, pkt_send_len, &header_node);
            
            // Send the packet to the driver.
            err = route_iface->m_driver->sendIp4Packet(frag_pkt, route_addr, retryReq);
            
            // If this was the last fragment or there was an error, return.
            if (!more_fragments || err != IpErr::SUCCESS) {
                return err;
            }
            
            // Update the fragment offset and skip the sent data.
            size_t data_sent = pkt_send_len - Ip4Header::Size;
            fragment_offset += data_sent;
            dgram.skipBytes(data_sent);
        }
    }
    
    bool routeIp4 (Ip4Addr dst_addr, Iface *force_iface, Iface **route_iface, Ip4Addr *route_addr)
    {
        // When an interface is forced the logic is almost the same except that only this
        // interface is considered and we also allow the all-ones broadcast address.
        
        if (force_iface != nullptr) {
            if (dst_addr == Ip4Addr::AllOnesAddr() || force_iface->ip4AddrIsLocal(dst_addr)) {
                *route_addr = dst_addr;
            }
            else if (force_iface->m_have_gateway && force_iface->ip4AddrIsLocal(force_iface->m_gateway)) {
                *route_addr = force_iface->m_gateway;
            }
            else {
                return false;
            }
            *route_iface = force_iface;
            return true;
        }
        
        // Look for an interface where dst_addr is inside the local subnet
        // (and in case of multiple matches find a most specific one).
        // Also look for an interface with a gateway to use in case there
        // is no local subnet match.
        
        Iface *local_iface = nullptr;
        Iface *gw_iface = nullptr;
        
        for (Iface *iface = m_iface_list.first(); iface != nullptr; iface = m_iface_list.next(iface)) {
            if (iface->ip4AddrIsLocal(dst_addr)) {
                if (local_iface == nullptr || iface->m_addr.prefix > local_iface->m_addr.prefix) {
                    local_iface = iface;
                }
            }
            if (iface->m_have_gateway && iface->ip4AddrIsLocal(iface->m_gateway)) {
                if (gw_iface == nullptr) {
                    gw_iface = iface;
                }
            }
        }
        
        if (local_iface != nullptr) {
            *route_iface = local_iface;
            *route_addr = dst_addr;
        }
        else if (gw_iface != nullptr) {
            *route_iface = gw_iface;
            *route_addr = gw_iface->m_gateway;
        }
        else {
            return false;
        }
        return true;
    }
    
    class ProtoListenerCallback {
    public:
        virtual void recvIp4Dgram (Ip4DgramMeta const &meta, IpBufRef dgram) = 0;
    };
    
    class ProtoListener {
        friend IpStack;
        
    public:
        void init (IpStack *stack, uint8_t proto, ProtoListenerCallback *callback)
        {
            AMBRO_ASSERT(stack != nullptr)
            AMBRO_ASSERT(callback != nullptr)
            
            m_stack = stack;
            m_callback = callback;
            m_proto = proto;
            
            m_stack->m_proto_listeners_list.prepend(this);
        }
        
        void deinit ()
        {
            m_stack->m_proto_listeners_list.remove(this);
        }
        
    private:
        IpStack *m_stack;
        ProtoListenerCallback *m_callback;
        APrinter::DoubleEndedListNode<ProtoListener> m_listeners_list_node;
        uint8_t m_proto;
    };
    
public:
    class Iface :
        private IpIfaceDriverCallback<Iface>
    {
        friend IpStack;
        
    public:
        using CallbackImpl = Iface;
        
        void init (IpStack *stack, IpIfaceDriver<CallbackImpl> *driver)
        {
            AMBRO_ASSERT(stack != nullptr)
            AMBRO_ASSERT(driver != nullptr)
            
            // Initialize stuffs.
            m_stack = stack;
            m_driver = driver;
            m_have_addr = false;
            m_have_gateway = false;
            
            // Get the MTU.
            m_ip_mtu = APrinter::MinValueU((uint16_t)UINT16_MAX, m_driver->getIpMtu());
            AMBRO_ASSERT(m_ip_mtu >= MinIpIfaceMtu)
            
            // Connect driver callbacks.
            m_driver->setCallback(this);
            
            // Register interface.
            m_stack->m_iface_list.prepend(this);
        }
        
        void deinit ()
        {
            // Unregister interface.
            m_stack->m_iface_list.remove(this);
            
            // Disconnect driver callbacks.
            m_driver->setCallback(nullptr);
        }
        
        void setIp4Addr (IpIfaceIp4AddrSetting value)
        {
            AMBRO_ASSERT(!value.present || value.prefix <= Ip4Addr::Bits)
            
            m_have_addr = value.present;
            if (value.present) {
                m_addr.addr = value.addr;
                m_addr.netmask = Ip4Addr::PrefixMask(value.prefix);
                m_addr.netaddr = m_addr.addr & m_addr.netmask;
                m_addr.bcastaddr = m_addr.netaddr | (Ip4Addr::AllOnesAddr() & ~m_addr.netmask);
                m_addr.prefix = value.prefix;
            }
        }
        
        IpIfaceIp4AddrSetting getIp4Addr ()
        {
            IpIfaceIp4AddrSetting value = {m_have_addr};
            if (m_have_addr) {
                value.prefix = m_addr.prefix;
                value.addr = m_addr.addr;
            }
            return value;
        }
        
        void setIp4Gateway (IpIfaceIp4GatewaySetting value)
        {
            m_have_gateway = value.present;
            if (value.present) {
                m_gateway = value.addr;
            }
        }
        
        IpIfaceIp4GatewaySetting getIp4Gateway ()
        {
            IpIfaceIp4GatewaySetting value = {m_have_gateway};
            if (m_have_gateway) {
                value.addr = m_gateway;
            }
            return value;
        }
        
    public:
        inline bool ip4AddrIsLocal (Ip4Addr addr)
        {
            return m_have_addr && (addr & m_addr.netmask) == m_addr.netaddr;
        }
        
        inline bool ip4AddrIsLocalBcast (Ip4Addr addr)
        {
            return m_have_addr && addr == m_addr.bcastaddr;
        }
        
        inline bool ip4AddrIsLocalAddr (Ip4Addr addr)
        {
            return m_have_addr && addr == m_addr.addr;
        }
        
        // NOTE: Assuming no IP options.
        inline size_t getIp4DgramMtu ()
        {
            return m_ip_mtu - Ip4Header::Size;
        }
        
    private:
        friend IpIfaceDriverCallback<Iface>;
        
        IpIfaceIp4Addrs const * getIp4Addrs ()
        {
            return m_have_addr ? &m_addr : nullptr;
        }
        
        void recvIp4Packet (IpBufRef pkt)
        {
            m_stack->processRecvedIp4Packet(this, pkt);
        }
        
    private:
        APrinter::DoubleEndedListNode<Iface> m_iface_list_node;
        IpStack *m_stack;
        IpIfaceDriver<CallbackImpl> *m_driver;
        size_t m_ip_mtu;
        IpIfaceIp4Addrs m_addr;
        Ip4Addr m_gateway;
        bool m_have_addr;
        bool m_have_gateway;
    };
    
private:
    void processRecvedIp4Packet (Iface *iface, IpBufRef pkt)
    {
        // Check base IP header length.
        if (AMBRO_UNLIKELY(!pkt.hasHeader(Ip4Header::Size))) {
            return;
        }
        
        // Read IP header fields.
        auto ip4_header = Ip4Header::MakeRef(pkt.getChunkPtr());
        uint8_t version_ihl    = ip4_header.get(Ip4Header::VersionIhl());
        uint16_t total_len     = ip4_header.get(Ip4Header::TotalLen());
        uint16_t flags_offset  = ip4_header.get(Ip4Header::FlagsOffset());
        uint8_t ttl            = ip4_header.get(Ip4Header::TimeToLive());
        uint8_t proto          = ip4_header.get(Ip4Header::Protocol());
        Ip4Addr src_addr       = ip4_header.get(Ip4Header::SrcAddr());
        Ip4Addr dst_addr       = ip4_header.get(Ip4Header::DstAddr());
        
        // Check IP version.
        if (AMBRO_UNLIKELY((version_ihl >> Ip4VersionShift) != 4)) {
            return;
        }
        
        // Check header length.
        // We require the entire header to fit into the first buffer.
        uint8_t header_len = (version_ihl & Ip4IhlMask) * 4;
        if (AMBRO_UNLIKELY(header_len < Ip4Header::Size || !pkt.hasHeader(header_len))) {
            return;
        }
        
        // Check total length.
        if (AMBRO_UNLIKELY(total_len < header_len || total_len > pkt.tot_len)) {
            return;
        }
        
        // Sanity check source address - reject broadcast addresses.
        if (AMBRO_UNLIKELY(
            src_addr == Ip4Addr::AllOnesAddr() ||
            iface->ip4AddrIsLocalBcast(src_addr)))
        {
            return;
        }
        
        // Check destination address.
        // Accept only: all-ones broadcast, subnet broadcast, unicast to interface address.
        if (AMBRO_UNLIKELY(
            !iface->ip4AddrIsLocalAddr(dst_addr) &&
            !iface->ip4AddrIsLocalBcast(dst_addr) &&
            dst_addr != Ip4Addr::AllOnesAddr()))
        {
            return;
        }
        
        // Verify IP header checksum.
        uint16_t calc_chksum = IpChksum(ip4_header.data, header_len);
        if (AMBRO_UNLIKELY(calc_chksum != 0)) {
            return;
        }
        
        // Create a reference to the payload.
        IpBufRef dgram = pkt.hideHeader(header_len).subTo(total_len - header_len);
        
        // Check for fragmentation.
        bool more_fragments = (flags_offset & Ip4FlagMF) != 0;
        uint16_t fragment_offset_8b = flags_offset & Ip4OffsetMask;
        if (AMBRO_UNLIKELY(more_fragments || fragment_offset_8b != 0)) {
            // Get the fragment offset in bytes.
            uint16_t fragment_offset = fragment_offset_8b * 8;
            
            // Perform reassembly.
            if (!m_reassembly.reassembleIp4(
                ip4_header.get(Ip4Header::Ident()), src_addr, dst_addr, proto, ttl,
                more_fragments, fragment_offset, ip4_header.data, header_len, dgram))
            {
                return;
            }
            // Continue processing the reassembled datagram.
            // Note, dgram was modified pointing to the reassembled data.
        }
        
        // Create the datagram meta-info struct.
        Ip4DgramMeta meta = {dst_addr, src_addr, ttl, proto, iface};
        
        // Do protocol-specific processing.
        recvIp4Dgram(meta, dgram);
    }
    
    void recvIp4Dgram (Ip4DgramMeta const &meta, IpBufRef dgram)
    {
        if (meta.proto == Ip4ProtocolIcmp) {
            return recvIcmp4Dgram(meta, dgram);
        }
        
        for (ProtoListener *lis = m_proto_listeners_list.first(); lis != nullptr; lis = m_proto_listeners_list.next(lis)) {
            if (lis->m_proto == meta.proto) {
                return lis->m_callback->recvIp4Dgram(meta, dgram);
            }
        }
    }
    
    void recvIcmp4Dgram (Ip4DgramMeta const &meta, IpBufRef dgram)
    {
        // Check ICMP header length.
        if (!dgram.hasHeader(Icmp4Header::Size)) {
            return;
        }
        
        // Read ICMP header fields.
        auto icmp4_header = Icmp4Header::MakeRef(dgram.getChunkPtr());
        uint8_t type    = icmp4_header.get(Icmp4Header::Type());
        uint8_t code    = icmp4_header.get(Icmp4Header::Code());
        uint16_t chksum = icmp4_header.get(Icmp4Header::Chksum());
        
        // Verify ICMP checksum.
        uint16_t calc_chksum = IpChksum(dgram);
        if (calc_chksum != 0) {
            return;
        }
        
        // Get ICMP data by hiding the ICMP header.
        IpBufRef icmp_data = dgram.hideHeader(Icmp4Header::Size);
        
        if (type == Icmp4TypeEchoRequest) {
            // Got echo request, send echo reply.
            auto rest = icmp4_header.get(Icmp4Header::Rest());
            sendIcmp4EchoReply(rest, icmp_data, meta.remote_addr, meta.iface);
        }
    }
    
    void sendIcmp4EchoReply (Icmp4RestType rest, IpBufRef data, Ip4Addr dst_addr, Iface *iface)
    {
        // Can only reply when we have an address assigned.
        if (!iface->m_have_addr) {
            return;
        }
        
        // Allocate memory for headers.
        TxAllocHelper<BufAllocator, Icmp4Header::Size, HeaderBeforeIp4Dgram> dgram_alloc(Icmp4Header::Size);
        
        // Write the ICMP header.
        auto icmp4_header = Icmp4Header::MakeRef(dgram_alloc.getPtr());
        icmp4_header.set(Icmp4Header::Type(),   Icmp4TypeEchoReply);
        icmp4_header.set(Icmp4Header::Code(),   0);
        icmp4_header.set(Icmp4Header::Chksum(), 0);
        icmp4_header.set(Icmp4Header::Rest(),   rest);
        
        // Construct the datagram reference with header and data.
        IpBufNode data_node = data.toNode();
        dgram_alloc.setNext(&data_node, data.tot_len);
        IpBufRef dgram = dgram_alloc.getBufRef();
        
        // Calculate ICMP checksum.
        uint16_t calc_chksum = IpChksum(dgram);
        icmp4_header.set(Icmp4Header::Chksum(), calc_chksum);
        
        // Send the datagram.
        Ip4DgramMeta meta = {iface->m_addr.addr, dst_addr, IcmpTTL, Ip4ProtocolIcmp, iface};
        sendIp4Dgram(meta, dgram);
    }
    
    static size_t round_frag_length (uint8_t header_length, size_t pkt_length)
    {
        return header_length + (((pkt_length - header_length) / 8) * 8);
    }
    
private:
    using IfaceList = APrinter::DoubleEndedList<Iface, &Iface::m_iface_list_node, false>;
    using ProtoListenersList = APrinter::DoubleEndedList<ProtoListener, &ProtoListener::m_listeners_list_node, false>;
    
    Reassembly m_reassembly;
    IfaceList m_iface_list;
    ProtoListenersList m_proto_listeners_list;
    uint16_t m_next_id;
};

APRINTER_ALIAS_STRUCT_EXT(IpStackService, (
    APRINTER_AS_VALUE(size_t, HeaderBeforeIp),
    APRINTER_AS_VALUE(uint8_t, IcmpTTL),
    APRINTER_AS_VALUE(int, MaxReassEntrys),
    APRINTER_AS_VALUE(uint16_t, MaxReassSize)
), (
    APRINTER_ALIAS_STRUCT_EXT(Compose, (
        APRINTER_AS_TYPE(Context),
        APRINTER_AS_TYPE(BufAllocator)
    ), (
        using Params = IpStackService;
        APRINTER_DEF_INSTANCE(Compose, IpStack)
    ))
))

#include <aipstack/EndNamespace.h>

#endif
