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

#ifndef AMBROLIB_AXIS_DRIVER_H
#define AMBROLIB_AXIS_DRIVER_H

#include <stdint.h>

#include <aprinter/meta/FixedPoint.h>
#include <aprinter/meta/WrapFunction.h>
#include <aprinter/meta/Options.h>
#include <aprinter/meta/TypeListUtils.h>
#include <aprinter/meta/TupleForEach.h>
#include <aprinter/meta/IndexElemTuple.h>
#include <aprinter/base/Object.h>
#include <aprinter/math/StoredNumber.h>
#include <aprinter/base/DebugObject.h>
#include <aprinter/base/Assert.h>
#include <aprinter/base/Inline.h>
#include <aprinter/driver/AxisDriverConsumer.h>

#include <aprinter/BeginNamespace.h>

#define AXIS_STEPPER_AMUL_EXPR(x, t, a) ((a).template shiftBits<(-amul_shift)>())
#define AXIS_STEPPER_V0_EXPR(x, t, a) (((x) + (a).absVal()).template shiftBits<(-discriminant_prec)>())
#define AXIS_STEPPER_DISCRIMINANT_EXPR(x, t, a) ((((x).toSigned() - (a)).toUnsignedUnsafe() * ((x).toSigned() - (a)).toUnsignedUnsafe()).template shiftBits<(-2 * discriminant_prec)>())
#define AXIS_STEPPER_TMUL_EXPR(x, t, a) ((t).template bitsTo<time_mul_bits>())

#define AXIS_STEPPER_AMUL_EXPR_HELPER(args) AXIS_STEPPER_AMUL_EXPR(args)
#define AXIS_STEPPER_V0_EXPR_HELPER(args) AXIS_STEPPER_V0_EXPR(args)
#define AXIS_STEPPER_DISCRIMINANT_EXPR_HELPER(args) AXIS_STEPPER_DISCRIMINANT_EXPR(args)
#define AXIS_STEPPER_TMUL_EXPR_HELPER(args) AXIS_STEPPER_TMUL_EXPR(args)

#define AXIS_STEPPER_DUMMY_VARS (StepFixedType()), (TimeFixedType()), (AccelFixedType())

template <
    int tstep_bits, int ttime_bits,
    int ttime_mul_bits, int tdiscriminant_prec,
    int trel_t_extra_prec
>
struct AxisDriverPrecisionParams {
    static const int step_bits = tstep_bits;
    static const int time_bits = ttime_bits;
    static const int time_mul_bits = ttime_mul_bits;
    static const int discriminant_prec = tdiscriminant_prec;
    static const int rel_t_extra_prec = trel_t_extra_prec;
};

using AxisDriverAvrPrecisionParams = AxisDriverPrecisionParams<11, 22, 24, 1, 0>;
using AxisDriverDuePrecisionParams = AxisDriverPrecisionParams<11, 28, 28, 3, 4>;

template <typename Context, typename ParentObject, typename Stepper, typename ConsumersList, typename Params>
class AxisDriver {
private:
    AMBRO_DECLARE_TUPLE_FOREACH_HELPER(Foreach_call_command_callback, call_command_callback)
    AMBRO_DECLARE_TUPLE_FOREACH_HELPER(Foreach_call_prestep_callback, call_prestep_callback)
    
    static const int step_bits = Params::PrecisionParams::step_bits;
    static const int time_bits = Params::PrecisionParams::time_bits;
    static const int time_mul_bits = Params::PrecisionParams::time_mul_bits;
    static const int discriminant_prec = Params::PrecisionParams::discriminant_prec;
    static const int rel_t_extra_prec = Params::PrecisionParams::rel_t_extra_prec;
    static const int amul_shift = 2 * (1 + discriminant_prec);
    
    struct TimerHandler;
    
public:
    struct Object;
    using Clock = typename Context::Clock;
    using TimeType = typename Clock::TimeType;
    using TimerInstance = typename Params::TimerService::template InterruptTimer<Context, Object, TimerHandler>;
    using StepFixedType = FixedPoint<step_bits, false, 0>;
    using DirStepFixedType = FixedPoint<step_bits + 2, false, 0>;
    using AccelFixedType = FixedPoint<step_bits, true, 0>;
    using TimeFixedType = FixedPoint<time_bits, false, 0>;
    using TimeMulFixedType = decltype(AXIS_STEPPER_TMUL_EXPR_HELPER(AXIS_STEPPER_DUMMY_VARS));
    using CommandCallbackContext = typename TimerInstance::HandlerContext;
    using TMulStored = StoredNumber<TimeMulFixedType::num_bits, TimeMulFixedType::is_signed>;
    using DiscriminantType = decltype(AXIS_STEPPER_DISCRIMINANT_EXPR_HELPER(AXIS_STEPPER_DUMMY_VARS));
    using V0Type = decltype(AXIS_STEPPER_V0_EXPR_HELPER(AXIS_STEPPER_DUMMY_VARS));
    using AMulType = decltype(AXIS_STEPPER_AMUL_EXPR_HELPER(AXIS_STEPPER_DUMMY_VARS));
    
private:
    using TheDebugObject = DebugObject<Context, Object>;
    
public:
    struct Command {
        DirStepFixedType dir_x;
        AMulType a_mul;
        TMulStored t_mul_stored;
    };
    
    AMBRO_ALWAYS_INLINE static void generate_command (bool dir, StepFixedType x, TimeFixedType t, AccelFixedType a, Command *cmd)
    {
        AMBRO_ASSERT(a >= -x)
        AMBRO_ASSERT(a <= x)
        
        cmd->t_mul_stored = TMulStored::store(AXIS_STEPPER_TMUL_EXPR(x, t, a).m_bits.m_int);
        cmd->dir_x = DirStepFixedType::importBits(
            x.bitsValue() |
            ((typename DirStepFixedType::IntType)dir << step_bits) |
            ((typename DirStepFixedType::IntType)(a.bitsValue() >= 0) << (step_bits + 1))
        );
        cmd->a_mul = AXIS_STEPPER_AMUL_EXPR(x, t, a);
    }
    
    static void init (Context c)
    {
        auto *o = Object::self(c);
        
        TimerInstance::init(c);
#ifdef AMBROLIB_ASSERTIONS
        o->m_running = false;
#endif
#ifdef AXISDRIVER_DETECT_OVERLOAD
        o->m_overload = false;
#endif
        
        TheDebugObject::init(c);
    }
    
    static void deinit (Context c)
    {
        auto *o = Object::self(c);
        TheDebugObject::deinit(c);
        AMBRO_ASSERT(!o->m_running)
        
        TimerInstance::deinit(c);
    }
    
    static void setPrestepCallbackEnabled (Context c, bool enabled)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        AMBRO_ASSERT(!o->m_running)
        
        o->m_prestep_callback_enabled = enabled;
    }
    
    template <typename TheConsumer>
    static void start (Context c, TimeType start_time, Command *first_command)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        AMBRO_ASSERT(!o->m_running)
        AMBRO_ASSERT(first_command)
        
#ifdef AMBROLIB_ASSERTIONS
        o->m_running = true;
#endif
        o->m_consumer_id = TypeListIndex<typename ConsumersList::List, TheConsumer>::Value;
        o->m_time = start_time;
        
        bool command_completed = load_command(c, first_command);
        TimeType timer_t = command_completed ? o->m_time : start_time;
        
#ifdef AXISDRIVER_DETECT_OVERLOAD
        o->m_overload = false;
#endif
        TimerInstance::setFirst(c, timer_t);
    }
    
    static void stop (Context c)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        
        TimerInstance::unset(c);
#ifdef AMBROLIB_ASSERTIONS
        o->m_running = false;
#endif
    }
    
    static StepFixedType getAbortedCmdSteps (Context c, bool *dir)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        AMBRO_ASSERT(!o->m_running)
        
        *dir = (o->m_current_command->dir_x.bitsValue() & ((typename DirStepFixedType::IntType)1 << step_bits));
        if (!o->m_notend) {
            return StepFixedType::importBits(0);
        } else {
            if (o->m_notdecel) {
                return StepFixedType::importBits(o->m_pos.bitsValue() + 1);
            } else {
                return StepFixedType::importBits((o->m_x.bitsValue() - o->m_pos.bitsValue()) + 1);
            }
        }
    }
    
    static StepFixedType getPendingCmdSteps (Context c, Command const *cmd, bool *dir)
    {
        *dir = (cmd->dir_x.bitsValue() & ((typename DirStepFixedType::IntType)1 << step_bits));
        return StepFixedType::importBits(cmd->dir_x.bitsValue() & (((typename DirStepFixedType::IntType)1 << step_bits) - 1));
    }
    
#ifdef AXISDRIVER_DETECT_OVERLOAD
    static bool overloadOccurred (Context c)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        AMBRO_ASSERT(!o->m_running)
        
        return o->m_overload;
    }
#endif
    
    using GetTimer = TimerInstance;
    
private:
    template <int ConsumerIndex>
    struct CallbackHelper {
        using TheConsumer = TypeListGet<typename ConsumersList::List, ConsumerIndex>;
        
        template <typename... Args>
        static bool call_command_callback (Args... args)
        {
            return TheConsumer::CommandCallback::call(args...);
        }
        
        template <typename... Args>
        static bool call_prestep_callback (Args... args)
        {
            return TheConsumer::PrestepCallback::call(args...);
        }
    };
    
    template <typename ThisContext>
    static bool load_command (ThisContext c, Command *command)
    {
        auto *o = Object::self(c);
        
        o->m_current_command = command;
        Stepper::setDir(c, command->dir_x.bitsValue() & ((typename DirStepFixedType::IntType)1 << step_bits));
        o->m_notdecel = (command->dir_x.bitsValue() & ((typename DirStepFixedType::IntType)1 << (step_bits + 1)));
        StepFixedType x = StepFixedType::importBits(command->dir_x.bitsValue() & (((typename DirStepFixedType::IntType)1 << step_bits) - 1));
        o->m_notend = (x.bitsValue() != 0);
        
        if (AMBRO_UNLIKELY(!o->m_notend)) {
            o->m_time += TimeMulFixedType::importBits(TMulStored::retrieve(command->t_mul_stored)).template bitsTo<time_bits>().bitsValue();
            return true;
        } else {
            auto xs = x.toSigned().template shiftBits<(-discriminant_prec)>();
            auto a = command->a_mul.template undoShiftBitsLeft<(amul_shift-discriminant_prec)>();
            auto x_minus_a = (xs - a).toUnsignedUnsafe();
            if (AMBRO_LIKELY(o->m_notdecel)) {
                o->m_v0 = (xs + a).toUnsignedUnsafe();
                o->m_pos = StepFixedType::importBits(x.bitsValue() - 1);
                o->m_time += TimeMulFixedType::importBits(TMulStored::retrieve(command->t_mul_stored)).template bitsTo<time_bits>().bitsValue();
            } else {
                o->m_x = x;
                o->m_v0 = x_minus_a;
                o->m_pos = StepFixedType::importBits(1);
            }
            o->m_discriminant = x_minus_a * x_minus_a;
            return false;
        }
    }
    
    static bool timer_handler (typename TimerInstance::HandlerContext c)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(o->m_running)
        
#ifdef AXISDRIVER_DETECT_OVERLOAD
        if ((TimeType)(Clock::getTime(c) - TimerInstance::getLastSetTime(c)) >= (TimeType)(0.001 * Clock::time_freq)) {
            o->m_overload = true;
        }
#endif
        
        Command *current_command = o->m_current_command;
        if (AMBRO_LIKELY(!o->m_notend)) {
            IndexElemTuple<typename ConsumersList::List, CallbackHelper> dummy;
            bool res = TupleForOneAlways<bool>(o->m_consumer_id, &dummy, Foreach_call_command_callback(), c, &current_command);
            if (AMBRO_UNLIKELY(!res)) {
#ifdef AMBROLIB_ASSERTIONS
                o->m_running = false;
#endif
                return false;
            }
            
            bool command_completed = load_command(c, current_command);
            if (command_completed) {
                TimerInstance::setNext(c, o->m_time);
                return true;
            }
            
#ifdef AMBROLIB_AVR
            // Force gcc to load the parameters we computed here (m_x, m_v0, m_pos)
            // when they're used below even though they could be kept in registers.
            // If we don't do that, these optimizations may use lots more registers
            // and we end up with slower code.
            asm volatile ("" ::: "memory");
#endif
        }
        
        if (AMBRO_UNLIKELY(o->m_prestep_callback_enabled)) {
            IndexElemTuple<typename ConsumersList::List, CallbackHelper> dummy;
            bool res = TupleForOneAlways<bool>(o->m_consumer_id, &dummy, Foreach_call_prestep_callback(), c);
            if (AMBRO_UNLIKELY(res)) {
#ifdef AMBROLIB_ASSERTIONS
                o->m_running = false;
#endif
                return false;
            }
        }
        
        Stepper::stepOn(c);
        
        // We need to ensure that the step signal is sufficiently long for the stepper driver
        // to register. To this end, we do the timely calculations in between stepOn and stepOff().
        // But to prevent the compiler from moving upwards any significant part of the calculation,
        // we do a volatile read of the discriminant (an input to the calculation).
        
        auto discriminant_bits = *(typename DiscriminantType::IntType volatile *)&o->m_discriminant.m_bits.m_int;
        o->m_discriminant.m_bits.m_int = discriminant_bits + current_command->a_mul.m_bits.m_int;
        AMBRO_ASSERT(o->m_discriminant.bitsValue() >= 0)
        
        auto q = (o->m_v0 + FixedSquareRoot<true>(o->m_discriminant, OptionForceInline())).template shift<-1>();
        
        auto t_frac = FixedFracDivide<rel_t_extra_prec>(o->m_pos, q, OptionForceInline());
        
        auto t_mul = TimeMulFixedType::importBits(TMulStored::retrieve(current_command->t_mul_stored));
        TimeFixedType t = FixedResMultiply(t_mul, t_frac);
        
        // Now make sure the calculations above happen before stepOff().
        *(uint8_t volatile *)&o->m_dummy = t.bitsValue();
        
        Stepper::stepOff(c);
        
        TimeType next_time;
        if (AMBRO_LIKELY(!o->m_notdecel)) {
            if (AMBRO_LIKELY(o->m_pos == o->m_x)) {
                o->m_time += t_mul.template bitsTo<time_bits>().bitsValue();
                o->m_notend = false;
                next_time = o->m_time;
            } else {
                o->m_pos.m_bits.m_int++;
                next_time = (o->m_time + t.bitsValue());
            }
        } else {
            if (o->m_pos.bitsValue() == 0) {
                o->m_notend = false;
            }
            o->m_pos.m_bits.m_int--;
            next_time = (o->m_time - t.bitsValue());
        }
        
        TimerInstance::setNext(c, next_time);
        
        return true;
    }
    
    struct TimerHandler : public AMBRO_WFUNC_TD(&AxisDriver::timer_handler) {};
    
public:
    struct Object : public ObjBase<AxisDriver, ParentObject, MakeTypeList<
        TheDebugObject,
        TimerInstance
    >> {
#ifdef AMBROLIB_ASSERTIONS
        bool m_running;
#endif
        uint8_t m_consumer_id;
        Command *m_current_command;
        bool m_notend;
        bool m_notdecel;
        StepFixedType m_x;
        StepFixedType m_pos;
        DiscriminantType m_discriminant;
        TimeType m_time;
        V0Type m_v0;
        bool m_prestep_callback_enabled;
        uint8_t m_dummy;
#ifdef AXISDRIVER_DETECT_OVERLOAD
        bool m_overload;
#endif
    };
};

template <
    typename TTimerService,
    typename TPrecisionParams
>
struct AxisDriverService {
    using TimerService = TTimerService;
    using PrecisionParams = TPrecisionParams;
    
    template <typename Context, typename ParentObject, typename Stepper, typename ConsumersList>
    using AxisDriver = AxisDriver<Context, ParentObject, Stepper, ConsumersList, AxisDriverService>;
};

#include <aprinter/EndNamespace.h>

#endif
