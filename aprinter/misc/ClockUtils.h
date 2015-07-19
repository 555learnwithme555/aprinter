/*
 * Copyright (c) 2015 Ambroz Bizjak
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

#ifndef APRINTER_CLOCK_UTILS_H
#define APRINTER_CLOCK_UTILS_H

#include <stdint.h>

#include <aprinter/meta/TypesAreEqual.h>

#include <aprinter/BeginNamespace.h>

template <typename Context>
class ClockUtils {
    static_assert(TypesAreEqual<typename Context::Clock::TimeType, uint32_t>::Value, "");
    
public:
    using Clock = typename Context::Clock;
    using TimeType = typename Clock::TimeType;
    static constexpr double time_unit = Clock::time_unit;
    static constexpr double time_freq = Clock::time_freq;
    
    inline static bool timeGreaterOrEqual (TimeType t1, TimeType t2)
    {
        return (TimeType)(t1 - t2) < UINT32_C(0x80000000);
    }
    
    template <typename ThisContext>
    inline static TimeType getTimeAfter (ThisContext c, TimeType after_ticks)
    {
        return (TimeType)(Context::Clock::getTime(c) + after_ticks);
    }
    
    class PollTimer {
    public:
        template <typename ThisContext>
        inline void setAfter (ThisContext c, TimeType after_ticks)
        {
            m_set_time = getTimeAfter(c, after_ticks);
        }
        
        inline void addTime (TimeType add_ticks)
        {
            m_set_time += add_ticks;
        }
        
        template <typename ThisContext>
        inline bool isExpired (ThisContext c)
        {
            return timeGreaterOrEqual(Context::Clock::getTime(c), m_set_time);
        }
        
    private:
        TimeType m_set_time;
    };
};

#include <aprinter/EndNamespace.h>

#endif
