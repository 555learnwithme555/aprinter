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

#ifndef AMBROLIB_STEPPERS_H
#define AMBROLIB_STEPPERS_H

#include <aprinter/meta/TypeList.h>
#include <aprinter/meta/Tuple.h>
#include <aprinter/meta/TupleGet.h>
#include <aprinter/meta/TypeListGet.h>
#include <aprinter/meta/FilterTypeList.h>
#include <aprinter/meta/IsEqualFunc.h>
#include <aprinter/meta/ComposeFunctions.h>
#include <aprinter/meta/GetMemberTypeFunc.h>
#include <aprinter/meta/SequenceList.h>
#include <aprinter/meta/TypeListLength.h>
#include <aprinter/meta/IndexElemTuple.h>
#include <aprinter/meta/Position.h>
#include <aprinter/meta/MapTypeList.h>
#include <aprinter/meta/ValueTemplateFunc.h>
#include <aprinter/meta/ChooseInt.h>
#include <aprinter/meta/TypeListFold.h>
#include <aprinter/meta/WrapValue.h>
#include <aprinter/base/DebugObject.h>

#include <aprinter/BeginNamespace.h>

template <typename TDirPin, typename TStepPin, typename TEnablePin, bool TInvertDir>
struct StepperDef {
    using DirPin = TDirPin;
    using StepPin = TStepPin;
    using EnablePin = TEnablePin;
    static const bool InvertDir = TInvertDir;
};

template <typename Position, typename Context, typename StepperDefsList>
class Steppers : private DebugObject<Context, void> {
    AMBRO_MAKE_SELF(Context, Steppers, Position)
    AMBRO_DECLARE_TUPLE_FOREACH_HELPER(Foreach_init, init)
    AMBRO_DECLARE_TUPLE_FOREACH_HELPER(Foreach_deinit, deinit)
    
    static int const NumSteppers = TypeListLength<StepperDefsList>::value;
    using MaskType = typename ChooseInt<NumSteppers, false>::Type;
    
public:
    template <int StepperIndex>
    class Stepper {
        friend Steppers;
        
        AMBRO_DECLARE_GET_MEMBER_TYPE_FUNC(GetMemberType_EnablePin, EnablePin)
        AMBRO_DECLARE_GET_MEMBER_TYPE_FUNC(GetMemberType_TheWrappedMask, TheWrappedMask)
        
        using ThisDef = TypeListGet<StepperDefsList, StepperIndex>;
        using EnablePin = typename ThisDef::EnablePin;
        static MaskType const TheMask = (MaskType)1 << StepperIndex;
        using TheWrappedMask = WrapValue<MaskType, TheMask>;
        
        template <typename X, typename Y>
        using OrMaskFunc = WrapValue<MaskType, (X::value | Y::value)>;
        
        // workaround clang bug
        template <int X>
        using Stepper_ = Stepper<X>;
        
        static MaskType const SameEnableMask = TypeListFold<
            MapTypeList<
                FilterTypeList<
                    MapTypeList<
                        SequenceList<TypeListLength<StepperDefsList>::value>,
                        ValueTemplateFunc<int, Stepper_>
                    >,
                    ComposeFunctions<
                        IsEqualFunc<EnablePin>,
                        GetMemberType_EnablePin
                    >
                >,
                GetMemberType_TheWrappedMask
            >,
            WrapValue<MaskType, 0>,
            OrMaskFunc
        >::value;
        
        static bool const SharesEnable = (SameEnableMask != TheMask);
        
    public:
        static void enable (Context c)
        {
            Steppers *s = Steppers::self(c);
            s->debugAccess(c);
            if (SharesEnable) {
                s->m_mask |= TheMask;
            }
            c.pins()->template set<EnablePin>(c, false);
        }
        
        static void disable (Context c)
        {
            Steppers *s = Steppers::self(c);
            s->debugAccess(c);
            if (SharesEnable) {
                s->m_mask &= ~TheMask;
                if (!(s->m_mask & SameEnableMask)) {
                    c.pins()->template set<EnablePin>(c, true);
                }
            } else {
                c.pins()->template set<EnablePin>(c, true);
            }
        }
        
        template <typename ThisContext>
        static void setDir (ThisContext c, bool dir)
        {
            Steppers *s = Steppers::self(c);
            s->debugAccess(c);
            c.pins()->template set<typename ThisDef::DirPin>(c, maybe_invert_dir(dir));
        }
        
        template <typename ThisContext>
        static void stepOn (ThisContext c)
        {
            Steppers *s = Steppers::self(c);
            s->debugAccess(c);
            c.pins()->template set<typename ThisDef::StepPin>(c, true);
        }
        
        template <typename ThisContext>
        static void stepOff (ThisContext c)
        {
            Steppers *s = Steppers::self(c);
            s->debugAccess(c);
            c.pins()->template set<typename ThisDef::StepPin>(c, false);
        }
        
        static void emergency ()
        {
            Context::Pins::template emergencySet<typename ThisDef::EnablePin>(true);
        }
        
    public: // private, workaround gcc bug
        static bool maybe_invert_dir (bool dir)
        {
            return (ThisDef::InvertDir) ? !dir : dir;
        }
        
        static void init (Context c)
        {
            c.pins()->template set<typename ThisDef::DirPin>(c, maybe_invert_dir(false));
            c.pins()->template set<typename ThisDef::StepPin>(c, false);
            c.pins()->template set<typename ThisDef::EnablePin>(c, true);
            c.pins()->template setOutput<typename ThisDef::DirPin>(c);
            c.pins()->template setOutput<typename ThisDef::StepPin>(c);
            c.pins()->template setOutput<typename ThisDef::EnablePin>(c);
        }
        
        static void deinit (Context c)
        {
            c.pins()->template set<ThisDef::EnablePin>(c, true);
        }
    };
    
    static void init (Context c)
    {
        Steppers *o = self(c);
        o->m_mask = 0;
        SteppersTuple dummy;
        TupleForEachForward(&dummy, Foreach_init(), c);
        o->debugInit(c);
    }
    
    static void deinit (Context c)
    {
        Steppers *o = self(c);
        o->debugDeinit(c);
        SteppersTuple dummy;
        TupleForEachReverse(&dummy, Foreach_deinit(), c);
    }
    
private:
    using SteppersTuple = IndexElemTuple<StepperDefsList, Stepper>;
    
    MaskType m_mask;
};

#include <aprinter/EndNamespace.h>

#endif
