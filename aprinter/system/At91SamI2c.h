/*
 * Copyright (c) 2014 Ambroz Bizjak
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

#ifndef AMBROLIB_AT91SAM_I2C_H
#define AMBROLIB_AT91SAM_I2C_H

#include <stdint.h>
#include <stddef.h>
#include <sam/drivers/pmc/pmc.h>

#include <aprinter/meta/Object.h>
#include <aprinter/meta/MakeTypeList.h>
#include <aprinter/base/DebugObject.h>
#include <aprinter/base/Assert.h>
#include <aprinter/base/Lock.h>
#include <aprinter/system/InterruptLock.h>
#include <aprinter/system/At91SamPins.h>

#include <aprinter/BeginNamespace.h>

#define APRINTER_AT91SAM_I2C_DEFINE_DEVICE(Index) \
struct At91SamI2cDevice##Index { \
    static int const DeviceIndex = Index; \
    static Twi volatile * dev () { return TWI##Index; } \
    static IRQn const Irq = TWI##Index##_IRQn; \
    static int const Id = ID_TWI##Index; \
    using TwckPin = APRINTER_AT91SAM_I2C_TWI##Index##_TWCK_PIN; \
    using TwckPeriph = APRINTER_AT91SAM_I2C_TWI##Index##_TWCK_PERIPH; \
    using TwdPin = APRINTER_AT91SAM_I2C_TWI##Index##_TWD_PIN; \
    using TwdPeriph = APRINTER_AT91SAM_I2C_TWI##Index##_TWD_PERIPH; \
};

#if defined(__SAM3X8E__)

#define APRINTER_AT91SAM_I2C_TWI0_TWCK_PIN At91SamPin<At91SamPioA, 18>
#define APRINTER_AT91SAM_I2C_TWI0_TWCK_PERIPH At91SamPeriphA
#define APRINTER_AT91SAM_I2C_TWI0_TWD_PIN At91SamPin<At91SamPioA, 17>
#define APRINTER_AT91SAM_I2C_TWI0_TWD_PERIPH At91SamPeriphA

#define APRINTER_AT91SAM_I2C_TWI1_TWCK_PIN At91SamPin<At91SamPioB, 13>
#define APRINTER_AT91SAM_I2C_TWI1_TWCK_PERIPH At91SamPeriphA
#define APRINTER_AT91SAM_I2C_TWI1_TWD_PIN At91SamPin<At91SamPioB, 12>
#define APRINTER_AT91SAM_I2C_TWI1_TWD_PERIPH At91SamPeriphA

#else
#error "Unsupported device"
#endif

#ifdef TWI0
APRINTER_AT91SAM_I2C_DEFINE_DEVICE(0)
#endif
#ifdef TWI1
APRINTER_AT91SAM_I2C_DEFINE_DEVICE(1)
#endif

template <typename Context, typename ParentObject, typename Handler, typename Params>
class At91SamI2c {
public:
    using Device = typename Params::Device;
    
private:
    static_assert(Params::Ckdiv < 8, "");
    using FastEvent = typename Context::EventLoop::template FastEventSpec<At91SamI2c>;
    enum {STATE_IDLE, STATE_WRITING, STATE_READING, STATE_DONE};
    
public:
    struct Object;
    
    
    static void init (Context c)
    {
        auto *o = Object::self(c);
        
        Context::Pins::template setPeripheral<typename Device::TwckPin>(c, typename Device::TwckPeriph());
        Context::Pins::template setPeripheral<typename Device::TwdPin>(c, typename Device::TwdPeriph());
        
        Context::EventLoop::template initFastEvent<FastEvent>(c, At91SamI2c::event_handler);
        
        o->state = STATE_IDLE;
        
        pmc_enable_periph_clk(Device::Id);
        Device::dev()->TWI_CWGR = TWI_CWGR_CKDIV(Params::Ckdiv) | TWI_CWGR_CHDIV(Params::Chldiv) | TWI_CWGR_CLDIV(Params::Chldiv);
        Device::dev()->TWI_CR = TWI_CR_SVDIS | TWI_CR_MSEN;
        Device::dev()->TWI_IDR = UINT32_MAX;
        NVIC_ClearPendingIRQ(Device::Irq);
        NVIC_SetPriority(Device::Irq, INTERRUPT_PRIORITY);
        NVIC_EnableIRQ(Device::Irq);
        
        o->debugInit(c);
    }
    
    static void deinit (Context c)
    {
        auto *o = Object::self(c);
        o->debugDeinit(c);
        
        NVIC_DisableIRQ(Device::Irq);
        Device::dev()->TWI_IDR = UINT32_MAX;
        NVIC_ClearPendingIRQ(Device::Irq);
        pmc_disable_periph_clk(Device::Id);
        
        Context::EventLoop::template resetFastEvent<FastEvent>(c);
    }
    
    static void startWrite (Context c, uint8_t addr, uint8_t const *data, size_t length)
    {
        auto *o = Object::self(c);
        o->debugAccess(c);
        AMBRO_ASSERT(o->state == STATE_IDLE)
        AMBRO_ASSERT(addr < 128)
        AMBRO_ASSERT(length > 0)
        
        AMBRO_LOCK_T(InterruptTempLock(), c, lock_c) {
            o->state = STATE_WRITING;
            o->success = true;
            o->write_data = data;
            o->write_length = length;
        }
        Device::dev()->TWI_MMR = TWI_MMR_DADR(addr);
        Device::dev()->TWI_THR = data[0];
        Device::dev()->TWI_IER = TWI_IER_TXRDY | TWI_IER_NACK;
    }
    
    static void startRead (Context c, uint8_t addr, uint8_t *data, size_t length)
    {
        auto *o = Object::self(c);
        o->debugAccess(c);
        AMBRO_ASSERT(o->state == STATE_IDLE)
        AMBRO_ASSERT(addr < 128)
        AMBRO_ASSERT(length > 0)
        
        AMBRO_LOCK_T(InterruptTempLock(), c, lock_c) {
            o->state = STATE_READING;
            o->success = true;
            o->read_data = data;
            o->read_length = length;
        }
        Device::dev()->TWI_MMR = TWI_MMR_DADR(addr) | TWI_MMR_MREAD;
        Device::dev()->TWI_CR = TWI_CR_START | ((length == 1) ? TWI_CR_STOP : 0);
        Device::dev()->TWI_IER = TWI_IER_RXRDY | TWI_IER_NACK;
    }
    
    static void twi_irq (InterruptContext<Context> c)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(o->state == STATE_WRITING || o->state == STATE_READING)
        
        uint32_t sr = Device::dev()->TWI_SR;
        
        if (o->state == STATE_WRITING) {
            if (o->write_length == 0) {
                if ((sr & TWI_SR_NACK) || !(sr & TWI_SR_TXCOMP)) {
                    o->success = false;
                }
                goto done;
            } else {
                if ((sr & TWI_SR_NACK) || !(sr & TWI_SR_TXRDY)) {
                    o->success = false;
                    goto done;
                }
                o->write_data++;
                o->write_length--;
                if (o->write_length == 0) {
                    Device::dev()->TWI_CR = TWI_CR_STOP;
                    Device::dev()->TWI_IDR = UINT32_MAX;
                    Device::dev()->TWI_IER = TWI_IER_TXCOMP | TWI_IER_NACK;
                } else {
                    Device::dev()->TWI_THR = o->write_data[0];
                }
            }
        } else {
            if ((sr & TWI_SR_NACK) || !(sr & TWI_SR_RXRDY)) {
                o->success = false;
                goto done;
            } else {
                if (o->read_length == 2) {
                    Device::dev()->TWI_CR = TWI_CR_STOP;
                }
                o->read_data[0] = Device::dev()->TWI_RHR;
                o->read_data++;
                o->read_length--;
                if (o->read_length == 0) {
                    goto done;
                }
            }
        }
        
        return;
    done:
        Device::dev()->TWI_IDR = UINT32_MAX;
        o->state = STATE_DONE;
        Context::EventLoop::template triggerFastEvent<FastEvent>(c);
    }
    
    using EventLoopFastEvents = MakeTypeList<FastEvent>;
    
private:
    static void event_handler (Context c)
    {
        auto *o = Object::self(c);
        o->debugAccess(c);
        AMBRO_ASSERT(o->state == STATE_DONE)
        
        o->state = STATE_IDLE;
        return Handler::call(c, o->success);
    }
    
public:
    struct Object : public ObjBase<At91SamI2c, ParentObject, EmptyTypeList>,
        public DebugObject<Context, void>
    {
        uint8_t state;
        bool success;
        uint8_t const *write_data;
        size_t write_length;
        uint8_t *read_data;
        size_t read_length;
    };
};

template <typename TDevice, uint8_t TCkdiv, uint8_t TChldiv>
struct At91SamI2cService {
    using Device = TDevice;
    static uint8_t const Ckdiv = TCkdiv;
    static uint8_t const Chldiv = TChldiv;
    
    template <typename Context, typename ParentObject, typename Handler>
    using I2c = At91SamI2c<Context, ParentObject, Handler, At91SamI2cService>;
};

#define AMBRO_AT91SAM_I2C_GLOBAL(TheDeviceIndex, TheI2c, context) \
extern "C" \
__attribute__((used)) \
void TWI##TheDeviceIndex##_Handler (void) \
{ \
    static_assert(TheDeviceIndex == TheI2c::Device::DeviceIndex, "Invalid i2c device index."); \
    TheI2c::twi_irq(MakeInterruptContext((context))); \
}

#include <aprinter/EndNamespace.h>

#endif
