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

#ifndef AMBROLIB_AVR_WATCHDOG_H
#define AMBROLIB_AVR_WATCHDOG_H

#include <avr/io.h>
#include <avr/wdt.h>

#include <aprinter/meta/PowerOfTwo.h>
#include <aprinter/meta/Position.h>
#include <aprinter/base/DebugObject.h>

#include <aprinter/BeginNamespace.h>

template <int TWatchdogPrescaler>
struct AvrWatchdogParams {
    static const int WatchdogPrescaler = TWatchdogPrescaler;
};

template <typename Position, typename Context, typename Params>
class AvrWatchdog : private DebugObject<Context, void>
{
    static_assert(Params::WatchdogPrescaler >= 0, "");
    static_assert(Params::WatchdogPrescaler < 10, "");
    
    static AvrWatchdog * self (Context c)
    {
        return PositionTraverse<typename Context::TheRootPosition, Position>(c.root());
    }
    
public:
    static constexpr double WatchdogTime = PowerOfTwoFunc<double>(11 + Params::WatchdogPrescaler) / 131072.0;
    
    static void init (Context c)
    {
        AvrWatchdog *o = self(c);
        
        wdt_enable(Params::WatchdogPrescaler);
        
        o->debugInit(c);
    }
    
    static void deinit (Context c)
    {
        AvrWatchdog *o = self(c);
        o->debugDeinit(c);
        
        wdt_disable();
    }
    
    template <typename ThisContext>
    static void reset (ThisContext c)
    {
        AvrWatchdog *o = self(c);
        o->debugAccess(c);
        
        wdt_reset();
    }
};

#define AMBRO_AVR_WATCHDOG_GLOBAL \
void clear_mcusr () __attribute__((naked)) __attribute__((section("init3"))); \
void clear_mcusr () \
{ \
    MCUSR = 0; \
    wdt_disable(); \
}

#include <aprinter/EndNamespace.h>

#endif
