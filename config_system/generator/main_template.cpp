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

/*
 * $${GENERATED_WARNING}
 * 
 * Board for build: $${BoardForBuild}
 */

#include <stdint.h>
#include <stdio.h>

$${PLATFORM_INCLUDES}
static void emergency (void);

#define AMBROLIB_EMERGENCY_ACTION { cli(); emergency(); }
#define AMBROLIB_ABORT_ACTION { while (1); }

#include <aprinter/meta/WrapValue.h>
#include <aprinter/meta/WrapDouble.h>
#include <aprinter/meta/TypeListUtils.h>
#include <aprinter/base/Object.h>
#include <aprinter/base/DebugObject.h>
#include <aprinter/base/Assert.h>
#include <aprinter/base/PlacementNew.h>

$${AprinterIncludes}
using namespace APrinter;

$${EXTRA_CONSTANTS}
APRINTER_CONFIG_START

$${ConfigOptions}
APRINTER_CONFIG_END

struct MyContext;
struct Program;

using MyDebugObjectGroup = DebugObjectGroup<MyContext, Program>;
$${GlobalResourceExprs}
struct MyContext {
    using DebugGroup = MyDebugObjectGroup;
$${GlobalResourceContextAliases}
    void check () const;
};

$${CodeBeforeProgram}
struct Program : public ObjBase<void, void, MakeTypeList<
    MyDebugObjectGroup,
$${GlobalResourceProgramChildren}
>> {
    static Program * self (MyContext c);
};

union ProgramMemory {
    ProgramMemory () {}
    ~ProgramMemory () {}
    
    Program program;
} program_memory;

Program * Program::self (MyContext c) { return &program_memory.program; }
void MyContext::check () const {}

$${GlobalCode}
static void emergency (void)
{
    $${EmergencyProvider}::emergency();
}

extern "C" __attribute__((used)) void __cxa_pure_virtual(void)
{
    AMBRO_ASSERT_ABORT("pure virtual function call")
}

int main ()
{
$${InitCalls}
    MyContext c;
    
    new(&program_memory.program) Program();
    
    MyDebugObjectGroup::init(c);
    
$${GlobalResourceInit}
$${FinalInitCalls}
}
