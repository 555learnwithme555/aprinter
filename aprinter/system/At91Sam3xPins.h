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

#ifndef AMBROLIB_AT91SAM3X_PINS_H
#define AMBROLIB_AT91SAM3X_PINS_H

#include <stdint.h>
#include <sam/drivers/pmc/pmc.h>

#include <aprinter/meta/Position.h>
#include <aprinter/base/DebugObject.h>

#include <aprinter/BeginNamespace.h>

template <uint32_t TAddr>
struct At91Sam3xPio {
    static const uint32_t Addr = TAddr;
};

using At91Sam3xPioA = At91Sam3xPio<(uint32_t)PIOA>;
using At91Sam3xPioB = At91Sam3xPio<(uint32_t)PIOB>;
using At91Sam3xPioC = At91Sam3xPio<(uint32_t)PIOC>;
using At91Sam3xPioD = At91Sam3xPio<(uint32_t)PIOD>;

template <typename TPio, int TPinIndex>
struct At91Sam3xPin {
    using Pio = TPio;
    static const int PinIndex = TPinIndex;
};

template <typename Position, typename Context>
class At91Sam3xPins
: private DebugObject<Context, void>
{
    static At91Sam3xPins * self (Context c)
    {
        return PositionTraverse<typename Context::TheRootPosition, Position>(c.root());
    }
    
    template <typename ThePio>
    static Pio volatile * pio ()
    {
        return (Pio volatile *)ThePio::Addr;
    }
    
public:
    static void init (Context c)
    {
        At91Sam3xPins *o = self(c);
        pmc_enable_periph_clk(ID_PIOA);
        pmc_enable_periph_clk(ID_PIOB);
        pmc_enable_periph_clk(ID_PIOC);
        pmc_enable_periph_clk(ID_PIOD);
        o->debugInit(c);
    }
    
    static void deinit (Context c)
    {
        At91Sam3xPins *o = self(c);
        o->debugDeinit(c);
        pmc_disable_periph_clk(ID_PIOD);
        pmc_disable_periph_clk(ID_PIOC);
        pmc_disable_periph_clk(ID_PIOB);
        pmc_disable_periph_clk(ID_PIOA);
    }
    
    template <typename Pin, typename ThisContext>
    static void setInput (ThisContext c)
    {
        At91Sam3xPins *o = self(c);
        o->debugAccess(c);
        
        pio<typename Pin::Pio>()->PIO_ODR = (UINT32_C(1) << Pin::PinIndex);
        pio<typename Pin::Pio>()->PIO_PER = (UINT32_C(1) << Pin::PinIndex);
    }
    
    template <typename Pin, typename ThisContext>
    static void setOutput (ThisContext c)
    {
        At91Sam3xPins *o = self(c);
        o->debugAccess(c);
        
        pio<typename Pin::Pio>()->PIO_OER = (UINT32_C(1) << Pin::PinIndex);
        pio<typename Pin::Pio>()->PIO_PER = (UINT32_C(1) << Pin::PinIndex);
    }
    
    template <typename Pin, typename ThisContext>
    static void setPullup (ThisContext c, bool enabled)
    {
        At91Sam3xPins *o = self(c);
        o->debugAccess(c);
        
        if (enabled) {
            pio<typename Pin::Pio>()->PIO_PUER = (UINT32_C(1) << Pin::PinIndex);
        } else {
            pio<typename Pin::Pio>()->PIO_PUDR = (UINT32_C(1) << Pin::PinIndex);
        }
    }
    
    template <typename Pin, typename ThisContext>
    static bool get (ThisContext c)
    {
        At91Sam3xPins *o = self(c);
        o->debugAccess(c);
        
        return (pio<typename Pin::Pio>()->PIO_PDSR & (UINT32_C(1) << Pin::PinIndex));
    }
    
    template <typename Pin, typename ThisContext>
    static void set (ThisContext c, bool x)
    {
        At91Sam3xPins *o = self(c);
        o->debugAccess(c);
        
        if (x) {
            pio<typename Pin::Pio>()->PIO_SODR = (UINT32_C(1) << Pin::PinIndex);
        } else {
            pio<typename Pin::Pio>()->PIO_CODR = (UINT32_C(1) << Pin::PinIndex);
        }
    }
    
    template <typename Pin>
    static void emergencySet (bool x)
    {
       if (x) {
            pio<typename Pin::Pio>()->PIO_SODR = (UINT32_C(1) << Pin::PinIndex);
        } else {
            pio<typename Pin::Pio>()->PIO_CODR = (UINT32_C(1) << Pin::PinIndex);
        }
     }
};

#include <aprinter/EndNamespace.h>

#endif
