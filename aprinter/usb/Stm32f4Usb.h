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

#ifndef AMBROLIB_STM32F4_USB_H
#define AMBROLIB_STM32F4_USB_H

#include <stdint.h>
#include <usb_regs.h>

#include <aprinter/meta/Object.h>
#include <aprinter/meta/MakeTypeList.h>
#include <aprinter/base/DebugObject.h>
#include <aprinter/base/Lock.h>
#include <aprinter/system/InterruptLock.h>
#include <aprinter/usb/usb_proto.h>

#include <aprinter/BeginNamespace.h>

extern _reent r;
template <
    uint32_t TOtgAddress,
    uint8_t TTrdtim,
    uint8_t TToutcal,
    enum IRQn TIrq
>
struct Stm32f4UsbInfo {
    static uint32_t const OtgAddress = TOtgAddress;
    static uint8_t const Trdtim = TTrdtim;
    static uint8_t const Toutcal = TToutcal;
    static enum IRQn const Irq = TIrq;
};

using Stm32F4UsbInfoFS = Stm32f4UsbInfo<
    USB_OTG_FS_BASE_ADDR,
    5,
    7,
    OTG_FS_IRQn
>;

template <typename Context, typename ParentObject, typename Info>
class Stm32f4Usb {
private:
    using FastEvent = typename Context::EventLoop::template FastEventSpec<Stm32f4Usb>;
    
public:
    struct Object;
    
    enum State {
        STATE_WAITING_RESET = 0,
        STATE_WAITING_ENUM = 1,
        STATE_ENUM_DONE = 2
    };
    
    static void init (Context c)
    {
        auto *o = Object::self(c);
        
        Context::EventLoop::template initFastEvent<FastEvent>(c, Stm32f4Usb::event_handler);
        
        RCC_AHB2PeriphClockCmd(RCC_AHB2Periph_OTG_FS, ENABLE);
        
        //reset_otg(c);
        
        USB_OTG_GINTSTS_TypeDef intsts;
        USB_OTG_GUSBCFG_TypeDef usbcfg;
        USB_OTG_GCCFG_TypeDef gccfg;
        USB_OTG_GAHBCFG_TypeDef ahbcfg;
        USB_OTG_GINTMSK_TypeDef intmsk;
        USB_OTG_DCFG_TypeDef dcfg;
        
        // OTG init
        
        //core()->GINTSTS = core()->GINTSTS;
        
        ahbcfg.d32 = 0;
        ahbcfg.b.glblintrmsk = 1;
        ahbcfg.b.ptxfemplvl = 1;
        core()->GAHBCFG = ahbcfg.d32;
        
        usbcfg.d32 = 0;
        usbcfg.b.physel = 1;
        usbcfg.b.force_dev = 1;
        usbcfg.b.toutcal = Info::Toutcal;
        usbcfg.b.usbtrdtim = Info::Trdtim;
        usbcfg.b.srpcap = 1;
        usbcfg.b.hnpcap = 1;
        core()->GUSBCFG = usbcfg.d32;
        
        intmsk.d32 = 0;
        intmsk.b.otgintr = 1;
        intmsk.b.modemismatch = 1;
        core()->GINTMSK = intmsk.d32;
        
        //core()->GRXFSIZ = 128;
        
        // device init
        
        dcfg.d32 = device()->DCFG;
        dcfg.b.devspd = 3;
        dcfg.b.nzstsouthshk = 1;
        device()->DCFG = dcfg.d32;
        
        //*pcgcctl() = 0;
        
        intmsk.d32 = core()->GINTMSK;
        intmsk.b.usbreset = 1;
        intmsk.b.enumdone = 1;
        intmsk.b.erlysuspend = 1;
        intmsk.b.usbsuspend = 1;
        intmsk.b.sofintr = 1;
        intmsk.b.inepintr = 1;
        core()->GINTMSK = intmsk.d32;
        
        //device()->DAINTMSK = 0xF;
        
        gccfg.d32 = 0;
        gccfg.b.vbussensingB = 1;
        gccfg.b.pwdn = 1;
        core()->GCCFG = gccfg.d32;
        
        o->state = STATE_WAITING_RESET;
        
        NVIC_ClearPendingIRQ(Info::Irq);
        NVIC_SetPriority(Info::Irq, INTERRUPT_PRIORITY);
        NVIC_EnableIRQ(Info::Irq);
        
        o->debugInit(c);
    }
    
    static void deinit (Context c)
    {
        auto *o = Object::self(c);
        o->debugDeinit(c);
        
        NVIC_DisableIRQ(Info::Irq);
        
        device()->DCFG = 0;
        core()->GCCFG = 0;
        core()->GINTMSK = 0;
        core()->GUSBCFG = 0;
        core()->GAHBCFG = 0;
        
        NVIC_ClearPendingIRQ(Info::Irq);
        
        RCC_AHB2PeriphClockCmd(RCC_AHB2Periph_OTG_FS, DISABLE);
        
        Context::EventLoop::template resetFastEvent<FastEvent>(c);
    }
    
    static State getState (Context c)
    {
        auto *o = Object::self(c);
        
        State state;
        AMBRO_LOCK_T(InterruptTempLock(), c, lock_c) {
            state = o->state;
        }
        return state;
    }
    
    static void usb_irq (InterruptContext<Context> c)
    {
        auto *o = Object::self(c);
        
        USB_OTG_GINTSTS_TypeDef intsts;
        intsts.d32 = core()->GINTSTS;
        
        USB_OTG_GINTSTS_TypeDef intsts_clear;
        intsts_clear.d32 = 0;
        
        if (intsts.b.otgintr) {
            USB_OTG_GOTGINT_TypeDef otgint;
            otgint.d32 = core()->GOTGINT;
            core()->GOTGINT = otgint.d32;
            if (otgint.b.sesenddet) {
                o->state = STATE_WAITING_RESET;
            }
        }
        
        if (intsts.b.modemismatch) {
            intsts_clear.b.modemismatch = 1;
        }
        
        if (intsts.b.usbreset) {
            intsts_clear.b.usbreset = 1;
            if (o->state == STATE_WAITING_RESET) {
                o->state = STATE_WAITING_ENUM;
            }
        }
        
        if (intsts.b.enumdone) {
            intsts_clear.b.enumdone = 1;
            if (o->state == STATE_WAITING_ENUM) {
                o->state = STATE_ENUM_DONE;
            }
        }
        
        if (intsts.b.erlysuspend) {
            intsts_clear.b.erlysuspend = 1;
        }
        
        if (intsts.b.usbsuspend) {
            intsts_clear.b.usbsuspend = 1;
        }
        
        if (intsts.b.sofintr) {
            intsts_clear.b.sofintr = 1;
        }
        
        /*
        if (intsts.b.inepint) {
            USB_OTG_DAINT_TypeDef daint;
            daint.d32 = device()->DAINT;
            
            if (daint.in & (1 << 0)) {
                
            }
        }
        */
        
        core()->GINTSTS = intsts_clear.d32;
        
        Context::EventLoop::template triggerFastEvent<FastEvent>(c);
    }
    
    using EventLoopFastEvents = MakeTypeList<FastEvent>;
    
private:
    static USB_OTG_GREGS * core ()
    {
        return (USB_OTG_GREGS *)(Info::OtgAddress + USB_OTG_CORE_GLOBAL_REGS_OFFSET);
    }
    
    static USB_OTG_DREGS * device ()
    {
        return (USB_OTG_DREGS *)(Info::OtgAddress + USB_OTG_DEV_GLOBAL_REG_OFFSET);
    }
    
    static uint32_t volatile * pcgcctl ()
    {
        return (uint32_t volatile *)(Info::OtgAddress + USB_OTG_PCGCCTL_OFFSET);
    }
    
    static void event_handler (Context c)
    {
        auto *o = Object::self(c);
        o->debugAccess(c);
        
        /*
        AMBRO_LOCK_T(InterruptTempLock(), c, lock_c) {
            if (o->int_usbreset) {
                o->reset_finished = true;
            }
            if (o->int_enumdone) {
                o->enum_done = true;
            }
            o->int_usbreset = false;
            o->int_enumdone = false;
        }
        */
    }
    
    static void reset_otg (Context c)
    {
        USB_OTG_GRSTCTL_TypeDef grstctl;
        
        do {
            grstctl.d32 = core()->GRSTCTL;
        } while (!grstctl.b.ahbidle);
        
        grstctl.b.csftrst = 1;
        core()->GRSTCTL = grstctl.d32;
        
        do {
            grstctl.d32 = core()->GRSTCTL;
        } while (grstctl.b.csftrst);
    }
    
public:
    struct Object : public ObjBase<Stm32f4Usb, ParentObject, EmptyTypeList>,
        public DebugObject<Context, void>
    {
        State state;
    };
};

#define AMBRO_STM32F4_USB_GLOBAL(the_usb, context) \
extern "C" \
__attribute__((used)) \
void OTG_FS_IRQHandler (void) \
{ \
    the_usb::usb_irq(MakeInterruptContext((context))); \
}

#include <aprinter/EndNamespace.h>

#endif
