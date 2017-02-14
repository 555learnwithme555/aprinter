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

#ifndef APRINTER_IPSTACK_IP_TCP_PROTO_H
#define APRINTER_IPSTACK_IP_TCP_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <limits.h>

#include <tuple>

#include <aprinter/meta/ServiceUtils.h>
#include <aprinter/meta/MinMax.h>
#include <aprinter/meta/BitsInFloat.h>
#include <aprinter/meta/PowerOfTwo.h>
#include <aprinter/meta/StructIf.h>
#include <aprinter/meta/ChooseInt.h>
#include <aprinter/meta/BasicMetaUtils.h>
#include <aprinter/base/Assert.h>
#include <aprinter/base/OneOf.h>
#include <aprinter/base/Callback.h>
#include <aprinter/base/Preprocessor.h>
#include <aprinter/base/LoopUtils.h>
#include <aprinter/base/Accessor.h>
#include <aprinter/structure/LinkedList.h>
#include <aprinter/structure/LinkModel.h>
#include <aprinter/system/TimedEventWrapper.h>

#include <aipstack/misc/Buf.h>
#include <aipstack/misc/SendRetry.h>
#include <aipstack/proto/IpAddr.h>
#include <aipstack/proto/Ip4Proto.h>
#include <aipstack/proto/Tcp4Proto.h>
#include <aipstack/proto/TcpUtils.h>
#include <aipstack/ip/IpStack.h>

#include "IpTcpProto_constants.h"
#include "IpTcpProto_api.h"
#include "IpTcpProto_input.h"
#include "IpTcpProto_output.h"

#include <aipstack/BeginNamespace.h>

/**
 * TCP protocol implementation.
 */
template <typename Arg>
class IpTcpProto :
    private Arg::TheIpStack::ProtoListenerCallback
{
    APRINTER_USE_VALS(Arg::Params, (TcpTTL, NumTcpPcbs, NumOosSegs,
                                    EphemeralPortFirst, EphemeralPortLast,
                                    LinkWithArrayIndices))
    APRINTER_USE_TYPES1(Arg::Params, (PcbIndexService))
    APRINTER_USE_TYPES1(Arg, (Context, BufAllocator, TheIpStack))
    
    APRINTER_USE_TYPE1(Context, Clock)
    APRINTER_USE_TYPE1(Clock, TimeType)
    APRINTER_USE_ONEOF
    
    APRINTER_USE_TYPES1(TheIpStack, (Ip4DgramMeta, ProtoListener, Iface, Ip4DestUnreachMeta,
                                     MtuRef))
    
    static_assert(NumTcpPcbs > 0, "");
    static_assert(NumOosSegs > 0 && NumOosSegs < 16, "");
    static_assert(EphemeralPortFirst > 0, "");
    static_assert(EphemeralPortFirst <= EphemeralPortLast, "");
    
    template <typename> friend class IpTcpProto_constants;
    template <typename> friend class IpTcpProto_api;
    template <typename> friend class IpTcpProto_input;
    template <typename> friend class IpTcpProto_output;
    
public:
    APRINTER_USE_TYPES1(TcpUtils, (SeqType, PortType))
    
private:
    using Constants = IpTcpProto_constants<IpTcpProto>;
    using Api = IpTcpProto_api<IpTcpProto>;
    using Input = IpTcpProto_input<IpTcpProto>;
    using Output = IpTcpProto_output<IpTcpProto>;
    
    APRINTER_USE_TYPES1(TcpUtils, (TcpState))
    APRINTER_USE_VALS(TcpUtils, (state_is_active, accepting_data_in_state,
                                 can_output_in_state, snd_open_in_state,
                                 seq_diff, seq_add))
    
    struct TcpPcb;
    
    // PCB flags, see flags in TcpPcb.
    using FlagsType = uint16_t;
    struct PcbFlags { enum : FlagsType {
        ACK_PENDING = 1 << 0, // ACK is needed; used in input processing
        OUT_PENDING = 1 << 1, // pcb_output is needed; used in input processing
        FIN_SENT    = 1 << 2, // A FIN has been sent, and is included in snd_nxt
        FIN_PENDING = 1 << 3, // A FIN is to be transmitted
        RTT_PENDING = 1 << 5, // Round-trip-time is being measured
        RTT_VALID   = 1 << 6, // Round-trip-time is not in initial state
        OOSEQ_FIN   = 1 << 7, // Out-of-sequence FIN has been received
        CWND_INCRD  = 1 << 8, // cwnd has been increaded by snd_mss this round-trip
        RTX_ACTIVE  = 1 << 9, // A segment has been retransmitted and not yet acked
        RECOVER     = 1 << 10, // The recover variable valid (and >=snd_una)
        IDLE_TIMER  = 1 << 11, // If rtx_timer is running it is for idle timeout
        WND_SCALE   = 1 << 12, // Window scaling is used
        CWND_INIT   = 1 << 13, // Current cwnd is the initial cwnd
    }; };
    
    // For retransmission time calculations we right-shift the Clock time
    // to obtain granularity between 1ms and 2ms.
    static int const RttShift = APrinter::BitsInFloat(1e-3 / Clock::time_unit);
    static_assert(RttShift >= 0, "");
    static constexpr double RttTimeFreq = Clock::time_freq / APrinter::PowerOfTwoFunc<double>(RttShift);
    
    // We store such scaled times in 16-bit variables.
    // This gives us a range of at least 65 seconds.
    using RttType = uint16_t;
    static RttType const RttTypeMax = (RttType)-1;
    static constexpr double RttTypeMaxDbl = RttTypeMax;
    
    // For intermediate RTT results we need a larger type.
    using RttNextType = uint32_t;
    
    // Number of ephemeral ports.
    static PortType const NumEphemeralPorts = EphemeralPortLast - EphemeralPortFirst + 1;
    
    // Unsigned integer type usable as an index for the PCBs array.
    // We use the largest value of that type as null (which cannot
    // be a valid PCB index).
    using PcbIndexType = APrinter::ChooseIntForMax<NumTcpPcbs, false>;
    static PcbIndexType const PcbIndexNull = PcbIndexType(-1);
    
    // Represents a segment of contiguous out-of-sequence data.
    struct OosSeg {
        SeqType start;
        SeqType end;
    };
    
    // PCB key for the PCB index.
    using PcbKey = std::tuple<
        PortType, // remote_port
        Ip4Addr,  // remote_addr
        PortType, // local_port
        Ip4Addr   // local_addr
    >;
    
    struct PcbLinkModel;
    
    // Instantiate the PCB index.
    struct PcbIndexAccessor;
    using PcbIndexLookupKeyArg = PcbKey const &;
    struct PcbIndexKeyFuncs;
    APRINTER_MAKE_INSTANCE(PcbIndex, (PcbIndexService::template Index<
        PcbIndexAccessor, PcbIndexLookupKeyArg, PcbIndexKeyFuncs, PcbLinkModel>))
    
public:
    APRINTER_USE_TYPES1(Api, (TcpConnection, TcpConnectionCallback,
                              TcpListenParams, TcpListener, TcpListenerCallback))
    
    static SeqType const MaxRcvWnd = Constants::MaxRcvWnd;
    
private:
    /**
     * Timers:
     * AbrtTimer: for aborting PCB (TIME_WAIT, abandonment)
     * OutputTimer: for pcb_output after send buffer extension
     * RtxTimer: for retransmission, window probe and cwnd idle reset
     */
    APRINTER_DECL_TIMERS(TcpPcbTimers, typename Context, TcpPcb, (AbrtTimer, OutputTimer, RtxTimer))
    
    /**
     * A TCP Protocol Control Block.
     * These are maintained internally within the stack and may
     * survive deinit/reset of an associated TcpConnection object.
     */
    struct TcpPcb :
        // Send retry request (inherited for efficiency).
        public IpSendRetry::Request,
        // PCB timers.
        public TcpPcbTimers
    {
        // Node for the PCB index.
        typename PcbIndex::Node index_hook;
        
        // Node for the unreferenced PCBs list.
        // The function pcb_is_in_unreferenced_list specifies exactly when
        // a PCB is suposed to be in the unreferenced list. The only
        // exception to this is while pcb_unlink_con is during the callback
        // pcb_unlink_con-->pcb_aborted-->connectionAborted.
        APrinter::LinkedListNode<PcbLinkModel> unrefed_list_node;
        
        // Pointer back to IpTcpProto.
        IpTcpProto *tcp;    
        
        union {
            // Pointer to the associated TcpListener, if in SYN_RCVD.
            TcpListener *lis;
            
            // Pointer to any associated TcpConnection, otherwise.
            TcpConnection *con;
        };
        
        // Addresses and ports.
        Ip4Addr local_addr;
        Ip4Addr remote_addr;
        PortType local_port;
        PortType remote_port;
        
        // Sender variables.
        SeqType snd_una;
        SeqType snd_nxt;
        SeqType snd_wnd;
        SeqType snd_wl1;
        SeqType snd_wl2;
        IpBufRef snd_buf_cur;
        size_t snd_psh_index;
        
        // Congestion control variables.
        SeqType cwnd;
        SeqType ssthresh;
        SeqType cwnd_acked;
        SeqType recover;
        
        // Receiver variables.
        SeqType rcv_nxt;
        SeqType rcv_wnd;
        SeqType rcv_ann;
        SeqType rcv_ann_thres;
        
        // Out-of-sequence segment information.
        OosSeg ooseq[NumOosSegs];
        SeqType ooseq_fin;
        
        // Round-trip-time and retransmission time management.
        SeqType rtt_test_seq;
        TimeType rtt_test_time;
        RttType rttvar;
        RttType srtt;
        RttType rto;
        
        // The base send MSS. It is computed based on the interface
        // MTU and the MTU option provided by the peer.
        // In the SYN_SENT state this is set based on the interface MTU and
        // the calculation is completed at the transition to ESTABLISHED.
        uint16_t base_snd_mss;
        
        // The maximum segment size we will send.
        // This is dynamic based on Path MTU Discovery, but it will always
        // be between Constants::MinAllowedMss and base_snd_mss.
        // It is first initialized at the transition to ESTABLISHED state,
        // before that is is undefined.
        // Due to invariants and other requirements associated with snd_mss,
        // fixups must be performed when snd_mss is changed, especially of
        // cwnd and rtx_timer (see pcb_update_snd_mss).
        uint16_t snd_mss;
        
        // The maximum segment size we are willing to accept. This is
        // calculated based on the interface MTU and does not change
        // from when the PCB is initialized in SYN_SENT/SYN_RCVD state.
        uint16_t rcv_mss;
        
        // Flags (see comments in PcbFlags).
        FlagsType flags;
        
        // PCB state.
        TcpState state;
        
        // Flags for IpStack::sendIp4Dgram (IpSendFlags).
        uint8_t ip_send_flags;
        
        // Number of valid elements in ooseq_segs;
        uint8_t num_ooseq : 4;
        
        // Number of duplicate ACKs (>=FastRtxDupAcks means we're in fast recovery).
        uint8_t num_dupack : Constants::DupAckBits;
        
        // Window shift values.
        uint8_t snd_wnd_shift : 4;
        uint8_t rcv_wnd_shift : 4;
        
        // MTU reference.
        // It is setup if and only if SYN_SENT or (PCB referenced and can_output_in_state).
        MtuRef mtu_ref;
        
        // Convenience functions for flags.
        inline bool hasFlag (FlagsType flag) { return (flags & flag) != 0; }
        inline void setFlag (FlagsType flag) { flags |= flag; }
        inline void clearFlag (FlagsType flag) { flags &= ~flag; }
        
        // Convenience functions for buffer length.
        inline size_t sndBufLen () { return (con != nullptr) ? con->m_snd_buf.tot_len : 0; }
        inline size_t rcvBufLen () { return (con != nullptr) ? con->m_rcv_buf.tot_len : 0; }
        
        // Trampolines for timer handlers.
        inline void timerExpired (AbrtTimer, Context) { pcb_abrt_timer_handler(this); }
        inline void timerExpired (OutputTimer, Context) { Output::pcb_output_timer_handler(this); }
        inline void timerExpired (RtxTimer, Context) { Output::pcb_rtx_timer_handler(this); }
        
        // Send retry callback.
        void retrySending () override final { Output::pcb_send_retry(this); }
    };
    
    // Define the hook accessor for the PCB index.
    struct PcbIndexAccessor : public APRINTER_MEMBER_ACCESSOR(&TcpPcb::index_hook) {};
    
public:
    /**
     * Initialize the TCP protocol implementation.
     * 
     * The TCP will register itself with the IpStack to receive incoming TCP packets.
     */
    void init (TheIpStack *stack)
    {
        AMBRO_ASSERT(stack != nullptr)
        
        // Remember things.
        m_stack = stack;
        
        // Initialize the protocol listener for the TCP protocol number.
        m_proto_listener.init(m_stack, Ip4ProtocolTcp, this);
        
        // Initialize the list of listeners.
        m_listeners_list.init();
        
        // Clear m_current_pcb which tracks the current PCB being
        // processed by pcb_input().
        m_current_pcb = nullptr;
        
        // Set the initial counter for ephemeral ports.
        m_next_ephemeral_port = EphemeralPortFirst;
        
        // Initialize the list of unreferenced PCBs.
        m_unrefed_pcbs_list.init();
        
        // Initialize the PCB indices.
        m_pcb_index_active.init();
        m_pcb_index_timewait.init();
        
        for (TcpPcb &pcb : m_pcbs) {
            // Initialize the PCB timers.
            pcb.tim(AbrtTimer()).init(Context());
            pcb.tim(OutputTimer()).init(Context());
            pcb.tim(RtxTimer()).init(Context());
            
            // Initialize the send-retry Request object.
            pcb.IpSendRetry::Request::init();
            
            // Initialize the MTU reference.
            pcb.mtu_ref.init();
            
            // Initialize some PCB variables.
            pcb.tcp = this;
            pcb.state = TcpState::CLOSED;
            pcb.con = nullptr;
            
            // Add the PCB to the list of unreferenced PCBs.
            m_unrefed_pcbs_list.prepend(link_model_ref(pcb), link_model_state());
        }
    }
    
    /**
     * Deinitialize the TCP protocol implementation.
     * 
     * Any TCP listeners and connections must have been deinited.
     * It is not permitted to call this from any TCP callbacks.
     */
    void deinit ()
    {
        AMBRO_ASSERT(m_listeners_list.isEmpty())
        AMBRO_ASSERT(m_current_pcb == nullptr)
        
        for (TcpPcb &pcb : m_pcbs) {
            AMBRO_ASSERT(pcb.state != TcpState::SYN_RCVD)
            AMBRO_ASSERT(pcb.con == nullptr)
            pcb.mtu_ref.deinit(m_stack);
            pcb.IpSendRetry::Request::deinit();
            pcb.tim(RtxTimer()).deinit(Context());
            pcb.tim(OutputTimer()).deinit(Context());
            pcb.tim(AbrtTimer()).deinit(Context());
        }
        
        m_proto_listener.deinit();
    }
    
private:
    void recvIp4Dgram (Ip4DgramMeta const &ip_meta, IpBufRef dgram) override final
    {
        Input::recvIp4Dgram(this, ip_meta, dgram);
    }
    
    void handleIp4DestUnreach (Ip4DestUnreachMeta const &du_meta,
                               Ip4DgramMeta const &ip_meta, IpBufRef dgram_initial) override final
    {
        Input::handleIp4DestUnreach(this, du_meta, ip_meta, dgram_initial);
    }
    
    TcpPcb * allocate_pcb ()
    {
        // No PCB available?
        if (m_unrefed_pcbs_list.isEmpty()) {
            return nullptr;
        }
        
        // Get a PCB to use.
        TcpPcb *pcb = m_unrefed_pcbs_list.lastNotEmpty(link_model_state());
        AMBRO_ASSERT(pcb_is_in_unreferenced_list(pcb))
        
        // Abort the PCB if it's not closed.
        if (pcb->state != TcpState::CLOSED) {
            pcb_abort(pcb);
        } else {
            pcb_assert_closed(pcb);
        }
        
        return pcb;
    }
    
    void pcb_assert_closed (TcpPcb *pcb)
    {
        AMBRO_ASSERT(!pcb->tim(AbrtTimer()).isSet(Context()))
        AMBRO_ASSERT(!pcb->tim(OutputTimer()).isSet(Context()))
        AMBRO_ASSERT(!pcb->tim(RtxTimer()).isSet(Context()))
        AMBRO_ASSERT(!pcb->IpSendRetry::Request::isQueued())
        AMBRO_ASSERT(pcb->tcp == this)
        AMBRO_ASSERT(pcb->state == TcpState::CLOSED)
        AMBRO_ASSERT(pcb->con == nullptr)
    }
    
    inline static void pcb_abort (TcpPcb *pcb)
    {
        // This function aborts a PCB while sending an RST in
        // all states except these.
        bool send_rst = pcb->state !=
            OneOf(TcpState::SYN_SENT, TcpState::SYN_RCVD, TcpState::TIME_WAIT);
        
        pcb_abort(pcb, send_rst);
    }
    
    static void pcb_abort (TcpPcb *pcb, bool send_rst)
    {
        AMBRO_ASSERT(pcb->state != TcpState::CLOSED)
        IpTcpProto *tcp = pcb->tcp;
        
        // Send RST if desired.
        if (send_rst) {
            Output::pcb_send_rst(pcb);
        }
        
        if (pcb->state == TcpState::SYN_RCVD) {
            // Disassociate the TcpListener.
            pcb_unlink_lis(pcb);
        } else {
            // Disassociate any TcpConnection. This will call the
            // connectionAborted callback if we do have a TcpConnection.
            pcb_unlink_con(pcb, true);
        }
        
        // If this is called from input processing of this PCB,
        // clear m_current_pcb. This way, input processing can
        // detect aborts performed from within user callbacks.
        if (tcp->m_current_pcb == pcb) {
            tcp->m_current_pcb = nullptr;
        }
        
        // Remove the PCB from the index in which it is.
        if (pcb->state == TcpState::TIME_WAIT) {
            tcp->m_pcb_index_timewait.removeEntry(tcp->link_model_state(), tcp->link_model_ref(*pcb));
        } else {
            tcp->m_pcb_index_active.removeEntry(tcp->link_model_state(), tcp->link_model_ref(*pcb));
        }
        
        // Make sure the PCB is at the end of the unreferenced list.
        if (pcb != tcp->m_unrefed_pcbs_list.lastNotEmpty(tcp->link_model_state())) {
            tcp->m_unrefed_pcbs_list.remove(tcp->link_model_ref(*pcb), tcp->link_model_state());
            tcp->m_unrefed_pcbs_list.append(tcp->link_model_ref(*pcb), tcp->link_model_state());
        }
        
        // Reset other relevant fields to initial state.
        pcb->tim(AbrtTimer()).unset(Context());
        pcb->tim(OutputTimer()).unset(Context());
        pcb->tim(RtxTimer()).unset(Context());
        pcb->mtu_ref.reset(pcb->tcp->m_stack);
        pcb->IpSendRetry::Request::reset();
        pcb->state = TcpState::CLOSED;
        
        tcp->pcb_assert_closed(pcb);
    }
    
    static void pcb_go_to_time_wait (TcpPcb *pcb)
    {
        AMBRO_ASSERT(pcb->state != OneOf(TcpState::CLOSED, TcpState::SYN_RCVD,
                                         TcpState::TIME_WAIT))
        
        // Disassociate any TcpConnection. This will call the
        // connectionAborted callback if we do have a TcpConnection.
        pcb_unlink_con(pcb, false);
        
        // Set snd_nxt to snd_una in order to not accept any more acknowledgements.
        // This is currently not necessary since we only enter TIME_WAIT after
        // having received a FIN, but in the future we might do some non-standard
        // transitions where this is not the case.
        pcb->snd_nxt = pcb->snd_una;
        
        // Change state.
        pcb->state = TcpState::TIME_WAIT;
        
        // Move the PCB from the active index to the time-wait index.
        IpTcpProto *tcp = pcb->tcp;
        tcp->m_pcb_index_active.removeEntry(tcp->link_model_state(), tcp->link_model_ref(*pcb));
        tcp->m_pcb_index_timewait.addEntry(tcp->link_model_state(), tcp->link_model_ref(*pcb));
        
        // Stop these timers due to asserts in their handlers.
        pcb->tim(OutputTimer()).unset(Context());
        pcb->tim(RtxTimer()).unset(Context());
        
        // Reset the MTU reference.
        pcb->mtu_ref.reset(pcb->tcp->m_stack);
        
        // Start the TIME_WAIT timeout.
        pcb->tim(AbrtTimer()).appendAfter(Context(), Constants::TimeWaitTimeTicks);
    }
    
    static void pcb_unlink_con (TcpPcb *pcb, bool closing)
    {
        AMBRO_ASSERT(pcb->state != OneOf(TcpState::CLOSED, TcpState::SYN_RCVD))
        
        if (pcb->con != nullptr) {
            // Inform the connection object about the aborting.
            // Note that the PCB is not yet on the list of unreferenced
            // PCBs, which protects it from being aborted by allocate_pcb
            // during this callback.
            TcpConnection *con = pcb->con;
            AMBRO_ASSERT(con->m_pcb == pcb)
            con->pcb_aborted();
            
            // The pcb->con has been cleared by con->pcb_aborted().
            AMBRO_ASSERT(pcb->con == nullptr)
            
            // Add the PCB to the list of unreferenced PCBs.
            IpTcpProto *tcp = pcb->tcp;
            if (closing) {
                tcp->m_unrefed_pcbs_list.append(tcp->link_model_ref(*pcb), tcp->link_model_state());
            } else {
                tcp->m_unrefed_pcbs_list.prepend(tcp->link_model_ref(*pcb), tcp->link_model_state());
            }
        }
    }
    
    static void pcb_unlink_lis (TcpPcb *pcb)
    {
        AMBRO_ASSERT(pcb->state == TcpState::SYN_RCVD)
        AMBRO_ASSERT(pcb->lis != nullptr)
        
        TcpListener *lis = pcb->lis;
        
        // Decrement the listener's PCB count.
        AMBRO_ASSERT(lis->m_num_pcbs > 0)
        lis->m_num_pcbs--;
        
        // Is this a PCB which is being accepted?
        if (lis->m_accept_pcb == pcb) {
            // Break the link from the listener.
            lis->m_accept_pcb = nullptr;
            
            // The PCB was removed from the list of unreferenced
            // PCBs, so we have to add it back.
            IpTcpProto *tcp = pcb->tcp;
            tcp->m_unrefed_pcbs_list.append(tcp->link_model_ref(*pcb), tcp->link_model_state());
        }
        
        // Clear pcb->con since we will be going to CLOSED state
        // and it is was not undefined due to the union with pcb->lis.
        pcb->con = nullptr;
    }
    
    // This is called from TcpConnection::reset when the TcpConnection
    // is abandoning the PCB.
    static void pcb_con_abandoned (TcpPcb *pcb, bool snd_buf_nonempty)
    {
        AMBRO_ASSERT(pcb->state == TcpState::SYN_SENT || state_is_active(pcb->state))
        AMBRO_ASSERT(pcb->con == nullptr) // TcpConnection just cleared it
        AMBRO_ASSERT(snd_buf_nonempty || pcb->snd_buf_cur.tot_len == 0)
        IpTcpProto *tcp = pcb->tcp;
        
        // Add the PCB to the unreferenced PCBs list.
        // This has not been done by TcpConnection.
        tcp->m_unrefed_pcbs_list.append(tcp->link_model_ref(*pcb), tcp->link_model_state());
        
        // Abort if in SYN_SENT state or some data is queued.
        // The pcb_abort() will decide whether to send an RST
        // (no RST in SYN_SENT, RST otherwise).
        if (pcb->state == TcpState::SYN_SENT || snd_buf_nonempty) {
            return pcb_abort(pcb);
        }
        
        // Reset the MTU reference.
        pcb->mtu_ref.reset(pcb->tcp->m_stack);
        
        // Arrange for sending the FIN.
        if (snd_open_in_state(pcb->state)) {
            Output::pcb_end_sending(pcb);
        }
        
        // If we haven't received a FIN, ensure that at least rcv_mss
        // window is advertised.
        if (accepting_data_in_state(pcb->state)) {
            if (pcb->rcv_wnd < pcb->rcv_mss) {
                pcb->rcv_wnd = pcb->rcv_mss;
            }
            if (seq_diff(pcb->rcv_ann, pcb->rcv_nxt) < pcb->rcv_mss) {
                Output::pcb_need_ack(pcb);
            }
        }
        
        // Start the abort timeout.
        pcb->tim(AbrtTimer()).appendAfter(Context(), Constants::AbandonedTimeoutTicks);
    }
    
    static void pcb_abrt_timer_handler (TcpPcb *pcb)
    {
        AMBRO_ASSERT(pcb->state != TcpState::CLOSED)
        
        pcb_abort(pcb);
    }
    
    // This is used from input processing to call one of the TcpConnection
    // private functions then check whether the PCB is still alive.
    template <typename Action>
    inline static bool pcb_event (TcpPcb *pcb, Action action)
    {
        AMBRO_ASSERT(pcb->tcp->m_current_pcb == pcb)
        AMBRO_ASSERT(pcb->state != TcpState::SYN_RCVD)
        AMBRO_ASSERT(pcb->con == nullptr || pcb->con->m_pcb == pcb)
        
        if (pcb->con == nullptr) {
            return true;
        }
        
        IpTcpProto *tcp = pcb->tcp;
        action(pcb->con);
        
        return tcp->m_current_pcb != nullptr;
    }
    
    static inline SeqType make_iss ()
    {
        return Clock::getTime(Context());
    }
    
    TcpListener * find_listener (Ip4Addr addr, PortType port)
    {
        for (TcpListener *lis = m_listeners_list.first(); lis != nullptr; lis = m_listeners_list.next(lis)) {
            AMBRO_ASSERT(lis->m_listening)
            if (lis->m_addr == addr && lis->m_port == port) {
                return lis;
            }
        }
        return nullptr;
    }
    
    void unlink_listener (TcpListener *lis)
    {
        // Abort any PCBs associated with the listener (without RST).
        for (TcpPcb &pcb : m_pcbs) {
            if (pcb.state == TcpState::SYN_RCVD && pcb.lis == lis) {
                pcb_abort(&pcb, false);
            }
        }
    }
    
    IpErr create_connection (TcpConnection *con, Ip4Addr remote_addr, PortType remote_port,
                             size_t user_rcv_wnd, TcpPcb **out_pcb)
    {
        AMBRO_ASSERT(con != nullptr)
        AMBRO_ASSERT(out_pcb != nullptr)
        
        // Determine the local interface.
        Iface *iface;
        Ip4Addr route_addr;
        if (!m_stack->routeIp4(remote_addr, nullptr, &iface, &route_addr)) {
            return IpErr::NO_IP_ROUTE;
        }
        
        // Determine the local IP address.
        IpIfaceIp4AddrSetting addr_setting = iface->getIp4Addr();
        if (!addr_setting.present) {
            return IpErr::NO_IP_ROUTE;
        }
        Ip4Addr local_addr = addr_setting.addr;
        
        // Determine the local port.
        PortType local_port = get_ephemeral_port(local_addr, remote_addr, remote_port);
        if (local_port == 0) {
            return IpErr::NO_PORT_AVAIL;
        }
        
        // Calculate the MSS based on the interface MTU.
        uint16_t iface_mss = TcpUtils::calc_mss_from_mtu(iface->getMtu() - Ip4Header::Size);
        if (iface_mss < Constants::MinAllowedMss) {
            return IpErr::NO_HEADER_SPACE;
        }
        
        // Allocate the PCB.
        TcpPcb *pcb = allocate_pcb();
        if (pcb == nullptr) {
            return IpErr::NO_PCB_AVAIL;
        }
        
        // Setup the MTU reference.
        if (!pcb->mtu_ref.setup(m_stack, remote_addr, iface)) {
            // PCB is CLOSED, this is not a leak.
            return IpErr::NO_IPMTU_AVAIL;
        }
        
        // NOTE: If another error case is added after this, make sure
        // to reset the mtu_ref before abandoning the PCB!
        
        // Remove the PCB from the unreferenced PCBs list.
        m_unrefed_pcbs_list.remove(link_model_ref(*pcb), link_model_state());
        
        // Generate an initial sequence number.
        SeqType iss = make_iss();
        
        // The initial receive window will be at least one
        // because we must be prepared to accept the SYN.
        SeqType rcv_wnd = 1 + APrinter::MinValueU(seq_diff(Constants::MaxRcvWnd, 1), user_rcv_wnd);
        
        // Initialize most of the PCB.
        pcb->state = TcpState::SYN_SENT;
        pcb->flags = PcbFlags::WND_SCALE; // to send the window scale option
        pcb->ip_send_flags = IpSendFlags::DontFragmentNow | IpSendFlags::DontFragmentFlag;
        pcb->con = con;
        pcb->local_addr = local_addr;
        pcb->remote_addr = remote_addr;
        pcb->local_port = local_port;
        pcb->remote_port = remote_port;
        pcb->rcv_nxt = 0; // it is sent in the SYN
        pcb->rcv_wnd = rcv_wnd;
        pcb->rcv_ann = pcb->rcv_nxt;
        pcb->rcv_ann_thres = Constants::DefaultWndAnnThreshold;
        pcb->rcv_mss = iface_mss;
        pcb->snd_una = iss;
        pcb->snd_nxt = iss;
        pcb->snd_buf_cur = IpBufRef{};
        pcb->snd_psh_index = 0;
        pcb->base_snd_mss = iface_mss; // will be updated when the SYN-ACK is received
        pcb->rto = Constants::InitialRtxTime;
        pcb->num_ooseq = 0;
        pcb->num_dupack = 0;
        pcb->snd_wnd_shift = 0;
        pcb->rcv_wnd_shift = Constants::RcvWndShift;
        
        // snd_mss will be initialized at transition to ESTABLISHED
        
        // Add the PCB to the active index.
        m_pcb_index_active.addEntry(link_model_state(), link_model_ref(*pcb));
        
        // Start the connection timeout.
        pcb->tim(AbrtTimer()).appendAfter(Context(), Constants::SynSentTimeoutTicks);
        
        // Start the retransmission timer.
        pcb->tim(RtxTimer()).appendAfter(Context(), Output::pcb_rto_time(pcb));
        
        // Send the SYN.
        Output::pcb_send_syn(pcb);
        
        // Return the PCB.
        *out_pcb = pcb;
        return IpErr::SUCCESS;
    }
    
    PortType get_ephemeral_port (Ip4Addr local_addr, Ip4Addr remote_addr, PortType remote_port)
    {
        for (PortType i : APrinter::LoopRangeAuto(NumEphemeralPorts)) {
            PortType port = m_next_ephemeral_port;
            m_next_ephemeral_port = (port < EphemeralPortLast) ? (port + 1) : EphemeralPortFirst;
            
            if (find_pcb_by_addr(local_addr, port, remote_addr, remote_port) == nullptr) {
                return port;
            }
        }
        
        return 0;
    }
    
    inline static bool pcb_is_in_unreferenced_list (TcpPcb *pcb)
    {
        return pcb->state == TcpState::SYN_RCVD ? pcb->lis->m_accept_pcb != pcb
                                                : pcb->con == nullptr;
    }
    
    void move_unrefed_pcb_to_front (TcpPcb *pcb)
    {
        AMBRO_ASSERT(pcb_is_in_unreferenced_list(pcb))
        
        if (pcb != m_unrefed_pcbs_list.first(link_model_state())) {
            m_unrefed_pcbs_list.remove(link_model_ref(*pcb), link_model_state());
            m_unrefed_pcbs_list.prepend(link_model_ref(*pcb), link_model_state());
        }
    }
    
    TcpPcb * find_pcb_by_addr (Ip4Addr local_addr, PortType local_port,
                               Ip4Addr remote_addr, PortType remote_port)
    {
        PcbKey key{remote_port, remote_addr, local_port, local_addr};
        
        // Look in the active index first.
        TcpPcb *pcb = m_pcb_index_active.findEntry(link_model_state(), key);
        AMBRO_ASSERT(pcb == nullptr || pcb->state != OneOf(TcpState::CLOSED, TcpState::TIME_WAIT))
        
        // If not found, look in the time-wait index.
        if (pcb == nullptr) {
            pcb = m_pcb_index_timewait.findEntry(link_model_state(), key);
            AMBRO_ASSERT(pcb == nullptr || pcb->state == TcpState::TIME_WAIT)
        }
        
        return pcb;
    }
    
    // This is used by the two PCB indexes to obtain the keys
    // definint the ordering of the PCBs.
    struct PcbIndexKeyFuncs {
        inline static PcbKey GetKeyOfEntry (TcpPcb &pcb)
        {
            return PcbKey{pcb.remote_port, pcb.remote_addr, pcb.local_port, pcb.local_addr};
        }
    };
    
    // Define the link model for data structures of PCBs.
    struct PcbLinkModel : public APrinter::If<LinkWithArrayIndices,
        APrinter::ArrayLinkModel<TcpPcb, PcbIndexType, PcbIndexNull>,
        APrinter::PointerLinkModel<TcpPcb>
    > {};
    APRINTER_USE_TYPES1(PcbLinkModel, (Ref, State))
    
    // Returns the link model State value.
    APRINTER_FUNCTION_IF_ELSE_EXT(LinkWithArrayIndices, inline, State, link_model_state (), {
        return m_pcbs;
    }, {
        return State();
    })
    
    // Returns the link model Ref for a PCB.
    APRINTER_FUNCTION_IF_ELSE_EXT(LinkWithArrayIndices, inline, Ref, link_model_ref (TcpPcb &pcb), {
        return Ref(pcb, &pcb - m_pcbs);
    }, {
        return pcb;
    })
    
private:
    using ListenersList = APrinter::DoubleEndedList<TcpListener, &TcpListener::m_listeners_node, false>;
    
    using UnrefedPcbsList = APrinter::LinkedList<
        APRINTER_MEMBER_ACCESSOR_TN(&TcpPcb::unrefed_list_node), PcbLinkModel, true>;
    
    TheIpStack *m_stack;
    ProtoListener m_proto_listener;
    ListenersList m_listeners_list;
    TcpPcb *m_current_pcb;
    PortType m_next_ephemeral_port;
    UnrefedPcbsList m_unrefed_pcbs_list;
    typename PcbIndex::Index m_pcb_index_active;
    typename PcbIndex::Index m_pcb_index_timewait;
    TcpPcb m_pcbs[NumTcpPcbs];
};

APRINTER_ALIAS_STRUCT_EXT(IpTcpProtoService, (
    APRINTER_AS_VALUE(uint8_t, TcpTTL),
    APRINTER_AS_VALUE(int, NumTcpPcbs),
    APRINTER_AS_VALUE(uint8_t, NumOosSegs),
    APRINTER_AS_VALUE(uint16_t, EphemeralPortFirst),
    APRINTER_AS_VALUE(uint16_t, EphemeralPortLast),
    APRINTER_AS_TYPE(PcbIndexService),
    APRINTER_AS_VALUE(bool, LinkWithArrayIndices)
), (
    APRINTER_ALIAS_STRUCT_EXT(Compose, (
        APRINTER_AS_TYPE(Context),
        APRINTER_AS_TYPE(BufAllocator),
        APRINTER_AS_TYPE(TheIpStack)
    ), (
        using Params = IpTcpProtoService;
        APRINTER_DEF_INSTANCE(Compose, IpTcpProto)
    ))
))

#include <aipstack/EndNamespace.h>

#endif
