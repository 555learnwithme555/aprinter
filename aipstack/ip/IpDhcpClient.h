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

#ifndef APRINTER_IPSTACK_IP_DHCP_CLIENT_H
#define APRINTER_IPSTACK_IP_DHCP_CLIENT_H

#include <stdint.h>
#include <string.h>

#include <aprinter/meta/ServiceUtils.h>
#include <aprinter/meta/MinMax.h>
#include <aprinter/base/Preprocessor.h>
#include <aprinter/base/Assert.h>
#include <aprinter/base/Hints.h>
#include <aprinter/base/OneOf.h>
#include <aprinter/misc/ClockUtils.h>
#include <aprinter/system/TimedEventWrapper.h>

#include <aipstack/misc/Buf.h>
#include <aipstack/misc/Chksum.h>
#include <aipstack/proto/IpAddr.h>
#include <aipstack/proto/Ip4Proto.h>
#include <aipstack/proto/DhcpProto.h>
#include <aipstack/proto/EthernetProto.h>
#include <aipstack/ip/IpStack.h>
#include <aipstack/ip/hw/IpHwCommon.h>
#include <aipstack/ip/hw/IpEthHw.h>

#include <aipstack/BeginNamespace.h>

template <typename Arg>
class IpDhcpClient;

template <typename Arg>
APRINTER_DECL_TIMERS_CLASS(IpDhcpClientTimers, typename Arg::Context, IpDhcpClient<Arg>, (DhcpTimer))

template <typename Arg>
class IpDhcpClient :
    private IpDhcpClientTimers<Arg>::Timers,
    private Arg::IpStack::IfaceListener
{
    APRINTER_USE_TYPES1(Arg, (Context, IpStack))
    APRINTER_USE_TYPES1(Context, (Clock))
    APRINTER_USE_TYPES1(Clock, (TimeType))
    APRINTER_USE_TYPES1(IpStack, (Ip4DgramMeta, Iface))
    APRINTER_USE_ONEOF
    using TheClockUtils = APrinter::ClockUtils<Context>;
    
    APRINTER_USE_TIMERS_CLASS(IpDhcpClientTimers<Arg>, (DhcpTimer)) 
    
    enum class DhcpState {
        Resetting,
        SentDiscover,
        SentRequest,
        Finished,
        Renewing
    };
    
    static uint8_t const MaxDnsServers = 4;
    
    static uint8_t const XidReuseMax = 8;
    
    static uint16_t const DhcpServerPort = 67;
    static uint16_t const DhcpClientPort = 68;
    
    static uint32_t const RenewRequestTimeoutSeconds = 20;
    
    static TimeType const ResetTimeoutTicks   = 4.0 * Clock::time_freq;
    static TimeType const RequestTimeoutTicks = 3.0 * Clock::time_freq;
    static TimeType const RenewRequestTimeoutTicks = RenewRequestTimeoutSeconds * Clock::time_freq;
    
    static uint32_t MaxTimerSeconds = TheClockUtils::WorkingTimeSpanTicks / (TimeType)Clock::time_freq;
    
    static uint32_t RenewTimeForLeaseTime (uint32_t lease_time)
    {
        return lease_time / 2;
    }
    
private:
    IpStack *m_ipstack;
    uint32_t m_xid;
    DhcpState m_state;
    uint8_t m_xid_reuse_count;
    uint8_t m_request_count;
    uint32_t m_time_left;
    struct {
        Ip4Addr yiaddr;
        uint32_t dhcp_server_identifier;
    } m_offered;
    struct {
        uint32_t ip_address_lease_time;
        Ip4Addr subnet_mask;
        bool have_router;
        uint8_t domain_name_servers_count;
        Ip4Addr router;
        Ip4Addr domain_name_servers[MaxDnsServers];
        MacAddr server_mac;
    } m_acked;
    
public:
    void init (IpStack *stack, Iface *iface)
    {
        AMBRO_ASSERT(iface->getHwType() == IpHwType::Ethernet)
        
        m_ipstack = stack;
        
        IfaceListener::init(iface, Ip4ProtocolUdp);
        tim(DhcpTimer()).init(Context());
        
        start_process(true);
    }
    
    void deinit ()
    {
        tim(DhcpTimer()).deinit(Context());
        IfaceListener::deinit();
    }
    
private:
    inline IpEthHw * ethHw ()
    {
        return IfaceListener::getIface()->template getHwIface<IpEthHw>();
    }
    
    void set_timer_for_time_left (uint32_t max_timer_seconds)
    {
        uint32_t seconds = APrinter::MinValue(m_time_left, max_timer_seconds);
        m_time_left -= seconds;
        tim(DhcpTimer()).appendAfter(Context(), seconds * (TimeType)Clock::time_freq);
    }
    
    void start_process (bool force_new_xid)
    {
        if (force_new_xid || m_xid_reuse_count >= XidReuseMax) {
            m_xid = Clock::getTime(Context());
            m_xid_reuse_count = 0;
        }
        
        m_xid_reuse_count++;
        
        // TODO: send discover
        // send_message(o, DHCP_MESSAGE_TYPE_DISCOVER, o->xid, 0, 0, 0, 0);
        
        tim(DhcpTimer()).appendAfter(Context(), ResetTimeoutTicks);
        m_state = DhcpState::SentDiscover;
    }
    
    void timerExpired (DhcpTimer, Context)
    {
        switch (m_state) {
            // Timer is set for restarting discovery.
            case DhcpState::Resetting:
            case DhcpState::SentDiscover: {
                bool force_new_xid = m_state == DhcpState::Resetting;
                start_process(force_new_xid);
            } break;
            
            // Timer is set for starting renewal.
            case DhcpState::Finished: {
                // If we have more time left, restart timer.
                if (m_time_left > 0) {
                    set_timer_for_time_left(MaxTimerSeconds);
                    return;
                }
                
                // Set m_time_left to the time until the lease expires.
                m_time_left = m_acked.ip_address_lease_time - RenewTimeForLeaseTime(m_acked.ip_address_lease_time);
                
                // Send request.
                // TODO
                // send_message(o, DHCP_MESSAGE_TYPE_REQUEST, o->xid, 1, o->offered.yiaddr, 0, 0);
                
                // Start timer for sending request or lease timeout.
                set_timer_for_time_left(RenewRequestTimeoutSeconds);
                
                // Go to state Renewing.
                m_state = DhcpState::Renewing;
            } break;
            
            // Timer is set for sending another reqnewal request.
            case DhcpState::Renewing: {
                // Has the lease expired?
                if (m_time_left == 0) {
                    handle_lease_expired();
                    return;
                }
                
                // Send request.
                // TODO
                // send_message(o, DHCP_MESSAGE_TYPE_REQUEST, o->xid, 1, o->offered.yiaddr, 0, 0);
                
                // Restart timer for sending request or lease timeout.
                set_timer_for_time_left(RenewRequestTimeoutSeconds);
            } break;
            
            default:
                AMBRO_ASSERT(false);
        }
    }
    
    void handle_lease_expired ()
    {
        // Restart discovery.
        start_process(true);
        
        // Handle down.
        handle_dhcp_down();
    }
    
    bool recvIp4Dgram (Ip4DgramMeta const &ip_meta, IpBufRef dgram) override final
    {
        // Check that there is a UDP header.
        if (AMBRO_UNLIKELY(!dgram.hasHeader(Udp4Header::Size))) {
            goto reject;
        }
        
        auto udp_header = Udp4Header::MakeRef(dgram.getChunkPtr());
        
        // Check for expected source and destination port.
        if (AMBRO_LIKELY(udp_header.get(Udp4Header::SrcPort()) != DhcpServerPort)) {
            goto reject;
        }
        if (AMBRO_LIKELY(udp_header.get(Udp4Header::DstPort()) != DhcpClientPort)) {
            goto reject;
        }
        
        // Check UDP length.
        uint16_t udp_length = udp_header.get(Udp4Header::Length());
        if (AMBRO_UNLIKELY(udp_length < Udp4Header::Size || udp_length > dgram.tot_len)) {
            goto discard;
        }
        
        // Truncate dgram to UDP length.
        dgram = dgram.subTo(udp_length);
        
        // Check checksum.
        uint16_t checksum = udp_header.get(Udp4Header::Checksum());
        if (checksum != 0) {
            IpChksumAccumulator chksum_accum;
            chksum_accum.addWords(&ip_meta.remote_addr.data);
            chksum_accum.addWords(&ip_meta.local_addr.data);
            chksum_accum.addWord(APrinter::WrapType<uint16_t>(), Ip4ProtocolUdp);
            chksum_accum.addWord(APrinter::WrapType<uint16_t>(), udp_length);
            chksum_accum.addIpBuf(dgram);
            if (chksum_accum.getChksum() != 0) {
                goto discard;
            }
        }
        
        // Process message for DHCP.
        processReceivedDhcpMessage(ip_meta, dgram.subFrom(Udp4Header::Size));
        
    discard:
        // Inhibit further processing of packet.
        return true;
        
    reject:
        // Continue processing of packet.
        return false;
    }
    
    void processReceivedDhcpMessage (Ip4DgramMeta const &ip_meta, IpBufRef msg)
    {
        if (m_state == DhcpState::Resetting) {
            return;
        }
        
        // Check that there is a DHCP header.
        if (msg.tot_len < DhcpHeader::Size) {
            return;
        }
        
        // Copy DHCP header to contiguous memory.
        DhcpHeader::Val dhcp_header;
        msg.takeBytes(DhcpHeader::Size, dhcp_header.data);
        
        // Sanity checks.
        if (dhcp_header.get(DhcpHeader::DhcpOp()) != DhcpOp::BootReply) {
            return;
        }
        if (dhcp_header.get(DhcpHeader::DhcpHtype()) != DhcpHwAddrType::Ethernet) {
            return;
        }
        if (dhcp_header.get(DhcpHeader::DhcpHlen()) != MacAddr::Size) {
            return;
        }
        if (dhcp_header.get(DhcpHeader::DhcpXid()) != m_xid) {
            return;
        }
        if (MacAddr::decode(dhcp_header.ref(DhcpHeader::DhcpChaddr())) != ethHw()->getMacAddr()) {
            return;
        }
        if (dhcp_header.get(DhcpHeader::DhcpMagic()) != DhcpMagicNumber) {
            return;
        }
        
        // Parse DHCP options.
        DhcpOpts opts;
        if (!parse_dhcp_options(msg, opts)) {
            return;
        }
        
        // Sanity check DHCP message type.
        if (!opts.have.dhcp_message_type || opts.dhcp_message_type !=
            OneOf(DhcpMessageType::Offer, DhcpMessageType::Ack, DhcpMessageType::Nak))
        {
            return;
        }
        
        // Check that there is a server identifier.
        if (!opts.have.dhcp_server_identifier) {
            return;
        }
        
        // Handle NAK message.
        if (opts.dhcp_message_type == DhcpMessageType::Nak) {
            if (m_state != OneOf(DhcpState::SentRequest, DhcpState::Finished, DhcpState::Renewing)) {
                return;
            }
            
            if (opts.dhcp_server_identifier != m_offered.dhcp_server_identifier) {
                return;
            }
            
            DhcpState prev_state = m_state;
            
            tim(DhcpTimer()).appendAfter(Context(), ResetTimeoutTicks);
            m_state = DhcpState::Resetting;
            
            if (prev_state == OneOf(DhcpState::Finished, DhcpState::Renewing)) {
                // Handle down.
                handle_dhcp_down();
            }
            
            return;
        }
        
        // Sanity check Your IP Address.
        Ip4Addr yiaddr = dhcp_header.get(DhcpHeader::DhcpYiaddr());
        if (yiaddr == Ip4Addr::ZeroAddr()) {
            return;
        }
        
        // Check that we have an IP Address lease time.
        if (!opts.have.ip_address_lease_time) {
            return;
        }
        
        // Check that we have a subnet mask.
        if (!opts.have.subnet_mask) {
            return;
        }
        
        if (m_state == DhcpState::SentDiscover && opts.dhcp_message_type == DhcpMessageType::Offer) {
            // Remember offer.
            m_offered.yiaddr = yiaddr;
            m_offered.dhcp_server_identifier = opts.dhcp_server_identifier;
            
            // Send request.
            // TODO
            // send_message(o, DHCP_MESSAGE_TYPE_REQUEST, o->xid, 1, o->offered.yiaddr, 1, o->offered.dhcp_server_identifier);
            
            // Start timer for request.
            tim(DhcpTimer()).appendAfter(Context(), RequestTimeoutTicks);
            
            // Go to state SentRequest.
            m_state = DhcpState::SentRequest;
            
            // Initialize request count.
            m_request_count = 1;
        }
        else if (m_state == OneOf(DhcpState::SentRequest, DhcpState::Renewing) &&
                 opts.dhcp_message_type == DhcpMessageType::Ack)
        {
            // Sanity check against the offer.
            if (yiaddr != m_offered.yiaddr ||
                opts.dhcp_server_identifier != m_offered.dhcp_server_identifier)
            {
                return;
            }
            
            if (m_state == DhcpState::SentRequest) {
                // Remember information.
                m_acked.subnet_mask = opts.subnet_mask;
                m_acked.have_router = opts.have.router;
                if (opts.have.router) {
                    m_acked.router = opts.router;
                }
                m_acked.domain_name_servers_count = opts.have.dns_servers;
                ::memcpy(m_acked.domain_name_servers, opts.dns_servers, opts.have.dns_servers * sizeof(Ip4Addr));
                m_acked.server_mac = ethHw()->getRxEthHeader().get(EthHeader::SrcMac());
            }
            
            // Remember the lease time.
            m_acked.ip_address_lease_time = opts.ip_address_lease_time;
            
            // Go to state Finished.
            DhcpState prev_state = m_state;
            m_state = DhcpState::Finished;
            
            // Start renew timeout.
            m_time_left = RenewTimeForLeaseTime(m_acked.ip_address_lease_time);
            set_timer_for_time_left(MaxTimerSeconds);
            
            // If we were in SentRequest state (not renewing), handle up.
            if (prev_state == DhcpState::SentRequest) {
                handle_dhcp_up();
            }
        }
    }
    
    void handle_dhcp_up ()
    {
        Iface *iface = IfaceListener::getIface();
        
        // Set IP address with prefix length.
        uint8_t prefix = m_acked.subnet_mask.countLeadingOnes();
        iface->setIp4Addr(IpIfaceIp4AddrSetting{true, prefix, m_offered.yiaddr});
        
        // Set gateway if provided.
        if (m_acked.have_router) {
            iface->setIp4Gateway(IpIfaceIp4GatewaySetting{true, m_acked.router});
        }
    }
    
    void handle_dhcp_down ()
    {
        Iface *iface = IfaceListener::getIface();
        
        // Remove gateway.
        iface->setIp4Gateway(IpIfaceIp4GatewaySetting{false});
        
        // Remove IP address.
        iface->setIp4Addr(IpIfaceIp4AddrSetting{false});
    }
    
    struct DhcpOpts {
        struct Have {
            bool dhcp_message_type : 1;
            bool dhcp_server_identifier : 1;
            bool ip_address_lease_time : 1;
            bool subnet_mask : 1;
            bool router : 1;
            uint8_t dns_servers;
        } have;
        
        uint8_t dhcp_message_type;
        uint32_t dhcp_server_identifier;
        uint32_t ip_address_lease_time;
        Ip4Addr subnet_mask;
        Ip4Addr router;
        Ip4Addr dns_servers[MaxDnsServers];
    };
    
    static bool parse_dhcp_options (IpBufRef data, DhcpOpts &opts)
    {
        // Clear all the "have" fields.
        opts.have = DhcpOpts::Have{};
        
        bool have_end = false;
        
        while (data.tot_len > 0) {
            // Read option type.
            uint8_t opt_type = data.takeByte();
            
            // Pad option?
            if (opt_type == DhcpOptionType::Pad) {
                continue;
            }
            
            // It is an error for options other than pad to follow
            // the end option.
            if (have_end) {
                return false;
            }
            
            // End option?
            if (opt_type == DhcpOptionType::End) {
                // Skip over any following pad options.
                have_end = true;
                continue;
            }
            
            // Read option length.
            if (data.tot_len == 0) {
                return false;
            }
            uint8_t opt_len = data.takeByte();
            
            // Check option length.
            if (opt_len > data.tot_len) {
                return false;
            }
            
            // Handle different options.
            switch (opt_type) {
                case DhcpOptionType::DhcpMessageType: {
                    if (opt_len != DhcpOptMsgType::Size) {
                        goto skip_data;
                    }
                    DhcpOptMsgType::Val val;
                    data.takeBytes(opt_len, val.data);
                    opts.have.dhcp_message_type = true;
                    opts.dhcp_message_type = val.get(DhcpOptMsgType::MsgType());
                } break;
                
                case DhcpOptionType::DhcpServerIdentifier: {
                    if (opt_len != DhcpOptServerId::Size) {
                        goto skip_data;
                    }
                    DhcpOptServerId::Val val;
                    data.takeBytes(opt_len, val.data);
                    opts.have.dhcp_server_identifier = true;
                    opts.dhcp_server_identifier = val.get(DhcpOptServerId::ServerId());
                } break;
                
                case DhcpOptionType::IpAddressLeaseTime: {
                    if (opt_len != DhcpOptTime::Size) {
                        goto skip_data;
                    }
                    DhcpOptTime::Val val;
                    data.takeBytes(opt_len, val.data);
                    opts.have.ip_address_lease_time = true;
                    opts.ip_address_lease_time = val.get(DhcpOptTime::Time());
                } break;
                
                case DhcpOptionType::SubnetMask: {
                    if (opt_len != DhcpOptAddr::Size) {
                        goto skip_data;
                    }
                    DhcpOptAddr::Val val;
                    data.takeBytes(opt_len, val.data);
                    opts.have.subnet_mask = true;
                    opts.subnet_mask = val.get(DhcpOptAddr::Addr());
                } break;
                
                case DhcpOptionType::Router: {
                    if (opt_len != DhcpOptAddr::Size) {
                        goto skip_data;
                    }
                    DhcpOptAddr::Val val;
                    data.takeBytes(opt_len, val.data);
                    opts.have.router = true;
                    opts.router = val.get(DhcpOptAddr::Addr());
                } break;
                
                case DhcpOptionType::DomainNameServer: {
                    if (opt_len % DhcpOptAddr::Size != 0) {
                        goto skip_data;
                    }
                    uint8_t num_servers = opt_len / DhcpOptAddr::Size;
                    for (uint8_t i = 0; i < num_servers; i++) {
                        DhcpOptAddr::Val val;
                        data.takeBytes(DhcpOptAddr::Size, val.data);
                        if (opts.have.dns_servers < MaxDnsServers) {
                            opts.dns_servers[opts.have.dns_servers++] = val.get(DhcpOptAddr::Addr());
                        }
                    }
                } break;
                
                skip_data:
                default: {
                    // Unknown or bad option, skip over the option data.
                    data.skipBytes(opt_len);
                } break;
            }
        }
        
        // Check that the end option was found.
        if (!have_end) {
            return false;
        }
        
        return true;
    }
    
};

APRINTER_ALIAS_STRUCT_EXT(IpDhcpClientService, (
    APRINTER_AS_VALUE(int, ToDo)
), (
    APRINTER_ALIAS_STRUCT_EXT(Compose, (
        APRINTER_AS_TYPE(Context),
        APRINTER_AS_TYPE(IpStack)
    ), (
        using Params = IpDhcpClientService;
        APRINTER_DEF_INSTANCE(Compose, IpDhcpClient)
    ))
))

#include <aipstack/EndNamespace.h>

#endif
