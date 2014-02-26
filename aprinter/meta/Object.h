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

#ifndef AMBROLIB_OBJECT_H
#define AMBROLIB_OBJECT_H

#include <aprinter/meta/MakeTypeList.h>
#include <aprinter/meta/Tuple.h>
#include <aprinter/meta/TypeListIndex.h>
#include <aprinter/meta/IsEqualFunc.h>
#include <aprinter/meta/MapTypeList.h>
#include <aprinter/meta/TupleGet.h>

#include <aprinter/BeginNamespace.h>

namespace ObjectPrivate {
    AMBRO_DECLARE_GET_MEMBER_TYPE_FUNC(GetMemberType_Object, Object)
}

template <typename TClass, typename TParentObject, typename TNestedClassesList>
struct ObjBase : public Tuple<MapTypeList<TNestedClassesList, ObjectPrivate::GetMemberType_Object>> {
    using Class = TClass;
    using ParentObject = TParentObject;
    using NestedClassesList = TNestedClassesList;
    using NestedClassesTuple = Tuple<MapTypeList<TNestedClassesList, ObjectPrivate::GetMemberType_Object>>;
    
    template <typename Context, typename DelayClass = Class>
    static typename DelayClass::Object * self (Context c)
    {
        static int const IndexInParent = TypeListIndex<typename ParentObject::NestedClassesList, IsEqualFunc<Class>>::value;
        ParentObject *parent_object = ParentObject::self(c);
        return TupleGetElem<IndexInParent>(static_cast<typename ParentObject::NestedClassesTuple *>(parent_object));
    }
};

#include <aprinter/EndNamespace.h>

#endif
