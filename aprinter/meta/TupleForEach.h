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

#ifndef AMBROLIB_TUPLE_FOR_EACH_H
#define AMBROLIB_TUPLE_FOR_EACH_H

#include <aprinter/meta/TypeList.h>
#include <aprinter/meta/Tuple.h>
#include <aprinter/meta/TypesAreEqual.h>
#include <aprinter/base/Inline.h>
#include <aprinter/base/Likely.h>

#include <aprinter/BeginNamespace.h>

template <typename TheTuple>
struct TupleForEach;

template <typename Head, typename Tail>
struct TupleForEach<Tuple<ConsTypeList<Head, Tail>>> {
    typedef Tuple<ConsTypeList<Head, Tail>> TupleType;
    typedef typename TupleType::TailTupleType TailTupleType;
    
    template <typename Func, typename... Args>
    AMBRO_ALWAYS_INLINE static void call_forward (TupleType *tuple, Func func, Args... args)
    {
        func(tuple->getHead(), args...);
        TupleForEach<TailTupleType>::call_forward(tuple->getTail(), func, args...);
    }
    
    template <typename Func, typename... Args>
    AMBRO_ALWAYS_INLINE static void call_reverse (TupleType *tuple, Func func, Args... args)
    {
        TupleForEach<TailTupleType>::call_reverse(tuple->getTail(), func, args...);
        func(tuple->getHead(), args...);
    }
    
    template <typename Func, typename... Args>
    AMBRO_ALWAYS_INLINE static bool call_forward_interruptible (TupleType *tuple, Func func, Args... args)
    {
        if (!func(tuple->getHead(), args...)) {
            return false;
        }
        return TupleForEach<TailTupleType>::call_forward_interruptible(tuple->getTail(), func, args...);
    }
    
    template <typename AccRes, typename Func, typename... Args>
    AMBRO_ALWAYS_INLINE static auto call_forward_accres (TupleType *tuple, AccRes acc_res, Func func, Args... args) -> decltype(TupleForEach<TailTupleType>::call_forward_accres(tuple->getTail(), func(tuple->getHead(), acc_res, args...), func, args...))
    {
        return TupleForEach<TailTupleType>::call_forward_accres(tuple->getTail(), func(tuple->getHead(), acc_res, args...), func, args...);
    }
};

template <>
struct TupleForEach<Tuple<EmptyTypeList>> {
    typedef Tuple<EmptyTypeList> TupleType;
    
    template <typename Func, typename... Args>
    AMBRO_ALWAYS_INLINE static void call_forward (TupleType *tuple, Func func, Args... args)
    {
    }
    
    template <typename Func, typename... Args>
    AMBRO_ALWAYS_INLINE static void call_reverse (TupleType *tuple, Func func, Args... args)
    {
    }
    
    template <typename Func, typename... Args>
    AMBRO_ALWAYS_INLINE static bool call_forward_interruptible (TupleType *tuple, Func func, Args... args)
    {
        return true;
    }
    
    template <typename AccRes, typename Func, typename... Args>
    AMBRO_ALWAYS_INLINE static auto call_forward_accres (TupleType *tuple, AccRes acc_res, Func func, Args... args) -> decltype(acc_res)
    {
        return acc_res;
    }
};

template <typename TupleType, int Offset, typename Ret, typename IndexType>
struct TupleForOneHelper;

template <typename Head, typename Tail, int Offset, typename Ret, typename IndexType>
struct TupleForOneHelper<Tuple<ConsTypeList<Head, Tail>>, Offset, Ret, IndexType> {
    using TupleType = Tuple<ConsTypeList<Head, Tail>>;
    using TailTupleType = typename TupleType::TailTupleType;
    
    template <typename Func, typename... Args>
    AMBRO_ALWAYS_INLINE static Ret call_always (IndexType index, TupleType *tuple, Func func, Args... args)
    {
        if (AMBRO_LIKELY((index == Offset || TypesAreEqual<TailTupleType, Tuple<EmptyTypeList>>::Value))) {
            return func(tuple->getHead(), args...);
        }
        return TupleForOneHelper<TailTupleType, Offset + 1, Ret, IndexType>::call_always(index, tuple->getTail(), func, args...);
    }
};

template <int Offset, typename Ret, typename IndexType>
struct TupleForOneHelper<Tuple<EmptyTypeList>, Offset, Ret, IndexType> {
    using TupleType = Tuple<EmptyTypeList>;
    
    template <typename Func, typename... Args>
    AMBRO_ALWAYS_INLINE static Ret call_always (IndexType index, TupleType *tuple, Func func, Args... args)
    {
        __builtin_unreachable();
    }
};

template <typename TupleType, typename Func, typename... Args>
AMBRO_ALWAYS_INLINE void TupleForEachForward (TupleType *tuple, Func func, Args... args)
{
    return TupleForEach<TupleType>::call_forward(tuple, func, args...);
}

template <typename TupleType, typename Func, typename... Args>
AMBRO_ALWAYS_INLINE void TupleForEachReverse (TupleType *tuple, Func func, Args... args)
{
    return TupleForEach<TupleType>::call_reverse(tuple, func, args...);
}

template <typename TupleType, typename Func, typename... Args>
AMBRO_ALWAYS_INLINE bool TupleForEachForwardInterruptible (TupleType *tuple, Func func, Args... args)
{
    return TupleForEach<TupleType>::call_forward_interruptible(tuple, func, args...);
}

template <typename TupleType, typename InitialAccRes, typename Func, typename... Args>
AMBRO_ALWAYS_INLINE auto TupleForEachForwardAccRes (TupleType *tuple, InitialAccRes initial_acc_res, Func func, Args... args) -> decltype(TupleForEach<TupleType>::call_forward_accres(tuple, initial_acc_res, func, args...))
{
    return TupleForEach<TupleType>::call_forward_accres(tuple, initial_acc_res, func, args...);
}

template <typename Ret = void, typename IndexType, typename TupleType, typename Func, typename... Args>
AMBRO_ALWAYS_INLINE Ret TupleForOneAlways (IndexType index, TupleType *tuple, Func func, Args... args)
{
    return TupleForOneHelper<TupleType, 0, Ret, IndexType>::call_always(index, tuple, func, args...);
}

#define AMBRO_DECLARE_TUPLE_FOREACH_HELPER(helper_name, func_name) \
struct helper_name { \
    template <typename TupleForEachElemPtrType, typename... TupleForEachArgs> \
    AMBRO_ALWAYS_INLINE auto operator() (TupleForEachElemPtrType tuple_for_each_elem, TupleForEachArgs... tuple_for_each_args) -> decltype(tuple_for_each_elem->func_name(tuple_for_each_args...)) \
    { \
        return tuple_for_each_elem->func_name(tuple_for_each_args...); \
    } \
};

#include <aprinter/EndNamespace.h>

#endif
