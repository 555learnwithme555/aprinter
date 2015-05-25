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

#ifndef APRINTER_STM32F4SDIO_H
#define APRINTER_STM32F4SDIO_H

#include <stdint.h>

#include <aprinter/base/Object.h>
#include <aprinter/base/DebugObject.h>
#include <aprinter/base/Assert.h>
#include <aprinter/base/Callback.h>
#include <aprinter/system/Stm32f4Pins.h>

#include <aprinter/BeginNamespace.h>

template <typename Context, typename ParentObject, typename Handler, typename Params>
class Stm32f4Sdio {
public:
    struct Object;
    
private:
    using TheDebugObject = DebugObject<Context, Object>;
    using FastEvent = typename Context::EventLoop::template FastEventSpec<Stm32f4Sdio>;
    
    enum {
        STATE_DEAD,
        STATE_POWERON,
        STATE_READY,
        STATE_EXECCMD
    };
    
    static int const SdPinsAF = 12;
    using SdPinsMode = Stm32f4PinOutputMode<Stm32f4PinOutputTypeNormal, Stm32f4PinOutputSpeedHigh, Stm32f4PinPullModePullUp>;
    
    using SdPinCK = Stm32f4Pin<Stm32f4PortC, 12>;
    using SdPinCmd = Stm32f4Pin<Stm32f4PortD, 2>;
    using SdPinD0 = Stm32f4Pin<Stm32f4PortC, 8>;
    using SdPinD1 = Stm32f4Pin<Stm32f4PortC, 9>;
    using SdPinD2 = Stm32f4Pin<Stm32f4PortC, 10>;
    using SdPinD3 = Stm32f4Pin<Stm32f4PortC, 11>;
    
    static void dma_clk_enable () { __HAL_RCC_DMA2_CLK_ENABLE(); }
    static uint32_t const DmaChannel = DMA_CHANNEL_4;
    static DMA_Stream_TypeDef * dma_rx_stream () { return DMA2_Stream3; }
    static DMA_Stream_TypeDef * dma_tx_stream () { return DMA2_Stream6; }
    static SDIO_TypeDef * sdio () { return SDIO; }
    
public:
    enum ResponseType {
        RESPONSE_NONE,
        RESPONSE_SHORT,
        RESPONSE_LONG
    };
    
    struct CommandParams {
        uint8_t cmd_index;
        uint32_t argument;
        ResponseType response_type;
    };
    
    enum ErrorCode {
        ERROR_NONE,
        ERROR_RESPONSE_TIMEOUT,
        ERROR_RESPONSE_CHECKSUM,
        ERROR_BAD_RESPONSE_CMD
    };
    
    struct CommandResults {
        ErrorCode error_code;
        uint32_t response[4];
    };
    
    static void init (Context c)
    {
        auto *o = Object::self(c);
        
        Context::EventLoop::template initFastEvent<FastEvent>(c, Stm32f4Sdio::event_handler);
        o->timer.init(c, APRINTER_CB_STATFUNC_T(&Stm32f4Sdio::timer_handler));
        
        o->state = STATE_DEAD;
        
        TheDebugObject::init(c);
    }
    
    static void deinit (Context c)
    {
        auto *o = Object::self(c);
        TheDebugObject::deinit(c);
        
        reset_internal(c);
        
        o->timer.deinit(c);
    }
    
    static void reset (Context c)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        
        reset_internal(c);
        
        o->state = STATE_DEAD;
    }
    
    static void startPowerOn (Context c)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        AMBRO_ASSERT(o->state == STATE_DEAD)
        
        msp_init(c);
        
        SD_InitTypeDef tmpinit = SD_InitTypeDef();
        tmpinit.ClockEdge           = SDIO_CLOCK_EDGE_RISING;
        tmpinit.ClockBypass         = SDIO_CLOCK_BYPASS_DISABLE;
        tmpinit.ClockPowerSave      = SDIO_CLOCK_POWER_SAVE_DISABLE;
        tmpinit.BusWide             = SDIO_BUS_WIDE_1B;
        tmpinit.HardwareFlowControl = SDIO_HARDWARE_FLOW_CONTROL_DISABLE;
        tmpinit.ClockDiv            = SDIO_INIT_CLK_DIV;
        SDIO_Init(sdio(), tmpinit);
        
        __SDIO_DISABLE(); 
        
        SDIO_PowerState_ON(sdio());
        
        o->state = STATE_POWERON;
    }
    
    static void completePowerOn (Context c)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        AMBRO_ASSERT(o->state == STATE_POWERON)
        
        __SDIO_ENABLE();
        
        o->state = STATE_READY;
    }
    
    static void startCommand (Context c, CommandParams cmd_params)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        AMBRO_ASSERT(o->state == STATE_READY)
        
        uint32_t response_sdio;
        switch (cmd_params.response_type) {
            case RESPONSE_NONE:
                response_sdio = SDIO_RESPONSE_NO; break;
            case RESPONSE_SHORT:
                response_sdio = SDIO_RESPONSE_SHORT; break;
            case RESPONSE_LONG:
                response_sdio = SDIO_RESPONSE_LONG; break;
            default:
                AMBRO_ASSERT(0);
        }
        
        SDIO_CmdInitTypeDef cmd = SDIO_CmdInitTypeDef();
        cmd.Argument = cmd_params.argument;
        cmd.CmdIndex = cmd_params.cmd_index;
        cmd.Response = response_sdio;
        cmd.WaitForInterrupt = SDIO_WAIT_NO;
        cmd.CPSM = SDIO_CPSM_ENABLE;
        SDIO_SendCommand(sdio(), &cmd);
        
        Context::EventLoop::template triggerFastEvent<FastEvent>(c);
        o->state = STATE_EXECCMD;
        o->cmd_index = cmd_params.cmd_index;
        o->response_type = cmd_params.response_type;
    }
    
    using EventLoopFastEvents = MakeTypeList<FastEvent>;
    
private:
    static void msp_init (Context c)
    {
        auto *o = Object::self(c);
        
        dma_clk_enable();
        
        // DMA Rx
        o->dma_rx = DMA_HandleTypeDef();
        o->dma_rx.Instance = dma_rx_stream();
        o->dma_rx.Init.Channel             = DmaChannel;
        o->dma_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
        o->dma_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
        o->dma_rx.Init.MemInc              = DMA_MINC_ENABLE;
        o->dma_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
        o->dma_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
        o->dma_rx.Init.Mode                = DMA_PFCTRL;
        o->dma_rx.Init.Priority            = DMA_PRIORITY_VERY_HIGH;
        o->dma_rx.Init.FIFOMode            = DMA_FIFOMODE_ENABLE;
        o->dma_rx.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
        o->dma_rx.Init.MemBurst            = DMA_MBURST_INC4;
        o->dma_rx.Init.PeriphBurst         = DMA_PBURST_INC4;
        HAL_DMA_DeInit(&o->dma_rx);
        HAL_DMA_Init(&o->dma_rx);
        
        // DMA Tx
        o->dma_tx = DMA_HandleTypeDef();
        o->dma_tx.Instance = dma_tx_stream();
        o->dma_tx.Init.Channel             = DmaChannel;
        o->dma_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
        o->dma_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
        o->dma_tx.Init.MemInc              = DMA_MINC_ENABLE;
        o->dma_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
        o->dma_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
        o->dma_tx.Init.Mode                = DMA_PFCTRL;
        o->dma_tx.Init.Priority            = DMA_PRIORITY_VERY_HIGH;
        o->dma_tx.Init.FIFOMode            = DMA_FIFOMODE_ENABLE;
        o->dma_tx.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
        o->dma_tx.Init.MemBurst            = DMA_MBURST_INC4;
        o->dma_tx.Init.PeriphBurst         = DMA_PBURST_INC4;
        HAL_DMA_DeInit(&o->dma_tx);
        HAL_DMA_Init(&o->dma_tx);
        
        // SDIO
        __HAL_RCC_SDIO_CLK_ENABLE();
    }
    
    static void msp_deinit (Context c)
    {
        auto *o = Object::self(c);
        
        __HAL_RCC_SDIO_CLK_DISABLE();
        HAL_DMA_DeInit(&o->dma_tx);
        HAL_DMA_DeInit(&o->dma_rx);
    }
    
    static void reset_internal (Context c)
    {
        auto *o = Object::self(c);
        
        if (o->state >= STATE_POWERON) {
            SDIO_PowerState_OFF(sdio());
            msp_deinit(c);
        }
        o->timer.unset(c);
        Context::EventLoop::template resetFastEvent<FastEvent>(c);
    }
    
    static void event_handler (Context c)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        AMBRO_ASSERT(o->state == STATE_EXECCMD)
        
        CommandResults results = CommandResults();
        uint32_t status = sdio()->STA;
        if (o->response_type == RESPONSE_NONE) {
            if (!(status & SDIO_FLAG_CMDSENT)) {
                Context::EventLoop::template triggerFastEvent<FastEvent>(c);
                return;
            }
        } else {
            if ((status & SDIO_FLAG_CCRCFAIL)) {
                results.error_code = ERROR_RESPONSE_CHECKSUM;
                goto report;
            }
            if ((status & SDIO_FLAG_CTIMEOUT)) {
                results.error_code = ERROR_RESPONSE_TIMEOUT;
                goto report;
            }
            if (!(status & SDIO_FLAG_CMDREND)) {
                Context::EventLoop::template triggerFastEvent<FastEvent>(c);
                return;
            }
            if (SDIO_GetCommandResponse(sdio()) != o->cmd_index) {
                results.error_code = ERROR_BAD_RESPONSE_CMD;
                goto report;
            }
            results.response[0] = sdio()->RESP1;
            results.response[1] = sdio()->RESP2;
            results.response[2] = sdio()->RESP3;
            results.response[3] = sdio()->RESP4;
        }
        results.error_code = ERROR_NONE;
    report:
        o->state = STATE_READY;
        return Handler::call(c, results);
    }
    
    static void timer_handler (Context c)
    {
        auto *o = Object::self(c);
        TheDebugObject::access(c);
        
        //
    }
    
public:
    struct Object : public ObjBase<Stm32f4Sdio, ParentObject, MakeTypeList<
        TheDebugObject
    >> {
        DMA_HandleTypeDef dma_rx;
        DMA_HandleTypeDef dma_tx;
        typename Context::EventLoop::QueuedEvent timer;
        uint8_t state;
        uint8_t cmd_index;
        uint8_t response_type;
    };
};

struct Stm32f4SdioService {
    template <typename Context, typename ParentObject, typename Handler>
    using Sdio = Stm32f4Sdio<Context, ParentObject, Handler, Stm32f4SdioService>;
};

#include <aprinter/EndNamespace.h>

#endif
