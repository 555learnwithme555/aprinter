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

#ifndef APRINTER_STRING_TOOLS_H
#define APRINTER_STRING_TOOLS_H

#include <stddef.h>
#include <string.h>
#include <stdint.h>

#include <aprinter/base/MemRef.h>
#include <aprinter/base/Hints.h>

namespace APrinter {

static char AsciiToLower (char c)
{
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

static bool AsciiCaseInsensStringEqualToMem (char const *str1, char const *str2, size_t str2_len)
{
    while (*str1 != '\0') {
        if (str2_len == 0 || AsciiToLower(*str1) != AsciiToLower(*str2)) {
            return false;
        }
        str1++;
        str2++;
        str2_len--;
    }
    return (str2_len == 0);
}

static bool StringDecodeHexDigit (char c, int *out)
{
    if (c >= '0' && c <= '9') {
        *out = c - '0';
    }
    else if (c >= 'A' && c <= 'F') {
        *out = 10 + (c - 'A');
    }
    else if (c >= 'a' && c <= 'f') {
        *out = 10 + (c - 'a');
    }
    else {
        return false;
    }
    return true;
}

}

#endif
