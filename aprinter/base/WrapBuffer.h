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

#ifndef AMBROLIB_WRAP_BUFFER_H
#define AMBROLIB_WRAP_BUFFER_H

#include <stddef.h>
#include <string.h>

#include <aprinter/meta/MinMax.h>

#include <aprinter/BeginNamespace.h>

struct WrapBuffer {
    static WrapBuffer Make (size_t wrap, char *ptr1, char *ptr2)
    {
        return WrapBuffer{wrap, ptr1, ptr2};
    }
    
    static WrapBuffer Make (char *ptr)
    {
        return WrapBuffer{(size_t)-1, ptr, nullptr};
    }
    
    inline void copyOut (size_t offset, size_t length, char *dst) const
    {
        if (length > 0) {
            if (offset < wrap) {
                size_t to_copy = MinValue(length, wrap - offset);
                memcpy(dst, ptr1 + offset, to_copy);
                offset += to_copy;
                length -= to_copy;
                dst += to_copy;
            }
            if (length > 0) {
                memcpy(dst, ptr2 + (offset - wrap), length);
            }
        }
    }
    
    inline void copyIn (size_t offset, size_t length, char const *src) const
    {
        if (length > 0) {
            if (offset < wrap) {
                size_t to_copy = MinValue(length, wrap - offset);
                memcpy(ptr1 + offset, src, to_copy);
                offset += to_copy;
                length -= to_copy;
                src += to_copy;
            }
            if (length > 0) {
                memcpy(ptr2 + (offset - wrap), src, length);
            }
        }
    }
    
    inline WrapBuffer subFrom (size_t offset) const
    {
        if (offset < wrap) {
            return WrapBuffer::Make(wrap - offset, ptr1 + offset, ptr2);
        } else {
            return WrapBuffer::Make(ptr2 + (offset - wrap));
        }
    }
    
    size_t wrap;
    char *ptr1;
    char *ptr2;
};

#include <aprinter/EndNamespace.h>

#endif
