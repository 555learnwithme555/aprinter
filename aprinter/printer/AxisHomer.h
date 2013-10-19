/*
 * Copyright (c) 2013 Ambroz Bizjak
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

#ifndef AMBROLIB_AXIS_HOMER_H
#define AMBROLIB_AXIS_HOMER_H

#include <stdint.h>
#include <math.h>

#include <aprinter/meta/WrapCallback.h>
#include <aprinter/meta/MakeTypeList.h>
#include <aprinter/meta/ForwardHandler.h>
#include <aprinter/meta/Position.h>
#include <aprinter/base/DebugObject.h>
#include <aprinter/base/Assert.h>
#include <aprinter/math/FloatTools.h>
#include <aprinter/printer/MotionPlanner.h>

#include <aprinter/BeginNamespace.h>

template <
    typename Position, typename Context, typename TheAxisStepper,
    int PlannerStepBits,
    typename PlannerDistanceFactor, typename PlannerCorneringDistance,
    int PlannerStepperSegmentBufferSize, int PlannerSegmentBufferSizeExp,
    typename SwitchPin, bool SwitchInvert, bool HomeDir,
    typename GetAxisStepper, typename FinishedHandler
>
class AxisHomer
: private DebugObject<Context, void>
{
private:
    struct PlannerPosition;
    struct PlannerGetAxisStepper;
    struct PlannerPullHandler;
    struct PlannerFinishedHandler;
    struct PlannerPrestepCallback;
    
    using PlannerAxes = MakeTypeList<MotionPlannerAxisSpec<TheAxisStepper, PlannerGetAxisStepper, PlannerStepBits, PlannerDistanceFactor, PlannerCorneringDistance, PlannerPrestepCallback>>;
    using Planner = MotionPlanner<PlannerPosition, Context, PlannerAxes, PlannerStepperSegmentBufferSize, PlannerSegmentBufferSizeExp, PlannerPullHandler, PlannerFinishedHandler>;
    using PlannerCommand = typename Planner::InputCommand;
    enum {STATE_FAST, STATE_RETRACT, STATE_SLOW, STATE_END};
    
public:
    using StepFixedType = typename Planner::template Axis<0>::StepFixedType;
    
    struct HomingParams {
        StepFixedType fast_max_dist;
        StepFixedType retract_dist;
        StepFixedType slow_max_dist;
        double fast_speed;
        double retract_speed;
        double slow_speed;
        double max_accel;
    };
    
    void init (Context c, HomingParams params)
    {
        AMBRO_ASSERT(params.fast_max_dist.bitsValue() > 0)
        AMBRO_ASSERT(params.retract_dist.bitsValue() > 0)
        AMBRO_ASSERT(params.slow_max_dist.bitsValue() > 0)
        AMBRO_ASSERT(FloatIsPosOrPosZero(params.fast_speed))
        AMBRO_ASSERT(FloatIsPosOrPosZero(params.retract_speed))
        AMBRO_ASSERT(FloatIsPosOrPosZero(params.slow_speed))
        AMBRO_ASSERT(FloatIsPosOrPosZero(params.max_accel))
        
        m_planner.init(c, true);
        m_state = STATE_FAST;
        m_command_sent = false;
        m_params = params;
        
        this->debugInit(c);
    }
    
    void deinit (Context c)
    {
        this->debugDeinit(c);
        
        if (m_state != STATE_END) {
            m_planner.deinit(c);
        }
    }
    
    using TheAxisStepperConsumer = typename Planner::template TheAxisStepperConsumer<0>;
    using EventLoopFastEvents = typename Planner::EventLoopFastEvents;
    
private:
    void planner_pull_handler (Context c)
    {
        this->debugAccess(c);
        AMBRO_ASSERT(m_state != STATE_END)
        
        if (m_command_sent) {
            m_planner.waitFinished(c);
            return;
        }
        
        PlannerCommand cmd;
        cmd.type = 0;
        cmd.rel_max_v_rec = 0.0;
        switch (m_state) {
            case STATE_FAST: {
                cmd.axes.elem.dir = HomeDir;
                cmd.axes.elem.x = m_params.fast_max_dist;
                cmd.axes.elem.max_v_rec = 1.0 / m_params.fast_speed;
                cmd.axes.elem.max_a_rec = 1.0 / m_params.max_accel;
            } break;
            case STATE_RETRACT: {
                cmd.axes.elem.dir = !HomeDir;
                cmd.axes.elem.x = m_params.retract_dist;
                cmd.axes.elem.max_v_rec = 1.0 / m_params.retract_speed;
                cmd.axes.elem.max_a_rec = 1.0 / m_params.max_accel;
            } break;
            case STATE_SLOW: {
                cmd.axes.elem.dir = HomeDir;
                cmd.axes.elem.x = m_params.slow_max_dist;
                cmd.axes.elem.max_v_rec = 1.0 / m_params.slow_speed;
                cmd.axes.elem.max_a_rec = 1.0 / m_params.max_accel;
            } break;
        }
        
        m_planner.commandDone(c, &cmd);
        m_command_sent = true;
    }
    
    void planner_finished_handler (Context c)
    {
        this->debugAccess(c);
        AMBRO_ASSERT(m_state != STATE_END)
        AMBRO_ASSERT(m_command_sent)
        
        m_planner.deinit(c);
        m_state++;
        
        if (m_state == STATE_END) {
            return FinishedHandler::call(this, c, true);
        }
        
        m_planner.init(c, m_state != STATE_RETRACT);
        m_command_sent = false;
    }
    
    bool planner_prestep_callback (typename Planner::template Axis<0>::StepperCommandCallbackContext c)
    {
        return (c.pins()->template get<SwitchPin>(c) != SwitchInvert);
    }
    
    Planner m_planner;
    uint8_t m_state;
    bool m_command_sent;
    HomingParams m_params;
    
    struct PlannerPosition : public MemberPosition<Position, Planner, &AxisHomer::m_planner> {};
    struct PlannerGetAxisStepper : public AMBRO_FHANDLER_TD(&AxisHomer::m_planner, GetAxisStepper) {};
    struct PlannerPullHandler : public AMBRO_WCALLBACK_TD(&AxisHomer::planner_pull_handler, &AxisHomer::m_planner) {};
    struct PlannerFinishedHandler : public AMBRO_WCALLBACK_TD(&AxisHomer::planner_finished_handler, &AxisHomer::m_planner) {};
    struct PlannerPrestepCallback : public AMBRO_WCALLBACK_TD(&AxisHomer::planner_prestep_callback, &AxisHomer::m_planner) {};
};

#include <aprinter/EndNamespace.h>

#endif
