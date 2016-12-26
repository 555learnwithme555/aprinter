/*
 * Copyright (c) 2016 Ambroz Bizjak
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

#ifndef APRINTER_IPSTACK_BOOST_AVL_INDEX_H
#define APRINTER_IPSTACK_BOOST_AVL_INDEX_H

#include <aprinter/meta/ServiceUtils.h>
#include <aprinter/base/Preprocessor.h>
#include <aprinter/base/Accessor.h>
#include <aprinter/base/Assert.h>
#include <aprinter/structure/LinkModel.h>
#include <aprinter/structure/AvlTree.h>
#include <aprinter/structure/TreeCompare.h>

#include <aipstack/BeginNamespace.h>

template <typename Arg>
class AvlTreeIndex {
    APRINTER_USE_TYPES1(Arg, (Entry, HookAccessor, LookupKeyArg, KeyFuncs))
    
    using LinkModel = APrinter::PointerLinkModel<Entry>;
    using State = typename LinkModel::State;
    using Ref = typename LinkModel::Ref;
    using TreeNode = APrinter::AvlTreeNode<LinkModel>;
    
public:
    class Node {
        friend AvlTreeIndex;
        
        TreeNode tree_node;
    };
    
    class Index {
        using TreeNodeAccessor = APrinter::ComposedAccessor<
            HookAccessor,
            APRINTER_MEMBER_ACCESSOR_TN(&Node::tree_node)
        >;
        
        struct TreeCompare : public APrinter::TreeCompare<LinkModel, KeyFuncs> {};
        
        using EntryTree = APrinter::AvlTree<Entry, TreeNodeAccessor, TreeCompare, LinkModel>;
        
    public:
        inline void init ()
        {
            m_tree.init();
        }
        
        inline void addEntry (Entry &e)
        {
            bool inserted = m_tree.insert(State(), e, nullptr);
            AMBRO_ASSERT(inserted)
        }
        
        inline void removeEntry (Entry &e)
        {
            m_tree.remove(State(), e);
        }
        
        inline Entry * findEntry (LookupKeyArg key)
        {
            Entry *entry = m_tree.template lookup<LookupKeyArg>(State(), key).pointer();
            AMBRO_ASSERT(entry == nullptr || KeyFuncs::GetKeyOfEntry(*entry) == key)
            return entry;
        }
        
    private:
        EntryTree m_tree;
    };
};

struct AvlTreeIndexService {
    APRINTER_ALIAS_STRUCT_EXT(Index, (
        APRINTER_AS_TYPE(Entry),
        APRINTER_AS_TYPE(HookAccessor),
        APRINTER_AS_TYPE(LookupKeyArg),
        APRINTER_AS_TYPE(KeyFuncs)
    ), (
        APRINTER_DEF_INSTANCE(Index, AvlTreeIndex)
    ))
};

#include <aipstack/EndNamespace.h>

#endif
