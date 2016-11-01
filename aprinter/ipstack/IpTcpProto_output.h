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

#ifndef APRINTER_IPSTACK_IP_TCP_PROTO_OUTPUT_H
#define APRINTER_IPSTACK_IP_TCP_PROTO_OUTPUT_H

#include <stdint.h>
#include <stddef.h>

#include <aprinter/meta/MinMax.h>
#include <aprinter/base/Preprocessor.h>
#include <aprinter/base/Assert.h>
#include <aprinter/base/Hints.h>
#include <aprinter/ipstack/Buf.h>
#include <aprinter/ipstack/Chksum.h>
#include <aprinter/ipstack/proto/Tcp4Proto.h>
#include <aprinter/ipstack/proto/TcpUtils.h>

#include <aprinter/BeginNamespace.h>

template <typename TcpProto>
class IpTcpProto_output
{
    APRINTER_USE_TYPES2(TcpUtils, (FlagsType, SeqType, PortType, TcpState, TcpSegMeta,
                                   OptionFlags, TcpOptions))
    APRINTER_USE_VALS(TcpUtils, (seq_add, seq_diff, seq_lte, seq_lt, tcplen,
                                 can_output_in_state, accepting_data_in_state,
                                 snd_open_in_state))
    APRINTER_USE_TYPES1(TcpProto, (Context, Ip4DgramMeta, TcpPcb, PcbFlags, BufAllocator,
                                   Input, Clock, TimeType, RttType, RttNextType))
    APRINTER_USE_VALS(TcpProto, (RttTypeMax))
    APRINTER_USE_VALS(TcpProto::TheIpStack, (HeaderBeforeIp4Dgram))
    
public:
    inline static size_t pcb_snd_buf_len (TcpPcb *pcb)
    {
        return (pcb->con != nullptr) ? pcb->con->m_snd_buf.tot_len : 0;
    }
    
    // Check if our FIN has been ACKed.
    static bool pcb_fin_acked (TcpPcb *pcb)
    {
        return pcb->hasFlag(PcbFlags::FIN_SENT) && pcb->snd_una == pcb->snd_nxt;
    }
    
    // Calculate the offset of the current send buffer position.
    static size_t pcb_snd_offset (TcpPcb *pcb)
    {
        AMBRO_ASSERT(pcb->con == nullptr ||
                     pcb->snd_buf_cur.tot_len <= pcb->con->m_snd_buf.tot_len)
        
        return (pcb->con == nullptr) ? 0 :
               (pcb->con->m_snd_buf.tot_len - pcb->snd_buf_cur.tot_len);
    }
    
    // Send SYN+ACK packet in SYN_RCVD state, with MSS option.
    static void pcb_send_syn_ack (TcpPcb *pcb)
    {
        AMBRO_ASSERT(pcb->state == TcpState::SYN_RCVD)
        
        TcpOptions tcp_opts;
        tcp_opts.options = OptionFlags::MSS;
        tcp_opts.mss = pcb->rcv_mss;
        TcpSegMeta tcp_meta = {pcb->local_port, pcb->remote_port, pcb->snd_una, pcb->rcv_nxt,
                               Input::pcb_ann_wnd(pcb), Tcp4FlagSyn|Tcp4FlagAck, &tcp_opts};
        send_tcp(pcb->tcp, pcb->local_addr, pcb->remote_addr, tcp_meta);
    }
    
    // Send an empty ACK (which may be a window update).
    static void pcb_send_empty_ack (TcpPcb *pcb)
    {
        TcpSegMeta tcp_meta = {pcb->local_port, pcb->remote_port, pcb->snd_nxt, pcb->rcv_nxt,
                               Input::pcb_ann_wnd(pcb), Tcp4FlagAck};
        send_tcp(pcb->tcp, pcb->local_addr, pcb->remote_addr, tcp_meta);
    }
    
    // Send an RST for this PCB.
    static void pcb_send_rst (TcpPcb *pcb)
    {
        send_rst(pcb->tcp, pcb->local_addr, pcb->remote_addr,
                 pcb->local_port, pcb->remote_port,
                 pcb->snd_nxt, true, pcb->rcv_nxt);
    }
    
    static void pcb_need_ack (TcpPcb *pcb)
    {
        AMBRO_ASSERT(pcb->state != TcpState::CLOSED)
        
        // If we're in input processing just set a flag that ACK is
        // needed which will be picked up at the end, otherwise send
        // an ACK ourselves.
        if (pcb->tcp->m_current_pcb == pcb) {
            pcb->setFlag(PcbFlags::ACK_PENDING);
        } else {
            pcb_send_empty_ack(pcb);
        }
    }
    
    static void pcb_snd_buf_extended (TcpPcb *pcb)
    {
        AMBRO_ASSERT(snd_open_in_state(pcb->state))
        
        // Start the output timer if not running.
        if (!pcb->output_timer.isSet(Context())) {
            pcb->output_timer.appendAfter(Context(), TcpProto::OutputTimerTicks);
        }
    }
    
    static void pcb_end_sending (TcpPcb *pcb)
    {
        AMBRO_ASSERT(snd_open_in_state(pcb->state))
        
        // Make the appropriate state transition, effectively
        // queuing a FIN for sending.
        if (pcb->state == TcpState::ESTABLISHED) {
            pcb->state = TcpState::FIN_WAIT_1;
        } else {
            AMBRO_ASSERT(pcb->state == TcpState::CLOSE_WAIT)
            pcb->state = TcpState::LAST_ACK;
        }
        
        // Queue a FIN for sending.
        pcb->setFlag(PcbFlags::FIN_PENDING);
        
        // Push output.
        pcb_push_output(pcb);
    }
    
    static void pcb_push_output (TcpPcb *pcb)
    {
        AMBRO_ASSERT(can_output_in_state(pcb->state))
        
        // Set the push index to the end of the send buffer.
        pcb->snd_psh_index = pcb_snd_buf_len(pcb);
        
        // Schedule a call to pcb_output soon.
        if (pcb == pcb->tcp->m_current_pcb) {
            pcb->setFlag(PcbFlags::OUT_PENDING);
        } else {
            if (!pcb->output_timer.isSet(Context())) {
                pcb->output_timer.appendAfter(Context(), TcpProto::OutputTimerTicks);
            }
        }
    }
    
    // Check if there is any unacknowledged or unsent data or FIN.
    static bool pcb_has_snd_outstanding (TcpPcb *pcb)
    {
        AMBRO_ASSERT(can_output_in_state(pcb->state))
        
        return pcb_snd_buf_len(pcb) > 0 || !snd_open_in_state(pcb->state);
    }
    
    // Determine of the rtx_timer needs to be running.
    static bool pcb_need_rtx_timer (TcpPcb *pcb)
    {
        AMBRO_ASSERT(can_output_in_state(pcb->state))
        AMBRO_ASSERT(pcb_has_snd_outstanding(pcb))
        
        // Need the timer either if we have any sent but unacknowledged
        // data/FIN or if the send window is empty while delaying transmission
        // is not acceptable.
        return pcb->snd_buf_cur.tot_len < pcb_snd_buf_len(pcb) ||
               pcb->hasFlag(PcbFlags::FIN_SENT) ||
               (pcb->snd_wnd == 0 && !pcb_may_delay_snd(pcb));
    }
    
    // Determine if sending can be delayed in expectation of a larger segment.
    static bool pcb_may_delay_snd (TcpPcb *pcb)
    {
        return pcb->snd_buf_cur.tot_len < pcb->snd_mss &&
               pcb->snd_psh_index <= pcb_snd_offset(pcb) &&
               snd_open_in_state(pcb->state);
    }
    
    static SeqType pcb_output_segment (TcpPcb *pcb, IpBufRef data, bool fin, SeqType rem_wnd)
    {
        AMBRO_ASSERT(can_output_in_state(pcb->state))
        AMBRO_ASSERT(data.tot_len <= pcb_snd_buf_len(pcb))
        AMBRO_ASSERT(!fin || !snd_open_in_state(pcb->state))
        AMBRO_ASSERT(data.tot_len > 0 || fin)
        AMBRO_ASSERT(rem_wnd > 0)
        
        // Determine segment data length.
        size_t seg_data_len = MinValueU(data.tot_len, MinValueU(rem_wnd, pcb->snd_mss));
        
        // Determine offset from start of send buffer.
        size_t offset = pcb_snd_buf_len(pcb) - data.tot_len;
        
        // Determine segment flags, calculate sequence length.
        FlagsType seg_flags = Tcp4FlagAck;
        SeqType seg_seqlen = seg_data_len;
        if (seg_data_len == data.tot_len && fin && rem_wnd > seg_data_len) {
            seg_flags |= Tcp4FlagFin|Tcp4FlagPsh;
            seg_seqlen++;
        }
        else if (pcb->snd_psh_index > offset && pcb->snd_psh_index <= offset + seg_data_len) {
            seg_flags |= Tcp4FlagPsh;
        }
        
        // Send the segment.
        SeqType seq_num = seq_add(pcb->snd_una, offset);
        TcpSegMeta tcp_meta = {pcb->local_port, pcb->remote_port, seq_num, pcb->rcv_nxt,
                               Input::pcb_ann_wnd(pcb), seg_flags};
        send_tcp(pcb->tcp, pcb->local_addr, pcb->remote_addr, tcp_meta, data.subTo(seg_data_len));
        
        // Calculate the end sequence number of the sent segment.
        SeqType seg_endseq = seq_add(seq_num, seg_seqlen);
        
        // Stop a round-trip-time measurement if we have retransmitted
        // a segment containing the associated sequence number.
        if (pcb->hasFlag(PcbFlags::RTT_PENDING) &&
            seq_lte(seq_num, pcb->rtt_test_seq, pcb->snd_una) &&
            seq_lt(pcb->rtt_test_seq, seg_endseq, pcb->snd_una))
        {
            pcb->clearFlag(PcbFlags::RTT_PENDING);
        }
        
        // Did we send anything new?
        if (seq_lt(pcb->snd_nxt, seg_endseq, pcb->snd_una)) {
            // Start a round-trip-time measurement if not already started.
            if (!pcb->hasFlag(PcbFlags::RTT_PENDING)) {
                pcb->setFlag(PcbFlags::RTT_PENDING);
                pcb->rtt_test_seq = pcb->snd_nxt;
                pcb->rtt_test_time = Clock::getTime(Context());
            }
            
            // Bump snd_nxt.
            pcb->snd_nxt = seg_endseq;
        }
        
        // If we sent FIN set the FIN_SENT flag.
        if ((seg_flags & Tcp4FlagFin) != 0) {
            pcb->setFlag(PcbFlags::FIN_SENT);
        }
        
        return seg_seqlen;
    }
    
    /**
     * Drives transmission of data including FIN.
     * Returns whether a (presumably) valid ACK has been sent.
     */
    static bool pcb_output (TcpPcb *pcb)
    {
        AMBRO_ASSERT(can_output_in_state(pcb->state))
        
        // If there is nothing outstanding, stop the retransmission timer and return.
        if (!pcb_has_snd_outstanding(pcb)) {
            pcb->rtx_timer.unset(Context());
            return false;
        }
        
        // Calculate how much the window permits us to send.
        SeqType rem_wnd = pcb->snd_wnd - MinValueU(pcb->snd_wnd, pcb_snd_offset(pcb));
        
        // Will need to know if we sent anything.
        bool sent = false;
        
        // While we have something to send and some window is available...
        while ((pcb->snd_buf_cur.tot_len > 0 || pcb->hasFlag(PcbFlags::FIN_PENDING)) && rem_wnd > 0)
        {
            // If we have less than MSS of data left to send which is
            // not being pushed (due to sendPush or close), delay sending.
            if (pcb_may_delay_snd(pcb)) {
                break;
            }
            
            // Send a segment.
            bool fin = pcb->hasFlag(PcbFlags::FIN_PENDING);
            SeqType seg_seqlen = pcb_output_segment(pcb, pcb->snd_buf_cur, fin, rem_wnd);
            AMBRO_ASSERT(seg_seqlen > 0 && seg_seqlen <= rem_wnd)
            
            // Advance snd_buf_cur over any data just sent.
            size_t data_sent = MinValueU(seg_seqlen, pcb->snd_buf_cur.tot_len);
            pcb->snd_buf_cur.skipBytes(data_sent);
            
            // If we sent a FIN, clear the FIN_PENDING flag.
            if (seg_seqlen > data_sent) {
                AMBRO_ASSERT(pcb->hasFlag(PcbFlags::FIN_PENDING))
                AMBRO_ASSERT(seg_seqlen - 1 == data_sent)
                pcb->clearFlag(PcbFlags::FIN_PENDING);
            }
            
            // Update local state.
            rem_wnd -= seg_seqlen;
            sent = true;
        }
        
        if (pcb_need_rtx_timer(pcb)) {
            // Start timer for retransmission or window probe, if not already.
            if (!pcb->rtx_timer.isSet(Context())) {
                TimeType rtx_time = (TimeType)pcb->rto << TcpProto::RttShift;
                pcb->rtx_timer.appendAfter(Context(), rtx_time);
            }
        } else {
            // Stop the timer.
            pcb->rtx_timer.unset(Context());
        }
        
        return sent;
    }
    
    static void pcb_output_timer_handler (TcpPcb *pcb)
    {
        AMBRO_ASSERT(can_output_in_state(pcb->state))
        
        // Drive the transmission.
        pcb_output(pcb);
    }
    
    static void pcb_rtx_timer_handler (TcpPcb *pcb)
    {
        AMBRO_ASSERT(can_output_in_state(pcb->state))
        AMBRO_ASSERT(pcb_has_snd_outstanding(pcb))
        AMBRO_ASSERT(pcb_need_rtx_timer(pcb))
        
        // Compute a maximum number of sequence counts to send.
        // We must not send more than one segment, but we must be
        // able to send at least something in case of window probes.
        SeqType rem_wnd = MinValueU(pcb->snd_mss, MaxValue((SeqType)1, pcb->snd_wnd));
        
        // Send a segment from the start of the send buffer.
        IpBufRef data = (pcb->con != nullptr) ? pcb->con->m_snd_buf : IpBufRef{};
        bool fin = !snd_open_in_state(pcb->state);
        SeqType seg_seqlen = pcb_output_segment(pcb, data, fin, rem_wnd);
        AMBRO_ASSERT(seg_seqlen > 0 && seg_seqlen <= rem_wnd)
        
        // Double the retransmission timeout.
        RttType doubled_rto = (pcb->rto > RttTypeMax / 2) ? RttTypeMax : (2 * pcb->rto);
        pcb->rto = MinValue(TcpProto::MaxRtxTime, doubled_rto);
        
        // Restart this timer with the new timeout.
        TimeType rtx_time = (TimeType)pcb->rto << TcpProto::RttShift;
        pcb->rtx_timer.appendAfter(Context(), rtx_time);
    }
    
    static void pcb_rtt_test_seq_acked (TcpPcb *pcb)
    {
        AMBRO_ASSERT(pcb->hasFlag(PcbFlags::RTT_PENDING))
        
        // Clear the flag to indicate end of RTT measurement.
        pcb->clearFlag(PcbFlags::RTT_PENDING);
        
        // Calculate how much time has passed, also in RTT units.
        TimeType time_diff = Clock::getTime(Context()) - pcb->rtt_test_time;
        RttType this_rtt = MinValueU(RttTypeMax, time_diff >> TcpProto::RttShift);
        
        // Update RTTVAR and SRTT.
        if (!pcb->hasFlag(PcbFlags::RTT_VALID)) {
            pcb->setFlag(PcbFlags::RTT_VALID);
            pcb->rttvar = this_rtt/2;
            pcb->srtt = this_rtt;
        } else {
            RttType rtt_diff = AbsoluteDiff(pcb->srtt, this_rtt);
            pcb->rttvar = ((RttNextType)3 * pcb->rttvar + rtt_diff) / 4;
            pcb->srtt = ((RttNextType)7 * pcb->srtt + this_rtt) / 8;
        }
        
        // Update RTO.
        pcb_update_rto(pcb);
    }
    
    // Update the RTO from RTTVAR and SRTT.
    static void pcb_update_rto (TcpPcb *pcb)
    {
        int const k = 4;
        RttType k_rttvar = (pcb->rttvar > RttTypeMax / k) ? RttTypeMax : (k * pcb->rttvar);
        RttType var_part = MaxValue((RttType)1, k_rttvar);
        RttType base_rto = (var_part > RttTypeMax - pcb->srtt) ? RttTypeMax : (pcb->srtt + var_part);
        pcb->rto = MaxValue(TcpProto::MinRtxTime, MinValue(TcpProto::MaxRtxTime, base_rto));
    }
    
    // Send an RST as a reply to a received segment.
    // This conforms to RFC 793 handling of segments not belonging to a known
    // connection.
    static void send_rst_reply (TcpProto *tcp, Ip4DgramMeta const &ip_meta,
                                TcpSegMeta const &tcp_meta, size_t tcp_data_len)
    {
        SeqType rst_seq_num;
        bool rst_ack;
        SeqType rst_ack_num;
        if ((tcp_meta.flags & Tcp4FlagAck) != 0) {
            rst_seq_num = tcp_meta.ack_num;
            rst_ack = false;
            rst_ack_num = 0;
        } else {
            rst_seq_num = 0;
            rst_ack = true;
            rst_ack_num = tcp_meta.seq_num + tcplen(tcp_meta.flags, tcp_data_len);
        }
        
        send_rst(tcp, ip_meta.local_addr, ip_meta.remote_addr,
                 tcp_meta.local_port, tcp_meta.remote_port,
                 rst_seq_num, rst_ack, rst_ack_num);
    }
    
    static void send_rst (TcpProto *tcp, Ip4Addr local_addr, Ip4Addr remote_addr,
                          PortType local_port, PortType remote_port,
                          SeqType seq_num, bool ack, SeqType ack_num)
    {
        FlagsType flags = Tcp4FlagRst | (ack ? Tcp4FlagAck : 0);
        TcpSegMeta tcp_meta = {local_port, remote_port, seq_num, ack_num, 0, flags};
        send_tcp(tcp, local_addr, remote_addr, tcp_meta);
    }
    
    static void send_tcp (TcpProto *tcp, Ip4Addr local_addr, Ip4Addr remote_addr,
                          TcpSegMeta const &tcp_meta, IpBufRef data=IpBufRef{})
    {
        // Compute length of TCP options.
        uint8_t opts_len = (tcp_meta.opts != nullptr) ? TcpUtils::calc_options_len(*tcp_meta.opts) : 0;
        
        // Allocate memory for headers.
        TxAllocHelper<BufAllocator, Tcp4Header::Size+TcpUtils::MaxOptionsWriteLen, HeaderBeforeIp4Dgram>
            dgram_alloc(Tcp4Header::Size+opts_len);
        
        // Caculate the offset+flags field.
        FlagsType offset_flags = ((FlagsType)(5+opts_len/4) << TcpOffsetShift) | tcp_meta.flags;
        
        // Write the TCP header.
        auto tcp_header = Tcp4Header::MakeRef(dgram_alloc.getPtr());
        tcp_header.set(Tcp4Header::SrcPort(),     tcp_meta.local_port);
        tcp_header.set(Tcp4Header::DstPort(),     tcp_meta.remote_port);
        tcp_header.set(Tcp4Header::SeqNum(),      tcp_meta.seq_num);
        tcp_header.set(Tcp4Header::AckNum(),      tcp_meta.ack_num);
        tcp_header.set(Tcp4Header::OffsetFlags(), offset_flags);
        tcp_header.set(Tcp4Header::WindowSize(),  tcp_meta.window_size);
        tcp_header.set(Tcp4Header::Checksum(),    0);
        tcp_header.set(Tcp4Header::UrgentPtr(),   0);
        
        // Write any TCP options.
        if (tcp_meta.opts != nullptr) {
            TcpUtils::write_options(*tcp_meta.opts, dgram_alloc.getPtr() + Tcp4Header::Size);
        }
        
        // Construct the datagram reference including any data.
        IpBufNode data_node;
        if (data.tot_len > 0) {
            data_node = data.toNode();
            dgram_alloc.setNext(&data_node, data.tot_len);
        }
        IpBufRef dgram = dgram_alloc.getBufRef();
        
        // Calculate TCP checksum.
        IpChksumAccumulator chksum_accum;
        chksum_accum.addWords(&local_addr.data);
        chksum_accum.addWords(&remote_addr.data);
        chksum_accum.addWord(WrapType<uint16_t>(), Ip4ProtocolTcp);
        chksum_accum.addWord(WrapType<uint16_t>(), dgram.tot_len);
        chksum_accum.addIpBuf(dgram);
        uint16_t calc_chksum = chksum_accum.getChksum();
        tcp_header.set(Tcp4Header::Checksum(), calc_chksum);
        
        // Send the datagram.
        Ip4DgramMeta meta = {local_addr, remote_addr, TcpProto::TcpTTL, Ip4ProtocolTcp};
        tcp->m_stack->sendIp4Dgram(meta, dgram);
    }
};

#include <aprinter/EndNamespace.h>

#endif
