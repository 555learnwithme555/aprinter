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

#include <aprinter/meta/WrapFunction.h>
#include <aprinter/meta/MinMax.h>
#include <aprinter/meta/ChooseInt.h>
#include <aprinter/base/Object.h>
#include <aprinter/base/DebugObject.h>
#include <aprinter/base/Callback.h>
#include <aprinter/base/Assert.h>
#include <aprinter/base/BinaryTools.h>
#include <aprinter/base/WrapBuffer.h>
#include <aprinter/misc/Utf8Encoder.h>
#include <aprinter/structure/DoubleEndedList.h>

#include <aprinter/BeginNamespace.h>

template <typename Context, typename ParentObject, typename TheBlockAccess, typename InitHandler, typename Params>
class FatFs {
public:
    struct Object;
    
private:
    using TheDebugObject = DebugObject<Context, Object>;
    using BlockAccessUser = typename TheBlockAccess::User;
    using BlockIndexType = typename TheBlockAccess::BlockIndexType;
    static size_t const BlockSize = TheBlockAccess::BlockSize;
    static_assert(BlockSize >= 0x47, "BlockSize not enough for EBPB");
    static_assert(BlockSize % 32 == 0, "BlockSize not a multiple of 32");
    using SectorIndexType = BlockIndexType;
    using ClusterIndexType = uint32_t;
    using ClusterBlockIndexType = uint16_t;
    static size_t const FatEntriesPerBlock = BlockSize / 4;
    static size_t const DirEntriesPerBlock = BlockSize / 32;
    using DirEntriesPerBlockType = ChooseIntForMax<DirEntriesPerBlock, false>;
    static_assert(Params::MaxFileNameSize >= 12, "");
    using FileNameLenType = ChooseIntForMax<Params::MaxFileNameSize, false>;
    static_assert(Params::NumCacheEntries >= 2, "");
    
    enum {FS_STATE_INIT, FS_STATE_READY, FS_STATE_FAILED};
    
    class BaseReader;
    class ClusterChain;
    class CacheRef;
    class CacheEntry;
    
public:
    enum EntryType {ENTRYTYPE_DIR, ENTRYTYPE_FILE};
    
    class FsEntry {
    public:
        inline EntryType getType () const { return (EntryType)type; }
        inline uint32_t getFileSize () const { return file_size; }
        
    private:
        friend FatFs;
        
        uint8_t type;
        uint32_t file_size;
        ClusterIndexType cluster_index;
    };
    
    struct SharedBuffer {
        char buffer[BlockSize];
    };
    
    static bool isPartitionTypeSupported (uint8_t type)
    {
        return (type == 0xB || type == 0xC);
    }
    
    static void init (Context c, SharedBuffer *init_buffer, typename TheBlockAccess::BlockRange block_range)
    {
        auto *o = Object::self(c);
        
        o->block_range = block_range;
        
        o->state = FS_STATE_INIT;
        o->u.init.block_user.init(c, APRINTER_CB_STATFUNC_T(&FatFs::init_block_read_handler));
        o->u.init.block_user.startRead(c, get_abs_block_index(c, 0), WrapBuffer::Make(init_buffer->buffer));
        
        TheDebugObject::init(c);
    }
    
    static void deinit (Context c)
    {
        auto *o = Object::self(c);
        TheDebugObject::deinit(c);
        
        if (o->state == FS_STATE_INIT) {
            o->u.init.block_user.deinit(c);
        } else if (o->state == FS_STATE_READY) {
            for (int i = 0; i < Params::NumCacheEntries; i++) {
                o->u.fs.cache_entries[i].deinit(c);
            }
        }
    }
    
    static FsEntry getRootEntry (Context c)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        AMBRO_ASSERT(o->state == FS_STATE_READY)
        
        FsEntry entry;
        entry.type = ENTRYTYPE_DIR;
        entry.file_size = 0;
        entry.cluster_index = o->u.fs.root_cluster;
        return entry;
    }
    
    class DirLister {
    private:
        enum {DIRLISTER_STATE_WAITREQ, DIRLISTER_STATE_READING, DIRLISTER_STATE_EVENT};
        
    public:
        using DirListerHandler = Callback<void(Context c, bool is_error, char const *name, FsEntry entry)>;
        
        void init (Context c, FsEntry dir_entry, SharedBuffer *buffer, DirListerHandler handler)
        {
            TheDebugObject::access(c);
            AMBRO_ASSERT(dir_entry.type == ENTRYTYPE_DIR)
            
            m_buffer = buffer;
            m_handler = handler;
            m_event.init(c, APRINTER_CB_OBJFUNC_T(&DirLister::event_handler, this));
            m_reader.init(c, dir_entry.cluster_index, APRINTER_CB_OBJFUNC_T(&DirLister::reader_handler, this));
            m_state = DIRLISTER_STATE_WAITREQ;
            m_block_entry_pos = DirEntriesPerBlock;
            m_vfat_seq = -1;
        }
        
        // WARNING: Only allowed together with deiniting the whole FatFs and underlying storage!
        void deinit (Context c)
        {
            TheDebugObject::access(c);
            
            m_reader.deinit(c);
            m_event.deinit(c);
        }
        
        void requestEntry (Context c)
        {
            TheDebugObject::access(c);
            AMBRO_ASSERT(m_state == DIRLISTER_STATE_WAITREQ)
            
            next_entry(c);
        }
        
    private:
        void event_handler (Context c)
        {
            auto *o = Object::self(c);
            TheDebugObject::access(c);
            AMBRO_ASSERT(m_state == DIRLISTER_STATE_EVENT)
            AMBRO_ASSERT(m_block_entry_pos < DirEntriesPerBlock)
            
            char const *entry_ptr = m_buffer->buffer + ((size_t)m_block_entry_pos * 32);
            uint8_t first_byte =    ReadBinaryInt<uint8_t, BinaryLittleEndian>(entry_ptr + 0x0);
            uint8_t attrs =         ReadBinaryInt<uint8_t, BinaryLittleEndian>(entry_ptr + 0xB);
            uint8_t type_byte =     ReadBinaryInt<uint8_t, BinaryLittleEndian>(entry_ptr + 0xC);
            uint8_t checksum_byte = ReadBinaryInt<uint8_t, BinaryLittleEndian>(entry_ptr + 0xD);
            
            if (first_byte == 0) {
                m_state = DIRLISTER_STATE_WAITREQ;
                return m_handler(c, false, nullptr, FsEntry());
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
                return next_entry(c);
            }
            
            // Forget VFAT state but remember for use in this entry.
            int8_t cur_vfat_seq = m_vfat_seq;
            m_vfat_seq = -1;
            
            // Free marker.
            if (first_byte == 0xE5) {
                return next_entry(c);
            }
            
            // Ignore: volume label or device.
            if ((attrs & 0x8) || (attrs & 0x40)) {
                return next_entry(c);
            }
            
            bool is_dir = (attrs & 0x10);
            bool is_dot_entry = (first_byte == (uint8_t)'.');
            uint32_t file_size = ReadBinaryInt<uint32_t, BinaryLittleEndian>(entry_ptr + 0x1C);
            
            ClusterIndexType first_cluster = mask_cluster_entry(
                ReadBinaryInt<uint16_t, BinaryLittleEndian>(entry_ptr + 0x1A) |
                ((uint32_t)ReadBinaryInt<uint16_t, BinaryLittleEndian>(entry_ptr + 0x14) << 16));
            
            if (is_dot_entry && first_cluster == 0) {
                first_cluster = o->u.fs.root_cluster;
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
            entry.type = is_dir ? ENTRYTYPE_DIR : ENTRYTYPE_FILE;
            entry.file_size = file_size;
            entry.cluster_index = first_cluster;
            
            m_state = DIRLISTER_STATE_WAITREQ;
            return m_handler(c, false, filename, entry);
        }
        
        void reader_handler (Context c, uint8_t status)
        {
            TheDebugObject::access(c);
            AMBRO_ASSERT(m_state == DIRLISTER_STATE_READING)
            
            if (status != BaseReader::BASEREAD_STATUS_OK) {
                bool is_error = (status != BaseReader::BASEREAD_STATUS_EOF);
                m_state = DIRLISTER_STATE_WAITREQ;
                return m_handler(c, is_error, nullptr, FsEntry());
            }
            
            m_block_entry_pos = 0;
            m_event.appendNowNotAlready(c);
            m_state = DIRLISTER_STATE_EVENT;
        }
        
        void next_entry (Context c)
        {
            if (m_block_entry_pos == DirEntriesPerBlock) {
                m_reader.requestBlock(c, WrapBuffer::Make(m_buffer->buffer));
                m_state = DIRLISTER_STATE_READING;
            } else {
                m_event.appendNowNotAlready(c);
                m_state = DIRLISTER_STATE_EVENT;
            }
        }
        
        SharedBuffer *m_buffer;
        DirListerHandler m_handler;
        typename Context::EventLoop::QueuedEvent m_event;
        BaseReader m_reader;
        uint8_t m_state;
        DirEntriesPerBlockType m_block_entry_pos;
        int8_t m_vfat_seq;
        uint8_t m_vfat_csum;
        FileNameLenType m_filename_pos;
        char m_filename[Params::MaxFileNameSize + 1];
    };
    
    class FileReader {
    public:
        using FileReaderHandler = Callback<void(Context c, bool is_error, size_t length)>;
        
        void init (Context c, FsEntry file_entry, FileReaderHandler handler)
        {
            TheDebugObject::access(c);
            AMBRO_ASSERT(file_entry.type == ENTRYTYPE_FILE)
            
            m_handler = handler;
            m_first_cluster = file_entry.cluster_index;
            m_file_size = file_entry.file_size;
            init_reader(c);
        }
        
        // WARNING: Only allowed together with deiniting the whole FatFs and underlying storage!
        void deinit (Context c)
        {
            TheDebugObject::access(c);
            
            m_reader.deinit(c);
        }
        
        void rewind (Context c)
        {
            TheDebugObject::access(c);
            AMBRO_ASSERT(m_reader.isIdle(c))
            
            m_reader.deinit(c);
            init_reader(c);
        }
        
        void requestBlock (Context c, WrapBuffer buf)
        {
            TheDebugObject::access(c);
            
            m_reader.requestBlock(c, buf);
        }
        
    private:
        void init_reader (Context c)
        {
            m_reader.init(c, m_first_cluster, APRINTER_CB_OBJFUNC_T(&FileReader::reader_handler, this));
            m_rem_file_size = m_file_size;
        }
        
        void reader_handler (Context c, uint8_t status)
        {
            TheDebugObject::access(c);
            
            bool is_error = true;
            size_t read_length = 0;
            
            do {
                if (status != BaseReader::BASEREAD_STATUS_OK) {
                    is_error = (status != BaseReader::BASEREAD_STATUS_EOF || m_rem_file_size > 0);
                    break;
                }
                
                is_error = false;
                read_length = MinValue((uint32_t)BlockSize, m_rem_file_size);
                m_rem_file_size -= read_length;
            } while (0);
            
            return m_handler(c, is_error, read_length);
        }
        
        FileReaderHandler m_handler;
        ClusterIndexType m_first_cluster;
        uint32_t m_file_size;
        BaseReader m_reader;
        uint32_t m_rem_file_size;
    };
    
private:
    static void init_block_read_handler (Context c, bool read_error)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        AMBRO_ASSERT(o->state == FS_STATE_INIT)
        
        char *buffer = o->u.init.block_user.getBuffer(c).ptr1;
        o->u.init.block_user.deinit(c);
        
        uint8_t error_code = 99;
        do {
            if (read_error) {
                error_code = 20;
                goto error;
            }
            
            uint16_t sector_size =          ReadBinaryInt<uint16_t, BinaryLittleEndian>(buffer + 0xB);
            uint8_t sectors_per_cluster =   ReadBinaryInt<uint8_t,  BinaryLittleEndian>(buffer + 0xD);
            uint16_t num_reserved_sectors = ReadBinaryInt<uint16_t, BinaryLittleEndian>(buffer + 0xE);
            o->u.fs.num_fats =              ReadBinaryInt<uint8_t,  BinaryLittleEndian>(buffer + 0x10);
            uint16_t max_root =             ReadBinaryInt<uint16_t, BinaryLittleEndian>(buffer + 0x11);
            uint32_t sectors_per_fat =      ReadBinaryInt<uint32_t, BinaryLittleEndian>(buffer + 0x24);
            uint32_t root_cluster =         ReadBinaryInt<uint32_t, BinaryLittleEndian>(buffer + 0x2C);
            uint8_t sig =                   ReadBinaryInt<uint8_t,  BinaryLittleEndian>(buffer + 0x42);
            
            if (sector_size == 0 || sector_size % BlockSize != 0) {
                error_code = 22;
                goto error;
            }
            uint16_t blocks_per_sector = sector_size / BlockSize;
            
            if (sectors_per_cluster > UINT16_MAX / blocks_per_sector) {
                error_code = 23;
                goto error;
            }
            o->u.fs.blocks_per_cluster = blocks_per_sector * sectors_per_cluster;
            
            if ((uint32_t)num_reserved_sectors * sector_size < 0x47) {
                error_code = 24;
                goto error;
            }
            
            if (o->u.fs.num_fats != 1 && o->u.fs.num_fats != 2) {
                error_code = 25;
                goto error;
            }
            
            if (sig != 0x28 && sig != 0x29) {
                error_code = 26;
                goto error;
            }
            
            if (max_root != 0) {
                error_code = 27;
                goto error;
            }
            
            o->u.fs.root_cluster = mask_cluster_entry(root_cluster);
            if (o->u.fs.root_cluster < 2) {
                error_code = 28;
                goto error;
            }
            
            uint16_t entries_per_sector = sector_size / 4;
            if (sectors_per_fat == 0 || sectors_per_fat > UINT32_MAX / entries_per_sector) {
                error_code = 29;
                goto error;
            }
            o->u.fs.num_fat_entries = (ClusterIndexType)sectors_per_fat * entries_per_sector;
            
            uint64_t fat_end_sectors_calc = (uint64_t)num_reserved_sectors + (uint64_t)o->u.fs.num_fats * sectors_per_fat;
            if (fat_end_sectors_calc > o->block_range.getLength() / blocks_per_sector) {
                error_code = 29;
                goto error;
            }
            o->u.fs.num_reserved_blocks = (BlockIndexType)num_reserved_sectors * blocks_per_sector;
            o->u.fs.fat_end_blocks = fat_end_sectors_calc * blocks_per_sector;
            
            for (int i = 0; i < Params::NumCacheEntries; i++) {
                o->u.fs.cache_entries[i].init(c);
            }
            
            error_code = 0;
        } while (0);
        
    error:
        o->state = error_code ? FS_STATE_FAILED : FS_STATE_READY;
        return InitHandler::call(c, error_code);
    }
    
    static ClusterIndexType mask_cluster_entry (uint32_t entry_value)
    {
        return (entry_value & UINT32_C(0x0FFFFFFF));
    }
    
    static bool is_cluster_idx_valid (ClusterIndexType cluster_idx)
    {
        return (cluster_idx >= 2 && cluster_idx < UINT32_C(0xFFFFFF8));
    }
    
    static BlockIndexType get_abs_block_index (Context c, BlockIndexType rel_block)
    {
        auto *o = Object::self(c);
        return o->block_range.start_block + rel_block;
    }
    
    static BlockIndexType num_blocks_per_fat (Context c)
    {
        auto *o = Object::self(c);
        return o->u.fs.num_fat_entries / FatEntriesPerBlock;
    }
    
    static bool get_cluster_block_idx (Context c, ClusterIndexType cluster_idx, ClusterBlockIndexType cluster_block_idx, BlockIndexType *out_block_idx)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(is_cluster_idx_valid(cluster_idx))
        AMBRO_ASSERT(cluster_block_idx < o->u.fs.blocks_per_cluster)
        
        uint64_t blocks_after_fat_end = (uint64_t)(cluster_idx - 2) * o->u.fs.blocks_per_cluster + cluster_block_idx;
        if (blocks_after_fat_end >= o->block_range.getLength() - o->u.fs.fat_end_blocks) {
            return false;
        }
        *out_block_idx = o->u.fs.fat_end_blocks + blocks_after_fat_end;
        return true;
    }
    
    static bool get_fat_entry_block_idx (Context c, ClusterIndexType cluster_idx, BlockIndexType *out_block_idx, size_t *out_block_offset)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(is_cluster_idx_valid(cluster_idx))
        
        if (cluster_idx >= o->u.fs.num_fat_entries) {
            return false;
        }
        *out_block_idx = o->u.fs.num_reserved_blocks + (cluster_idx / FatEntriesPerBlock);
        *out_block_offset = (size_t)4 * (cluster_idx % FatEntriesPerBlock);
        return true;
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
    
    static CacheEntry * get_cache_entry (Context c, BlockIndexType block)
    {
        auto *o = Object::self(c);
        
        CacheEntry *invalid_entry = nullptr;
        CacheEntry *unused_entry = nullptr;
        
        for (int i = 0; i < Params::NumCacheEntries; i++) {
            CacheEntry *ce = &o->u.fs.cache_entries[i];
            auto ce_state = ce->getState(c);
            
            if (ce_state != CacheEntry::State::INVALID && ce->getBlock(c) == block) {
                return ce;
            }
            
            if (ce_state == CacheEntry::State::INVALID) {
                invalid_entry = ce;
            } else if (ce_state == CacheEntry::State::IDLE && ce->isUnused(c)) {
                unused_entry = ce;
            }
        }
        
        CacheEntry *entry = invalid_entry ? invalid_entry : unused_entry;
        if (!entry) {
            return nullptr;
        }
        
        entry->assignBlockAndStartReading(c, block);
        
        return entry;
    }
    
    class BaseReader {
    public:
        enum {BASEREAD_STATUS_ERR, BASEREAD_STATUS_EOF, BASEREAD_STATUS_OK};
        
        using BaseReadHandler = Callback<void(Context c, uint8_t status)>;
        
        void init (Context c, ClusterIndexType first_cluster, BaseReadHandler handler)
        {
            auto *o = Object::self(c);
            TheDebugObject::access(c);
            AMBRO_ASSERT(o->state == FS_STATE_READY)
            
            m_handler = handler;
            m_state = State::IDLE;
            m_event.init(c, APRINTER_CB_OBJFUNC_T(&BaseReader::event_handler, this));
            m_chain.init(c, first_cluster, APRINTER_CB_OBJFUNC_T(&BaseReader::chain_handler, this));
            m_block_user.init(c, APRINTER_CB_OBJFUNC_T(&BaseReader::read_handler, this));
            m_block_in_cluster = o->u.fs.blocks_per_cluster;
        }
        
        // WARNING: Only allowed together with deiniting the whole FatFs and underlying storage or when not reading!
        void deinit (Context c)
        {
            m_block_user.deinit(c);
            m_chain.deinit(c);
            m_event.deinit(c);
        }
        
        void requestBlock (Context c, WrapBuffer buf)
        {
            TheDebugObject::access(c);
            AMBRO_ASSERT(m_state == State::IDLE)
            
            m_state = State::CHECK_EVENT;
            m_req_buf = buf;
            m_event.appendNowNotAlready(c);
        }
        
        bool isIdle (Context c)
        {
            return (m_state == State::IDLE);
        }
        
    private:
        enum class State : uint8_t {IDLE, CHECK_EVENT, NEXT_CLUSTER, READING_DATA};
        
        void event_handler (Context c)
        {
            auto *o = Object::self(c);
            TheDebugObject::access(c);
            AMBRO_ASSERT(m_state == State::CHECK_EVENT)
            
            if (m_block_in_cluster == o->u.fs.blocks_per_cluster) {
                m_chain.requestNext(c);
                m_state = State::NEXT_CLUSTER;
                return;
            }
            
            BlockIndexType block_idx;
            if (!get_cluster_block_idx(c, m_chain.getCurrentCluster(c), m_block_in_cluster, &block_idx)) {
                return complete_request(c, BASEREAD_STATUS_ERR);
            }
            
            m_block_user.startRead(c, get_abs_block_index(c, block_idx), m_req_buf);
            m_state = State::READING_DATA;
        }
        
        void chain_handler (Context c, bool error)
        {
            auto *o = Object::self(c);
            TheDebugObject::access(c);
            AMBRO_ASSERT(m_state == State::NEXT_CLUSTER)
            AMBRO_ASSERT(m_block_in_cluster == o->u.fs.blocks_per_cluster)
             
            if (error || m_chain.endReached(c)) {
                return complete_request(c, error ? BASEREAD_STATUS_ERR : BASEREAD_STATUS_EOF);
            }
            
            m_block_in_cluster = 0;
            m_state = State::CHECK_EVENT;
            m_event.appendNowNotAlready(c);
        }
        
        void read_handler (Context c, bool is_read_error)
        {
            auto *o = Object::self(c);
            TheDebugObject::access(c);
            AMBRO_ASSERT(m_state == State::READING_DATA)
            AMBRO_ASSERT(m_block_in_cluster < o->u.fs.blocks_per_cluster)
            
            if (is_read_error) {
                return complete_request(c, BASEREAD_STATUS_ERR);
            }
            
            m_block_in_cluster++;
            
            return complete_request(c, BASEREAD_STATUS_OK);
        }
        
        void complete_request (Context c, uint8_t status)
        {
            m_state = State::IDLE;
            
            return m_handler(c, status);
        }
        
        BaseReadHandler m_handler;
        State m_state;
        typename Context::EventLoop::QueuedEvent m_event;
        ClusterChain m_chain;
        BlockAccessUser m_block_user;
        ClusterBlockIndexType m_block_in_cluster;
        WrapBuffer m_req_buf;
    };
    
    class ClusterChain {
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
        
        ClusterIndexType getFirstCluster (Context c)
        {
            return m_first_cluster;
        }
        
        void rewind (Context c)
        {
            AMBRO_ASSERT(m_state == State::IDLE)
            
            rewind_internal(c);
        }
        
        void requestNext (Context c)
        {
            AMBRO_ASSERT(m_state == State::IDLE)
            AMBRO_ASSERT(m_iter_state != IterState::END)
            
            m_state = State::REQUEST_NEXT_CHECK;
            m_event.prependNowNotAlready(c);
        }
        
        bool endReached (Context c)
        {
            AMBRO_ASSERT(m_state == State::IDLE)
            
            return (m_iter_state == IterState::END);
        }
        
        ClusterIndexType getCurrentCluster (Context c)
        {
            AMBRO_ASSERT(m_state == State::IDLE)
            AMBRO_ASSERT(m_iter_state == IterState::CLUSTER)
            
            return m_current_cluster;
        }
        
    private:
        enum class State : uint8_t {IDLE, REQUEST_NEXT_CHECK, READING_FAT_FOR_NEXT};
        
        enum class IterState : uint8_t {START, CLUSTER, END};
        
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
                    AMBRO_ASSERT(m_iter_state != IterState::END)
                    
                    if (m_iter_state != IterState::START) {
                        BlockIndexType fat_block_idx;
                        size_t fat_block_offset;
                        if (!get_fat_entry_block_idx(c, m_current_cluster, &fat_block_idx, &fat_block_offset)) {
                            return complete_request(c, true);
                        }
                        
                        if (!m_fat_cache_ref.isBlockSelected(c) || m_fat_cache_ref.getBlock(c) != fat_block_idx) {
                            m_state = State::READING_FAT_FOR_NEXT;
                            m_fat_cache_ref.requestBlock(c, fat_block_idx);
                            return;
                        }
                        
                        m_prev_cluster = m_current_cluster;
                        m_current_cluster = mask_cluster_entry(ReadBinaryInt<uint32_t, BinaryLittleEndian>(m_fat_cache_ref.getData(c) + fat_block_offset));
                    }
                    
                    if (is_cluster_idx_valid(m_current_cluster)) {
                        m_iter_state = IterState::CLUSTER;
                    } else {
                        m_iter_state = IterState::END;
                        m_fat_cache_ref.reset(c);
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
        CacheRef m_fat_cache_ref;
        ClusterChainHandler m_handler;
        State m_state;
        IterState m_iter_state;
        ClusterIndexType m_first_cluster;
        ClusterIndexType m_current_cluster;
        ClusterIndexType m_prev_cluster;
    };
    
    class CacheRef {
        friend class CacheEntry;
        
    public:
        using CacheHandler = Callback<void(Context c, bool error)>;
        
        void init (Context c, CacheHandler handler)
        {
            m_handler = handler;
            m_timer.init(c, APRINTER_CB_OBJFUNC_T(&CacheRef::timer_handler, this));
            m_state = State::INVALID;
            m_entry = nullptr;
        }
        
        void deinit (Context c)
        {
            if (m_entry) {
                m_entry->detachUser(c, this);
            }
            m_timer.deinit(c);
        }
        
        void reset (Context c)
        {
            if (m_entry) {
                m_entry->detachUser(c, this);
                m_entry = nullptr;
            }
            m_timer.unset(c);
            m_state = State::INVALID;
        }
        
        bool isBlockSelected (Context c)
        {
            return m_state != State::INVALID;
        }
        
        BlockIndexType getBlock (Context c)
        {
            AMBRO_ASSERT(m_state != State::INVALID)
            
            return m_block;
        }
        
        bool isAvailable (Context c)
        {
            return m_state == State::AVAILABLE;
        }
        
        char * getData (Context c)
        {
            AMBRO_ASSERT(m_state == State::AVAILABLE)
            
            return m_entry->getData(c);
        }
        
        void requestBlock (Context c, BlockIndexType block)
        {
            if (m_entry) {
                m_entry->detachUser(c, this);
                m_entry = nullptr;
            }
            
            m_timer.unset(c);
            m_timer.prependNowNotAlready(c);
            m_state = State::REQUEST_EVENT;
            m_dirt_state = DirtState::CLEAN;
            m_flush_state = FlushState::IDLE;
            m_block = block;
        }
        
        void markDirty (Context c)
        {
            AMBRO_ASSERT(m_state == State::AVAILABLE)
            
            m_dirt_state = DirtState::DIRTY;
        }
        
        void requestFlush (Context c)
        {
            AMBRO_ASSERT(m_state == State::AVAILABLE)
            AMBRO_ASSERT(m_flush_state == FlushState::IDLE)
            
            m_flush_state = FlushState::CHECK_EVENT;
            m_flush_error = false;
            m_timer.prependNowNotAlready(c);
        }
        
    private:
        enum class State : uint8_t {INVALID, REQUEST_EVENT, WAITING_READ, READ_COMPL_EVENT, AVAILABLE};
        enum class DirtState : uint8_t {CLEAN, DIRTY, WRITING};
        enum class FlushState : uint8_t {IDLE, CHECK_EVENT, WRITING};
        
        void timer_handler (Context c)
        {
            switch (m_state) {
                case State::REQUEST_EVENT: {
                    AMBRO_ASSERT(!m_entry)
                    
                    m_entry = get_cache_entry(c, m_block);
                    if (!m_entry) {
                        return complete_init(c, true);
                    }
                    
                    m_entry->attachUser(c, this);
                    
                    if (m_entry->getState(c) != CacheEntry::State::READING) {
                        return complete_init(c, false);
                    }
                    
                    m_state = State::WAITING_READ;
                } break;
                
                case State::READ_COMPL_EVENT: {
                    bool error = !m_entry;
                    return complete_init(c, error);
                } break;
                
                case State::AVAILABLE: {
                    AMBRO_ASSERT(m_flush_state == FlushState::CHECK_EVENT)
                    
                    if (m_dirt_state == DirtState::CLEAN || m_flush_error) {
                        m_flush_state = FlushState::IDLE;
                        return m_handler(c, m_flush_error);
                    }
                    
                    if (m_entry->getState(c) != CacheEntry::State::WRITING) {
                        m_entry->startWriting(c);
                    }
                    
                    m_flush_state = FlushState::WRITING;
                } break;
                
                default:
                    AMBRO_ASSERT(false);
            }
        }
        
        void cache_event (Context c, typename CacheEntry::Event event, bool error)
        {
            AMBRO_ASSERT(m_entry)
            
            switch (event) {
                case CacheEntry::Event::READ_COMPLETED: {
                    AMBRO_ASSERT(m_state == State::WAITING_READ)
                    
                    if (error) {
                        m_entry->detachUser(c, this);
                        m_entry = nullptr;
                    }
                    
                    m_state = State::READ_COMPL_EVENT;
                    m_timer.prependNowNotAlready(c);
                } break;
                
                case CacheEntry::Event::WRITE_STARTED: {
                    AMBRO_ASSERT(m_state == State::READ_COMPL_EVENT || m_state == State::AVAILABLE)
                    
                    if (m_dirt_state == DirtState::DIRTY) {
                        m_dirt_state = DirtState::WRITING;
                    }
                } break;
                
                case CacheEntry::Event::WRITE_COMPLETED: {
                    AMBRO_ASSERT(m_state == State::READ_COMPL_EVENT || m_state == State::AVAILABLE)
                    
                    if (m_dirt_state == DirtState::WRITING && !error) {
                        m_dirt_state = DirtState::CLEAN;
                    }
                    
                    if (m_flush_state == FlushState::WRITING) {
                        m_flush_state = FlushState::CHECK_EVENT;
                        m_timer.prependNowNotAlready(c);
                    }
                } break;
            }
        }
        
        void complete_init (Context c, bool error)
        {
            AMBRO_ASSERT(m_dirt_state == DirtState::CLEAN)
            AMBRO_ASSERT(m_flush_state == FlushState::IDLE)
            
            if (error) {
                m_state = State::INVALID;
            } else {
                m_state = State::AVAILABLE;
            }
            return m_handler(c, error);
        }
        
        CacheHandler m_handler;
        typename Context::EventLoop::QueuedEvent m_timer;
        State m_state;
        DirtState m_dirt_state;
        FlushState m_flush_state;
        bool m_flush_error;
        CacheEntry *m_entry;
        BlockIndexType m_block;
        DoubleEndedListNode<CacheRef> m_cache_users_list_node;
    };
    
    class CacheEntry {
    public:
        enum class State : uint8_t {INVALID, READING, IDLE, WRITING};
        
        enum class Event : uint8_t {READ_COMPLETED, WRITE_STARTED, WRITE_COMPLETED};
        
        void init (Context c)
        {
            m_block_user.init(c, APRINTER_CB_OBJFUNC_T(&CacheEntry::block_user_handler, this));
            m_cache_users_list.init();
            m_state = State::INVALID;
        }
        
        void deinit (Context c)
        {
            m_block_user.deinit(c);
        }
        
        State getState (Context c)
        {
            return m_state;
        }
        
        BlockIndexType getBlock (Context c)
        {
            AMBRO_ASSERT(m_state != State::INVALID)
            
            return m_block;
        }
        
        char * getData (Context c)
        {
            AMBRO_ASSERT(m_state != State::INVALID)
            AMBRO_ASSERT(m_state != State::READING)
            
            return m_buffer;
        }
        
        bool isUnused (Context c)
        {
            return m_cache_users_list.isEmpty();
        }
        
        void attachUser (Context c, CacheRef *user)
        {
            AMBRO_ASSERT(m_state != State::INVALID)
            
            m_cache_users_list.append(user);
        }
        
        void detachUser (Context c, CacheRef *user)
        {
            m_cache_users_list.remove(user);
        }
        
        void assignBlockAndStartReading (Context c, BlockIndexType block)
        {
            auto *o = Object::self(c);
            AMBRO_ASSERT(m_state == State::INVALID || m_state == State::IDLE)
            AMBRO_ASSERT(m_cache_users_list.isEmpty())
            
            m_state = State::READING;
            m_block = block;
            m_block_user.startRead(c, get_abs_block_index(c, m_block), WrapBuffer::Make(m_buffer));
        }
        
        void startWriting (Context c)
        {
            auto *o = Object::self(c);
            AMBRO_ASSERT(m_state == State::IDLE)
            
            m_state = State::WRITING;
            m_next_fat_index = 1;
            m_block_user.startWrite(c, get_abs_block_index(c, m_block), WrapBuffer::Make(m_buffer));
            
            raise_cache_event(c, Event::WRITE_STARTED, false);
        }
        
    private:
        bool is_fat_block (Context c)
        {
            auto *o = Object::self(c);
            return m_block >= o->u.fs.num_reserved_blocks && (m_block - o->u.fs.num_reserved_blocks) < num_blocks_per_fat(c);
        }
        
        void raise_cache_event (Context c, Event event, bool error)
        {
            CacheRef *ref = m_cache_users_list.first();
            while (ref) {
                CacheRef *next = m_cache_users_list.next(ref);
                ref->cache_event(c, event, error);
                ref = next;
            }
        }
        
        void block_user_handler (Context c, bool error)
        {
            auto *o = Object::self(c);
            AMBRO_ASSERT(m_state == State::READING || m_state == State::WRITING)
            
            if (!error && m_state == State::WRITING && is_fat_block(c) && m_next_fat_index < o->u.fs.num_fats) {
                BlockIndexType write_block = m_block + m_next_fat_index * num_blocks_per_fat(c);
                m_next_fat_index++;
                m_block_user.startWrite(c, get_abs_block_index(c, write_block), WrapBuffer::Make(m_buffer));
                return;
            }
            
            Event event = (m_state == State::READING) ? Event::READ_COMPLETED : Event::WRITE_COMPLETED;
            
            m_state = (event == Event::READ_COMPLETED && error) ? State::INVALID : State::IDLE;
            
            raise_cache_event(c, event, error);
            
            AMBRO_ASSERT(m_state != State::IDLE || m_cache_users_list.isEmpty())
        }
        
        using CacheRefsList = DoubleEndedList<CacheRef, &CacheRef::m_cache_users_list_node>;
        
        BlockAccessUser m_block_user;
        CacheRefsList m_cache_users_list;
        State m_state;
        BlockIndexType m_block;
        uint8_t m_next_fat_index;
        char m_buffer[BlockSize];
    };
    
public:
    struct Object : public ObjBase<FatFs, ParentObject, MakeTypeList<
        TheDebugObject
    >> {
        typename TheBlockAccess::BlockRange block_range;
        uint8_t state;
        union {
            struct {
                BlockAccessUser block_user;
            } init;
            struct {
                uint8_t num_fats;
                ClusterIndexType root_cluster;
                ClusterBlockIndexType blocks_per_cluster;
                ClusterIndexType num_fat_entries;
                BlockIndexType num_reserved_blocks;
                BlockIndexType fat_end_blocks;
                CacheEntry cache_entries[Params::NumCacheEntries];
            } fs;
        } u;
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
