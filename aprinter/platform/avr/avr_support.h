/*
 * Copyright (c) 2014 Ambroz Bizjak
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

#ifndef APRINTER_AVR_SUPPORT_H
#define APRINTER_AVR_SUPPORT_H

#include <avr/interrupt.h>
#include <avr/sfr_defs.h>

// Generated file with numeric register addresses.
#include <aprinter_avr_reg_addrs.h>

#include <aprinter/system/InterruptLockCommon.h>

#define APRINTER_INTERRUPT_LOCK_MODE APRINTER_INTERRUPT_LOCK_MODE_SIMPLE

// Replacement for _SFT_IO_ADDR which works reliably as a constant expression,
// based on the register addresses in the generated aprinter_avr_reg_addrs.h.
// The "reg" passed must have an additional underscore to prevent undesired
// expansion according to existing define for the register.
// This only works with registers listed in avr_reg_addr_preprocess.h, more
// registers should be added there as needed.
#define APRINTER_SFR_IO_ADDR(reg) (APrinter_AVR_##reg##ADDR - __SFR_OFFSET)

inline static void memory_barrier (void)
{
    asm volatile ("" : : : "memory");
}

#endif
