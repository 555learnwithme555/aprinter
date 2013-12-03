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

#ifndef AMBRO_AVR_ASM_DIV_11_16_L15_S13_H
#define AMBRO_AVR_ASM_DIV_11_16_L15_S13_H

#include <stdint.h>

#include <aprinter/meta/Options.h>

#include <aprinter/BeginNamespace.h>

#define DIVIDE_11_16_L15_S13_ITER_17_19(i) \
"    lsl %A[n]\n" \
"    rol %B[n]\n" \
"    cp %A[n],%A[d]\n" \
"    cpc %B[n],%B[d]\n" \
"    brcs zero_bit_" #i "__%=\n" \
"    sub %A[n],%A[d]\n" \
"    sbc %B[n],%B[d]\n" \
"    ori %B[q],1<<(21-" #i ")\n" \
"zero_bit_" #i "__%=:\n"

#define DIVIDE_11_16_L15_S13_ITER_20_21(i) \
"    lsl %A[n]\n" \
"    rol %B[n]\n" \
"    rol __tmp_reg__\n" \
"    cp %A[n],%A[d]\n" \
"    cpc %B[n],%B[d]\n" \
"    cpc __tmp_reg__,__zero_reg__\n" \
"    brcs zero_bit_" #i "__%=\n" \
"    sub %A[n],%A[d]\n" \
"    sbc %B[n],%B[d]\n" \
"    sbc __tmp_reg__,__zero_reg__\n" \
"    ori %B[q],1<<(21-" #i ")\n" \
"zero_bit_" #i "__%=:\n"

#define DIVIDE_11_16_L15_S13_ITER_22_27(i) \
"    lsl %A[n]\n" \
"    rol %B[n]\n" \
"    rol __tmp_reg__\n" \
"    cp %A[n],%A[d]\n" \
"    cpc %B[n],%B[d]\n" \
"    cpc __tmp_reg__,__zero_reg__\n" \
"    brcs zero_bit_" #i "__%=\n" \
"    sub %A[n],%A[d]\n" \
"    sbc %B[n],%B[d]\n" \
"    sbc __tmp_reg__,__zero_reg__\n" \
"    ori %A[q],1<<(29-" #i ")\n" \
"zero_bit_" #i "__%=:\n"

#define DIVIDE_11_16_L15_S13_ITER_28_28(i) \
"    lsl %A[n]\n" \
"    rol %B[n]\n" \
"    rol __tmp_reg__\n" \
"    rol %[tmp]\n" \
"    cp %A[n],%A[d]\n" \
"    cpc %B[n],%B[d]\n" \
"    cpc __tmp_reg__,__zero_reg__\n" \
"    cpc %[tmp],__zero_reg__\n" \
"    brcs zero_bit_" #i "__%=\n" \
"    sub %A[n],%A[d]\n" \
"    sbc %B[n],%B[d]\n" \
"    sbc __tmp_reg__,__zero_reg__\n" \
"    sbc %[tmp],__zero_reg__\n" \
"    ori %A[q],1<<(29-" #i ")\n" \
"zero_bit_" #i "__%=:\n"

/**
 * Division 2^15*(11bit/16bit), saturated to 13 bits.
 * 
 * Cycles in worst case: 133
 * = 4 + (4 * 8) + (1 * 11) + (7 * 11) + 9
 */
__attribute__((always_inline)) inline static uint16_t div_11_16_l15_s13 (uint16_t n, uint16_t d, OptionForceInline opt)
{
    uint16_t q;
    uint8_t tmp;
    
    asm(
        "    clr __tmp_reg__\n"
        "    movw %A[q],__tmp_reg__\n"
        "    clr %[tmp]\n"
        "    lsl %A[n]\n"
        "    rol %B[n]\n"
        "    lsl %A[n]\n"
        "    rol %B[n]\n"
        DIVIDE_11_16_L15_S13_ITER_17_19(17)
        DIVIDE_11_16_L15_S13_ITER_17_19(18)
        DIVIDE_11_16_L15_S13_ITER_17_19(19)
        DIVIDE_11_16_L15_S13_ITER_20_21(20)
        DIVIDE_11_16_L15_S13_ITER_20_21(21)
        DIVIDE_11_16_L15_S13_ITER_22_27(22)
        DIVIDE_11_16_L15_S13_ITER_22_27(23)
        DIVIDE_11_16_L15_S13_ITER_22_27(24)
        DIVIDE_11_16_L15_S13_ITER_22_27(25)
        DIVIDE_11_16_L15_S13_ITER_22_27(26)
        DIVIDE_11_16_L15_S13_ITER_22_27(27)
        DIVIDE_11_16_L15_S13_ITER_28_28(28)
        "    lsl %A[n]\n"
        "    rol %B[n]\n"
        "    rol __tmp_reg__\n"
        "    rol %[tmp]\n"
        "    cp %A[n],%A[d]\n"
        "    cpc %B[n],%B[d]\n"
        "    cpc __tmp_reg__,__zero_reg__\n"
        "    cpc %[tmp],__zero_reg__\n"
        "    sbci %A[q],-1\n"
        
        : [q] "=&d" (q),
          [n] "=&r" (n),
          [tmp] "=&r" (tmp)
        : "[n]" (n),
          [d] "r" (d)
    );
    
    return q;
}

template <typename Option = int>
static uint16_t div_11_16_l15_s13 (uint16_t n, uint16_t d, Option opt = 0)
{
    return div_11_16_l15_s13(n, d, OptionForceInline());
}

#include <aprinter/EndNamespace.h>

#endif
