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

#include <limits>

#include <aprinter/meta/ServiceUtils.h>
#include <aprinter/meta/ChooseInt.h>
#include <aprinter/base/Assert.h>
#include <aprinter/base/OneOf.h>
#include <aprinter/base/LoopUtils.h>
#include <aprinter/base/Preprocessor.h>
#include <aprinter/base/Hints.h>
#include <aprinter/base/Accessor.h>
#include <aprinter/structure/ObserverNotification.h>
#include <aprinter/structure/LinkModel.h>
#include <aprinter/structure/LinkedList.h>
#include <aprinter/structure/TreeCompare.h>
#include <aprinter/system/TimedEventWrapper.h>
#include <aprinter/misc/ClockUtils.h>

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
APRINTER_DECL_TIMERS_CLASS(EthIpIfaceTimers,
    typename Arg::Context, EthIpIface<Arg>, (ArpTimer))

template <typename Arg>
class EthIpIface : public Arg::Iface,
    private EthIpIfaceTimers<Arg>::Timers,
    private IpEthHw::HwIface
{
    APRINTER_USE_VALS(Arg::Params, (NumArpEntries, ArpProtectCount, HeaderBeforeEth))
    APRINTER_USE_TYPES1(Arg::Params, (TimersStructureService))
    APRINTER_USE_TYPES1(Arg, (Context, Iface))
    
    APRINTER_USE_ONEOF
    APRINTER_USE_TYPES2(APrinter, (Observer, Observable))
    APRINTER_USE_TYPE1(Context, Clock)
    APRINTER_USE_TYPE1(Clock, TimeType)
    APRINTER_USE_TIMERS_CLASS(EthIpIfaceTimers<Arg>, (ArpTimer))
    
    using TheClockUtils = APrinter::ClockUtils<Context>;
    using IpStack = typename Iface::IfaceIpStack;
    
    static size_t const EthArpPktSize = EthHeader::Size + ArpIp4Header::Size;
    
    static_assert(NumArpEntries > 0, "");
    static_assert(ArpProtectCount >= 0, "");
    static_assert(ArpProtectCount <= NumArpEntries, "");
    
    static int const ArpNonProtectCount = NumArpEntries - ArpProtectCount;
    
    using ArpEntryIndexType = APrinter::ChooseIntForMax<NumArpEntries, false>;
    static ArpEntryIndexType const ArpEntryNull =
        std::numeric_limits<ArpEntryIndexType>::max();
    
    // Number of ARP resolution attempts in the QUERY and REFRESHING states.
    static uint8_t const ArpQueryAttempts = 3;
    static uint8_t const ArpRefreshAttempts = 3;
    
    // These need to fit in 4 bits available in ArpEntry::attempts_left.
    static_assert(ArpQueryAttempts <= 15, "");
    static_assert(ArpRefreshAttempts <= 15, "");
    
    // Base ARP response timeout, doubled for each retransmission.
    static TimeType const ArpBaseResponseTimeoutTicks = 1.0 * Clock::time_freq;
    
    // Time after a VALID entry will go to REFRESHING when used.
    static TimeType const ArpValidTimeoutTicks = 60.0 * Clock::time_freq;
    
    struct ArpEntry;
    struct ArpEntriesAccessor;
    struct ArpEntryTimerCompare;
    
    // Link model for ARP entry data structures.
    //struct ArpEntriesLinkModel = APrinter::PointerLinkModel<ArpEntry> {};
    struct ArpEntriesLinkModel : public APrinter::ArrayLinkModelWithAccessor<
        ArpEntry, ArpEntryIndexType, ArpEntryNull, EthIpIface, ArpEntriesAccessor> {};
    using ArpEntryRef = typename ArpEntriesLinkModel::Ref;
    
    // Nodes in ARP entry data structures.
    using ArpEntryListNode = APrinter::LinkedListNode<ArpEntriesLinkModel>;
    using ArpEntryTimersNode = typename TimersStructureService::template Node<ArpEntriesLinkModel>;
    
    // ARP entry states.
    struct ArpEntryState { enum : uint8_t {FREE, QUERY, VALID, REFRESHING}; };
    
    // ARP entry states where the entry timer is allowed to be active.
    inline static auto one_of_timer_entry_states ()
    {
        return OneOf(ArpEntryState::QUERY, ArpEntryState::VALID, ArpEntryState::REFRESHING);
    }
    
    // ARP table entry (in array m_arp_entries)
    struct ArpEntry {
        // Entry state (ArpEntryState::*).
        uint8_t state : 2;
        
        // Whether the entry is weak (seen by chance) or hard (needed at some point).
        // FREE entries must be weak.
        bool weak : 1;
        
        // Whether the entry timer is active (entry is inserted into m_timers_structure).
        bool timer_active : 1;
        
        // QUERY and REFRESHING states: How many more response timeouts before the
        // entry becomes FREE or REFRESHING respectively.
        // VALID state: 1 if the timeout has not elapsed yet, 0 if it has.
        uint8_t attempts_left : 4;
        
        // MAC address of the entry (valid in VALID and REFRESHING states).
        MacAddr mac_addr;
        
        // Node in linked lists (m_used_entries_list or m_free_entries_list).
        ArpEntryListNode list_node;
        
        // Node in the timers data structure (m_timers_structure).
        ArpEntryTimersNode timers_node;
        
        // Time when the entry timeout expires (valid if timer_active).
        TimeType timer_time;
        
        // IP address of the entry (valid in all states except FREE).
        Ip4Addr ip_addr;
        
        // List of send-retry waiters to be notified when resolution is complete.
        IpSendRetry::List retry_list;
    };
    
    // Accessors for data structure nodes.
    struct ArpEntryListNodeAccessor : public APRINTER_MEMBER_ACCESSOR(&ArpEntry::list_node) {};
    struct ArpEntryTimersNodeAccessor : public APRINTER_MEMBER_ACCESSOR(&ArpEntry::timers_node) {};
    
    // Linked list type.
    using ArpEntryList = APrinter::LinkedList<
        ArpEntryListNodeAccessor, ArpEntriesLinkModel, true>;
    
    // Comparison for ARP entry timers using TreeCompare.
    class ArpEntryTimerKeyFuncs;
    struct ArpEntryTimerCompare : public APrinter::TreeCompare<
        ArpEntriesLinkModel, ArpEntryTimerKeyFuncs> {};
    
    // Data structure type for ARP entry timers.
    using TimersStructure = typename TimersStructureService::template Structure<
        ArpEntryTimersNodeAccessor, ArpEntryTimerCompare, ArpEntriesLinkModel>;
    
public:
    void init (IpStack *stack)
    {
        // Initialize timer.
        tim(ArpTimer()).init(Context());
        
        // Initialize ARP observable.
        m_arp_observable.init();
        
        // Remember the (pointer to the) device MAC address.
        m_mac_addr = driverGetMacAddr();
        
        // Initialize data structures.
        m_used_entries_list.init();
        m_free_entries_list.init();
        m_timers_structure.init();
        
        // Initialize ARP entries...
        for (auto &e : m_arp_entries) {
            // State FREE, timer not active.
            e.state = ArpEntryState::FREE;
            e.weak = false; // irrelevant, for efficiency
            e.timer_active = false;
            e.attempts_left = 0; // irrelevant, for efficiency
            
            // Insert to free list.
            m_free_entries_list.append({e, *this}, *this);
            
            // Initialize the send-retry list for this entry.
            e.retry_list.init();
        }
        
        // Initialize the IpStack interface.
        Iface::init(stack);
    }
    
    void deinit ()
    {
        // Deinitialize the IpStack interface.
        Iface::deinit();
        
        // There must be no more ARP observers.
        AMBRO_ASSERT(!m_arp_observable.hasObservers())
        
        // Deinitialize ARP entries...
        for (auto &e : m_arp_entries) {
            // Deinitialize the send-retry list (unlink any requests).
            e.retry_list.deinit();
        }
        
        // Deinitialize timer.
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
    void recvArpPacket (IpBufRef pkt)
    {
        if (AMBRO_UNLIKELY(!pkt.hasHeader(ArpIp4Header::Size))) {
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
        
        uint16_t op_type    = arp_header.get(ArpIp4Header::OpType());
        MacAddr src_mac     = arp_header.get(ArpIp4Header::SrcHwAddr());
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
        // First look if the first used entry is a match, as an optimization.
        ArpEntryRef entry_ref = m_used_entries_list.first(*this);
        
        if (AMBRO_LIKELY(!entry_ref.isNull() && (*entry_ref).ip_addr == ip_addr)) {
            // Fast path, the first used entry is a match.
            AMBRO_ASSERT((*entry_ref).state != ArpEntryState::FREE)
            
            // Make sure the entry is hard as get_arp_entry would do below.
            (*entry_ref).weak = false;
        } else {
            // Slow path: use get_arp_entry, make a hard entry.
            GetArpEntryRes get_res = get_arp_entry(ip_addr, false, entry_ref);
            
            // Did we not get an (old or new) entry for this address?
            if (AMBRO_UNLIKELY(get_res != GetArpEntryRes::GotArpEntry)) {
                // If this is a broadcast IP address, return the broadcast MAC address.
                if (get_res == GetArpEntryRes::BroadcastAddr) {
                    *mac_addr = MacAddr::BroadcastAddr();
                    return IpErr::SUCCESS;
                } else {
                    // Failure, cannot get MAC address.
                    return IpErr::NO_HW_ROUTE;
                }
            }
        }
        
        ArpEntry &entry = *entry_ref;
        
        // Got a VALID or REFRESHING entry?
        if (AMBRO_LIKELY(entry.state >= ArpEntryState::VALID)) {
            // If it is a timed out VALID entry, transition to REFRESHING.
            if (AMBRO_UNLIKELY(entry.attempts_left == 0)) {
                // REFRESHING entry never has attempts_left==0 so no need to check for
                // VALID state in the if. We have a VALID entry and the timer is also
                // not active (needed by set_entry_timer) since attempts_left==0 implies
                // that it has expired already
                AMBRO_ASSERT(entry.state == ArpEntryState::VALID)
                AMBRO_ASSERT(!entry.timer_active)
                
                // Go to REFRESHING state, start timeout, send first unicast request.
                entry.state = ArpEntryState::REFRESHING;
                entry.attempts_left = ArpRefreshAttempts;
                set_entry_timer(entry);
                update_timer();
                send_arp_packet(ArpOpTypeRequest, entry.mac_addr, entry.ip_addr);
            }
            
            // Success, return MAC address.
            *mac_addr = entry.mac_addr;
            return IpErr::SUCCESS;
        } else {
            // If this is a FREE entry, initialize it.
            if (entry.state == ArpEntryState::FREE) {
                // Timer is not active for FREE entries (needed by set_entry_timer).
                AMBRO_ASSERT(!entry.timer_active)
                
                // Go to QUERY state, start timeout, send first broadcast request.
                // NOTE: Entry is already inserted to m_used_entries_list.
                entry.state = ArpEntryState::QUERY;
                entry.attempts_left = ArpQueryAttempts;
                set_entry_timer(entry);
                update_timer();
                send_arp_packet(ArpOpTypeRequest, MacAddr::BroadcastAddr(), ip_addr);
            }
            
            // Add a request to the retry list if a request is supplied.
            entry.retry_list.addRequest(retryReq);
            
            // Return ARP_QUERY error.
            return IpErr::ARP_QUERY;
        }
    }
    
    void save_hw_addr (Ip4Addr ip_addr, MacAddr mac_addr)
    {
        // Sanity check MAC address: not broadcast.
        if (AMBRO_UNLIKELY(mac_addr == MacAddr::BroadcastAddr())) {
            return;
        }
        
        // Get an entry, if a new entry is allocated it will be weak.
        ArpEntryRef entry_ref;
        GetArpEntryRes get_res = get_arp_entry(ip_addr, true, entry_ref);
        
        // Did we get an (old or new) entry for this address?
        if (get_res == GetArpEntryRes::GotArpEntry) {
            ArpEntry &entry = *entry_ref;
            
            // Set entry to VALID state, remember MAC address, start timeout.
            entry.state = ArpEntryState::VALID;
            entry.mac_addr = mac_addr;
            entry.attempts_left = 1;
            clear_entry_timer(entry); // set_entry_timer requires !timer_active
            set_entry_timer(entry);
            update_timer();
            
            // Dispatch send-retry requests.
            // NOTE: The handlers called may end up changing this ARP entry, including
            // reusing it for a different IP address. In that case retry_list.reset()
            // would be called from reset_arp_entry, but that is safe since
            // SentRetry::List supports it.
            entry.retry_list.dispatchRequests();
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
    
    enum class GetArpEntryRes {GotArpEntry, BroadcastAddr, InvalidAddr};
    
    // NOTE: If a FREE entry is obtained, then 'weak' and 'ip_addr' have been
    // set, the entry is already in m_used_entries_list, but the caller must
    // complete initializing it to a non-FREE state. Also, update_timer is needed
    // afterward then.
    GetArpEntryRes get_arp_entry (Ip4Addr ip_addr, bool weak, ArpEntryRef &out_entry)
    {
        // Look for a used entry with this IP address while also collecting
        // some information to be used in case we don't find an entry...
        
        int num_hard = 0;
        ArpEntryRef last_weak_entry_ref = ArpEntryRef::null();
        ArpEntryRef last_hard_entry_ref = ArpEntryRef::null();
        
        ArpEntryRef entry_ref = m_used_entries_list.first(*this);
        
        while (!entry_ref.isNull()) {
            ArpEntry &entry = *entry_ref;
            AMBRO_ASSERT(entry.state != ArpEntryState::FREE)
            
            if (entry.ip_addr == ip_addr) {
                break;
            }
            
            if (entry.weak) {
                last_weak_entry_ref = entry_ref;
            } else {
                num_hard++;
                last_hard_entry_ref = entry_ref;
            }
            
            entry_ref = m_used_entries_list.next(entry_ref, *this);
        }
        
        if (AMBRO_LIKELY(!entry_ref.isNull())) {
            // We found an entry with this IP address.
            // If this is a hard request, make sure the entry is hard.
            if (!weak) {
                (*entry_ref).weak = false;
            }
        } else {
            // We did not find an entry with this IP address.
            // First do some checks of the IP address...
            
            // If this is the all-ones address, return the broadcast MAC address.
            if (ip_addr == Ip4Addr::AllOnesAddr()) {
                return GetArpEntryRes::BroadcastAddr;
            }
            
            // Check for zero IP address.
            if (ip_addr == Ip4Addr::ZeroAddr()) {
                return GetArpEntryRes::InvalidAddr;
            }
            
            // Check if the interface has an IP address assigned.
            IpIfaceIp4Addrs const *ifaddr = Iface::getIp4AddrsFromDriver();
            if (ifaddr == nullptr) {
                return GetArpEntryRes::InvalidAddr;
            }
            
            // Check if the given IP address is in the subnet.
            if ((ip_addr & ifaddr->netmask) != ifaddr->netaddr) {
                return GetArpEntryRes::InvalidAddr;
            }
            
            // If this is the local broadcast address, return the broadcast MAC address.
            if (ip_addr == ifaddr->bcastaddr) {
                return GetArpEntryRes::BroadcastAddr;
            }
            
            // Check if there is a FREE entry available.
            entry_ref = m_free_entries_list.first(*this);
            
            if (!entry_ref.isNull()) {
                // Got a FREE entry.
                AMBRO_ASSERT((*entry_ref).state == ArpEntryState::FREE)
                AMBRO_ASSERT(!(*entry_ref).timer_active)
                AMBRO_ASSERT(!(*entry_ref).retry_list.hasRequests())
                
                // Move the entry from the free list to the used list.
                m_free_entries_list.removeFirst(*this);
                m_used_entries_list.prepend(entry_ref, *this);
            } else {
                // There is no FREE entry available, we will recycle a used entry.
                // Determine whether to recycle a weak or hard entry.
                bool use_weak;
                if (weak) {
                    use_weak = !(num_hard > ArpProtectCount || last_weak_entry_ref.isNull());
                } else {
                    int num_weak = NumArpEntries - num_hard;
                    use_weak = (num_weak > ArpNonProtectCount || last_hard_entry_ref.isNull());
                }
                
                // Get the entry to be recycled.
                entry_ref = use_weak ? last_weak_entry_ref : last_hard_entry_ref;
                AMBRO_ASSERT(!entry_ref.isNull())
                
                // Reset the entry, but keep it in the used list.
                reset_arp_entry(*entry_ref, true);
            }
            
            // NOTE: The entry is in FREE state now but in the used list.
            // The caller is responsible to set a non-FREE state ensuring
            // that the state corresponds with the list membership again.
            
            // Set IP address and weak flag.
            (*entry_ref).ip_addr = ip_addr;
            (*entry_ref).weak = weak;
        }
        
        // Bump to entry to the front of the used entries list.
        if (!(entry_ref == m_used_entries_list.first(*this))) {
            m_used_entries_list.remove(entry_ref, *this);
            m_used_entries_list.prepend(entry_ref, *this);
        }
        
        // Return the entry.
        out_entry = entry_ref;
        return GetArpEntryRes::GotArpEntry;
    }
    
    // NOTE: update_timer is needed after this.
    void reset_arp_entry (ArpEntry &entry, bool leave_in_used_list)
    {
        AMBRO_ASSERT(entry.state != ArpEntryState::FREE)
        
        // Make sure the entry timeout is not active.
        clear_entry_timer(entry);
        
        // Set the entry to FREE state.
        entry.state = ArpEntryState::FREE;
        
        // Reset the send-retry list for the entry.
        entry.retry_list.reset();
        
        // Move from used list to free list, unless requested not to.
        if (!leave_in_used_list) {
            m_used_entries_list.remove({entry, *this}, *this);
            m_free_entries_list.prepend({entry, *this}, *this);
        }
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
    
    // Set tne ARP entry timeout based on the entry state and attempts_left.
    void set_entry_timer (ArpEntry &entry)
    {
        AMBRO_ASSERT(!entry.timer_active)
        AMBRO_ASSERT(entry.state == one_of_timer_entry_states())
        AMBRO_ASSERT(entry.state != ArpEntryState::VALID || entry.attempts_left == 1)
        
        // Determine the relative timeout...
        TimeType timeout;
        if (entry.state == ArpEntryState::VALID) {
            // VALID entry (not expired yet, i.e. with attempts_left==1).
            timeout = ArpValidTimeoutTicks;
        } else {
            // QUERY or REFRESHING entry, compute timeout with exponential backoff.
            uint8_t attempts = (entry.state == ArpEntryState::QUERY) ?
                ArpQueryAttempts : ArpRefreshAttempts;
            AMBRO_ASSERT(entry.attempts_left <= attempts)
            timeout = ArpBaseResponseTimeoutTicks << (attempts - entry.attempts_left);
        }
        
        // Get the current time.
        TimeType now = Clock::getTime(Context());
        
        // Update the reference time to maximize the useful future range:
        // - If the timers structure is empty, set the reference time to now.
        // - Otherwise, if the current time is in the future range after the
        //   reference time (need this sanity check), set the reference time
        //   to the smaller of the current time and the first timeout.
        if (m_timers_structure.isEmpty()) {
            m_timers_ref_time = now;
        }
        else if (TheClockUtils::timeGreaterOrEqual(now, m_timers_ref_time)) {
            ArpEntry &first_timer = *m_timers_structure.first(*this);
            if (TheClockUtils::timeGreaterOrEqual(now, first_timer.timer_time)) {
                m_timers_ref_time = first_timer.timer_time;
            } else {
                m_timers_ref_time = now;
            }
        }
        
        // Calculate the absolute timeout time.
        TimeType abs_time = now + timeout;
        
        // If this absolute time is in the past relative to the reference time,
        // bump it up to the reference time. This is needed because time comparisons
        // (ArpEntryTimerKeyFuncs::CompareKeys) work correctly only when the
        // active timers span less than half of the TimeType range.
        if (!TheClockUtils::timeGreaterOrEqual(abs_time, m_timers_ref_time)) {
            abs_time = m_timers_ref_time;
        }
        
        // Set timer_active flag, store absolute time.
        entry.timer_active = true;
        entry.timer_time = abs_time;
        
        // Insert entry to m_timers_structure.
        m_timers_structure.insert({entry, *this}, *this);
    }
    
    // Make sure the entry timeout is not active.
    // NOTE: update_timer is needed after this.
    void clear_entry_timer (ArpEntry &entry)
    {
        if (entry.timer_active) {
            m_timers_structure.remove({entry, *this}, *this);
            entry.timer_active = false;
        }
    }
    
    // Make sure the ArpTimer is set to expire at the first entry timeout,
    // or unset it if there is no active entry timeout. This must be called
    // after every insertion/removal of an entry to m_timers_structure.
    void update_timer ()
    {
        ArpEntryRef first_ref = m_timers_structure.first(*this);
        
        if (first_ref.isNull()) {
            tim(ArpTimer()).unset(Context());
        } else {
            ArpEntry &entry = *first_ref;
            AMBRO_ASSERT(entry.timer_active)
            AMBRO_ASSERT(entry.state == one_of_timer_entry_states())
            
            tim(ArpTimer()).appendAfter(Context(), entry.timer_time);
        }
    }
    
    void timerExpired (ArpTimer, Context)
    {
        AMBRO_ASSERT(!m_timers_structure.isEmpty())
        
        // Get the current time.
        TimeType now = Clock::getTime(Context());
        
        // Determine the time based on which we consider timers expired.
        // The else case will dispatches all timers, after the clock jumped into
        // the left half of the reference time, because in this case comparing
        // timers to "now" the usual way may yield incorrect results.
        TimeType dispatch_time;
        if (TheClockUtils::timeGreaterOrEqual(now, m_timers_ref_time)) {
            dispatch_time = now;
        } else {
            dispatch_time = m_timers_ref_time + std::numeric_limits<TimeType>::max() / 2;
        }
        
        // Set the timer_time of expired timers to now. This allows us to
        // safely update m_timers_ref_time before dispatching any timer, and
        // effectively allows new timers to be started in the full future
        // range relative to now.
        ArpEntryRef timer_ref = m_timers_structure.findFirstLesserOrEqual(dispatch_time, *this);
        if (!timer_ref.isNull()) {
            do {
                ArpEntry &entry = *timer_ref;
                AMBRO_ASSERT(entry.timer_active)
                AMBRO_ASSERT(entry.state == one_of_timer_entry_states())
                
                entry.timer_time = now;
                
                timer_ref = m_timers_structure.findNextLesserOrEqual(dispatch_time, timer_ref, *this);
            } while (!timer_ref.isNull());
            
            m_timers_structure.assertValidHeap(*this);
        }
                
        // Update the reference time to 'now'. This is safe because the above updates
        // ensured that all active timers (including ones about to be dispatched)
        // belong to the future range relative to 'now'.
        m_timers_ref_time = now;
        
        // Dispatch expired timers...
        while (!(timer_ref = m_timers_structure.first(*this)).isNull()) {
            ArpEntry &entry = *timer_ref;
            AMBRO_ASSERT(entry.timer_active)
            AMBRO_ASSERT(entry.state == one_of_timer_entry_states())
            
            // If the timer is not yet expired, stop.
            if (entry.timer_time != m_timers_ref_time) {
                break;
            }
            
            // Make the entry timeout no longer active.
            m_timers_structure.remove({entry, *this}, *this);
            entry.timer_active = false;
            
            // Perform timeout processing for the entry.
            handle_entry_timeout(entry);
        }
        
        // Set the ArpTimer for the next expiration or unset.
        update_timer();
    }
    
    void handle_entry_timeout (ArpEntry &entry)
    {
        AMBRO_ASSERT(entry.state != ArpEntryState::FREE)
        AMBRO_ASSERT(!entry.timer_active)
        
        // Check if the IP address is still consistent with the interface
        // address settings. If not, reset the ARP entry.
        IpIfaceIp4Addrs const *ifaddr = Iface::getIp4AddrsFromDriver();        
        if (ifaddr == nullptr ||
            (entry.ip_addr & ifaddr->netmask) != ifaddr->netaddr ||
            entry.ip_addr == ifaddr->bcastaddr)
        {
            reset_arp_entry(entry, false);
            return;
        }
        
        switch (entry.state) {
            case ArpEntryState::QUERY: {
                // QUERY state: Decrement attempts_left then either reset the entry
                // in case of last attempt, else retransmit the broadcast query.
                AMBRO_ASSERT(entry.attempts_left > 0)
                
                entry.attempts_left--;
                if (entry.attempts_left == 0) {
                    reset_arp_entry(entry, false);
                } else {
                    set_entry_timer(entry);
                    send_arp_packet(ArpOpTypeRequest, MacAddr::BroadcastAddr(), entry.ip_addr);
                }
            } break;
            
            case ArpEntryState::VALID: {
                // VALID state: Set attempts_left to 0 to consider the entry expired.
                // Upon next use the entry it will go to REFRESHING state.
                AMBRO_ASSERT(entry.attempts_left == 1)
                
                entry.attempts_left = 0;
            } break;
            
            case ArpEntryState::REFRESHING: {
                // REFRESHING state: Decrement attempts_left then either move
                // the entry to QUERY state (and send the first broadcast query),
                // else retransmit the unicast query.
                AMBRO_ASSERT(entry.attempts_left > 0)
                
                entry.attempts_left--;
                if (entry.attempts_left == 0) {
                    entry.state = ArpEntryState::QUERY;
                    entry.attempts_left = ArpQueryAttempts;
                    send_arp_packet(ArpOpTypeRequest, MacAddr::BroadcastAddr(), entry.ip_addr);
                } else {
                    send_arp_packet(ArpOpTypeRequest, entry.mac_addr, entry.ip_addr);
                }
                set_entry_timer(entry);
            } break;
            
            default:
                AMBRO_ASSERT(false);
        }
    }
    
    // KeyFuncs class for TreeCompare used for m_timers_structure.
    class ArpEntryTimerKeyFuncs
    {
    public:
        // Get the key (timer time) of an entry.
        static TimeType GetKeyOfEntry (ArpEntry const &entry)
        {
            return entry.timer_time;
        }
        
        // Compare two keys (times).
        static int CompareKeys (TimeType time1, TimeType time2)
        {
            return !TheClockUtils::timeGreaterOrEqual(time1, time2) ? -1 : (time1 == time2) ? 0 : 1;
        }
    };
    
private:
    Observable m_arp_observable;
    MacAddr const *m_mac_addr;
    ArpEntryList m_used_entries_list;
    ArpEntryList m_free_entries_list;
    TimersStructure m_timers_structure;
    TimeType m_timers_ref_time;
    EthHeader::Ref m_rx_eth_header;
    ArpEntry m_arp_entries[NumArpEntries];
    
    struct ArpEntriesAccessor : public APRINTER_MEMBER_ACCESSOR(&EthIpIface::m_arp_entries) {};
};

APRINTER_ALIAS_STRUCT_EXT(EthIpIfaceService, (
    APRINTER_AS_VALUE(int,    NumArpEntries),
    APRINTER_AS_VALUE(int,    ArpProtectCount),
    APRINTER_AS_VALUE(size_t, HeaderBeforeEth),
    APRINTER_AS_TYPE(TimersStructureService)
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
