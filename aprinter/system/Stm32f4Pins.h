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

#ifndef AMBROLIB_STM32F4_PINS_H
#define AMBROLIB_STM32F4_PINS_H

#include <stdint.h>

#include <aprinter/base/Object.h>
#include <aprinter/base/DebugObject.h>
#include <aprinter/base/Lock.h>
#include <aprinter/system/InterruptLock.h>

#include <aprinter/BeginNamespace.h>

template <uint32_t TGpioAddr>
struct Stm32f4Port {
    static GPIO_TypeDef * gpio () { return (GPIO_TypeDef *)TGpioAddr; }
};

using Stm32f4PortA = Stm32f4Port<GPIOA_BASE>;
using Stm32f4PortB = Stm32f4Port<GPIOB_BASE>;
using Stm32f4PortC = Stm32f4Port<GPIOC_BASE>;
using Stm32f4PortD = Stm32f4Port<GPIOD_BASE>;
using Stm32f4PortE = Stm32f4Port<GPIOE_BASE>;
using Stm32f4PortF = Stm32f4Port<GPIOF_BASE>;
using Stm32f4PortG = Stm32f4Port<GPIOG_BASE>;
using Stm32f4PortH = Stm32f4Port<GPIOH_BASE>;
using Stm32f4PortI = Stm32f4Port<GPIOI_BASE>;
using Stm32f4PortJ = Stm32f4Port<GPIOJ_BASE>;
using Stm32f4PortK = Stm32f4Port<GPIOK_BASE>;

template <typename TPort, int TPinIndex>
struct Stm32f4Pin {
    using Port = TPort;
    static const int PinIndex = TPinIndex;
};

template <uint8_t TPupdr>
struct Stm32f4PinInputMode {
    static uint8_t const Pupdr = TPupdr;
};

using Stm32f4PinInputModeNormal = Stm32f4PinInputMode<0>;
using Stm32f4PinInputModePullUp = Stm32f4PinInputMode<1>;
using Stm32f4PinInputModePullDown = Stm32f4PinInputMode<2>;

template <uint8_t TOptyper>
struct Stm32f4PinOutputMode {
    static uint8_t const Optyper = TOptyper;
};

using Stm32f4PinOutputModeNormal = Stm32f4PinOutputMode<0>;
using Stm32f4PinOutputModeOpenDrain = Stm32f4PinOutputMode<1>;

template <typename Context, typename ParentObject>
class Stm32f4Pins {
public:
    struct Object;
    
private:
    using TheDebugObject = DebugObject<Context, Object>;
    
public:
    static void init (Context c)
    {
        __HAL_RCC_GPIOA_CLK_ENABLE();
        __HAL_RCC_GPIOB_CLK_ENABLE();
        __HAL_RCC_GPIOC_CLK_ENABLE();
        __HAL_RCC_GPIOD_CLK_ENABLE();
        __HAL_RCC_GPIOE_CLK_ENABLE();
        __HAL_RCC_GPIOF_CLK_ENABLE();
        __HAL_RCC_GPIOG_CLK_ENABLE();
        __HAL_RCC_GPIOH_CLK_ENABLE();
        __HAL_RCC_GPIOI_CLK_ENABLE();
        __HAL_RCC_GPIOJ_CLK_ENABLE();
        __HAL_RCC_GPIOK_CLK_ENABLE();
        
        TheDebugObject::init(c);
    }
    
    static void deinit (Context c)
    {
        TheDebugObject::deinit(c);
        
        __HAL_RCC_GPIOK_CLK_DISABLE();
        __HAL_RCC_GPIOJ_CLK_DISABLE();
        __HAL_RCC_GPIOI_CLK_DISABLE();
        __HAL_RCC_GPIOH_CLK_DISABLE();
        __HAL_RCC_GPIOG_CLK_DISABLE();
        __HAL_RCC_GPIOF_CLK_DISABLE();
        __HAL_RCC_GPIOE_CLK_DISABLE();
        __HAL_RCC_GPIOD_CLK_DISABLE();
        __HAL_RCC_GPIOC_CLK_DISABLE();
        __HAL_RCC_GPIOB_CLK_DISABLE();
        __HAL_RCC_GPIOA_CLK_DISABLE();
    }
    
    template <typename Pin, typename Mode = Stm32f4PinInputModeNormal, typename ThisContext>
    static void setInput (ThisContext c)
    {
        TheDebugObject::access(c);
        
        AMBRO_LOCK_T(InterruptTempLock(), c, lock_c) {
            set_moder<Pin, 0>();
            set_pupdr<Pin, Mode::Pupdr>();
        }
    }
    
    template <typename Pin, typename Mode = Stm32f4PinOutputModeNormal, typename ThisContext>
    static void setOutput (ThisContext c)
    {
        TheDebugObject::access(c);
        
        AMBRO_LOCK_T(InterruptTempLock(), c, lock_c) {
            set_optyper<Pin, Mode::Optyper>();
            set_moder<Pin, 1>();
            set_pupdr<Pin, 0>();
        }
    }
    
    template <typename Pin, int AfNumber, typename ThisContext>
    static void setAlternateFunction (ThisContext c)
    {
        TheDebugObject::access(c);
        
        AMBRO_LOCK_T(InterruptTempLock(), c, lock_c) {
            set_af<Pin, AfNumber>();
            set_moder<Pin, 2>();
            set_pupdr<Pin, 0>();
        }
    }
    
    template <typename Pin, typename ThisContext>
    static bool get (ThisContext c)
    {
        TheDebugObject::access(c);
        
        return (Pin::Port::gpio()->IDR & (UINT32_C(1) << Pin::PinIndex));
    }
    
    template <typename Pin, typename ThisContext>
    static void set (ThisContext c, bool x)
    {
        TheDebugObject::access(c);
        
        if (x) {
            Pin::Port::gpio()->BSRR = (UINT32_C(1) << Pin::PinIndex);
        } else {
            Pin::Port::gpio()->BSRR = (UINT32_C(1) << (16 + Pin::PinIndex));
        }
    }
    
    template <typename Pin>
    static void emergencySet (bool x)
    {
        if (x) {
            Pin::Port::gpio()->BSRR = (UINT32_C(1) << Pin::PinIndex);
        } else {
            Pin::Port::gpio()->BSRR = (UINT32_C(1) << (16 + Pin::PinIndex));
        }
    }
    
private:
    template <typename Pin, uint8_t Value>
    static void set_moder ()
    {
        Pin::Port::gpio()->MODER = (Pin::Port::gpio()->MODER & ~(UINT32_C(3) << (2 * Pin::PinIndex))) | ((uint32_t)Value << (2 * Pin::PinIndex));
    }
    
    template <typename Pin, uint8_t Value>
    static void set_pupdr ()
    {
        Pin::Port::gpio()->PUPDR = (Pin::Port::gpio()->PUPDR & ~(UINT32_C(3) << (2 * Pin::PinIndex))) | ((uint32_t)Value << (2 * Pin::PinIndex));
    }
    
    template <typename Pin, uint8_t Value>
    static void set_optyper ()
    {
        Pin::Port::gpio()->OTYPER = (Pin::Port::gpio()->OTYPER & ~(UINT32_C(1) << Pin::PinIndex)) | ((uint32_t)Value << Pin::PinIndex);
    }
    
    template <typename Pin, uint8_t Value>
    static void set_af ()
    {
        if (Pin::PinIndex >= 8) {
            Pin::Port::gpio()->AFR[1] = set_bits(4 * (Pin::PinIndex - 8), 4, Pin::Port::gpio()->AFR[1], Value);
        } else {
            Pin::Port::gpio()->AFR[0] = set_bits(4 * Pin::PinIndex, 4, Pin::Port::gpio()->AFR[0], Value);
        }
    }
    
    static uint32_t set_bits (int offset, int bits, uint32_t x, uint32_t val)
    {
        return (x & (uint32_t)~(((uint32_t)-1 >> (32 - bits)) << offset)) | (uint32_t)(val << offset);
    }
    
public:
    struct Object : public ObjBase<Stm32f4Pins, ParentObject, MakeTypeList<TheDebugObject>> {};
};

#include <aprinter/EndNamespace.h>

#endif
