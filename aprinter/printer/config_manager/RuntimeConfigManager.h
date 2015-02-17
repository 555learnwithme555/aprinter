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

#ifndef APRINTER_RUNTIME_CONFIG_MANAGER_H
#define APRINTER_RUNTIME_CONFIG_MANAGER_H

#include <stdint.h>
#include <string.h>

#include <aprinter/meta/Expr.h>
#include <aprinter/base/Object.h>
#include <aprinter/meta/TypeListGet.h>
#include <aprinter/meta/IndexElemList.h>
#include <aprinter/meta/ListForEach.h>
#include <aprinter/meta/IsEqualFunc.h>
#include <aprinter/meta/WrapType.h>
#include <aprinter/meta/FilterTypeList.h>
#include <aprinter/meta/WrapValue.h>
#include <aprinter/meta/TemplateFunc.h>
#include <aprinter/meta/IfFunc.h>
#include <aprinter/meta/FuncCall.h>
#include <aprinter/meta/StructIf.h>
#include <aprinter/meta/TypesAreEqual.h>
#include <aprinter/meta/JoinTypeLists.h>
#include <aprinter/meta/WrapFunction.h>
#include <aprinter/meta/DedummyIndexTemplate.h>
#include <aprinter/meta/ConstexprHash.h>
#include <aprinter/meta/ConstexprCrc32.h>
#include <aprinter/meta/ConstexprString.h>
#include <aprinter/meta/TypeListLength.h>
#include <aprinter/meta/StaticArray.h>
#include <aprinter/meta/GetMemberTypeFunc.h>
#include <aprinter/meta/ComposeFunctions.h>
#include <aprinter/meta/TypeDictList.h>
#include <aprinter/base/ProgramMemory.h>
#include <aprinter/base/Assert.h>
#include <aprinter/printer/Configuration.h>

#include <aprinter/BeginNamespace.h>

static char RuntimeConfigManager__tolower (char c)
{
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

static bool RuntimeConfigManager__compare_option (char const *name, ProgPtr<char> optname)
{
    while (1) {
        char c = RuntimeConfigManager__tolower(*name);
        char d = RuntimeConfigManager__tolower(*optname);
        if (c != d) {
            return false;
        }
        if (c == '\0') {
            return true;
        }
        ++name;
        ++optname;
    }
}

struct RuntimeConfigManagerNoStoreService {};

template <typename Context, typename ParentObject, typename ConfigOptionsList, typename ThePrinterMain, typename Handler, typename Params>
class RuntimeConfigManager {
public:
    struct Object;
    
private:
    AMBRO_DECLARE_LIST_FOREACH_HELPER(Foreach_reset_config, reset_config)
    AMBRO_DECLARE_LIST_FOREACH_HELPER(Foreach_get_set_cmd, get_set_cmd)
    AMBRO_DECLARE_LIST_FOREACH_HELPER(Foreach_dump_options_helper, dump_options_helper)
    AMBRO_DECLARE_GET_MEMBER_TYPE_FUNC(GetMemberType_Type, Type)
    
    template <typename TheOption>
    using OptionIsNotConstant = WrapBool<(!TypeDictListFind<typename TheOption::Properties, ConfigPropertyConstant>::Found)>;
    using StoreService = typename Params::StoreService;
    using FormatHasher = ConstexprHash<ConstexprCrc32>;
    using SupportedTypesList = MakeTypeList<double, bool>;
    
    template <typename Type>
    using GetTypeIndex = TypeDictListIndex<SupportedTypesList, Type>;
    
    static int const DumpConfigMCommand = 924;
    static int const GetConfigMCommand = 925;
    static int const SetConfigMCommand = 926;
    static int const ResetAllConfigMCommand = 927;
    static int const LoadConfigMCommand = 928;
    static int const SaveConfigMCommand = 929;
    
    static int const MaxDumpLineLen = 60;
    
public:
    using RuntimeConfigOptionsList = FilterTypeList<ConfigOptionsList, TemplateFunc<OptionIsNotConstant>>;
    static int const NumRuntimeOptions = TypeListLength<RuntimeConfigOptionsList>::Value;
    static bool const HasStore = !TypesAreEqual<StoreService, RuntimeConfigManagerNoStoreService>::Value;
    enum class OperationType {LOAD, STORE};
    
private:
    using CommandType = typename ThePrinterMain::CommandType;
    
    template <typename TheType, typename Dummy=void>
    struct TypeSpecific;
    
    template <typename Dummy>
    struct TypeSpecific<double, Dummy> {
        static void get_value_cmd (Context c, CommandType *cmd, double value)
        {
            cmd->reply_append_fp(c, value);
        }
        
        static void set_value_cmd (Context c, CommandType *cmd, double *value, double default_value)
        {
            *value = cmd->get_command_param_fp(c, 'V', default_value);
        }
    };
    
    template <typename Dummy>
    struct TypeSpecific<bool, Dummy> {
        static void get_value_cmd (Context c, CommandType *cmd, bool value)
        {
            cmd->reply_append_uint8(c, value);
        }
        
        static void set_value_cmd (Context c, CommandType *cmd, bool *value, bool default_value)
        {
            *value = cmd->get_command_param_uint32(c, 'V', default_value);
        }
    };
    
    template <int TypeIndex, typename Dummy=void>
    struct TypeGeneral {
        using Type = TypeListGet<SupportedTypesList, TypeIndex>;
        using TheTypeSpecific = TypeSpecific<Type>;
        using OptionsList = FilterTypeList<RuntimeConfigOptionsList, ComposeFunctions<IsEqualFunc<Type>, GetMemberType_Type>>;
        using PrevTypeGeneral = TypeGeneral<(TypeIndex - 1)>;
        static int const NumOptions = TypeListLength<OptionsList>::Value;
        static int const OptionCounter = PrevTypeGeneral::OptionCounter + NumOptions;
        
        template <typename Option>
        using OptionIndex = TypeDictListIndex<OptionsList, Option>;
        
        template <int OptionIndex>
        struct NameTableElem {
            using TheConfigOption = TypeListGet<OptionsList, OptionIndex>;
            static constexpr ProgPtr<char> value () { return ProgPtr<char>::Make(TheConfigOption::name()); }
        };
        
        template <int OptionIndex>
        struct DefaultTableElem {
            using TheConfigOption = TypeListGet<OptionsList, OptionIndex>;
            static constexpr Type value () { return TheConfigOption::DefaultValue::value(); }
        };
        
        using NameTable = StaticArray<ProgPtr<char>, NumOptions, NameTableElem>;
        using DefaultTable = StaticArray<Type, NumOptions, DefaultTableElem>;
        
        static int find_option (char const *name)
        {
            for (int i = 0; i < NumOptions; i++) {
                if (RuntimeConfigManager__compare_option(name, NameTable::readAt(i))) {
                    return i;
                }
            }
            return -1;
        }
        
        static void reset_config (Context c)
        {
            auto *o = Object::self(c);
            for (int i = 0; i < NumOptions; i++) {
                o->values[i] = DefaultTable::readAt(i);
            }
        }
        
        static bool get_set_cmd (Context c, CommandType *cmd, bool get_it, char const *name)
        {
            auto *o = Object::self(c);
            int index = find_option(name);
            if (index < 0) {
                return true;
            }
            if (get_it) {
                TheTypeSpecific::get_value_cmd(c, cmd, o->values[index]);
            } else {
                TheTypeSpecific::set_value_cmd(c, cmd, &o->values[index], DefaultTable::readAt(index));
            }
            return false;
        }
        
        static bool dump_options_helper (Context c, CommandType *cmd, int global_option_index)
        {
            auto *o = Object::self(c);
            AMBRO_ASSERT(global_option_index >= PrevTypeGeneral::OptionCounter)
            
            if (global_option_index < OptionCounter) {
                int index = global_option_index - PrevTypeGeneral::OptionCounter;
                cmd->reply_append_pstr(c, NameTable::readAt(index).m_ptr);
                cmd->reply_append_pstr(c, AMBRO_PSTR(" V"));
                TheTypeSpecific::get_value_cmd(c, cmd, o->values[index]);
                return false;
            }
            return true;
        }
        
        struct Object : public ObjBase<TypeGeneral, typename RuntimeConfigManager::Object, EmptyTypeList> {
            Type values[NumOptions];
        };
    };
    
    template <typename Dummy>
    struct TypeGeneral<(-1), Dummy> {
        static int const OptionCounter = 0;
    };
    
    using TypeGeneralList = IndexElemList<SupportedTypesList, DedummyIndexTemplate<TypeGeneral>::template Result>;
    
    template <int ConfigOptionIndex, typename Dummy0=void>
    struct ConfigOptionState {
        using TheConfigOption = TypeListGet<RuntimeConfigOptionsList, ConfigOptionIndex>;
        using Type = typename TheConfigOption::Type;
        using PrevOption = ConfigOptionState<(ConfigOptionIndex - 1)>;
        static constexpr FormatHasher CurrentHash = PrevOption::CurrentHash.addUint32(GetTypeIndex<Type>::Value).addString(TheConfigOption::name(), ConstexprStrlen(TheConfigOption::name()));
        using TheTypeGeneral = TypeGeneral<GetTypeIndex<Type>::Value>;
        static int const GeneralIndex = TheTypeGeneral::template OptionIndex<TheConfigOption>::Value;
        
        static Type * value (Context c) { return &TheTypeGeneral::Object::self(c)->values[GeneralIndex]; }
        
        static Type call (Context c)
        {
            return *value(c);
        }
    };
    
    template <typename Dummy>
    struct ConfigOptionState<(-1), Dummy> {
        static constexpr FormatHasher CurrentHash = FormatHasher();
    };
    
    using LastOptionState = ConfigOptionState<(NumRuntimeOptions - 1)>;
    
    template <typename Option>
    using FindOptionState = ConfigOptionState<TypeDictListIndex<RuntimeConfigOptionsList, Option>::Value>;
    
    template <typename Option>
    using OptionExprRuntime = VariableExpr<typename Option::Type, FindOptionState<Option>>;
    
    template <typename Option>
    using OptionExprConstant = ConstantExpr<typename Option::Type, typename Option::DefaultValue>;
    
    template <typename Option>
    using OptionExpr = FuncCall<
        IfFunc<
            TemplateFunc<OptionIsNotConstant>,
            TemplateFunc<OptionExprRuntime>,
            TemplateFunc<OptionExprConstant>
        >,
        Option
    >;
    
    AMBRO_STRUCT_IF(StoreFeature, HasStore) {
        struct Object;
        struct StoreHandler;
        using TheStore = typename StoreService::template Store<Context, Object, RuntimeConfigManager, StoreHandler>;
        enum {STATE_IDLE, STATE_LOADING, STATE_SAVING};
        
        static void init (Context c)
        {
            auto *o = Object::self(c);
            
            TheStore::init(c);
            o->state = STATE_IDLE;
        }
        
        static void deinit (Context c)
        {
            TheStore::deinit(c);
        }
        
        static bool checkCommand (Context c, CommandType *cmd)
        {
            auto *o = Object::self(c);
            
            auto cmd_num = cmd->getCmdNumber(c);
            if (cmd_num == LoadConfigMCommand || cmd_num == SaveConfigMCommand) {
                if (!cmd->tryLockedCommand(c)) {
                    return false;
                }
                OperationType type = (cmd_num == LoadConfigMCommand) ? OperationType::LOAD : OperationType::STORE;
                start_operation(c, type, true);
                return false;
            }
            return true;
        }
        
        static void start_operation (Context c, OperationType type, bool from_command)
        {
            auto *o = Object::self(c);
            AMBRO_ASSERT(o->state == STATE_IDLE)
            
            if (type == OperationType::LOAD) {
                TheStore::startReading(c);
                o->state = STATE_LOADING;
            } else {
                TheStore::startWriting(c);
                o->state = STATE_SAVING;
            }
            o->from_command = from_command;
        }
        
        static void store_handler (Context c, bool success)
        {
            auto *o = Object::self(c);
            AMBRO_ASSERT(o->state == STATE_LOADING || o->state == STATE_SAVING)
            
            o->state = STATE_IDLE;
            if (o->from_command) {
                auto *cmd = ThePrinterMain::get_locked(c);
                if (!success) {
                    cmd->reply_append_pstr(c, AMBRO_PSTR("error:Store\n"));
                }
                cmd->finishCommand(c);
            } else {
                Handler::call(c, success);
            }
        }
        struct StoreHandler : public AMBRO_WFUNC_TD(&StoreFeature::store_handler) {};
        
        struct Object : public ObjBase<StoreFeature, typename RuntimeConfigManager::Object, MakeTypeList<
            TheStore
        >> {
            uint8_t state;
            bool from_command;
        };
    } AMBRO_STRUCT_ELSE(StoreFeature) {
        struct Object {};
        static void init (Context c) {}
        static void deinit (Context c) {}
        static bool checkCommand (Context c, CommandType *cmd) { return true; }
    };
    
    static void reset_all_config (Context c)
    {
        ListForEachForward<TypeGeneralList>(Foreach_reset_config(), c);
    }
    
    static void work_dump (Context c)
    {
        auto *o = Object::self(c);
        
        CommandType *cmd = ThePrinterMain::get_locked(c);
        if (o->dump_current_option == NumRuntimeOptions) {
            goto finish;
        }
        if (!cmd->requestSendBufEvent(c, MaxDumpLineLen, RuntimeConfigManager::send_buf_event_handler)) {
            cmd->reply_append_pstr(c, AMBRO_PSTR("Error:Dump\n"));
            goto finish;
        }
        return;
    finish:
        cmd->finishCommand(c);
    }
    
    static void send_buf_event_handler (Context c)
    {
        auto *o = Object::self(c);
        AMBRO_ASSERT(o->dump_current_option < NumRuntimeOptions)
        
        CommandType *cmd = ThePrinterMain::get_locked(c);
        cmd->reply_append_pstr(c, AMBRO_PSTR("M926 I"));
        ListForEachForwardInterruptible<TypeGeneralList>(Foreach_dump_options_helper(), c, cmd, o->dump_current_option);
        cmd->reply_append_ch(c, '\n');
        cmd->reply_poke(c);
        o->dump_current_option++;
        work_dump(c);
    }
    
public:
    static constexpr uint32_t FormatHash = LastOptionState::CurrentHash.end();
    
    static void init (Context c)
    {
        reset_all_config(c);
        StoreFeature::init(c);
    }
    
    static void deinit (Context c)
    {
        StoreFeature::deinit(c);
    }
    
    static bool checkCommand (Context c, CommandType *cmd)
    {
        auto *o = Object::self(c);
        
        auto cmd_num = cmd->getCmdNumber(c);
        if (cmd_num == GetConfigMCommand || cmd_num == SetConfigMCommand || cmd_num == ResetAllConfigMCommand) {
            if (cmd_num == ResetAllConfigMCommand) {
                reset_all_config(c);
            } else {
                bool get_it = (cmd_num == GetConfigMCommand);
                char const *name = cmd->get_command_param_str(c, 'I', "");
                if (ListForEachForwardInterruptible<TypeGeneralList>(Foreach_get_set_cmd(), c, cmd, get_it, name)) {
                    cmd->reply_append_pstr(c, AMBRO_PSTR("Error:Unknown option\n"));
                } else if (get_it) {
                    cmd->reply_append_ch(c, '\n');
                }
            }
            cmd->finishCommand(c);
            return false;
        }
        if (cmd_num == DumpConfigMCommand) {
            if (!cmd->tryLockedCommand(c)) {
                return false;
            }
            o->dump_current_option = 0;
            work_dump(c);
            return false;
        }
        return StoreFeature::checkCommand(c, cmd);
    }
    
    template <typename Option>
    static void setOptionValue (Context c, Option, typename Option::Type value)
    {
        static_assert(OptionIsNotConstant<Option>::Value, "");
        
        *FindOptionState<Option>::value(c) = value;
    }
    
    template <typename Option>
    static typename Option::Type getOptionValue (Context c, Option)
    {
        static_assert(OptionIsNotConstant<Option>::Value, "");
        
        return *FindOptionState<Option>::value(c);
    }
    
    template <typename TheStoreFeature = StoreFeature>
    static void startOperation (Context c, OperationType type)
    {
        return TheStoreFeature::start_operation(c, type, false);
    }
    
    template <typename Option>
    static OptionExpr<Option> e (Option);
    
    template <typename TheStoreFeature = StoreFeature>
    using GetStore = typename TheStoreFeature::TheStore;
    
public:
    struct Object : public ObjBase<RuntimeConfigManager, ParentObject, JoinTypeLists<
        TypeGeneralList,
        MakeTypeList<
            StoreFeature
        >
    >> {
        int dump_current_option;
    };
};

template <
    typename TStoreService
>
struct RuntimeConfigManagerService {
    using StoreService = TStoreService;
    
    template <typename Context, typename ParentObject, typename ConfigOptionsList, typename ThePrinterMain, typename Handler>
    using ConfigManager = RuntimeConfigManager<Context, ParentObject, ConfigOptionsList, ThePrinterMain, Handler, RuntimeConfigManagerService>;
};

#include <aprinter/EndNamespace.h>

#endif
