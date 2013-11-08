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

#ifndef AMBROLIB_SPI_SDCARD_H
#define AMBROLIB_SPI_SDCARD_H

#include <stdint.h>

#include <aprinter/meta/Position.h>
#include <aprinter/meta/WrapFunction.h>
#include <aprinter/meta/MinMax.h>
#include <aprinter/meta/BitsInInt.h>
#include <aprinter/base/DebugObject.h>
#include <aprinter/base/Assert.h>

#include <aprinter/BeginNamespace.h>

template <
    typename TSsPin,
    template <typename, typename, typename, int> class TSpi
>
struct SpiSdCardParams {
    using SsPin = TSsPin;
    template <typename X, typename Y, typename Z, int W> using Spi = TSpi<X, Y, Z, W>;
};

template <typename Position, typename Context, typename Params, int MaxCommands, typename InitHandler, typename CommandHandler>
class SpiSdCard : public DebugObject<Context, void> {
    struct SpiHandler;
    struct SpiPosition;
    
    static const int SpiMaxCommands = max(6, 5 * MaxCommands);
    static const int SpiCommandBits = BitsInInt<SpiMaxCommands>::value;
    using TheSpi = typename Params::template Spi<SpiPosition, Context, SpiHandler, SpiCommandBits>;
    using SpiCommandSizeType = typename TheSpi::CommandSizeType;
    
    static SpiSdCard * self (Context c)
    {
        return PositionTraverse<typename Context::TheRootPosition, Position>(c.root());
    }
    
public:
    struct ReadState {
        uint8_t buf[7];
        SpiCommandSizeType spi_end_index;
    };
    
    static void init (Context c)
    {
        SpiSdCard *o = self(c);
        
        o->m_state = STATE_INACTIVE;
        
        c.pins()->template set<SsPin>(c, true);
        c.pins()->template setOutput<SsPin>(c);
        
        o->debugInit(c);
    }
    
    static void deinit (Context c)
    {
        SpiSdCard *o = self(c);
        o->debugDeinit(c);
        
        if (o->m_state != STATE_INACTIVE) {
            c.pins()->template set<SsPin>(c, true);
            o->m_spi.deinit(c);
        }
    }
    
    static bool isActive (Context c)
    {
        SpiSdCard *o = self(c);
        o->debugAccess(c);
        
        return (o->m_state != STATE_INACTIVE);
    }
    
    static void activate (Context c)
    {
        SpiSdCard *o = self(c);
        o->debugAccess(c);
        AMBRO_ASSERT(o->m_state == STATE_INACTIVE)
        
        o->m_spi.init(c);
        o->m_spi.cmdWriteByte(c, 0xff, 128);
        o->m_state = STATE_INIT1;
    }
    
    static void deactivate (Context c)
    {
        SpiSdCard *o = self(c);
        o->debugAccess(c);
        AMBRO_ASSERT(o->m_state != STATE_INACTIVE)
        
        deactivate_common(c);
    }
    
    static void isInited (Context c)
    {
        SpiSdCard *o = self(c);
        o->debugAccess(c);
        
        return (o->m_state == STATE_RUNNING);
    }
    
    static uint32_t getCapacityBlocks (Context c)
    {
        SpiSdCard *o = self(c);
        o->debugAccess(c);
        AMBRO_ASSERT(o->m_state == STATE_RUNNING)
        
        return o->m_capacity_blocks;
    }
    
    static void queueReadBlock (Context c, uint32_t block, uint8_t *data, ReadState *state)
    {
        SpiSdCard *o = self(c);
        o->debugAccess(c);
        AMBRO_ASSERT(o->m_state == STATE_RUNNING)
        AMBRO_ASSERT(block < o->m_capacity_blocks)
        
        uint32_t addr = o->m_sdhc ? block : (block * 512);
        sd_command(c, CMD_READ_SINGLE_BLOCK, addr, true, state->buf, state->buf);
        o->m_spi.cmdReadUntilDifferent(c, 0xff, 255, 0xff, state->buf + 1);
        o->m_spi.cmdReadBuffer(c, data, 512, 0xff);
        o->m_spi.cmdWriteByte(c, 0xff, 2);
        state->spi_end_index = o->m_spi.getEndIndex(c);
    }
    
    static bool checkReadBlock (Context c, ReadState *state, bool *out_error)
    {
        SpiSdCard *o = self(c);
        o->debugAccess(c);
        AMBRO_ASSERT(o->m_state == STATE_RUNNING)
        
        if (!o->m_spi.indexReached(c, state->spi_end_index)) {
            return false;
        }
        *out_error = (state->buf[0] != 0 || state->buf[1] != 0xfe);
        return true;
    }
    
    static void unsetEvent (Context c)
    {
        SpiSdCard *o = self(c);
        o->debugAccess(c);
        AMBRO_ASSERT(o->m_state == STATE_RUNNING)
        
        o->m_spi.unsetEvent(c);
    }
    
    TheSpi * getSpi ()
    {
        return &m_spi;
    }
    
    using EventLoopFastEvents = typename TheSpi::EventLoopFastEvents;
    
private:
    using SsPin = typename Params::SsPin;
    
    enum {
        STATE_RUNNING,
        STATE_INACTIVE,
        STATE_INIT1,
        STATE_INIT2,
        STATE_INIT3,
        STATE_INIT4,
        STATE_INIT5,
        STATE_INIT6,
        STATE_INIT7,
        STATE_INIT8
    };
    
    static const uint8_t CMD_GO_IDLE_STATE = 0;
    static const uint8_t CMD_SEND_IF_COND = 8;
    static const uint8_t CMD_SEND_CSD = 9;
    static const uint8_t CMD_SET_BLOCKLEN = 16;
    static const uint8_t CMD_READ_SINGLE_BLOCK = 17;
    static const uint8_t CMD_APP_CMD = 55;
    static const uint8_t CMD_READ_OCR = 58;
    static const uint8_t ACMD_SD_SEND_OP_COND = 41;
    static const uint8_t R1_IN_IDLE_STATE = (1 << 0);
    static const uint32_t OCR_CCS = (UINT32_C(1) << 30);
    static const uint32_t OCR_CPUS = (UINT32_C(1) << 31);
    
    static uint8_t crc7 (uint8_t const *data, uint8_t count, uint8_t crc)
    { 
        for (uint8_t a = 0; a < count; a++) {
            uint8_t byte = data[a];
            for (uint8_t i = 0; i < 8; i++) {
                crc <<= 1;
                if ((byte ^ crc) & 0x80) {
                    crc ^= 0x09;
                }
                byte <<= 1;
            }
        }
        return (crc & 0x7f);
    }
    
    static void sd_command (Context c, uint8_t cmd, uint32_t param, bool checksum, uint8_t *request_buf, uint8_t *response_buf)
    {
        SpiSdCard *o = self(c);
        
        request_buf[0] = cmd | 0x40;
        request_buf[1] = param >> 24;
        request_buf[2] = param >> 16;
        request_buf[3] = param >> 8;
        request_buf[4] = param;
        request_buf[5] = 1;
        if (checksum) {
            request_buf[5] |= crc7(request_buf, 5, 0) << 1;
        }
        o->m_spi.cmdWriteBuffer(c, 0xff, request_buf, 6);
        o->m_spi.cmdReadUntilDifferent(c, 0xff, 255, 0xff, response_buf);
    }
    
    static void sd_send_csd (Context c)
    {
        SpiSdCard *o = self(c);
        sd_command(c, CMD_SEND_CSD, 0, true, o->m_buf1, o->m_buf1);
        o->m_spi.cmdReadUntilDifferent(c, 0xff, 255, 0xff, o->m_buf1 + 1);
        o->m_spi.cmdWriteByte(c, 0xff, 5);
        o->m_spi.cmdReadBuffer(c, o->m_buf2, 6, 0xff);
        o->m_spi.cmdWriteByte(c, 0xff, 7);
    }
    
    static void spi_handler (Context c)
    {
        SpiSdCard *o = self(c);
        o->debugAccess(c);
        AMBRO_ASSERT(o->m_state != STATE_INACTIVE)
        
        if (AMBRO_LIKELY(o->m_state == STATE_RUNNING)) {
            return CommandHandler::call(c);
        }
        if (!o->m_spi.endReached(c)) {
            return;
        }
        switch (o->m_state) {
            case STATE_INIT1: {
                c.pins()->template set<SsPin>(c, false);
                sd_command(c, CMD_GO_IDLE_STATE, 0, true, o->m_buf1, o->m_buf1);
                o->m_state = STATE_INIT2;
                o->m_count = 255;
            } break;
            case STATE_INIT2: {
                if (o->m_buf1[0] != R1_IN_IDLE_STATE) {
                    o->m_count--;
                    if (o->m_count == 0) {
                        return error(c, 1);
                    }
                    sd_command(c, CMD_GO_IDLE_STATE, 0, true, o->m_buf1, o->m_buf1);
                    return;
                }
                sd_command(c, CMD_SEND_IF_COND, UINT32_C(0x1AA), true, o->m_buf1, o->m_buf1);
                o->m_state = STATE_INIT3;
            } break;
            case STATE_INIT3: {
                if (o->m_buf1[0] != 1) {
                    return error(c, 2);
                }
                sd_command(c, CMD_APP_CMD, 0, true, o->m_buf2, o->m_buf2);
                sd_command(c, ACMD_SD_SEND_OP_COND, UINT32_C(0x40000000), true, o->m_buf1, o->m_buf1);
                o->m_state = STATE_INIT4;
                o->m_count = 255;
            } break;
            case STATE_INIT4: {
                if (o->m_buf2[0] != 0 || o->m_buf1[0] != 0) {
                    o->m_count--;
                    if (o->m_count == 0) {
                        return error(c, 3);
                    }
                    sd_command(c, CMD_APP_CMD, 0, true, o->m_buf2, o->m_buf2);
                    sd_command(c, ACMD_SD_SEND_OP_COND, UINT32_C(0x40000000), true, o->m_buf1, o->m_buf1);
                    return;
                }
                sd_command(c, CMD_READ_OCR, 0, true, o->m_buf1, o->m_buf1);
                o->m_spi.cmdReadBuffer(c, o->m_buf1 + 1, 4, 0xff);
                o->m_state = STATE_INIT5;
            } break;
            case STATE_INIT5: {
                if (o->m_buf1[0] != 0) {
                    return error(c, 4);
                }
                uint32_t ocr = ((uint32_t)o->m_buf1[1] << 24) | ((uint32_t)o->m_buf1[2] << 16) | ((uint32_t)o->m_buf1[3] << 8) | ((uint32_t)o->m_buf1[4] << 0);
                if (!(ocr & OCR_CPUS)) {
                    return error(c, 5);
                }
                o->m_sdhc = ocr & OCR_CCS;
                if (!o->m_sdhc) {
                    sd_command(c, CMD_SET_BLOCKLEN, 512, true, o->m_buf1, o->m_buf1);
                    o->m_state = STATE_INIT6;
                } else {
                    sd_send_csd(c);
                    o->m_state = STATE_INIT7;
                }
            } break;
            case STATE_INIT6: {
                if (o->m_buf1[0] != 0) {
                    return error(c, 6);
                }
                sd_send_csd(c);
                o->m_state = STATE_INIT7;
            } break;
            case STATE_INIT7: {
                if (o->m_buf1[0] != 0) {
                    return error(c, 7);
                }
                if (o->m_buf1[1] != 0xfe) {
                    return error(c, 8);
                }
                if (o->m_sdhc) {
                    uint16_t c_size = o->m_buf2[4] | ((uint32_t)o->m_buf2[3] << 8) | ((uint32_t)(o->m_buf2[2] & 0x3f) << 16);
                    o->m_capacity_blocks = (c_size + 1) * UINT32_C(1024);
                } else {
                    uint8_t read_bl_len = o->m_buf2[0] & 0xf;
                    uint8_t c_size_mult = (o->m_buf2[5] >> 7) | ((o->m_buf2[4] & 0x3) << 1);
                    uint16_t c_size = (o->m_buf2[3] >> 6) | ((uint16_t)o->m_buf2[2] << 2) | ((uint16_t)(o->m_buf2[1] & 0x3) << 10);
                    uint16_t mult = ((uint16_t)1 << (c_size_mult + 2));
                    uint32_t blocknr = (uint32_t)(c_size + 1) * mult;
                    uint16_t block_len = (uint16_t)1 << read_bl_len;
                    o->m_capacity_blocks = blocknr * (block_len / 512);
                }
                o->m_state = STATE_RUNNING;
                return InitHandler::call(c, 0);
            } break;
        }
    }
    
    static void deactivate_common (Context c)
    {
        SpiSdCard *o = self(c);
        c.pins()->template set<SsPin>(c, true);
        o->m_spi.deinit(c);
        o->m_state = STATE_INACTIVE;
    }
    
    static void error (Context c, uint8_t code)
    {
        deactivate_common(c);
        return InitHandler::call(c, code);
    }
    
    TheSpi m_spi;
    uint8_t m_state;
    bool m_sdhc;
    union {
        struct {
            uint8_t m_buf1[6];
            uint8_t m_buf2[6];
            uint8_t m_count;
        };
        uint32_t m_capacity_blocks;
    };
    
    struct SpiHandler : public AMBRO_WFUNC_TD(&SpiSdCard::spi_handler) {};
    struct SpiPosition : public MemberPosition<Position, TheSpi, &SpiSdCard::m_spi> {};
};

#include <aprinter/EndNamespace.h>

#endif
