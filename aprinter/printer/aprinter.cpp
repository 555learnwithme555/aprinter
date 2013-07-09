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

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>

#include <avr/io.h>
#include <avr/interrupt.h>

#define AMBROLIB_ABORT_ACTION { cli(); while (1); }

#include <aprinter/meta/WrapFunction.h>
#include <aprinter/base/DebugObject.h>
#include <aprinter/base/Assert.h>
#include <aprinter/meta/MakeTypeList.h>
#include <aprinter/system/AvrEventLoop.h>
#include <aprinter/system/AvrClock.h>
#include <aprinter/system/AvrPins.h>
#include <aprinter/system/AvrPinWatcher.h>
#include <aprinter/system/AvrSerial.h>
#include <aprinter/system/AvrLock.h>
#include <aprinter/stepper/Steppers.h>
#include <aprinter/stepper/AxisStepper.h>
#include <aprinter/stepper/AxisSplitter.h>
#include <aprinter/stepper/AxisSharer.h>

#define CLOCK_TIMER_PRESCALER 2
#define LED1_PIN AvrPin<AvrPortA, 4>
#define LED2_PIN AvrPin<AvrPortA, 3>
#define WATCH_PIN AvrPin<AvrPortC, 2>
#define X_DIR_PIN AvrPin<AvrPortC, 5>
#define X_STEP_PIN AvrPin<AvrPortD, 7>
#define Y_DIR_PIN AvrPin<AvrPortC, 7>
#define Y_STEP_PIN AvrPin<AvrPortC, 6>
#define XYE_ENABLE_PIN AvrPin<AvrPortD, 6>
#define Z_DIR_PIN AvrPin<AvrPortB, 2>
#define Z_STEP_PIN AvrPin<AvrPortB, 3>
#define Z_ENABLE_PIN AvrPin<AvrPortA, 5>
#define BLINK_INTERVAL .051
#define SERIAL_BAUD 115200
#define SERIAL_RX_BUFFER 63
#define SERIAL_TX_BUFFER 63
#define STEPPER_COMMAND_BUFFER_BITS 4
#define SPEED_T_SCALE (0.092*2.0)
#define X_SCALE 1.2
#define Y_SCALE 1.0
#define STEPPERS \
    MakeTypeList< \
        StepperDef<X_DIR_PIN, X_STEP_PIN, XYE_ENABLE_PIN>, \
        StepperDef<Y_DIR_PIN, Y_STEP_PIN, XYE_ENABLE_PIN> \
    >::Type

using namespace APrinter;

struct MyContext;
struct EventLoopParams;
struct PinWatcherHandler;
struct SerialRecvHandler;
struct SerialSendHandler;
struct DriverGetStepperHandler0;
struct DriverGetStepperHandler1;

typedef DebugObjectGroup<MyContext> MyDebugObjectGroup;
typedef AvrClock<MyContext, CLOCK_TIMER_PRESCALER> MyClock;
typedef AvrEventLoop<EventLoopParams> MyLoop;
typedef AvrPins<MyContext> MyPins;
typedef AvrPinWatcherService<MyContext> MyPinWatcherService;
typedef AvrEventLoopQueuedEvent<MyLoop> MyTimer;
typedef AvrPinWatcher<MyContext, WATCH_PIN, PinWatcherHandler> MyPinWatcher;
typedef AvrSerial<MyContext, uint8_t, SERIAL_RX_BUFFER, SerialRecvHandler, uint8_t, SERIAL_TX_BUFFER, SerialSendHandler> MySerial;
typedef Steppers<MyContext, STEPPERS> MySteppers;
typedef SteppersStepper<MyContext, STEPPERS, 0> MySteppersStepper0;
typedef SteppersStepper<MyContext, STEPPERS, 1> MySteppersStepper1;
typedef AxisSharer<MyContext, STEPPER_COMMAND_BUFFER_BITS, MySteppersStepper0, DriverGetStepperHandler0, AvrClockInterruptTimer_TC1_OCA> MyAxisSharer0;
typedef AxisSharer<MyContext, STEPPER_COMMAND_BUFFER_BITS, MySteppersStepper1, DriverGetStepperHandler1, AvrClockInterruptTimer_TC1_OCB> MyAxisSharer1;
typedef AxisSharerUser<MyContext, STEPPER_COMMAND_BUFFER_BITS, MySteppersStepper0, DriverGetStepperHandler0, AvrClockInterruptTimer_TC1_OCA> MyAxisUser0;
typedef AxisSharerUser<MyContext, STEPPER_COMMAND_BUFFER_BITS, MySteppersStepper1, DriverGetStepperHandler1, AvrClockInterruptTimer_TC1_OCB> MyAxisUser1;

struct MyContext {
    typedef MyDebugObjectGroup DebugGroup;
    typedef AvrLock<MyContext> Lock;
    typedef MyClock Clock;
    typedef MyLoop EventLoop;
    typedef MyPins Pins;
    typedef MyPinWatcherService PinWatcherService;
    
    MyDebugObjectGroup * debugGroup () const;
    MyClock * clock () const;
    MyLoop * eventLoop () const;
    MyPins * pins () const;
    MyPinWatcherService * pinWatcherService () const;
};

struct EventLoopParams {
    typedef MyContext Context;
};

static MyDebugObjectGroup d_group;
static MyClock myclock;
static MyLoop myloop;
static MyPins mypins;
static MyPinWatcherService mypinwatcherservice;
static MyTimer mytimer;
static MyPinWatcher mypinwatcher;
static MySerial myserial;
static bool blink_state;
static MyClock::TimeType next_time;
static MySteppers steppers;
static MyAxisSharer0 axis_sharer0;
static MyAxisSharer1 axis_sharer1;
static MyAxisUser0 axis_user0;
static MyAxisUser1 axis_user1;
static int index0;
static int index1;
static int cnt0;
static int cnt1;
static int full;
static bool prev_button;

MyDebugObjectGroup * MyContext::debugGroup () const
{
    return &d_group;
}

MyClock * MyContext::clock () const
{
    return &myclock;
}

MyLoop * MyContext::eventLoop () const
{
    return &myloop;
}

MyPins * MyContext::pins () const
{
    return &mypins;
}

MyPinWatcherService * MyContext::pinWatcherService () const
{
    return &mypinwatcherservice;
}

AMBRO_AVR_CLOCK_ISRS(myclock, MyContext())
AMBRO_AVR_PIN_WATCHER_ISRS(mypinwatcherservice, MyContext())
AMBRO_AVR_SERIAL_ISRS(myserial, MyContext())
AMBRO_AVR_CLOCK_INTERRUPT_TIMER_TC1_OCA_ISRS(*axis_sharer0.getTimer(), MyContext())
AMBRO_AVR_CLOCK_INTERRUPT_TIMER_TC1_OCB_ISRS(*axis_sharer1.getTimer(), MyContext())

static void mytimer_handler (MyTimer *, MyContext c)
{
    blink_state = !blink_state;
    mypins.set<LED1_PIN>(c, blink_state);
    next_time += (MyClock::TimeType)(BLINK_INTERVAL / MyClock::time_unit);
    mytimer.appendAt(c, next_time);
}

static void pinwatcher_handler (MyPinWatcher *, MyContext c, bool state)
{
    mypins.set<LED2_PIN>(c, !state);
    if (!prev_button && state) {
        if (axis_user0.isActive(c) || axis_user1.isActive(c)) {
            if (axis_user0.isActive(c)) {
                axis_user0.getAxis(c)->stop(c);
                axis_user0.deactivate(c);
                steppers.getStepper<0>()->enable(c, false);
            }
            if (axis_user1.isActive(c)) {
                axis_user1.getAxis(c)->stop(c);
                axis_user1.deactivate(c);
                steppers.getStepper<1>()->enable(c, false);
            }
        } else {
            index0 = 0;
            index1 = 0;
            cnt0 = 0;
            cnt1 = 0;
            full = 0;
            steppers.getStepper<0>()->enable(c, true);
            steppers.getStepper<1>()->enable(c, true);
            axis_user0.activate(c);
            axis_user1.activate(c);
            axis_user0.getAxis(c)->start(c);
            axis_user1.getAxis(c)->start(c);
        }
    }
    prev_button = state;
}

static void serial_recv_handler (MySerial *, MyContext c)
{
}

static void serial_send_handler (MySerial *, MyContext c)
{
}

static MySteppersStepper0 * driver_get_stepper_handler0 (MyAxisSharer0 *) 
{
    return steppers.getStepper<0>();
}

static MySteppersStepper1* driver_get_stepper_handler1 (MyAxisSharer1 *)
{
    return steppers.getStepper<1>();
}

static void full_common (MyContext c)
{
    full++;
    if (full == 2) {
        MyClock::TimeType start_time = myclock.getTime(c);
        axis_user0.getAxis(c)->startStepping(c, start_time);
        axis_user1.getAxis(c)->startStepping(c, start_time);
    }
}

static void pull_cmd_handler0 (MyAxisUser0 *, MyContext c)
{
    if (cnt0 == 7 * 6) {
        if (!axis_user0.getAxis(c)->isStepping(c)) {
            full_common(c);
        }
        return;
    }
    float t_scale = SPEED_T_SCALE * X_SCALE;
    switch (index0) {
        case 0:
            axis_user0.getAxis(c)->commandDoneTest(c, true, X_SCALE * 20.0, 1.0 * t_scale, X_SCALE * 20.0);
            break;
        case 1:
            axis_user0.getAxis(c)->commandDoneTest(c, true, X_SCALE * 120.0, 3.0 * t_scale, X_SCALE * 0.0);
            break;
        case 2:
            axis_user0.getAxis(c)->commandDoneTest(c, true, X_SCALE * 20.0, 1.0 * t_scale, X_SCALE * -20.0);
            break;
        case 3:
            axis_user0.getAxis(c)->commandDoneTest(c, false, X_SCALE * 20.0, 1.0 * t_scale, X_SCALE * 20.0);
            break;
        case 4:
            axis_user0.getAxis(c)->commandDoneTest(c, false, X_SCALE * 120.0, 3.0 * t_scale, X_SCALE * 0.0);
            break;
        case 5:
            axis_user0.getAxis(c)->commandDoneTest(c, false, X_SCALE * 20.0, 1.0 * t_scale, X_SCALE * -20.0);
            break;
    }
    index0 = (index0 + 1) % 6;
    cnt0++;
}

static void pull_cmd_handler1 (MyAxisUser1 *, MyContext c)
{
    if (cnt1 == 6 * 8) {
        if (!axis_user1.getAxis(c)->isStepping(c)) {
            full_common(c);
        }
        return;
    }
    float t_scale = SPEED_T_SCALE * Y_SCALE;
    switch (index1) {
        case 0:
            axis_user1.getAxis(c)->commandDoneTest(c, true, Y_SCALE * 20.0, 1.0 * t_scale, Y_SCALE * 20.0);
            break;
        case 1:
            axis_user1.getAxis(c)->commandDoneTest(c, true, Y_SCALE * 120.0, 3.0 * t_scale, Y_SCALE * 0.0);
            break;
        case 2:
            axis_user1.getAxis(c)->commandDoneTest(c, true, Y_SCALE * 20.0, 1.0 * t_scale, Y_SCALE * -20.0);
            break;
        case 3:
            axis_user1.getAxis(c)->commandDoneTest(c, true, Y_SCALE * 0.0, 2.0 * t_scale, Y_SCALE * 0.0);
            break;
        case 4:
            axis_user1.getAxis(c)->commandDoneTest(c, false, Y_SCALE * 20.0, 1.0 * t_scale, Y_SCALE * 20.0);
            break;
        case 5:
            axis_user1.getAxis(c)->commandDoneTest(c, false, Y_SCALE * 120.0, 3.0 * t_scale, Y_SCALE * 0.0);
            break;
        case 6:
            axis_user1.getAxis(c)->commandDoneTest(c, false, Y_SCALE * 20.0, 1.0 * t_scale, Y_SCALE * -20.0);
            break;
        case 7:
            axis_user1.getAxis(c)->commandDoneTest(c, false, Y_SCALE * 0.0, 2.0 * t_scale, Y_SCALE * 0.0);
            break;
    }
    index1 = (index1 + 1) % 8;
    cnt1++;
}

static void buffer_full_handler0 (MyAxisUser0 *, MyContext c)
{
    full_common(c);
}

static void buffer_full_handler1 (MyAxisUser1 *, MyContext c)
{
    full_common(c);
}

static void buffer_empty_handler0 (MyAxisUser0 *, MyContext c)
{
    axis_user0.getAxis(c)->stop(c);
    axis_user0.deactivate(c);
    steppers.getStepper<0>()->enable(c, false);
}

static void buffer_empty_handler1 (MyAxisUser1 *, MyContext c)
{
    axis_user1.getAxis(c)->stop(c);
    axis_user1.deactivate(c);
    steppers.getStepper<1>()->enable(c, false);
}

struct PinWatcherHandler : public AMBRO_WFUNC(pinwatcher_handler) {};
struct SerialRecvHandler : public AMBRO_WFUNC(serial_recv_handler) {};
struct SerialSendHandler : public AMBRO_WFUNC(serial_send_handler) {};
struct DriverGetStepperHandler0 : public AMBRO_WFUNC(driver_get_stepper_handler0) {};
struct DriverGetStepperHandler1 : public AMBRO_WFUNC(driver_get_stepper_handler1) {};

FILE uart_output;

static int uart_putchar (char ch, FILE *stream)
{
    while (!(UCSR0A & (1 << UDRE0)));
    UDR0 = ch;
    return 1;
}

static void setup_uart_stdio ()
{
    uart_output.put = uart_putchar;
    uart_output.flags = _FDEV_SETUP_WRITE;
    stdout = &uart_output;
    stderr = &uart_output;
}

int main ()
{
    MyContext c;
    
    d_group.init(c);
    myclock.init(c);
#ifdef TCNT3
    myclock.initTC3(c);
#endif
    myloop.init(c);
    mypins.init(c);
    mypinwatcherservice.init(c);
    mytimer.init(c, mytimer_handler);
    mypinwatcher.init(c);
    myserial.init(c, SERIAL_BAUD);
    setup_uart_stdio();
    printf("HELLO\n");
    steppers.init(c);
    axis_sharer0.init(c);
    axis_sharer1.init(c);
    axis_user0.init(c, &axis_sharer0, pull_cmd_handler0, buffer_full_handler0, buffer_empty_handler0);
    axis_user1.init(c, &axis_sharer1, pull_cmd_handler1, buffer_full_handler1, buffer_empty_handler1);
    
    mypins.setOutput<LED1_PIN>(c);
    mypins.setOutput<LED2_PIN>(c);
    mypins.setInput<WATCH_PIN>(c);
    
    MyClock::TimeType ref_time = myclock.getTime(c);
    
    blink_state = false;
    next_time = myclock.getTime(c) + (uint32_t)(BLINK_INTERVAL / MyClock::time_unit);
    mytimer.appendAt(c, next_time);
    prev_button = false;
    
    /*
    uint32_t x = 0;
    do {
        uint16_t my = IntSqrt<29>::call(x);
        if (!((uint32_t)my * my <= x && ((uint32_t)my + 1) * ((uint32_t)my + 1) > x)) {
            printf("%" PRIu32 " BAD my=%" PRIu16 "\n", x, my);
        }
        x++;
    } while (x < ((uint32_t)1 << 29));
    */
    /*
    for (uint32_t i = 0; i < UINT32_C(1000000); i++) {
        uint32_t x;
        *((uint8_t *)&x + 0) = rand();
        *((uint8_t *)&x + 1) = rand();
        *((uint8_t *)&x + 2) = rand();
        *((uint8_t *)&x + 3) = rand() & 0x1F;
        uint16_t my = IntSqrt<29>::call(x);
        if (!((uint32_t)my * my <= x && ((uint32_t)my + 1) * ((uint32_t)my + 1) > x)) {
            printf("%" PRIu32 " BAD my=%" PRIu16 "\n", x, my);
        }
    }
    */
    myloop.run(c);
}
