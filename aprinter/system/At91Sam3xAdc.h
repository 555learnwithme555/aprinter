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

#ifndef AMBROLIB_AT91SAM3X_ADC_H
#define AMBROLIB_AT91SAM3X_ADC_H

#include <stdint.h>
#include <sam/drivers/pmc/pmc.h>

#include <aprinter/meta/TypeListGet.h>
#include <aprinter/meta/IndexElemTuple.h>
#include <aprinter/meta/TypeListIndex.h>
#include <aprinter/meta/IsEqualFunc.h>
#include <aprinter/meta/TypeListLength.h>
#include <aprinter/meta/TupleForEach.h>
#include <aprinter/meta/MakeTypeList.h>
#include <aprinter/meta/Position.h>
#include <aprinter/meta/StructIf.h>
#include <aprinter/meta/TuplePosition.h>
#include <aprinter/meta/GetMemberTypeFunc.h>
#include <aprinter/meta/MapTypeList.h>
#include <aprinter/meta/MinMax.h>
#include <aprinter/meta/TypeListFold.h>
#include <aprinter/meta/WrapValue.h>
#include <aprinter/base/DebugObject.h>
#include <aprinter/system/At91Sam3xPins.h>
#include <aprinter/system/InterruptLock.h>

#include <aprinter/BeginNamespace.h>

struct At91Sam3xAdcTempInput {};

template <
    typename TAdcFreq, uint8_t TAdcStartup, uint8_t TAdcSettling, uint8_t TAdcTracking, uint8_t TAdcTransfer,
    typename TAvgParams
>
struct At91Sam3xAdcParams {
    using AdcFreq = TAdcFreq;
    static const uint8_t AdcStartup = TAdcStartup;
    static const uint8_t AdcSettling = TAdcSettling;
    static const uint8_t AdcTracking = TAdcTracking;
    static const uint8_t AdcTransfer = TAdcTransfer;
    using AvgParams = TAvgParams;
};

struct At91Sam3xAdcNoAvgParams {
    static const bool Enabled = false;
};

template <
    typename TAvgInterval
>
struct At91Sam3xAdcAvgParams {
    static const bool Enabled = true;
    using AvgInterval = TAvgInterval;
};

template <typename TPin, uint16_t TSmoothFactor>
struct At91Sam3xAdcSmoothPin {};

template <typename Position, typename Context, typename PinsList, typename Params>
class At91Sam3xAdc
: private DebugObject<Context, void>
{
    static_assert(Params::AdcFreq::value() >= 1000000.0, "");
    static_assert(Params::AdcFreq::value() <= 20000000.0, "");
    static_assert(Params::AdcStartup < 16, "");
    static_assert(Params::AdcSettling < 4, "");
    static_assert(Params::AdcTracking < 16, "");
    static_assert(Params::AdcTransfer < 4, "");
    
    AMBRO_MAKE_SELF(Context, At91Sam3xAdc, Position)
    struct AvgFeaturePosition;
    template <int PinIndex> struct PinPosition;
    
    static const int32_t AdcPrescal = ((F_MCK / (2.0 * Params::AdcFreq::value())) - 1.0) + 0.5;
    static_assert(AdcPrescal >= 0, "");
    static_assert(AdcPrescal <= 255, "");
    
    static const int NumPins = TypeListLength<PinsList>::value;
    
    using AdcList = MakeTypeList<
        At91Sam3xPin<At91Sam3xPioA, 2>,
        At91Sam3xPin<At91Sam3xPioA, 3>,
        At91Sam3xPin<At91Sam3xPioA, 4>,
        At91Sam3xPin<At91Sam3xPioA, 6>,
        At91Sam3xPin<At91Sam3xPioA, 22>,
        At91Sam3xPin<At91Sam3xPioA, 23>,
        At91Sam3xPin<At91Sam3xPioA, 24>,
        At91Sam3xPin<At91Sam3xPioA, 16>,
        At91Sam3xPin<At91Sam3xPioB, 12>,
        At91Sam3xPin<At91Sam3xPioB, 13>,
        At91Sam3xPin<At91Sam3xPioB, 17>,
        At91Sam3xPin<At91Sam3xPioB, 18>,
        At91Sam3xPin<At91Sam3xPioB, 19>,
        At91Sam3xPin<At91Sam3xPioB, 20>,
        At91Sam3xPin<At91Sam3xPioB, 21>,
        At91Sam3xAdcTempInput
    >;
    
    AMBRO_DECLARE_TUPLE_FOREACH_HELPER(Foreach_init, init)
    AMBRO_DECLARE_TUPLE_FOREACH_HELPER(Foreach_make_pin_mask, make_pin_mask)
    AMBRO_DECLARE_TUPLE_FOREACH_HELPER(Foreach_calc_avg, calc_avg)
    AMBRO_DECLARE_GET_MEMBER_TYPE_FUNC(GetMemberType_Pin, Pin)
    
    AMBRO_STRUCT_IF(AvgFeature, Params::AvgParams::Enabled) {
        AMBRO_MAKE_SELF(Context, AvgFeature, AvgFeaturePosition)
        using Clock = typename Context::Clock;
        using TimeType = typename Clock::TimeType;
        static TimeType const Interval = Params::AvgParams::AvgInterval::value() / Clock::time_unit;
        
        static void init (Context c)
        {
            AvgFeature *o = self(c);
            o->m_next = Clock::getTime(c) + Interval;
        }
        
        static void work (InterruptContext<Context> c)
        {
            AvgFeature *o = self(c);
            At91Sam3xAdc *m = At91Sam3xAdc::self(c);
            
            if ((TimeType)(Clock::getTime(c) - o->m_next) < UINT32_C(0x80000000)) {
                TupleForEachForward(&m->m_pins, Foreach_calc_avg(), c);
                o->m_next += Interval;
            }
        }
        
        TimeType m_next;
    } AMBRO_STRUCT_ELSE(AvgFeature) {
        static void init (Context c) {}
        static void work (InterruptContext<Context> c) {}
    };
    
public:
    static void init (Context c)
    {
        At91Sam3xAdc *o = self(c);
        if (NumPins > 0) {
            o->m_avg_feature.init(c);
            TupleForEachForward(&o->m_pins, Foreach_init(), c);
            pmc_enable_periph_clk(ID_ADC);
            ADC->ADC_CHDR = UINT32_MAX;
            ADC->ADC_CHER = TupleForEachForwardAccRes(&o->m_pins, 0, Foreach_make_pin_mask());
            ADC->ADC_MR = ADC_MR_LOWRES | ADC_MR_PRESCAL(AdcPrescal) |
                          ((uint32_t)Params::AdcStartup << ADC_MR_STARTUP_Pos) |
                          ((uint32_t)Params::AdcSettling << ADC_MR_SETTLING_Pos) |
                          ADC_MR_TRACKTIM(Params::AdcTracking) |
                          ADC_MR_TRANSFER(Params::AdcTransfer);
            ADC->ADC_IDR = UINT32_MAX;
            NVIC_ClearPendingIRQ(ADC_IRQn);
            NVIC_SetPriority(ADC_IRQn, INTERRUPT_PRIORITY);
            NVIC_EnableIRQ(ADC_IRQn);
            ADC->ADC_IER = (uint32_t)1 << MaxAdcIndex;
            ADC->ADC_CR = ADC_CR_START;
        }
        o->debugInit(c);
    }
    
    static void deinit (Context c)
    {
        At91Sam3xAdc *o = self(c);
        o->debugDeinit(c);
        if (NumPins > 0) {
            NVIC_DisableIRQ(ADC_IRQn);
            ADC->ADC_IDR = UINT32_MAX;
            NVIC_ClearPendingIRQ(ADC_IRQn);
            ADC->ADC_MR = 0;
            ADC->ADC_CHDR = UINT32_MAX;
            pmc_disable_periph_clk(ID_ADC);
        }
    }
    
    template <typename Pin, typename ThisContext>
    static uint16_t getValue (ThisContext c)
    {
        At91Sam3xAdc *o = self(c);
        o->debugAccess(c);
        
        static int const PinIndex = TypeListIndex<FlatPinsList, IsEqualFunc<Pin>>::value;
        return AdcPin<PinIndex>::get_value(c);
    }
    
    static void adc_isr (InterruptContext<Context> c)
    {
        At91Sam3xAdc *o = self(c);
        o->m_avg_feature.work(c);
        ADC->ADC_CDR[MaxAdcIndex];
        ADC->ADC_CR = ADC_CR_START;
    }
    
private:
    template <int PinIndex>
    struct AdcPin {
        AMBRO_MAKE_SELF(Context, AdcPin, PinPosition<PinIndex>)
        template <typename TheListPin> struct Helper;
        struct HelperPosition;
        
        using ListPin = TypeListGet<PinsList, PinIndex>;
        using TheHelper = Helper<ListPin>;
        
        static uint32_t make_pin_mask (uint32_t accum)
        {
            return (accum | ((uint32_t)1 << AdcIndex));
        }
        
        static void init (Context c)
        {
            return self(c)->m_helper.init(c);
        }
        
        template <typename ThisContext>
        static void calc_avg (ThisContext c)
        {
            return self(c)->m_helper.calc_avg(c);
        }
        
        template <typename ThisContext>
        static uint16_t get_value (ThisContext c)
        {
            return self(c)->m_helper.get_value(c);
        }
        
        template <typename TheListPin>
        struct Helper {
            using RealPin = TheListPin;
            static void init (Context c) {}
            template <typename ThisContext>
            static void calc_avg (ThisContext c) {}
            template <typename ThisContext>
            static uint16_t get_value (ThisContext c) { return ADC->ADC_CDR[AdcIndex]; }
        };
        
        template <typename ThePin, uint16_t TheSmoothFactor>
        struct Helper<At91Sam3xAdcSmoothPin<ThePin, TheSmoothFactor>> {
            AMBRO_MAKE_SELF(Context, TheHelper, HelperPosition)
            using RealPin = ThePin;
            static_assert(TheSmoothFactor > 0, "");
            static_assert(TheSmoothFactor < 65536, "");
            static_assert(Params::AvgParams::Enabled, "");
            
            static void init (Context c)
            {
                TheHelper *o = self(c);
                o->m_state = 0;
            }
            
            template <typename ThisContext>
            static void calc_avg (ThisContext c)
            {
                TheHelper *o = self(c);
                o->m_state = (((uint64_t)(65536 - TheSmoothFactor) * ((uint32_t)ADC->ADC_CDR[AdcIndex] << 16)) + ((uint64_t)TheSmoothFactor * o->m_state)) >> 16;
            }
            
            template <typename ThisContext>
            static uint16_t get_value (ThisContext c)
            {
                TheHelper *o = self(c);
                uint32_t value;
                AMBRO_LOCK_T(InterruptTempLock(), c, lock_c) {
                    value = o->m_state;
                }
                return ((value >> 16) + ((value >> 15) & 1));
            }
            
            uint32_t m_state;
        };
        
        using Pin = typename TheHelper::RealPin;
        static int const AdcIndex = TypeListIndex<AdcList, IsEqualFunc<Pin>>::value;
        
        TheHelper m_helper;
        
        struct HelperPosition : public MemberPosition<PinPosition<PinIndex>, TheHelper, &AdcPin::m_helper> {};
    };
    
    using PinsTuple = IndexElemTuple<PinsList, AdcPin>;
    using FlatPinsList = MapTypeList<typename PinsTuple::ElemTypes, GetMemberType_Pin>;
    
    template <typename ListElem, typename AccumValue>
    using MaxAdcIndexFoldFunc = WrapInt<max(AccumValue::value, ListElem::AdcIndex)>;
    static int const MaxAdcIndex = TypeListFold<typename PinsTuple::ElemTypes, WrapInt<0>, MaxAdcIndexFoldFunc>::value;
    
    PinsTuple m_pins;
    AvgFeature m_avg_feature;
    
    struct AvgFeaturePosition : public MemberPosition<Position, AvgFeature, &At91Sam3xAdc::m_avg_feature> {};
    template <int PinIndex> struct PinPosition : public TuplePosition<Position, PinsTuple, &At91Sam3xAdc::m_pins, PinIndex> {};
};

#define AMBRO_AT91SAM3X_ADC_GLOBAL(adc, context) \
extern "C" \
__attribute__((used)) \
void ADC_Handler (void) \
{ \
    (adc).adc_isr(MakeInterruptContext(context)); \
}

#include <aprinter/EndNamespace.h>

#endif
