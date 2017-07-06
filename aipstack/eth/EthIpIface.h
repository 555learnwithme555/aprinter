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

#ifndef APRINTER_IPSTACK_ETH_IP_IFACE_H
#define APRINTER_IPSTACK_ETH_IP_IFACE_H

#include <stddef.h>
#include <stdint.h>

#include <aprinter/meta/ServiceUtils.h>
#include <aprinter/meta/ChooseInt.h>
#include <aprinter/base/Assert.h>
#include <aprinter/base/OneOf.h>
#include <aprinter/base/LoopUtils.h>
#include <aprinter/base/Preprocessor.h>
#include <aprinter/base/Hints.h>
#include <aprinter/structure/ObserverNotification.h>
#include <aprinter/system/TimedEventWrapper.h>

#include <aipstack/misc/Struct.h>
#include <aipstack/misc/Buf.h>
#include <aipstack/misc/SendRetry.h>
#include <aipstack/misc/TxAllocHelper.h>
#include <aipstack/misc/Err.h>
#include <aipstack/proto/IpAddr.h>
#include <aipstack/proto/EthernetProto.h>
#include <aipstack/proto/ArpProto.h>
#include <aipstack/ip/IpStack.h>
#include <aipstack/ip/hw/IpEthHw.h>

#include <aipstack/BeginNamespace.h>

struct EthIfaceState {
    bool link_up;
};

template <typename Arg>
class EthIpIface;

template <typename Arg>
APRINTER_DECL_TIMERS_CLASS(EthIpIfaceTimers, typename Arg::Context, EthIpIface<Arg>, (ArpTimer))

template <typename Arg>
class EthIpIface : public Arg::Iface,
    private EthIpIfaceTimers<Arg>::Timers,
    private IpEthHw::HwIface
{
    APRINTER_USE_VALS(Arg::Params, (NumArpEntries, ArpProtectCount, HeaderBeforeEth))
    APRINTER_USE_TYPES1(Arg, (Context, Iface))
    
    APRINTER_USE_TYPES2(APrinter, (Observer, Observable))
    APRINTER_USE_TYPE1(Context, Clock)
    APRINTER_USE_TYPE1(Clock, TimeType)
    
    using IpStack = typename Iface::IfaceIpStack;
    
    APRINTER_USE_TIMERS_CLASS(EthIpIfaceTimers<Arg>, (ArpTimer))
    
    static size_t const EthArpPktSize = EthHeader::Size + ArpIp4Header::Size;
    
    static_assert(NumArpEntries > 0, "");
    static_assert(ArpProtectCount >= 0, "");
    static_assert(ArpProtectCount <= NumArpEntries, "");
    
    static int const ArpNonProtectCount = NumArpEntries - ArpProtectCount;
    
    using ArpEntryIndexType = APrinter::ChooseIntForMax<NumArpEntries, true>;
    
    static TimeType const ArpTimerTicks = 1.0 * Clock::time_freq;
    
    static uint8_t const ArpQueryTimeout = 3;
    static uint8_t const ArpValidTimeout = 60;
    static uint8_t const ArpRefreshTimeout = 3;
    
public:
    void init (IpStack *stack)
    {
        tim(ArpTimer()).init(Context());
        m_arp_observable.init();
        
        m_mac_addr = driverGetMacAddr();
        
        m_first_arp_entry = 0;
        
        for (int i : APrinter::LoopRangeAuto(NumArpEntries)) {
            auto &e = m_arp_entries[i];
            e.next = (i < NumArpEntries-1) ? i+1 : -1;
            e.state = ArpEntryState::FREE;
            e.weak = true;
            e.retry_list.init();
        }
        
        tim(ArpTimer()).appendAfter(Context(), ArpTimerTicks);
        
        Iface::init(stack);
    }
    
    void deinit ()
    {
        Iface::deinit();
        
        AMBRO_ASSERT(!m_arp_observable.hasObservers())
        
        for (auto &e : m_arp_entries) {
            e.retry_list.deinit();
        }
        
        tim(ArpTimer()).deinit(Context());
    }
    
protected:
    // These functions are implemented or called by the Ethernet driver.
    
    virtual MacAddr const * driverGetMacAddr () = 0;
    
    virtual size_t driverGetEthMtu () = 0;
    
    virtual IpErr driverSendFrame (IpBufRef frame) = 0;
    
    virtual EthIfaceState driverGetEthState () = 0;
    
    void recvFrameFromDriver (IpBufRef frame)
    {
        if (AMBRO_UNLIKELY(!frame.hasHeader(EthHeader::Size))) {
            return;
        }
        
        m_rx_eth_header = EthHeader::MakeRef(frame.getChunkPtr());
        uint16_t ethtype = m_rx_eth_header.get(EthHeader::EthType());
        
        auto pkt = frame.hideHeader(EthHeader::Size);
        
        if (AMBRO_LIKELY(ethtype == EthTypeIpv4)) {
            Iface::recvIp4PacketFromDriver(pkt);
        }
        else if (ethtype == EthTypeArp) {
            recvArpPacket(pkt);
        }
    }
    
    inline void ethStateChangedFromDriver ()
    {
        Iface::stateChangedFromDriver();
    }
    
private:
    size_t driverGetIpMtu () override final
    {
        size_t eth_mtu = driverGetEthMtu();
        AMBRO_ASSERT(eth_mtu >= EthHeader::Size)
        return eth_mtu - EthHeader::Size;
    }
    
    IpErr driverSendIp4Packet (IpBufRef pkt, Ip4Addr ip_addr,
                               IpSendRetry::Request *retryReq) override final
    {
        MacAddr dst_mac;
        IpErr resolve_err = resolve_hw_addr(ip_addr, &dst_mac, retryReq);
        if (AMBRO_UNLIKELY(resolve_err != IpErr::SUCCESS)) {
            return resolve_err;
        }
        
        IpBufRef frame;
        if (AMBRO_UNLIKELY(!pkt.revealHeader(EthHeader::Size, &frame))) {
            return IpErr::NO_HEADER_SPACE;
        }
        
        auto eth_header = EthHeader::MakeRef(frame.getChunkPtr());
        eth_header.set(EthHeader::DstMac(),  dst_mac);
        eth_header.set(EthHeader::SrcMac(),  *m_mac_addr);
        eth_header.set(EthHeader::EthType(), EthTypeIpv4);
        
        return driverSendFrame(frame);
    }
    
    IpHwType driverGetHwType () override final
    {
        return IpHwType::Ethernet;
    }
    
    void * driverGetHwIface () override final
    {
        return static_cast<IpEthHw::HwIface *>(this);
    }
    
    IpIfaceDriverState driverGetState () override final
    {
        EthIfaceState eth_state = driverGetEthState();
        
        IpIfaceDriverState state = {};
        state.link_up = eth_state.link_up;
        return state;
    }
    
private: // IpEthHw::HwIface
    MacAddr getMacAddr () override final
    {
        return *m_mac_addr;
    }
    
    EthHeader::Ref getRxEthHeader () override final
    {
        return m_rx_eth_header;
    }
    
    IpErr sendArpQuery (Ip4Addr ip_addr) override final
    {
        return send_arp_packet(ArpOpTypeRequest, MacAddr::BroadcastAddr(), ip_addr);
    }
    
    Observable & getArpObservable () override final
    {
        return m_arp_observable;
    }
    
private:
    struct ArpEntryState { enum : uint8_t {FREE, QUERY, VALID, REFRESHING}; };
    
    struct ArpEntry {
        ArpEntryIndexType next;
        uint8_t state : 2;
        bool weak : 1;
        uint8_t time_left;
        MacAddr mac_addr;
        Ip4Addr ip_addr;
        IpSendRetry::List retry_list;
    };
    
    void recvArpPacket (IpBufRef pkt)
    {
        if (!pkt.hasHeader(ArpIp4Header::Size)) {
            return;
        }
        auto arp_header = ArpIp4Header::MakeRef(pkt.getChunkPtr());
        
        if (arp_header.get(ArpIp4Header::HwType())       != ArpHwTypeEth  ||
            arp_header.get(ArpIp4Header::ProtoType())    != EthTypeIpv4   ||
            arp_header.get(ArpIp4Header::HwAddrLen())    != MacAddr::Size ||
            arp_header.get(ArpIp4Header::ProtoAddrLen()) != Ip4Addr::Size)
        {
            return;
        }
        
        MacAddr src_mac = arp_header.get(ArpIp4Header::SrcHwAddr());
        
        uint16_t op_type    = arp_header.get(ArpIp4Header::OpType());
        Ip4Addr src_ip_addr = arp_header.get(ArpIp4Header::SrcProtoAddr());
        
        save_hw_addr(src_ip_addr, src_mac);
        
        if (op_type == ArpOpTypeRequest) {
            IpIfaceIp4Addrs const *ifaddr = Iface::getIp4AddrsFromDriver();
            
            if (ifaddr != nullptr &&
                arp_header.get(ArpIp4Header::DstProtoAddr()) == ifaddr->addr)
            {
                send_arp_packet(ArpOpTypeReply, src_mac, src_ip_addr);
            }
        }
    }
    
    AMBRO_ALWAYS_INLINE
    IpErr resolve_hw_addr (Ip4Addr ip_addr, MacAddr *mac_addr, IpSendRetry::Request *retryReq)
    {
        ArpEntry *entry = &m_arp_entries[m_first_arp_entry];
        if (AMBRO_LIKELY(entry->state != ArpEntryState::FREE && entry->ip_addr == ip_addr)) {
            // Fast path: the first entry is a match.
            entry->weak = false;
        } else {
            // Slow path: use get_arp_entry.
            GetArpEntryRes get_res = get_arp_entry(ip_addr, false, &entry);
            
            if (AMBRO_UNLIKELY(get_res != GetArpEntryRes::GotArpEntry)) {
                if (get_res == GetArpEntryRes::BroadcastAddr) {
                    *mac_addr = MacAddr::BroadcastAddr();
                    return IpErr::SUCCESS;
                }
                return IpErr::NO_HW_ROUTE;
            }
        }
        
        if (AMBRO_LIKELY(entry->state >= ArpEntryState::VALID)) {
            // Note: REFRESHING entry never has time_left==0 so no need to check
            // for VALID state here.
            if (AMBRO_UNLIKELY(entry->time_left == 0)) {
                entry->state = ArpEntryState::REFRESHING;
                entry->time_left = ArpRefreshTimeout;
                send_arp_packet(ArpOpTypeRequest, entry->mac_addr, entry->ip_addr);
            }
            
            *mac_addr = entry->mac_addr;
            return IpErr::SUCCESS;
        }
        else {
            if (entry->state == ArpEntryState::FREE) {
                entry->state = ArpEntryState::QUERY;
                entry->time_left = ArpQueryTimeout;
                send_arp_packet(ArpOpTypeRequest, MacAddr::BroadcastAddr(), ip_addr);
            }
            
            entry->retry_list.addRequest(retryReq);
            
            return IpErr::ARP_QUERY;
        }
    }
    
    void save_hw_addr (Ip4Addr ip_addr, MacAddr mac_addr)
    {
        ArpEntry *entry;
        GetArpEntryRes get_res = get_arp_entry(ip_addr, true, &entry);
        
        if (get_res == GetArpEntryRes::GotArpEntry) {
            entry->state = ArpEntryState::VALID;
            entry->time_left = ArpValidTimeout;
            entry->mac_addr = mac_addr;
            
            // Dispatch send-retry requests.
            // NOTE: The handlers called may end up changing this ARP entry, including
            // reusing it for a different IP address. In that case retry_list.reset()
            // would be called from reset_arp_entry, but that is safe since SentRetry::List
            // supports it.
            entry->retry_list.dispatchRequests();
        }
        
        // Notify the ARP observers so long as the address is not
        // obviously bad. It is important to do this even if no ARP
        // entry was obtained, since that will not happen if the
        // interface has no IP address configured, which is exactly
        // when DHCP needs to be notified.
        if (ip_addr != Ip4Addr::AllOnesAddr() && ip_addr != Ip4Addr::ZeroAddr()) {
            m_arp_observable.notifyKeepObservers([&](Observer &observer) {
                IpEthHw::HwIface::notifyArpObserver(observer, ip_addr, mac_addr);
            });
        }
    }
    
    enum class GetArpEntryRes { GotArpEntry, BroadcastAddr, InvalidAddr };
    
    GetArpEntryRes get_arp_entry (Ip4Addr ip_addr, bool weak, ArpEntry **out_entry)
    {
        ArpEntry *e;
        
        int index = m_first_arp_entry;
        int prev_index = -1;
        
        int num_hard = 0;
        int last_weak_index = -1;
        int last_weak_prev_index;
        int last_hard_index = -1;
        int last_hard_prev_index;
        
        while (index >= 0) {
            AMBRO_ASSERT(index < NumArpEntries)
            e = &m_arp_entries[index];
            
            if (e->state != ArpEntryState::FREE && e->ip_addr == ip_addr) {
                break;
            }
            
            if (e->weak) {
                last_weak_index = index;
                last_weak_prev_index = prev_index;
            } else {
                num_hard++;
                last_hard_index = index;
                last_hard_prev_index = prev_index;
            }
            
            prev_index = index;
            index = e->next;
        }
        
        if (AMBRO_LIKELY(index >= 0)) {
            if (!weak) {
                e->weak = false;
            }
        } else {
            if (ip_addr == Ip4Addr::AllOnesAddr()) {
                return GetArpEntryRes::BroadcastAddr;
            }
            
            if (ip_addr == Ip4Addr::ZeroAddr()) {
                return GetArpEntryRes::InvalidAddr;
            }
            
            IpIfaceIp4Addrs const *ifaddr = Iface::getIp4AddrsFromDriver();
            if (ifaddr == nullptr) {
                return GetArpEntryRes::InvalidAddr;
            }
            
            if ((ip_addr & ifaddr->netmask) != ifaddr->netaddr) {
                return GetArpEntryRes::InvalidAddr;
            }
            
            if (ip_addr == ifaddr->bcastaddr) {
                return GetArpEntryRes::BroadcastAddr;
            }
            
            bool use_weak;
            if (last_weak_index >= 0 && m_arp_entries[last_weak_index].state == ArpEntryState::FREE) {
                use_weak = true;
            } else {
                if (weak) {
                    use_weak = !(num_hard > ArpProtectCount || last_weak_index < 0);
                } else {
                    int num_weak = NumArpEntries - num_hard;
                    use_weak = (num_weak > ArpNonProtectCount || last_hard_index < 0);
                }
            }
            
            if (use_weak) {
                index = last_weak_index;
                prev_index = last_weak_prev_index;
            } else {
                index = last_hard_index;
                prev_index = last_hard_prev_index;
            }
            
            AMBRO_ASSERT(index >= 0)
            e = &m_arp_entries[index];
            
            reset_arp_entry(e);
            e->ip_addr = ip_addr;
            e->weak = weak;
        }
        
        if (prev_index >= 0) {
            m_arp_entries[prev_index].next = e->next;
            e->next = m_first_arp_entry;
            m_first_arp_entry = index;
        }
        
        *out_entry = e;
        return GetArpEntryRes::GotArpEntry;
    }
    
    static void reset_arp_entry (ArpEntry *e)
    {
        e->state = ArpEntryState::FREE;
        e->retry_list.reset();
    }
    
    IpErr send_arp_packet (uint16_t op_type, MacAddr dst_mac, Ip4Addr dst_ipaddr)
    {
        TxAllocHelper<EthArpPktSize, HeaderBeforeEth> frame_alloc(EthArpPktSize);
        
        auto eth_header = EthHeader::MakeRef(frame_alloc.getPtr());
        eth_header.set(EthHeader::DstMac(), dst_mac);
        eth_header.set(EthHeader::SrcMac(), *m_mac_addr);
        eth_header.set(EthHeader::EthType(), EthTypeArp);
        
        IpIfaceIp4Addrs const *ifaddr = Iface::getIp4AddrsFromDriver();
        Ip4Addr src_addr = (ifaddr != nullptr) ? ifaddr->addr : Ip4Addr::ZeroAddr();
        
        auto arp_header = ArpIp4Header::MakeRef(frame_alloc.getPtr() + EthHeader::Size);
        arp_header.set(ArpIp4Header::HwType(),       ArpHwTypeEth);
        arp_header.set(ArpIp4Header::ProtoType(),    EthTypeIpv4);
        arp_header.set(ArpIp4Header::HwAddrLen(),    MacAddr::Size);
        arp_header.set(ArpIp4Header::ProtoAddrLen(), Ip4Addr::Size);
        arp_header.set(ArpIp4Header::OpType(),       op_type);
        arp_header.set(ArpIp4Header::SrcHwAddr(),    *m_mac_addr);
        arp_header.set(ArpIp4Header::SrcProtoAddr(), src_addr);
        arp_header.set(ArpIp4Header::DstHwAddr(),    dst_mac);
        arp_header.set(ArpIp4Header::DstProtoAddr(), dst_ipaddr);
        
        return driverSendFrame(frame_alloc.getBufRef());
    }
    
    void timerExpired (ArpTimer, Context)
    {
        tim(ArpTimer()).appendAfter(Context(), ArpTimerTicks);
        
        IpIfaceIp4Addrs const *ifaddr = Iface::getIp4AddrsFromDriver();
        
        for (auto &e : m_arp_entries) {
            if (e.state == ArpEntryState::FREE) {
                continue;
            }
            
            if (ifaddr == nullptr ||
                (e.ip_addr & ifaddr->netmask) != ifaddr->netaddr ||
                e.ip_addr == ifaddr->bcastaddr)
            {
                reset_arp_entry(&e);
                continue;
            }
            
            switch (e.state) {
                case ArpEntryState::QUERY: {
                    e.time_left--;
                    if (e.time_left == 0) {
                        reset_arp_entry(&e);
                    } else {
                        send_arp_packet(ArpOpTypeRequest, MacAddr::BroadcastAddr(), e.ip_addr);
                    }
                } break;
                
                case ArpEntryState::VALID: {
                    if (e.time_left > 0) {
                        e.time_left--;
                    }
                } break;
                
                case ArpEntryState::REFRESHING: {
                    e.time_left--;
                    if (e.time_left == 0) {
                        e.state = ArpEntryState::QUERY;
                        e.time_left = ArpQueryTimeout;
                        send_arp_packet(ArpOpTypeRequest, MacAddr::BroadcastAddr(), e.ip_addr);
                    } else {
                        send_arp_packet(ArpOpTypeRequest, e.mac_addr, e.ip_addr);
                    }
                } break;
                
                default:
                    AMBRO_ASSERT(false);
            }
        }
    }
    
private:
    Observable m_arp_observable;
    MacAddr const *m_mac_addr;
    ArpEntryIndexType m_first_arp_entry;
    EthHeader::Ref m_rx_eth_header;
    ArpEntry m_arp_entries[NumArpEntries];
};

APRINTER_ALIAS_STRUCT_EXT(EthIpIfaceService, (
    APRINTER_AS_VALUE(int,    NumArpEntries),
    APRINTER_AS_VALUE(int,    ArpProtectCount),
    APRINTER_AS_VALUE(size_t, HeaderBeforeEth)
), (
    APRINTER_ALIAS_STRUCT_EXT(Compose, (
        APRINTER_AS_TYPE(Context),
        APRINTER_AS_TYPE(Iface)
    ), (
        using Params = EthIpIfaceService;
        APRINTER_DEF_INSTANCE(Compose, EthIpIface)
    ))
))

#include <aipstack/EndNamespace.h>

#endif
