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

#include <avr/io.h>
#include <avr/interrupt.h>

static void emergency (void);

#define AMBROLIB_EMERGENCY_ACTION { cli(); emergency(); }
#define AMBROLIB_ABORT_ACTION { while (1); }

#include <aprinter/meta/MakeTypeList.h>
#include <aprinter/meta/Position.h>
#include <aprinter/base/Assert.h>
#include <aprinter/base/DebugObject.h>
#include <aprinter/system/BusyEventLoop.h>
#include <aprinter/system/AvrClock.h>
#include <aprinter/system/AvrPins.h>
#include <aprinter/system/InterruptLock.h>
#include <aprinter/system/AvrAdc.h>
#include <aprinter/system/AvrWatchdog.h>
#include <aprinter/system/AvrSerial.h>
#include <aprinter/system/AvrSpi.h>
#include <aprinter/devices/SpiSdCard.h>
#include <aprinter/printer/PrinterMain.h>
#include <aprinter/printer/thermistor/GenericThermistor.h>
#include <aprinter/printer/temp_control/PidControl.h>
#include <aprinter/printer/temp_control/BinaryControl.h>
#include <aprinter/printer/arduino_mega_pins.h>

using namespace APrinter;

using LedBlinkInterval = AMBRO_WRAP_DOUBLE(0.5);
using DefaultInactiveTime = AMBRO_WRAP_DOUBLE(60.0);
using SpeedLimitMultiply = AMBRO_WRAP_DOUBLE(1.0 / 60.0);
using MaxStepsPerCycle = AMBRO_WRAP_DOUBLE(0.00137); // max stepping frequency relative to F_CPU
using ForceTimeout = AMBRO_WRAP_DOUBLE(0.1);
using TheAxisStepperPrecisionParams = AxisStepperAvrPrecisionParams;

using XDefaultStepsPerUnit = AMBRO_WRAP_DOUBLE(80.0);
using XDefaultMin = AMBRO_WRAP_DOUBLE(-53.0);
using XDefaultMax = AMBRO_WRAP_DOUBLE(210.0);
using XDefaultMaxSpeed = AMBRO_WRAP_DOUBLE(300.0);
using XDefaultMaxAccel = AMBRO_WRAP_DOUBLE(1500.0);
using XDefaultDistanceFactor = AMBRO_WRAP_DOUBLE(1.0);
using XDefaultCorneringDistance = AMBRO_WRAP_DOUBLE(40.0);
using XDefaultHomeFastMaxDist = AMBRO_WRAP_DOUBLE(280.0);
using XDefaultHomeRetractDist = AMBRO_WRAP_DOUBLE(3.0);
using XDefaultHomeSlowMaxDist = AMBRO_WRAP_DOUBLE(5.0);
using XDefaultHomeFastSpeed = AMBRO_WRAP_DOUBLE(40.0);
using XDefaultHomeRetractSpeed = AMBRO_WRAP_DOUBLE(50.0);
using XDefaultHomeSlowSpeed = AMBRO_WRAP_DOUBLE(5.0);

using YDefaultStepsPerUnit = AMBRO_WRAP_DOUBLE(80.0);
using YDefaultMin = AMBRO_WRAP_DOUBLE(0.0);
using YDefaultMax = AMBRO_WRAP_DOUBLE(155.0);
using YDefaultMaxSpeed = AMBRO_WRAP_DOUBLE(300.0);
using YDefaultMaxAccel = AMBRO_WRAP_DOUBLE(650.0);
using YDefaultDistanceFactor = AMBRO_WRAP_DOUBLE(1.0);
using YDefaultCorneringDistance = AMBRO_WRAP_DOUBLE(40.0);
using YDefaultHomeFastMaxDist = AMBRO_WRAP_DOUBLE(200.0);
using YDefaultHomeRetractDist = AMBRO_WRAP_DOUBLE(3.0);
using YDefaultHomeSlowMaxDist = AMBRO_WRAP_DOUBLE(5.0);
using YDefaultHomeFastSpeed = AMBRO_WRAP_DOUBLE(40.0);
using YDefaultHomeRetractSpeed = AMBRO_WRAP_DOUBLE(50.0);
using YDefaultHomeSlowSpeed = AMBRO_WRAP_DOUBLE(5.0);

using ZDefaultStepsPerUnit = AMBRO_WRAP_DOUBLE(4000.0);
using ZDefaultMin = AMBRO_WRAP_DOUBLE(0.0);
using ZDefaultMax = AMBRO_WRAP_DOUBLE(100.0);
using ZDefaultMaxSpeed = AMBRO_WRAP_DOUBLE(3.0);
using ZDefaultMaxAccel = AMBRO_WRAP_DOUBLE(30.0);
using ZDefaultDistanceFactor = AMBRO_WRAP_DOUBLE(1.0);
using ZDefaultCorneringDistance = AMBRO_WRAP_DOUBLE(40.0);
using ZDefaultHomeFastMaxDist = AMBRO_WRAP_DOUBLE(101.0);
using ZDefaultHomeRetractDist = AMBRO_WRAP_DOUBLE(0.8);
using ZDefaultHomeSlowMaxDist = AMBRO_WRAP_DOUBLE(1.2);
using ZDefaultHomeFastSpeed = AMBRO_WRAP_DOUBLE(2.0);
using ZDefaultHomeRetractSpeed = AMBRO_WRAP_DOUBLE(2.0);
using ZDefaultHomeSlowSpeed = AMBRO_WRAP_DOUBLE(0.6);

using EDefaultStepsPerUnit = AMBRO_WRAP_DOUBLE(928.0);
using EDefaultMin = AMBRO_WRAP_DOUBLE(-40000.0);
using EDefaultMax = AMBRO_WRAP_DOUBLE(40000.0);
using EDefaultMaxSpeed = AMBRO_WRAP_DOUBLE(45.0);
using EDefaultMaxAccel = AMBRO_WRAP_DOUBLE(250.0);
using EDefaultDistanceFactor = AMBRO_WRAP_DOUBLE(1.0);
using EDefaultCorneringDistance = AMBRO_WRAP_DOUBLE(40.0);

using UDefaultStepsPerUnit = AMBRO_WRAP_DOUBLE(660.0);
using UDefaultMin = AMBRO_WRAP_DOUBLE(-40000.0);
using UDefaultMax = AMBRO_WRAP_DOUBLE(40000.0);
using UDefaultMaxSpeed = AMBRO_WRAP_DOUBLE(45.0);
using UDefaultMaxAccel = AMBRO_WRAP_DOUBLE(250.0);
using UDefaultDistanceFactor = AMBRO_WRAP_DOUBLE(1.0);
using UDefaultCorneringDistance = AMBRO_WRAP_DOUBLE(55.0);

using ExtruderHeaterThermistorResistorR = AMBRO_WRAP_DOUBLE(4700.0);
using ExtruderHeaterThermistorR0 = AMBRO_WRAP_DOUBLE(100000.0);
using ExtruderHeaterThermistorBeta = AMBRO_WRAP_DOUBLE(3960.0);
using ExtruderHeaterThermistorMinTemp = AMBRO_WRAP_DOUBLE(10.0);
using ExtruderHeaterThermistorMaxTemp = AMBRO_WRAP_DOUBLE(300.0);
using ExtruderHeaterMinSafeTemp = AMBRO_WRAP_DOUBLE(20.0);
using ExtruderHeaterMaxSafeTemp = AMBRO_WRAP_DOUBLE(280.0);
using ExtruderHeaterPulseInterval = AMBRO_WRAP_DOUBLE(0.2);
using ExtruderHeaterControlInterval = ExtruderHeaterPulseInterval;
using ExtruderHeaterPidP = AMBRO_WRAP_DOUBLE(0.047);
using ExtruderHeaterPidI = AMBRO_WRAP_DOUBLE(0.0006);
using ExtruderHeaterPidD = AMBRO_WRAP_DOUBLE(0.17);
using ExtruderHeaterPidIStateMin = AMBRO_WRAP_DOUBLE(0.0);
using ExtruderHeaterPidIStateMax = AMBRO_WRAP_DOUBLE(0.4);
using ExtruderHeaterPidDHistory = AMBRO_WRAP_DOUBLE(0.7);
using ExtruderHeaterObserverInterval = AMBRO_WRAP_DOUBLE(0.5);
using ExtruderHeaterObserverTolerance = AMBRO_WRAP_DOUBLE(3.0);
using ExtruderHeaterObserverMinTime = AMBRO_WRAP_DOUBLE(3.0);

using UxtruderHeaterThermistorResistorR = AMBRO_WRAP_DOUBLE(4700.0);
using UxtruderHeaterThermistorR0 = AMBRO_WRAP_DOUBLE(100000.0);
using UxtruderHeaterThermistorBeta = AMBRO_WRAP_DOUBLE(3960.0);
using UxtruderHeaterThermistorMinTemp = AMBRO_WRAP_DOUBLE(10.0);
using UxtruderHeaterThermistorMaxTemp = AMBRO_WRAP_DOUBLE(300.0);
using UxtruderHeaterMinSafeTemp = AMBRO_WRAP_DOUBLE(20.0);
using UxtruderHeaterMaxSafeTemp = AMBRO_WRAP_DOUBLE(280.0);
using UxtruderHeaterPulseInterval = AMBRO_WRAP_DOUBLE(0.2);
using UxtruderHeaterControlInterval = UxtruderHeaterPulseInterval;
using UxtruderHeaterPidP = AMBRO_WRAP_DOUBLE(0.047);
using UxtruderHeaterPidI = AMBRO_WRAP_DOUBLE(0.0006);
using UxtruderHeaterPidD = AMBRO_WRAP_DOUBLE(0.17);
using UxtruderHeaterPidIStateMin = AMBRO_WRAP_DOUBLE(0.0);
using UxtruderHeaterPidIStateMax = AMBRO_WRAP_DOUBLE(0.4);
using UxtruderHeaterPidDHistory = AMBRO_WRAP_DOUBLE(0.7);
using UxtruderHeaterObserverInterval = AMBRO_WRAP_DOUBLE(0.5);
using UxtruderHeaterObserverTolerance = AMBRO_WRAP_DOUBLE(3.0);
using UxtruderHeaterObserverMinTime = AMBRO_WRAP_DOUBLE(3.0);

using BedHeaterThermistorResistorR = AMBRO_WRAP_DOUBLE(4700.0);
using BedHeaterThermistorR0 = AMBRO_WRAP_DOUBLE(10000.0);
using BedHeaterThermistorBeta = AMBRO_WRAP_DOUBLE(3480.0);
using BedHeaterThermistorMinTemp = AMBRO_WRAP_DOUBLE(10.0);
using BedHeaterThermistorMaxTemp = AMBRO_WRAP_DOUBLE(150.0);
using BedHeaterMinSafeTemp = AMBRO_WRAP_DOUBLE(20.0);
using BedHeaterMaxSafeTemp = AMBRO_WRAP_DOUBLE(120.0);
using BedHeaterPulseInterval = AMBRO_WRAP_DOUBLE(0.3);
using BedHeaterControlInterval = AMBRO_WRAP_DOUBLE(0.3);
using BedHeaterPidP = AMBRO_WRAP_DOUBLE(1.0);
using BedHeaterPidI = AMBRO_WRAP_DOUBLE(0.012);
using BedHeaterPidD = AMBRO_WRAP_DOUBLE(2.5);
using BedHeaterPidIStateMin = AMBRO_WRAP_DOUBLE(0.0);
using BedHeaterPidIStateMax = AMBRO_WRAP_DOUBLE(1.0);
using BedHeaterPidDHistory = AMBRO_WRAP_DOUBLE(0.8);
using BedHeaterObserverInterval = AMBRO_WRAP_DOUBLE(0.5);
using BedHeaterObserverTolerance = AMBRO_WRAP_DOUBLE(1.5);
using BedHeaterObserverMinTime = AMBRO_WRAP_DOUBLE(3.0);

using FanSpeedMultiply = AMBRO_WRAP_DOUBLE(1.0 / 255.0);
using FanPulseInterval = AMBRO_WRAP_DOUBLE(0.04);

using ProbeOffsetX = AMBRO_WRAP_DOUBLE(-18.0);
using ProbeOffsetY = AMBRO_WRAP_DOUBLE(-31.0);
using ProbeStartHeight = AMBRO_WRAP_DOUBLE(17.0);
using ProbeLowHeight = AMBRO_WRAP_DOUBLE(5.0);
using ProbeRetractDist = AMBRO_WRAP_DOUBLE(1.0);
using ProbeMoveSpeed = AMBRO_WRAP_DOUBLE(120.0);
using ProbeFastSpeed = AMBRO_WRAP_DOUBLE(2.0);
using ProbeRetractSpeed = AMBRO_WRAP_DOUBLE(3.0);
using ProbeSlowSpeed = AMBRO_WRAP_DOUBLE(0.6);
using ProbeP1X = AMBRO_WRAP_DOUBLE(0.0);
using ProbeP1Y = AMBRO_WRAP_DOUBLE(31.0);
using ProbeP2X = AMBRO_WRAP_DOUBLE(0.0);
using ProbeP2Y = AMBRO_WRAP_DOUBLE(155.0);
using ProbeP3X = AMBRO_WRAP_DOUBLE(205.0);
using ProbeP3Y = AMBRO_WRAP_DOUBLE(83.0);

/*
 * NOTE: If you need internal pull-ups for endstops, enable these
 * in main() below.
 */

using PrinterParams = PrinterMainParams<
    /*
     * Common parameters.
     */
    PrinterMainSerialParams<
        UINT32_C(250000), // BaudRate
        7, // RecvBufferSizeExp
        7, // SendBufferSizeExp
        GcodeParserParams<8>, // ReceiveBufferSizeExp
        AvrSerial,
        AvrSerialParams<true>
    >,
    MegaPin13, // LedPin
    LedBlinkInterval, // LedBlinkInterval
    DefaultInactiveTime, // DefaultInactiveTime
    SpeedLimitMultiply, // SpeedLimitMultiply
    MaxStepsPerCycle, // MaxStepsPerCycle
    24, // StepperSegmentBufferSize
    24, // EventChannelBufferSize
    13, // LookaheadBufferSize
    6, // LookaheadCommitCount
    ForceTimeout, // ForceTimeout
    double, // FpType
    AvrClockInterruptTimer_TC5_OCC, // EventChannelTimer
    AvrWatchdog,
    AvrWatchdogParams<
        WDTO_2S
    >,
    PrinterMainSdCardParams<
        SpiSdCard,
        SpiSdCardParams<
            AvrPin<AvrPortB, 0>, // SsPin
            AvrSpi
        >,
        FileGcodeParser, // BINARY: BinaryGcodeParser
        GcodeParserParams<8>, // BINARY: BinaryGcodeParserParams<8>
        2, // BufferBlocks
        100 // MaxCommandSize. BINARY: 43
    >,
    PrinterMainProbeParams<
        MakeTypeList<WrapInt<'X'>, WrapInt<'Y'>>, // PlatformAxesList
        'Z', // ProbeAxis
        MegaPin19, // ProbePin,
        AvrPinInputModePullUp, // ProbePinInputMode
        false, // ProbeInvert,
        MakeTypeList<ProbeOffsetX, ProbeOffsetY>, // ProbePlatformOffset
        ProbeStartHeight,
        ProbeLowHeight,
        ProbeRetractDist,
        ProbeMoveSpeed,
        ProbeFastSpeed,
        ProbeRetractSpeed,
        ProbeSlowSpeed,
        MakeTypeList< // ProbePoints
            MakeTypeList<ProbeP1X, ProbeP1Y>,
            MakeTypeList<ProbeP2X, ProbeP2Y>,
            MakeTypeList<ProbeP3X, ProbeP3Y>
        >
    >,
    PrinterMainNoCurrentParams,
    
    /*
     * Axes.
     */
    MakeTypeList<
        PrinterMainAxisParams<
            'X', // Name
            MegaPin55, // DirPin
            MegaPin54, // StepPin
            MegaPin38, // EnablePin
            true, // InvertDir
            XDefaultStepsPerUnit, // StepsPerUnit
            XDefaultMin, // Min
            XDefaultMax, // Max
            XDefaultMaxSpeed, // MaxSpeed
            XDefaultMaxAccel, // MaxAccel
            XDefaultDistanceFactor, // DistanceFactor
            XDefaultCorneringDistance, // CorneringDistance
            PrinterMainHomingParams<
                MegaPin3, // HomeEndPin
                AvrPinInputModePullUp, // HomeEndPinInputMode
                false, // HomeEndInvert
                false, // HomeDir
                XDefaultHomeFastMaxDist, // HomeFastMaxDist
                XDefaultHomeRetractDist, // HomeRetractDist
                XDefaultHomeSlowMaxDist, // HomeSlowMaxDist
                XDefaultHomeFastSpeed, // HomeFastSpeed
                XDefaultHomeRetractSpeed, // HomeRetractSpeed
                XDefaultHomeSlowSpeed // HomeSlowSpeed
            >,
            true, // EnableCartesianSpeedLimit
            32, // StepBits
            AxisStepperParams<
                AvrClockInterruptTimer_TC3_OCA, // StepperTimer
                TheAxisStepperPrecisionParams // PrecisionParams
            >,
            PrinterMainNoMicroStepParams
        >,
        PrinterMainAxisParams<
            'Y', // Name
            MegaPin61, // DirPin
            MegaPin60, // StepPin
            MegaPin56, // EnablePin
            true, // InvertDir
            YDefaultStepsPerUnit, // StepsPerUnit
            YDefaultMin, // Min
            YDefaultMax, // Max
            YDefaultMaxSpeed, // MaxSpeed
            YDefaultMaxAccel, // MaxAccel
            YDefaultDistanceFactor, // DistanceFactor
            YDefaultCorneringDistance, // CorneringDistance
            PrinterMainHomingParams<
                MegaPin14, // HomeEndPin
                AvrPinInputModePullUp, // HomeEndPinInputMode
                false, // HomeEndInvert
                false, // HomeDir
                YDefaultHomeFastMaxDist, // HomeFastMaxDist
                YDefaultHomeRetractDist, // HomeRetractDist
                YDefaultHomeSlowMaxDist, // HomeSlowMaxDist
                YDefaultHomeFastSpeed, // HomeFastSpeed
                YDefaultHomeRetractSpeed, // HomeRetractSpeed
                YDefaultHomeSlowSpeed // HomeSlowSpeed
            >,
            true, // EnableCartesianSpeedLimit
            32, // StepBits
            AxisStepperParams<
                AvrClockInterruptTimer_TC3_OCB, // StepperTimer
                TheAxisStepperPrecisionParams // PrecisionParams
            >,
            PrinterMainNoMicroStepParams
        >,
        PrinterMainAxisParams<
            'Z', // Name
            MegaPin48, // DirPin
            MegaPin46, // StepPin
            MegaPin62, // EnablePin
            false, // InvertDir
            ZDefaultStepsPerUnit, // StepsPerUnit
            ZDefaultMin, // Min
            ZDefaultMax, // Max
            ZDefaultMaxSpeed, // MaxSpeed
            ZDefaultMaxAccel, // MaxAccel
            ZDefaultDistanceFactor, // DistanceFactor
            ZDefaultCorneringDistance, // CorneringDistance
            PrinterMainHomingParams<
                MegaPin18, // HomeEndPin
                AvrPinInputModePullUp, // HomeEndPinInputMode
                false, // HomeEndInvert
                false, // HomeDir
                ZDefaultHomeFastMaxDist, // HomeFastMaxDist
                ZDefaultHomeRetractDist, // HomeRetractDist
                ZDefaultHomeSlowMaxDist, // HomeSlowMaxDist
                ZDefaultHomeFastSpeed, // HomeFastSpeed
                ZDefaultHomeRetractSpeed, // HomeRetractSpeed
                ZDefaultHomeSlowSpeed // HomeSlowSpeed
            >,
            true, // EnableCartesianSpeedLimit
            32, // StepBits
            AxisStepperParams<
                AvrClockInterruptTimer_TC3_OCC, // StepperTimer
                TheAxisStepperPrecisionParams // PrecisionParams
            >,
            PrinterMainNoMicroStepParams
        >,
        PrinterMainAxisParams<
            'E', // Name
            MegaPin28, // DirPin
            MegaPin26, // StepPin
            MegaPin24, // EnablePin
            true, // InvertDir
            EDefaultStepsPerUnit, // StepsPerUnit
            EDefaultMin, // Min
            EDefaultMax, // Max
            EDefaultMaxSpeed, // MaxSpeed
            EDefaultMaxAccel, // MaxAccel
            EDefaultDistanceFactor, // DistanceFactor
            EDefaultCorneringDistance, // CorneringDistance
            PrinterMainNoHomingParams,
            false, // EnableCartesianSpeedLimit
            32, // StepBits
            AxisStepperParams<
                AvrClockInterruptTimer_TC4_OCA, // StepperTimer
                TheAxisStepperPrecisionParams // PrecisionParams
            >,
            PrinterMainNoMicroStepParams
        >,
        PrinterMainAxisParams<
            'U', // Name
            MegaPin34, // DirPin
            MegaPin36, // StepPin
            MegaPin30, // EnablePin
            true, // InvertDir
            UDefaultStepsPerUnit, // StepsPerUnit
            UDefaultMin, // Min
            UDefaultMax, // Max
            UDefaultMaxSpeed, // MaxSpeed
            UDefaultMaxAccel, // MaxAccel
            UDefaultDistanceFactor, // DistanceFactor
            UDefaultCorneringDistance, // CorneringDistance
            PrinterMainNoHomingParams,
            false, // EnableCartesianSpeedLimit
            32, // StepBits
            AxisStepperParams<
                AvrClockInterruptTimer_TC4_OCB, // StepperTimer
                TheAxisStepperPrecisionParams // PrecisionParams
            >,
            PrinterMainNoMicroStepParams
        >
    >,
    
    /*
     * Transform and virtual axes.
     */
    PrinterMainNoTransformParams,
    
    /*
     * Heaters.
     */
    MakeTypeList<
        PrinterMainHeaterParams<
            'T', // Name
            104, // SetMCommand
            109, // WaitMCommand
            301, // SetConfigMCommand
            MegaPinA13, // AdcPin
            MegaPin10, // OutputPin
            false, // OutputInvert
            GenericThermistor< // Thermistor
                ExtruderHeaterThermistorResistorR,
                ExtruderHeaterThermistorR0,
                ExtruderHeaterThermistorBeta,
                ExtruderHeaterThermistorMinTemp,
                ExtruderHeaterThermistorMaxTemp
            >,
            ExtruderHeaterMinSafeTemp, // MinSafeTemp
            ExtruderHeaterMaxSafeTemp, // MaxSafeTemp
            ExtruderHeaterPulseInterval, // PulseInterval
            ExtruderHeaterControlInterval, // ControlInterval
            PidControl, // Control
            PidControlParams<
                ExtruderHeaterPidP, // PidP
                ExtruderHeaterPidI, // PidI
                ExtruderHeaterPidD, // PidD
                ExtruderHeaterPidIStateMin, // PidIStateMin
                ExtruderHeaterPidIStateMax, // PidIStateMax
                ExtruderHeaterPidDHistory // PidDHistory
            >,
            TemperatureObserverParams<
                ExtruderHeaterObserverInterval, // ObserverInterval
                ExtruderHeaterObserverTolerance, // ObserverTolerance
                ExtruderHeaterObserverMinTime // ObserverMinTime
            >,
            AvrClockInterruptTimer_TC4_OCC // TimerTemplate
        >,
        PrinterMainHeaterParams<
            'B', // Name
            140, // SetMCommand
            190, // WaitMCommand
            304, // SetConfigMCommand
            MegaPinA14, // AdcPin
            MegaPin8, // OutputPin
            false, // OutputInvert
            GenericThermistor< // Thermistor
                BedHeaterThermistorResistorR,
                BedHeaterThermistorR0,
                BedHeaterThermistorBeta,
                BedHeaterThermistorMinTemp,
                BedHeaterThermistorMaxTemp
            >,
            BedHeaterMinSafeTemp, // MinSafeTemp
            BedHeaterMaxSafeTemp, // MaxSafeTemp
            BedHeaterPulseInterval, // PulseInterval
            BedHeaterControlInterval, // ControlInterval
            PidControl, // Control
            PidControlParams<
                BedHeaterPidP, // PidP
                BedHeaterPidI, // PidI
                BedHeaterPidD, // PidD
                BedHeaterPidIStateMin, // PidIStateMin
                BedHeaterPidIStateMax, // PidIStateMax
                BedHeaterPidDHistory // PidDHistory
            >,
            TemperatureObserverParams<
                BedHeaterObserverInterval, // ObserverInterval
                BedHeaterObserverTolerance, // ObserverTolerance
                BedHeaterObserverMinTime // ObserverMinTime
            >,
            AvrClockInterruptTimer_TC5_OCA // TimerTemplate
        >,
        PrinterMainHeaterParams<
            'U', // Name
            404, // SetMCommand
            409, // WaitMCommand
            402, // SetConfigMCommand
            MegaPinA15, // AdcPin
            MegaPin9, // OutputPin
            false, // OutputInvert
            GenericThermistor< // Thermistor
                UxtruderHeaterThermistorResistorR,
                UxtruderHeaterThermistorR0,
                UxtruderHeaterThermistorBeta,
                UxtruderHeaterThermistorMinTemp,
                UxtruderHeaterThermistorMaxTemp
            >,
            UxtruderHeaterMinSafeTemp, // MinSafeTemp
            UxtruderHeaterMaxSafeTemp, // MaxSafeTemp
            UxtruderHeaterPulseInterval, // PulseInterval
            UxtruderHeaterControlInterval, // ControlInterval
            PidControl, // Control
            PidControlParams<
                UxtruderHeaterPidP, // PidP
                UxtruderHeaterPidI, // PidI
                UxtruderHeaterPidD, // PidD
                UxtruderHeaterPidIStateMin, // PidIStateMin
                UxtruderHeaterPidIStateMax, // PidIStateMax
                UxtruderHeaterPidDHistory // PidDHistory
            >,
            TemperatureObserverParams<
                UxtruderHeaterObserverInterval, // ObserverInterval
                UxtruderHeaterObserverTolerance, // ObserverTolerance
                UxtruderHeaterObserverMinTime // ObserverMinTime
            >,
            AvrClockInterruptTimer_TC5_OCB // TimerTemplate
        >
    >,
    
    /*
     * Fans.
     */
    MakeTypeList<
        PrinterMainFanParams<
            106, // SetMCommand
            107, // OffMCommand
            MegaPin4, // OutputPin
            false, // OutputInvert
            FanPulseInterval, // PulseInterval
            FanSpeedMultiply, // SpeedMultiply
            AvrClockInterruptTimer_TC1_OCA // TimerTemplate
        >,
        PrinterMainFanParams<
            406, // SetMCommand
            407, // OffMCommand
            MegaPin5, // OutputPin
            false, // OutputInvert
            FanPulseInterval, // PulseInterval
            FanSpeedMultiply, // SpeedMultiply
            AvrClockInterruptTimer_TC1_OCB // TimerTemplate
        >
    >
>;

// need to list all used ADC pins here
using AdcPins = MakeTypeList<
    MegaPinA13,
    MegaPinA14,
    MegaPinA15
>;

static const int AdcRefSel = 1;
static const int AdcPrescaler = 7;
static const int clock_timer_prescaler = 3;

struct MyContext;
struct MyLoopExtra;
struct Program;
struct ClockPosition;
struct LoopPosition;
struct PinsPosition;
struct AdcPosition;
struct PrinterPosition;
struct LoopExtraPosition;

using ProgramPosition = RootPosition<Program>;
using MyDebugObjectGroup = DebugObjectGroup<MyContext>;
using MyClock = AvrClock<ClockPosition, MyContext, clock_timer_prescaler>;
using MyLoop = BusyEventLoop<LoopPosition, LoopExtraPosition, MyContext, MyLoopExtra>;
using MyPins = AvrPins<PinsPosition, MyContext>;
using MyAdc = AvrAdc<AdcPosition, MyContext, AdcPins, AdcRefSel, AdcPrescaler>;
using MyPrinter = PrinterMain<PrinterPosition, MyContext, PrinterParams>;

struct MyContext {
    using DebugGroup = MyDebugObjectGroup;
    using Clock = MyClock;
    using EventLoop = MyLoop;
    using Pins = MyPins;
    using Adc = MyAdc;
    using TheRootPosition = ProgramPosition;
    
    MyDebugObjectGroup * debugGroup () const;
    MyClock * clock () const;
    MyLoop * eventLoop () const;
    MyPins * pins () const;
    MyAdc * adc () const;
    Program * root () const;
    void check () const;
};

struct MyLoopExtra : public BusyEventLoopExtra<LoopExtraPosition, MyLoop, typename MyPrinter::EventLoopFastEvents> {};

struct Program {
    MyDebugObjectGroup d_group;
    MyClock myclock;
    MyLoop myloop;
    MyPins mypins;
    MyAdc myadc;
    MyPrinter myprinter;
    MyLoopExtra myloopextra;
    uint16_t end;
};

struct ClockPosition : public MemberPosition<ProgramPosition, MyClock, &Program::myclock> {};
struct LoopPosition : public MemberPosition<ProgramPosition, MyLoop, &Program::myloop> {};
struct PinsPosition : public MemberPosition<ProgramPosition, MyPins, &Program::mypins> {};
struct AdcPosition : public MemberPosition<ProgramPosition, MyAdc, &Program::myadc> {};
struct PrinterPosition : public MemberPosition<ProgramPosition, MyPrinter, &Program::myprinter> {};
struct LoopExtraPosition : public MemberPosition<ProgramPosition, MyLoopExtra, &Program::myloopextra> {};

Program p;

MyDebugObjectGroup * MyContext::debugGroup () const { return &p.d_group; }
MyClock * MyContext::clock () const { return &p.myclock; }
MyLoop * MyContext::eventLoop () const { return &p.myloop; }
MyPins * MyContext::pins () const { return &p.mypins; }
MyAdc * MyContext::adc () const { return &p.myadc; }
Program * MyContext::root () const { return &p; }
void MyContext::check () const { AMBRO_ASSERT_FORCE(p.end == UINT16_C(0x1234)) }

AMBRO_AVR_CLOCK_ISRS(p.myclock, MyContext())
AMBRO_AVR_ADC_ISRS(p.myadc, MyContext())
AMBRO_AVR_SERIAL_ISRS(MyPrinter::GetSerial, MyContext())
AMBRO_AVR_CLOCK_INTERRUPT_TIMER_TC3_OCA_ISRS(MyPrinter::GetAxisTimer<0>, MyContext())
AMBRO_AVR_CLOCK_INTERRUPT_TIMER_TC3_OCB_ISRS(MyPrinter::GetAxisTimer<1>, MyContext())
AMBRO_AVR_CLOCK_INTERRUPT_TIMER_TC3_OCC_ISRS(MyPrinter::GetAxisTimer<2>, MyContext())
AMBRO_AVR_CLOCK_INTERRUPT_TIMER_TC4_OCA_ISRS(MyPrinter::GetAxisTimer<3>, MyContext())
AMBRO_AVR_CLOCK_INTERRUPT_TIMER_TC4_OCB_ISRS(MyPrinter::GetAxisTimer<4>, MyContext())
AMBRO_AVR_CLOCK_INTERRUPT_TIMER_TC4_OCC_ISRS(MyPrinter::GetHeaterTimer<0>, MyContext())
AMBRO_AVR_CLOCK_INTERRUPT_TIMER_TC5_OCA_ISRS(MyPrinter::GetHeaterTimer<1>, MyContext())
AMBRO_AVR_CLOCK_INTERRUPT_TIMER_TC5_OCB_ISRS(MyPrinter::GetHeaterTimer<2>, MyContext())
AMBRO_AVR_CLOCK_INTERRUPT_TIMER_TC5_OCC_ISRS(MyPrinter::GetEventChannelTimer, MyContext())
AMBRO_AVR_CLOCK_INTERRUPT_TIMER_TC1_OCA_ISRS(MyPrinter::GetFanTimer<0>, MyContext())
AMBRO_AVR_CLOCK_INTERRUPT_TIMER_TC1_OCB_ISRS(MyPrinter::GetFanTimer<1>, MyContext())
AMBRO_AVR_SPI_ISRS(MyPrinter::GetSdCard<>::GetSpi, MyContext())
AMBRO_AVR_WATCHDOG_GLOBAL

FILE uart_output;

static int uart_putchar (char ch, FILE *stream)
{
    MyPrinter::GetSerial::sendWaitFinished(MyContext());
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

static void emergency (void)
{
    MyPrinter::emergency();
}

int main ()
{
    sei();
    setup_uart_stdio();
    
    MyContext c;
    
    p.end = UINT16_C(0x1234);
    p.d_group.init(c);
    p.myclock.init(c);
    p.myclock.initTC3(c);
    p.myclock.initTC4(c);
    p.myclock.initTC5(c);
    p.myloop.init(c);
    p.mypins.init(c);
    p.myadc.init(c);
    p.myprinter.init(c);
    
    p.myloop.run(c);
}
