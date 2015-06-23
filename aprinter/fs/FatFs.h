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

#ifndef AMBROLIB_FAT_FS_H
#define AMBROLIB_FAT_FS_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <aprinter/meta/MinMax.h>
#include <aprinter/meta/ChooseInt.h>
#include <aprinter/base/Object.h>
#include <aprinter/base/DebugObject.h>
#include <aprinter/base/Callback.h>
#include <aprinter/base/Assert.h>
#include <aprinter/base/BinaryTools.h>
#include <aprinter/base/WrapBuffer.h>
#include <aprinter/misc/Utf8Encoder.h>
#include <aprinter/misc/AsciiTools.h>
#include <aprinter/structure/DoubleEndedList.h>
#include <aprinter/fs/BlockCache.h>

#include <aprinter/BeginNamespace.h>

template <typename Context, typename ParentObject, typename TheBlockAccess, typename InitHandler, typename Params>
class FatFs {
public:
    struct Object;
    
private:
    using TheDebugObject = DebugObject<Context, Object>;
    using TheBlockCache = BlockCache<Context, Object, TheBlockAccess, Params::NumCacheEntries>;
    
    using BlockAccessUser = typename TheBlockAccess::User;
    using BlockIndexType = typename TheBlockAccess::BlockIndexType;
    static size_t const BlockSize = TheBlockAccess::BlockSize;
    using CacheBlockRef = typename TheBlockCache::CacheRef;
    
    static_assert(BlockSize >= 0x47, "BlockSize not enough for EBPB");
    static_assert(BlockSize % 32 == 0, "BlockSize not a multiple of 32");
    static_assert(BlockSize >= 512, "BlockSize not enough for FS Information Sector");
    using ClusterIndexType = uint32_t;
    using ClusterBlockIndexType = uint16_t;
    static size_t const FatEntriesPerBlock = BlockSize / 4;
    static size_t const DirEntriesPerBlock = BlockSize / 32;
    using DirEntriesPerBlockType = ChooseIntForMax<DirEntriesPerBlock, false>;
    static_assert(Params::MaxFileNameSize >= 12, "");
    using FileNameLenType = ChooseIntForMax<Params::MaxFileNameSize, false>;
    static_assert(Params::NumCacheEntries >= 2, "");
    
    enum class FsState : uint8_t {INIT, READY, FAILED};
    enum class AllocationState : uint8_t {IDLE, CHECK_EVENT, REQUESTING_BLOCK};
    
    class ClusterChain;
    class ClusterRef;
    class DirectoryIterator;
    class FsInfo;
    
public:
    enum class EntryType : uint8_t {DIR, FILE};
    
    class FsEntry {
        friend FatFs;
        
    public:
        inline EntryType getType () const { return type; }
        inline uint32_t getFileSize () const { return file_size; }
        
    private:
        EntryType type;
        uint32_t file_size;
        ClusterIndexType cluster_index;
    };
    
    static bool isPartitionTypeSupported (uint8_t type)
    {
        return (type == 0xB || type == 0xC);
    }
    
    static void init (Context c, typename TheBlockAccess::BlockRange block_range)
    {
        auto *o = Object::self(c);
        
        TheBlockCache::init(c);
        o->alloc_event.init(c, APRINTER_CB_STATFUNC_T(&FatFs::alloc_event_handler));
        o->first_block_ref.init(c, APRINTER_CB_STATFUNC_T(&FatFs::first_block_ref_handler));
        o->alloc_block_ref.init(c, APRINTER_CB_STATFUNC_T(&FatFs::alloc_block_ref_handler));
        
        o->block_range = block_range;
        o->state = FsState::INIT;
        o->first_block_ref.requestBlock(c, get_abs_block_index(c, 0), 0, 1, true);
        
        TheDebugObject::init(c);
    }
    
    static void deinit (Context c)
    {
        auto *o = Object::self(c);
        TheDebugObject::deinit(c);
        
        o->alloc_block_ref.deinit(c);
        o->first_block_ref.deinit(c);
        o->alloc_event.deinit(c);
        TheBlockCache::deinit(c);
    }
    
    static FsEntry getRootEntry (Context c)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        AMBRO_ASSERT(o->state == FsState::READY)
        
        FsEntry entry;
        entry.type = EntryType::DIR;
        entry.file_size = 0;
        entry.cluster_index = o->root_cluster;
        return entry;
    }
    
    class DirLister {
    public:
        using DirListerHandler = Callback<void(Context c, bool is_error, char const *name, FsEntry entry)>;
        
        void init (Context c, FsEntry dir_entry, DirListerHandler handler)
        {
            auto *o = Object::self(c);
            TheDebugObject::access(c);
            AMBRO_ASSERT(o->state == FsState::READY)
            AMBRO_ASSERT(dir_entry.type == EntryType::DIR)
            
            m_dir_iter.init(c, dir_entry.cluster_index, handler);
        }
        
        void deinit (Context c)
        {
            TheDebugObject::access(c);
            
            m_dir_iter.deinit(c);
        }
        
        void requestEntry (Context c)
        {
            TheDebugObject::access(c);
            
            m_dir_iter.requestEntry(c);
        }
        
    private:
        DirectoryIterator m_dir_iter;
    };
    
    class Opener {
        enum class State : uint8_t {REQUESTING_ENTRY, COMPLETED};
        
    public:
        enum class OpenerStatus : uint8_t {SUCCESS, NOT_FOUND, ERROR};
        
        using OpenerHandler = Callback<void(Context c, OpenerStatus status, FsEntry entry)>;
        
        void init (Context c, FsEntry dir_entry, EntryType entry_type, char const *name, bool case_insens, OpenerHandler handler)
        {
            auto *o = Object::self(c);
            TheDebugObject::access(c);
            AMBRO_ASSERT(o->state == FsState::READY)
            AMBRO_ASSERT(dir_entry.type == EntryType::DIR)
            AMBRO_ASSERT(name)
            
            m_entry_type = entry_type;
            m_name = name;
            m_case_insens = case_insens;
            m_handler = handler;
            m_state = State::REQUESTING_ENTRY;
            m_dir_iter.init(c, dir_entry.cluster_index, APRINTER_CB_OBJFUNC_T(&Opener::dir_iter_handler, this));
            m_dir_iter.requestEntry(c);
        }
        
        void deinit (Context c)
        {
            if (m_state != State::COMPLETED) {
                m_dir_iter.deinit(c);
            }
        }
        
    private:
        void dir_iter_handler (Context c, bool is_error, char const *name, FsEntry entry)
        {
            TheDebugObject::access(c);
            AMBRO_ASSERT(m_state == State::REQUESTING_ENTRY)
            
            if (is_error || !name) {
                m_state = State::COMPLETED;
                m_dir_iter.deinit(c);
                OpenerStatus status = is_error ? OpenerStatus::ERROR : OpenerStatus::NOT_FOUND;
                return m_handler(c, status, FsEntry{});
            }
            if (entry.type != m_entry_type || !compare_filename_equal(name, m_name)) {
                m_dir_iter.requestEntry(c);
                return;
            }
            m_state = State::COMPLETED;
            m_dir_iter.deinit(c);
            return m_handler(c, OpenerStatus::SUCCESS, entry);
        }
        
        bool compare_filename_equal (char const *str1, char const *str2)
        {
            return m_case_insens ? AsciiCaseInsensStringEqual(str1, str2) : !strcmp(str1, str2);
        }
        
        EntryType m_entry_type;
        char const *m_name;
        bool m_case_insens;
        OpenerHandler m_handler;
        State m_state;
        DirectoryIterator m_dir_iter;
    };
    
    class File {
        enum class State : uint8_t {IDLE, EVENT, NEXT_CLUSTER, READING_DATA};
        
    public:
        using FileHandler = Callback<void(Context c, bool error, size_t length)>;
        
        void init (Context c, FsEntry file_entry, FileHandler handler)
        {
            auto *o = Object::self(c);
            TheDebugObject::access(c);
            AMBRO_ASSERT(o->state == FsState::READY)
            AMBRO_ASSERT(file_entry.type == EntryType::FILE)
            
            m_event.init(c, APRINTER_CB_OBJFUNC_T(&File::event_handler, this));
            m_chain.init(c, file_entry.cluster_index, APRINTER_CB_OBJFUNC_T(&File::chain_handler, this));
            m_block_user.init(c, APRINTER_CB_OBJFUNC_T(&File::block_user_handler, this));
            
            m_handler = handler;
            m_file_size = file_entry.file_size;
            m_state = State::IDLE;
            m_rem_file_size = m_file_size;
            m_block_in_cluster = o->blocks_per_cluster;
        }
        
        // NOTE: Not allowed when reader is busy, except when deiniting the whole FatFs and underlying storage!
        void deinit (Context c)
        {
            TheDebugObject::access(c);
            
            m_block_user.deinit(c);
            m_chain.deinit(c);
            m_event.deinit(c);
        }
        
        void rewind (Context c)
        {
            auto *o = Object::self(c);
            TheDebugObject::access(c);
            AMBRO_ASSERT(m_state == State::IDLE)
            
            m_chain.rewind(c);
            m_rem_file_size = m_file_size;
            m_block_in_cluster = o->blocks_per_cluster;
        }
        
        void requestBlock (Context c, WrapBuffer buf)
        {
            TheDebugObject::access(c);
            AMBRO_ASSERT(m_state == State::IDLE)
            
            m_request_buf = buf;
            m_state = State::EVENT;
            m_event.prependNowNotAlready(c);
        }
        
    private:
        void complete_request (Context c, bool error, size_t length=0)
        {
            m_state = State::IDLE;
            return m_handler(c, error, length);
        }
        
        void event_handler (Context c)
        {
            auto *o = Object::self(c);
            TheDebugObject::access(c);
            AMBRO_ASSERT(m_state == State::EVENT)
            AMBRO_ASSERT(m_block_in_cluster <= o->blocks_per_cluster)
            
            if (m_rem_file_size == 0) {
                return complete_request(c, false);
            }
            if (m_block_in_cluster == o->blocks_per_cluster) {
                m_state = State::NEXT_CLUSTER;
                m_chain.requestNext(c);
                return;
            }
            if (!is_cluster_idx_valid_for_data(c, m_chain.getCurrentCluster(c))) {
                return complete_request(c, true);
            }
            m_state = State::READING_DATA;
            BlockIndexType block_idx = get_cluster_data_block_index(c, m_chain.getCurrentCluster(c), m_block_in_cluster);
            m_block_user.startRead(c, get_abs_block_index(c, block_idx), m_request_buf);
        }
        
        void chain_handler (Context c, bool error)
        {
            auto *o = Object::self(c);
            TheDebugObject::access(c);
            AMBRO_ASSERT(m_state == State::NEXT_CLUSTER)
            AMBRO_ASSERT(m_rem_file_size > 0)
            AMBRO_ASSERT(m_block_in_cluster == o->blocks_per_cluster)
            
            if (error || m_chain.endReached(c)) {
                return complete_request(c, true);
            }
            m_block_in_cluster = 0;
            m_state = State::EVENT;
            m_event.prependNowNotAlready(c);
        }
        
        void block_user_handler (Context c, bool error)
        {
            auto *o = Object::self(c);
            TheDebugObject::access(c);
            AMBRO_ASSERT(m_state == State::READING_DATA)
            AMBRO_ASSERT(m_rem_file_size > 0)
            AMBRO_ASSERT(m_block_in_cluster < o->blocks_per_cluster)
            
            if (error) {
                return complete_request(c, error);
            }
            m_block_in_cluster++;
            size_t read_length = MinValue((uint32_t)BlockSize, m_rem_file_size);
            m_rem_file_size -= read_length;
            return complete_request(c, false, read_length);
        }
        
        typename Context::EventLoop::QueuedEvent m_event;
        ClusterChain m_chain;
        BlockAccessUser m_block_user;
        FileHandler m_handler;
        uint32_t m_file_size;
        State m_state;
        uint32_t m_rem_file_size;
        ClusterBlockIndexType m_block_in_cluster;
        WrapBuffer m_request_buf;
    };
    
private:
    static void first_block_ref_handler (Context c, bool error)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        AMBRO_ASSERT(o->state == FsState::INIT)
        
        uint8_t error_code = 99;
        do {
            if (error) {
                error_code = 20;
                goto error;
            }
            
            char const *buffer = o->first_block_ref.getData(c);
            
            uint16_t sector_size =          ReadBinaryInt<uint16_t, BinaryLittleEndian>(buffer + 0xB);
            uint8_t sectors_per_cluster =   ReadBinaryInt<uint8_t,  BinaryLittleEndian>(buffer + 0xD);
            uint16_t num_reserved_sectors = ReadBinaryInt<uint16_t, BinaryLittleEndian>(buffer + 0xE);
            uint8_t num_fats =              ReadBinaryInt<uint8_t,  BinaryLittleEndian>(buffer + 0x10);
            uint16_t max_root =             ReadBinaryInt<uint16_t, BinaryLittleEndian>(buffer + 0x11);
            uint32_t sectors_per_fat =      ReadBinaryInt<uint32_t, BinaryLittleEndian>(buffer + 0x24);
            uint32_t root_cluster =         ReadBinaryInt<uint32_t, BinaryLittleEndian>(buffer + 0x2C);
            uint16_t fs_info_sector =       ReadBinaryInt<uint16_t, BinaryLittleEndian>(buffer + 0x30);
            uint8_t sig =                   ReadBinaryInt<uint8_t,  BinaryLittleEndian>(buffer + 0x42);
            
            o->first_block_ref.reset(c);
            
            if (sector_size == 0 || sector_size % BlockSize != 0) {
                error_code = 22;
                goto error;
            }
            uint16_t blocks_per_sector = sector_size / BlockSize;
            
            if (sectors_per_cluster > UINT16_MAX / blocks_per_sector) {
                error_code = 23;
                goto error;
            }
            o->blocks_per_cluster = blocks_per_sector * sectors_per_cluster;
            
            if ((uint32_t)num_reserved_sectors * sector_size < 0x47) {
                error_code = 24;
                goto error;
            }
            
            if (num_fats != 1 && num_fats != 2) {
                error_code = 25;
                goto error;
            }
            o->num_fats = num_fats;
            
            if (sig != 0x28 && sig != 0x29) {
                error_code = 26;
                goto error;
            }
            
            if (max_root != 0) {
                error_code = 27;
                goto error;
            }
            
            o->root_cluster = mask_cluster_entry(root_cluster);
            if (o->root_cluster < 2) {
                error_code = 28;
                goto error;
            }
            
            uint16_t entries_per_sector = sector_size / 4;
            if (sectors_per_fat == 0 || sectors_per_fat > UINT32_MAX / entries_per_sector) {
                error_code = 29;
                goto error;
            }
            o->num_fat_entries = (ClusterIndexType)sectors_per_fat * entries_per_sector;
            
            uint64_t fat_end_sectors_calc = (uint64_t)num_reserved_sectors + (uint64_t)o->num_fats * sectors_per_fat;
            if (fat_end_sectors_calc > o->block_range.getLength() / blocks_per_sector) {
                error_code = 29;
                goto error;
            }
            o->num_reserved_blocks = (BlockIndexType)num_reserved_sectors * blocks_per_sector;
            o->fat_end_blocks = fat_end_sectors_calc * blocks_per_sector;
            
            uint32_t fs_info_block_calc = fs_info_sector * (uint32_t)blocks_per_sector;
            if (fs_info_block_calc >= o->num_reserved_blocks) {
                error_code = 31;
                goto error;
            }
            o->fs_info_block = fs_info_block_calc;
            
            ClusterIndexType valid_clusters_for_capacity = (o->block_range.getLength() - o->fat_end_blocks) / o->blocks_per_cluster;
            if (valid_clusters_for_capacity < 1) {
                error_code = 30;
                goto error;
            }
            o->num_valid_clusters = MinValue(valid_clusters_for_capacity, MinValue((ClusterIndexType)(o->num_fat_entries - 2), UINT32_C(0xFFFFFF6)));
            
            o->cluster_refs_list.init();
            o->alloc_state = AllocationState::IDLE;
            o->alloc_position = 0;
            
            error_code = 0;
        } while (0);
        
    error:
        o->state = error_code ? FsState::FAILED : FsState::READY;
        return InitHandler::call(c, error_code);
    }
    
    static ClusterIndexType mask_cluster_entry (uint32_t entry_value)
    {
        return (entry_value & UINT32_C(0x0FFFFFFF));
    }
    
    static uint32_t update_cluster_entry (uint32_t entry_value, ClusterIndexType new_value)
    {
        return (entry_value & UINT32_C(0xF0000000)) | new_value;
    }
    
    static bool is_cluster_idx_normal (ClusterIndexType cluster_idx)
    {
        return (cluster_idx >= 2 && cluster_idx < UINT32_C(0xFFFFFF8));
    }
    
    static bool is_cluster_idx_valid_for_fat (Context c, ClusterIndexType cluster_idx)
    {
        auto *o = Object::self(c);
        return cluster_idx < o->num_fat_entries;
    }
    
    static bool request_fat_cache_block (Context c, CacheBlockRef *block_ref, ClusterIndexType cluster_idx, bool disable_immediate_completion)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(is_cluster_idx_valid_for_fat(c, cluster_idx))
        
        BlockIndexType block_idx = o->num_reserved_blocks + (cluster_idx / FatEntriesPerBlock);
        BlockIndexType num_blocks_per_fat = o->num_fat_entries / FatEntriesPerBlock;
        return block_ref->requestBlock(c, get_abs_block_index(c, block_idx), num_blocks_per_fat, o->num_fats, disable_immediate_completion);
    }
    
    static char * get_fat_ptr_in_cache_block (Context c, CacheBlockRef *block_ref, ClusterIndexType cluster_idx)
    {
        AMBRO_ASSERT(is_cluster_idx_valid_for_fat(c, cluster_idx))
        
        return block_ref->getData(c) + ((size_t)4 * (cluster_idx % FatEntriesPerBlock));
    }
    
    static bool is_cluster_idx_valid_for_data (Context c, ClusterIndexType cluster_idx)
    {
        auto *o = Object::self(c);
        return cluster_idx >= 2 && cluster_idx - 2 < o->num_valid_clusters;
    }
    
    static BlockIndexType get_cluster_data_block_index (Context c, ClusterIndexType cluster_idx, ClusterBlockIndexType cluster_block_idx)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(is_cluster_idx_valid_for_data(c, cluster_idx))
        AMBRO_ASSERT(cluster_block_idx < o->blocks_per_cluster)
        
        return o->fat_end_blocks + ((BlockIndexType)(cluster_idx - 2) * o->blocks_per_cluster) + cluster_block_idx;
    }
    
    static BlockIndexType get_abs_block_index (Context c, BlockIndexType rel_block)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(rel_block < o->block_range.getLength())
        
        return o->block_range.getAbsBlockIndex(rel_block);
    }
    
    static void allocation_request_added (Context c)
    {
        auto *o = Object::self(c);
        if (o->alloc_state == AllocationState::IDLE) {
            start_new_allocation(c);
        }
    }
    
    static void start_new_allocation (Context c)
    {
        auto *o = Object::self(c);
        o->alloc_state = AllocationState::CHECK_EVENT;
        o->alloc_start = o->alloc_position;
        o->alloc_event.prependNowNotAlready(c);
    }
    
    static void complete_allocation_with_error (Context c)
    {
        complete_allocation_if_unused(c, true, 0);
    }
    
    static bool complete_allocation_if_unused (Context c, bool error, ClusterIndexType cluster_index)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(o->alloc_state != AllocationState::IDLE)
        
        ClusterRef *complete_request = nullptr;
        bool have_more_requests = false;
        for (ClusterRef *ref = o->cluster_refs_list.first(); ref; ref = o->cluster_refs_list.next(ref)) {
            AMBRO_ASSERT(ref->m_state != ClusterRef::State::INVALID)
            if (ref->m_state == ClusterRef::State::ALLOCATING) {
                if (!complete_request) {
                    complete_request = ref;
                } else {
                    have_more_requests = true;
                }
            } else if (!error && ref->m_index == cluster_index) {
                return false;
            }
        }
        
        if (have_more_requests) {
            start_new_allocation(c);
        } else {
            o->alloc_state = AllocationState::IDLE;
        }
        
        if (complete_request) {
            complete_request->allocation_result(c, error, cluster_index);
        }
        
        return true;
    }
    
    static void alloc_event_handler (Context c)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(o->alloc_state == AllocationState::CHECK_EVENT)
        
        while (true) {
            ClusterIndexType current_cluster = 2 + o->alloc_position;
            
            if (!request_fat_cache_block(c, &o->alloc_block_ref, current_cluster, false)) {
                o->alloc_state = AllocationState::REQUESTING_BLOCK;
                return;
            }
            
            o->alloc_position++;
            if (o->alloc_position == o->num_valid_clusters) {
                o->alloc_position = 0;
            }
            
            ClusterIndexType fat_value = mask_cluster_entry(ReadBinaryInt<uint32_t, BinaryLittleEndian>(get_fat_ptr_in_cache_block(c, &o->alloc_block_ref, current_cluster)));
            if (fat_value == 0) {
                if (complete_allocation_if_unused(c, false, current_cluster)) {
                    return;
                }
            }
            
            if (o->alloc_position == o->alloc_start) {
                return complete_allocation_with_error(c);
            }
        }
    }
    
    static void alloc_block_ref_handler (Context c, bool error)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(o->alloc_state == AllocationState::REQUESTING_BLOCK)
        
        if (error) {
            return complete_allocation_with_error(c);
        }
        o->alloc_state = AllocationState::CHECK_EVENT;
        o->alloc_event.prependNowNotAlready(c);
    }
    
    class ClusterChain {
        enum class State : uint8_t {IDLE, REQUEST_NEXT_CHECK, READING_FAT_FOR_NEXT};
        enum class IterState : uint8_t {START, CLUSTER, END};
        
    public:
        using ClusterChainHandler = Callback<void(Context c, bool error)>;
        
        void init (Context c, ClusterIndexType first_cluster, ClusterChainHandler handler)
        {
            m_event.init(c, APRINTER_CB_OBJFUNC_T(&ClusterChain::event_handler, this));
            m_fat_cache_ref.init(c, APRINTER_CB_OBJFUNC_T(&ClusterChain::fat_cache_ref_handler, this));
            
            m_handler = handler;
            m_state = State::IDLE;
            m_first_cluster = first_cluster;
            
            rewind_internal(c);
        }
        
        void deinit (Context c)
        {
            m_fat_cache_ref.deinit(c);
            m_event.deinit(c);
        }
        
        void rewind (Context c)
        {
            AMBRO_ASSERT(m_state == State::IDLE)
            
            rewind_internal(c);
        }
        
        void requestNext (Context c)
        {
            AMBRO_ASSERT(m_state == State::IDLE)
            
            m_state = State::REQUEST_NEXT_CHECK;
            m_event.prependNowNotAlready(c);
        }
        
        bool endReached (Context c)
        {
            AMBRO_ASSERT(m_state == State::IDLE)
            
            return m_iter_state == IterState::END;
        }
        
        ClusterIndexType getCurrentCluster (Context c)
        {
            AMBRO_ASSERT(m_state == State::IDLE)
            AMBRO_ASSERT(m_iter_state == IterState::CLUSTER)
            
            return m_current_cluster;
        }
        
    private:
        void rewind_internal (Context c)
        {
            m_iter_state = IterState::START;
            m_current_cluster = m_first_cluster;
            m_prev_cluster = 0;
        }
        
        void complete_request (Context c, bool error)
        {
            m_state = State::IDLE;
            return m_handler(c, error);
        }
        
        void event_handler (Context c)
        {
            switch (m_state) {
                case State::REQUEST_NEXT_CHECK: {
                    if (m_iter_state == IterState::CLUSTER) {
                        if (!is_cluster_idx_valid_for_fat(c, m_current_cluster)) {
                            return complete_request(c, true);
                        }
                        
                        if (!request_fat_cache_block(c, &m_fat_cache_ref, m_current_cluster, false)) {
                            m_state = State::READING_FAT_FOR_NEXT;
                            return;
                        }
                        
                        m_prev_cluster = m_current_cluster;
                        m_current_cluster = mask_cluster_entry(ReadBinaryInt<uint32_t, BinaryLittleEndian>(get_fat_ptr_in_cache_block(c, &m_fat_cache_ref, m_current_cluster)));
                    }
                    
                    if (m_iter_state != IterState::END) {
                        if (is_cluster_idx_normal(m_current_cluster)) {
                            m_iter_state = IterState::CLUSTER;
                        } else {
                            m_iter_state = IterState::END;
                        }
                    }
                    
                    return complete_request(c, false);
                } break;
                
                default:
                    AMBRO_ASSERT(false);
            }
        }
        
        void fat_cache_ref_handler (Context c, bool error)
        {
            switch (m_state) {
                case State::READING_FAT_FOR_NEXT: {
                    if (error) {
                        return complete_request(c, true);
                    }
                    m_state = State::REQUEST_NEXT_CHECK;
                    m_event.prependNowNotAlready(c);
                } break;
                
                default:
                    AMBRO_ASSERT(false);
            }
        }
        
        typename Context::EventLoop::QueuedEvent m_event;
        CacheBlockRef m_fat_cache_ref;
        ClusterChainHandler m_handler;
        State m_state;
        IterState m_iter_state;
        ClusterIndexType m_first_cluster;
        ClusterIndexType m_current_cluster;
        ClusterIndexType m_prev_cluster;
    };
    
    class ClusterRef {
        friend FatFs;
        enum class State : uint8_t {INVALID, ALLOCATING, REQUESTING_BLOCK, READY};
        
    public:
        using ClusterRefHandler = Callback<void(Context c, bool error)>;
        
        void init (Context c, ClusterRefHandler handler)
        {
            m_fat_block_ref.init(c, APRINTER_CB_OBJFUNC_T(&ClusterRef::fat_block_ref_handler, this));
            m_handler = handler;
            m_state = State::INVALID;
        }
        
        void deinit (Context c)
        {
            cleanup(c);
            m_fat_block_ref.deinit(c);
        }
        
        void reset (Context c)
        {
            cleanup(c);
        }
        
        bool requestCluster (Context c, ClusterIndexType index, bool disable_immediate_completion)
        {
            auto *o = Object::self(c);
            AMBRO_ASSERT(m_state == State::INVALID)
            AMBRO_ASSERT(is_cluster_idx_valid_for_fat(c, index))
            
            o->cluster_refs_list.append(this);
            return start_request_with_index(c, index, disable_immediate_completion);
        }
        
        void requestAllocation (Context c)
        {
            auto *o = Object::self(c);
            AMBRO_ASSERT(m_state == State::INVALID)
            
            m_state = State::ALLOCATING;
            o->cluster_refs_list.append(this);
            allocation_request_added(c);
        }
        
        ClusterIndexType getIndex (Context c)
        {
            AMBRO_ASSERT(m_state == State::READY)
            
            return m_index;
        }
        
        ClusterIndexType getFatValue (Context c)
        {
            AMBRO_ASSERT(m_state == State::READY)
            
            uint32_t full_value = ReadBinaryInt<uint32_t, BinaryLittleEndian>(get_fat_ptr(c));
            return mask_cluster_entry(full_value);
        }
        
        void setFatValue (Context c, ClusterIndexType value)
        {
            AMBRO_ASSERT(m_state == State::READY)
            
            uint32_t prev_full_value = ReadBinaryInt<uint32_t, BinaryLittleEndian>(get_fat_ptr(c));
            uint32_t new_full_value = update_cluster_entry(prev_full_value, value);
            WriteBinaryInt<uint32_t, BinaryLittleEndian>(new_full_value, get_fat_ptr(c));
            m_fat_block_ref.markDirty(c);
        }
        
    private:
        void cleanup (Context c)
        {
            auto *o = Object::self(c);
            if (m_state != State::INVALID) {
                o->cluster_refs_list.remove(this);
            }
            m_fat_block_ref.reset(c);
            m_state = State::INVALID;
        }
        
        bool start_request_with_index (Context c, ClusterIndexType index, bool disable_immediate_completion)
        {
            AMBRO_ASSERT(is_cluster_idx_valid_for_fat(c, index))
            
            m_index = index;
            bool immediately_completed = request_fat_cache_block(c, &m_fat_block_ref, index, disable_immediate_completion);
            m_state = immediately_completed ? State::READY : State::REQUESTING_BLOCK;
            return immediately_completed;
        }
        
        void fat_block_ref_handler (Context c, bool error)
        {
            auto *o = Object::self(c);
            AMBRO_ASSERT(m_state == State::REQUESTING_BLOCK)
            
            if (error) {
                m_state = State::INVALID;
                o->cluster_refs_list.remove(this);
            } else {
                m_state = State::READY;
            }
            return m_handler(c, error);
        }
        
        void allocation_result (Context c, bool error, ClusterIndexType index)
        {
            auto *o = Object::self(c);
            AMBRO_ASSERT(m_state == State::ALLOCATING)
            AMBRO_ASSERT(error || is_cluster_idx_valid_for_fat(c, index))
            
            if (error) {
                m_state = State::INVALID;
                o->cluster_refs_list.remove(this);
                return m_handler(c, error);
            }
            start_request_with_index(c, index, true);
        }
        
        char * get_fat_ptr (Context c)
        {
            return get_fat_ptr_in_cache_block(c, &m_fat_block_ref, m_index);
        }
        
        CacheBlockRef m_fat_block_ref;
        ClusterRefHandler m_handler;
        State m_state;
        ClusterIndexType m_index;
        DoubleEndedListNode<ClusterRef> m_node;
    };
    
    class DirectoryIterator {
        enum class State : uint8_t {WAIT_REQUEST, CHECK_NEXT_EVENT, REQUESTING_CLUSTER, REQUESTING_BLOCK};
        
    public:
        using DirectoryIteratorHandler = Callback<void(Context c, bool is_error, char const *name, FsEntry entry)>;
        
        void init (Context c, ClusterIndexType first_cluster, DirectoryIteratorHandler handler)
        {
            auto *o = Object::self(c);
            
            m_event.init(c, APRINTER_CB_OBJFUNC_T(&DirectoryIterator::event_handler, this));
            m_chain.init(c, first_cluster, APRINTER_CB_OBJFUNC_T(&DirectoryIterator::chain_handler, this));
            m_dir_block_ref.init(c, APRINTER_CB_OBJFUNC_T(&DirectoryIterator::dir_block_ref_handler, this));
            
            m_handler = handler;
            m_state = State::WAIT_REQUEST;
            m_block_in_cluster = o->blocks_per_cluster;
            m_block_entry_pos = DirEntriesPerBlock;
            m_vfat_seq = -1;
        }
        
        void deinit (Context c)
        {
            m_dir_block_ref.deinit(c);
            m_chain.deinit(c);
            m_event.deinit(c);
        }
        
        void requestEntry (Context c)
        {
            AMBRO_ASSERT(m_state == State::WAIT_REQUEST)
            
            schedule_event(c);
        }
        
    private:
        void complete_request (Context c, bool error, char const *name=nullptr, FsEntry entry=FsEntry{})
        {
            m_state = State::WAIT_REQUEST;
            return m_handler(c, error, name, entry);
        }
        
        void schedule_event (Context c)
        {
            m_state = State::CHECK_NEXT_EVENT;
            m_event.prependNowNotAlready(c);
        }
        
        void event_handler (Context c)
        {
            auto *o = Object::self(c);
            TheDebugObject::access(c);
            AMBRO_ASSERT(m_state == State::CHECK_NEXT_EVENT)
            
            if (m_block_entry_pos == DirEntriesPerBlock) {
                if (m_block_in_cluster == o->blocks_per_cluster) {
                    m_chain.requestNext(c);
                    m_state = State::REQUESTING_CLUSTER;
                    return;
                }
                
                if (!is_cluster_idx_valid_for_data(c, m_chain.getCurrentCluster(c))) {
                    return complete_request(c, true);
                }
                
                BlockIndexType block_idx = get_cluster_data_block_index(c, m_chain.getCurrentCluster(c), m_block_in_cluster);
                if (!m_dir_block_ref.requestBlock(c, get_abs_block_index(c, block_idx), 0, 1)) {
                    m_state = State::REQUESTING_BLOCK;
                    return;
                }
                
                m_block_in_cluster++;
                m_block_entry_pos = 0;
            }
            
            char const *entry_ptr = m_dir_block_ref.getData(c) + ((size_t)m_block_entry_pos * 32);
            uint8_t first_byte =    ReadBinaryInt<uint8_t, BinaryLittleEndian>(entry_ptr + 0x0);
            uint8_t attrs =         ReadBinaryInt<uint8_t, BinaryLittleEndian>(entry_ptr + 0xB);
            uint8_t type_byte =     ReadBinaryInt<uint8_t, BinaryLittleEndian>(entry_ptr + 0xC);
            uint8_t checksum_byte = ReadBinaryInt<uint8_t, BinaryLittleEndian>(entry_ptr + 0xD);
            
            if (first_byte == 0) {
                return complete_request(c, false);
            }
            
            m_block_entry_pos++;
            
            // VFAT entry
            if (first_byte != 0xE5 && attrs == 0xF && type_byte == 0) {
                int8_t entry_vfat_seq = first_byte & 0x1F;
                if ((first_byte & 0x60) == 0x40) {
                    // Start collection.
                    m_vfat_seq = entry_vfat_seq;
                    m_vfat_csum = checksum_byte;
                    m_filename_pos = Params::MaxFileNameSize;
                }
                
                if (entry_vfat_seq > 0 && m_vfat_seq != -1 && entry_vfat_seq == m_vfat_seq && checksum_byte == m_vfat_csum) {
                    // Collect entry.
                    char name_data[26];
                    memcpy(name_data + 0, entry_ptr + 0x1, 10);
                    memcpy(name_data + 10, entry_ptr + 0xE, 12);
                    memcpy(name_data + 22, entry_ptr + 0x1C, 4);
                    size_t chunk_len = 0;
                    for (size_t i = 0; i < sizeof(name_data); i += 2) {
                        uint16_t ch = ReadBinaryInt<uint16_t, BinaryLittleEndian>(name_data + i);
                        if (ch == 0) {
                            break;
                        }
                        char enc_buf[4];
                        int enc_len = Utf8EncodeChar(ch, enc_buf);
                        if (enc_len > m_filename_pos - chunk_len) {
                            goto cancel_vfat;
                        }
                        memcpy(m_filename + chunk_len, enc_buf, enc_len);
                        chunk_len += enc_len;
                    }
                    memmove(m_filename + (m_filename_pos - chunk_len), m_filename, chunk_len);
                    m_filename_pos -= chunk_len;
                    m_vfat_seq--;
                } else {
                cancel_vfat:
                    // Cancel any collection.
                    m_vfat_seq = -1;
                }
                
                // Go on reading directory entries.
                return schedule_event(c);
            }
            
            // Forget VFAT state but remember for use in this entry.
            int8_t cur_vfat_seq = m_vfat_seq;
            m_vfat_seq = -1;
            
            // Free marker.
            if (first_byte == 0xE5) {
                return schedule_event(c);
            }
            
            // Ignore: volume label or device.
            if ((attrs & 0x8) || (attrs & 0x40)) {
                return schedule_event(c);
            }
            
            bool is_dir = (attrs & 0x10);
            bool is_dot_entry = (first_byte == (uint8_t)'.');
            uint32_t file_size = ReadBinaryInt<uint32_t, BinaryLittleEndian>(entry_ptr + 0x1C);
            
            ClusterIndexType first_cluster = mask_cluster_entry(
                ReadBinaryInt<uint16_t, BinaryLittleEndian>(entry_ptr + 0x1A) |
                ((uint32_t)ReadBinaryInt<uint16_t, BinaryLittleEndian>(entry_ptr + 0x14) << 16));
            
            if (is_dot_entry && first_cluster == 0) {
                first_cluster = o->root_cluster;
            }
            
            char const *filename;
            if (!is_dot_entry && cur_vfat_seq == 0 && vfat_checksum(entry_ptr) == m_vfat_csum) {
                filename = m_filename + m_filename_pos;
                m_filename[Params::MaxFileNameSize] = 0;
            } else {
                char name_temp[8];
                memcpy(name_temp, entry_ptr + 0, 8);
                if (name_temp[0] == 0x5) {
                    name_temp[0] = 0xE5;
                }
                size_t name_len = fixup_83_name(name_temp, 8, bool(type_byte & 0x8));
                
                char ext_temp[3];
                memcpy(ext_temp, entry_ptr + 8, 3);
                size_t ext_len = fixup_83_name(ext_temp, 3, bool(type_byte & 0x10));
                
                size_t filename_len = 0;
                memcpy(m_filename + filename_len, name_temp, name_len);
                filename_len += name_len;
                if (ext_len > 0) {
                    m_filename[filename_len++] = '.';
                    memcpy(m_filename + filename_len, ext_temp, ext_len);
                    filename_len += ext_len;
                }
                m_filename[filename_len] = '\0';
                filename = m_filename;
            }
            
            FsEntry entry;
            entry.type = is_dir ? EntryType::DIR : EntryType::FILE;
            entry.file_size = file_size;
            entry.cluster_index = first_cluster;
            
            return complete_request(c, false, filename, entry);
        }
        
        void chain_handler (Context c, bool error)
        {
            TheDebugObject::access(c);
            AMBRO_ASSERT(m_state == State::REQUESTING_CLUSTER)
            
            if (error || m_chain.endReached(c)) {
                return complete_request(c, error);
            }
            m_block_in_cluster = 0;
            schedule_event(c);
        }
        
        void dir_block_ref_handler (Context c, bool error)
        {
            TheDebugObject::access(c);
            AMBRO_ASSERT(m_state == State::REQUESTING_BLOCK)
            
            if (error) {
                return complete_request(c, error);
            }
            schedule_event(c);
        }
        
        static uint8_t vfat_checksum (char const *data)
        {
            uint8_t csum = 0;
            for (int i = 0; i < 11; i++) {
                csum = (uint8_t)((uint8_t)((csum & 1) << 7) + (csum >> 1)) + (uint8_t)data[i];
            }
            return csum;
        }
        
        static size_t fixup_83_name (char *data, size_t length, bool lowercase)
        {
            while (length > 0 && data[length - 1] == ' ') {
                length--;
            }
            if (lowercase) {
                for (size_t i = 0; i < length; i++) {
                    if (data[i] >= 'A' && data[i] <= 'Z') {
                        data[i] += 32;
                    }
                }
            }
            return length;
        }
        
        typename Context::EventLoop::QueuedEvent m_event;
        ClusterChain m_chain;
        CacheBlockRef m_dir_block_ref;
        DirectoryIteratorHandler m_handler;
        ClusterBlockIndexType m_block_in_cluster;
        DirEntriesPerBlockType m_block_entry_pos;
        State m_state;
        int8_t m_vfat_seq;
        uint8_t m_vfat_csum;
        FileNameLenType m_filename_pos;
        char m_filename[Params::MaxFileNameSize + 1];
    };
    
    class FsInfo {
        enum class State : uint8_t {INVALID, REQUESTING_BLOCK, READY};
        
        static size_t const Sig1Offset = 0x0;
        static size_t const Sig2Offset = 0x1E4;
        static size_t const FreeClustersOffset = 0x1E8;
        static size_t const AllocatedClustersOffset = 0x1EC;
        static size_t const Sig3Offset = 0x1FC;
        
    public:
        using FsInfoHandler = Callback<void(Context c, bool error)>;
        
        void init (Context c, FsInfoHandler handler)
        {
            m_block_ref.init(c, APRINTER_CB_OBJFUNC_T(&FsInfo::block_ref_handler, this));
            m_handler = handler;
            m_state = State::INVALID;
        }
        
        void deinit (Context c)
        {
            m_block_ref.deinit(c);
        }
        
        void requestAccess (Context c)
        {
            auto *o = Object::self(c);
            AMBRO_ASSERT(m_state == State::INVALID)
            AMBRO_ASSERT(o->fs_info_block != 0)
            
            m_state = State::REQUESTING_BLOCK;
            m_block_ref.requestBlock(c, get_abs_block_index(c, o->fs_info_block), 0, 1, true);
        }
        
        uint32_t getNumFreeClusters (Context c)
        {
            AMBRO_ASSERT(m_state == State::READY)
            
            return ReadBinaryInt<uint32_t, BinaryLittleEndian>(m_block_ref.getData(c) + FreeClustersOffset);
        }
        
        uint32_t getNumAllocatedClusters (Context c)
        {
            AMBRO_ASSERT(m_state == State::READY)
            
            return ReadBinaryInt<uint32_t, BinaryLittleEndian>(m_block_ref.getData(c) + AllocatedClustersOffset);
        }
        
        void setNumFreeClusters (Context c, uint32_t value)
        {
            AMBRO_ASSERT(m_state == State::READY)
            
            WriteBinaryInt<uint32_t, BinaryLittleEndian>(value, m_block_ref.getData(c) + FreeClustersOffset);
            m_block_ref.markDirty(c);
        }
        
        void setNumAllocatedClusters (Context c, uint32_t value)
        {
            AMBRO_ASSERT(m_state == State::READY)
            
            WriteBinaryInt<uint32_t, BinaryLittleEndian>(value, m_block_ref.getData(c) + AllocatedClustersOffset);
            m_block_ref.markDirty(c);
        }
        
    private:
        void complete_request (Context c, bool error)
        {
            if (error) {
                m_state = State::INVALID;
                m_block_ref.reset(c);
            } else {
                m_state = State::READY;
            }
            return m_handler(c, error);
        }
        
        void block_ref_handler (Context c, bool error)
        {
            AMBRO_ASSERT(m_state == State::REQUESTING_BLOCK)
            
            if (error) {
                return complete_request(c, error);
            }
            char *buffer = m_block_ref.getData(c);
            uint32_t sig1 = ReadBinaryInt<uint32_t, BinaryLittleEndian>(buffer + Sig1Offset);
            uint32_t sig2 = ReadBinaryInt<uint32_t, BinaryLittleEndian>(buffer + Sig2Offset);
            uint32_t sig3 = ReadBinaryInt<uint32_t, BinaryLittleEndian>(buffer + Sig3Offset);
            bool signature_error = sig1 != UINT32_C(0x41615252) || sig2 != UINT32_C(0x61417272) || sig3 != UINT32_C(0xAA550000);
            return complete_request(c, signature_error);
        }
        
        CacheBlockRef m_block_ref;
        FsInfoHandler m_handler;
        State m_state;
    };
    
public:
    struct Object : public ObjBase<FatFs, ParentObject, MakeTypeList<
        TheDebugObject,
        TheBlockCache
    >> {
        typename Context::EventLoop::QueuedEvent alloc_event;
        CacheBlockRef first_block_ref;
        CacheBlockRef alloc_block_ref;
        typename TheBlockAccess::BlockRange block_range;
        FsState state;
        uint8_t num_fats;
        ClusterIndexType root_cluster;
        ClusterBlockIndexType blocks_per_cluster;
        ClusterIndexType num_fat_entries;
        BlockIndexType num_reserved_blocks;
        BlockIndexType fat_end_blocks;
        BlockIndexType fs_info_block;
        ClusterIndexType num_valid_clusters;
        DoubleEndedList<ClusterRef, &ClusterRef::m_node> cluster_refs_list;
        AllocationState alloc_state;
        ClusterIndexType alloc_position;
        ClusterIndexType alloc_start;
    };
};

template <
    int TMaxFileNameSize,
    int TNumCacheEntries
>
struct FatFsService {
    static int const MaxFileNameSize = TMaxFileNameSize;
    static int const NumCacheEntries = TNumCacheEntries;
    
    template <typename Context, typename ParentObject, typename TheBlockAccess, typename InitHandler>
    using Fs = FatFs<Context, ParentObject, TheBlockAccess, InitHandler, FatFsService>;
};

#include <aprinter/EndNamespace.h>

#endif
