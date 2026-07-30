#include <format>
#include <utility>
#include <bit>

#include <ObjectDumper/ObjectToString.hpp>
#include <SigScanner/SinglePassSigScanner.hpp>
#include <UE4SSProgram.hpp>

#pragma warning(disable : 4005)
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/Property/FFieldPathProperty.hpp>
#include <Unreal/CoreUObject/UObject/FStrProperty.hpp>
#include <Unreal/Property/FTextProperty.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObject.hpp>
#pragma warning(default : 4005)

namespace RC::ObjectDumper
{
    using namespace RC::Unreal;

    std::unordered_map<ToStringHash, ObjectToStringDecl> object_to_string_functions{};
    std::unordered_map<ToStringHash, ObjectToStringComplexDecl> object_to_string_complex_functions{};

    static auto to_address(uintptr_t address) -> uintptr_t
    {
        auto out_address = address;
        if (UE4SSProgram::settings_manager.ObjectDumper.UseModuleOffsets)
        {
            out_address -= std::bit_cast<uintptr_t>(SigScannerStaticData::m_modules_info.array[std::to_underlying(ScanTarget::MainExe)].lpBaseOfDll);
        }
        return out_address;
    }

    static auto to_address(void* address) -> uintptr_t
    {
        return to_address(std::bit_cast<uintptr_t>(address));
    }

    auto get_to_string(size_t hash) -> ObjectToStringDecl
    {
        return object_to_string_functions[hash];
    }

    auto get_to_string_complex(size_t hash) -> ObjectToStringComplexDecl
    {
        return object_to_string_complex_functions[hash];
    }

    auto to_string_exists(size_t hash) -> bool
    {
        return object_to_string_functions.contains(hash);
    }

    auto to_string_complex_exists(size_t hash) -> bool
    {
        return object_to_string_complex_functions.contains(hash);
    }

    auto object_trivial_dump_to_string(void* p_this, StringType& out_line, const CharType* post_delimiter) -> void
    {
        UObject* p_typed_this = static_cast<UObject*>(p_this);

        out_line.append(fmt::format(STR("[{:016X}] "), reinterpret_cast<uintptr_t>(p_this)));
        out_line.append(p_typed_this->GetFullName());
        out_line.append(fmt::format(STR(" [n: {:X}] [c: {:016X}] [or: {:016X}]"),
                                    p_typed_this->GetNamePrivate().GetComparisonIndex().ToUnstableInt(),
                                    reinterpret_cast<uintptr_t>(p_typed_this->GetClassPrivate()),
                                    reinterpret_cast<uintptr_t>(p_typed_this->GetOuterPrivate())));
    }

    auto object_to_string(void* p_this, StringType& out_line) -> void
    {
        object_trivial_dump_to_string(p_this, out_line);
    }

    auto property_trivial_dump_to_string(void* p_this, StringType& out_line) -> void
    {
        FProperty* p_typed_this = static_cast<FProperty*>(p_this);

        out_line.append(fmt::format(STR("[{:016X}] "), reinterpret_cast<uintptr_t>(p_this)));
        out_line.append(p_typed_this->GetFullName());
        out_line.append(fmt::format(STR(" [o: {:X}] "), p_typed_this->GetOffset_Internal()));

        auto property_class = p_typed_this->GetClass();
        out_line.append(fmt::format(STR("[n: {:X}] [c: {:016X}]"), p_typed_this->GetFName().GetComparisonIndex().ToUnstableInt(), property_class.HashObject()));

        if (Version::IsAtLeast(4, 25))
        {
            out_line.append(fmt::format(STR(" [owr: {:016X}]"), p_typed_this->GetOwnerVariant().HashObject()));
        }
    }

    auto property_to_string(void* p_this, StringType& out_line) -> void
    {
        property_trivial_dump_to_string(p_this, out_line);
    }

    auto arrayproperty_to_string(void* p_this, StringType& out_line) -> void
    {
        property_trivial_dump_to_string(p_this, out_line);

        FArrayProperty* p_typed_this = static_cast<FArrayProperty*>(p_this);
        out_line.append(fmt::format(STR(" [ai: {:016X}]"), reinterpret_cast<uintptr_t>(p_typed_this->GetInner())));
    }

    auto arrayproperty_to_string_complex(void* p_this, StringType& out_line, ObjectToStringComplexDeclCallable callable) -> void
    {
        FProperty* array_inner = static_cast<FArrayProperty*>(p_this)->GetInner();
        if (array_inner)
        {
            auto array_inner_class = array_inner->GetClass().HashObject();

            if (to_string_exists(array_inner_class))
            {
                get_to_string(array_inner_class)(array_inner, out_line);

                if (to_string_complex_exists(array_inner_class))
                {
                    // If this code is executed then we'll be having another line before we return to the dumper, so we need to explicitly add a new line
                    // If this code is not executed then we'll not be having another line and the dumper will add the new line
                    out_line.append(STR("\n"));

                    get_to_string_complex(array_inner_class)(array_inner, out_line, [&]([[maybe_unused]] void* prop) {
                        // It's possible that a new line is supposed to be appended here
                    });
                }

                callable(array_inner);
            }
            else
            {
                out_line.append(array_inner->GetFullName());
                callable(array_inner);
            }
        }
    }

    auto mapproperty_to_string(void* p_this, StringType& out_line) -> void
    {
        property_trivial_dump_to_string(p_this, out_line);

        FMapProperty* typed_this = static_cast<FMapProperty*>(p_this);
        FProperty* key_property = typed_this->GetKeyProp();
        FProperty* value_property = typed_this->GetValueProp();
        out_line.append(fmt::format(STR(" [kp: {:016X}] [vp: {:016X}]"), reinterpret_cast<uintptr_t>(key_property), reinterpret_cast<uintptr_t>(value_property)));
    }

    auto mapproperty_to_string_complex(void* p_this, StringType& out_line, ObjectToStringComplexDeclCallable callable) -> void
    {
        FMapProperty* typed_this = static_cast<FMapProperty*>(p_this);
        FProperty* key_property = typed_this->GetKeyProp();
        FProperty* value_property = typed_this->GetValueProp();
        if (key_property && value_property)
        {
            auto key_property_class = key_property->GetClass().HashObject();
            auto value_property_class = value_property->GetClass().HashObject();

            auto dump_property = [&](FProperty* property, ToStringHash property_class) {
                if (to_string_exists(property_class))
                {
                    get_to_string(property_class)(property, out_line);

                    if (to_string_complex_exists(property_class))
                    {
                        // If this code is executed then we'll be having another line before we return to the dumper, so we need to explicitly add a new line
                        // If this code is not executed then we'll not be having another line and the dumper will add the new line
                        out_line.append(STR("\n"));

                        get_to_string_complex(property_class)(property, out_line, [&]([[maybe_unused]] void* prop) {});
                    }

                    callable(property);
                }
                else
                {
                    out_line.append(property->GetFullName());
                    callable(property);
                }
            };

            dump_property(key_property, key_property_class);
            dump_property(value_property, value_property_class);
        }
    }

    auto classproperty_to_string(void* p_this, StringType& out_line) -> void
    {
        FClassProperty* typed_this = static_cast<FClassProperty*>(p_this);

        property_trivial_dump_to_string(p_this, out_line);
        // mc = MetaClass
        out_line.append(fmt::format(STR(" [mc: {:016X}]"), reinterpret_cast<UPTRINT>(ToRawPtr(typed_this->GetMetaClass()))));
    }

    auto delegateproperty_to_string(void* p_this, StringType& out_line) -> void
    {
        property_trivial_dump_to_string(p_this, out_line);

        FDelegateProperty* p_typed_this = static_cast<FDelegateProperty*>(p_this);
        out_line.append(fmt::format(STR(" [df: {:016X}]"), reinterpret_cast<UPTRINT>(ToRawPtr(p_typed_this->GetSignatureFunction()))));
    }

    auto fieldpathproperty_to_string(void* p_this, StringType& out_line) -> void
    {
        FFieldPathProperty* typed_this = static_cast<FFieldPathProperty*>(p_this);

        property_trivial_dump_to_string(p_this, out_line);
        out_line.append(fmt::format(STR(" [pc: {:016X}]"), reinterpret_cast<UPTRINT>(ToRawPtr(typed_this->GetPropertyClass()))));
    }

    auto interfaceproperty_to_string(void* p_this, StringType& out_line) -> void
    {
        FInterfaceProperty* typed_this = static_cast<FInterfaceProperty*>(p_this);

        property_trivial_dump_to_string(p_this, out_line);
        out_line.append(fmt::format(STR(" [ic: {:016X}]"), reinterpret_cast<UPTRINT>(ToRawPtr(typed_this->GetInterfaceClass()))));
    }

    auto multicastdelegateproperty_to_string(void* p_this, StringType& out_line) -> void
    {
        property_trivial_dump_to_string(p_this, out_line);

        FMulticastDelegateProperty* p_typed_this = static_cast<FMulticastDelegateProperty*>(p_this);
        out_line.append(fmt::format(STR(" [df: {:016X}]"), reinterpret_cast<UPTRINT>(ToRawPtr(p_typed_this->GetSignatureFunction()))));
    }

    auto objectproperty_to_string(void* p_this, StringType& out_line) -> void
    {
        FObjectProperty* typed_this = static_cast<FObjectProperty*>(p_this);

        property_trivial_dump_to_string(p_this, out_line);
        out_line.append(fmt::format(STR(" [pc: {:016X}]"), reinterpret_cast<UPTRINT>(ToRawPtr(typed_this->GetPropertyClass()))));
    }

    auto structproperty_to_string(void* p_this, StringType& out_line) -> void
    {
        FStructProperty* typed_this = static_cast<FStructProperty*>(p_this);

        property_trivial_dump_to_string(p_this, out_line);
        out_line.append(fmt::format(STR(" [ss: {:016X}]"), reinterpret_cast<UPTRINT>(ToRawPtr(typed_this->GetStruct()))));
    }

    auto enumproperty_to_string(void* p_this, StringType& out_line) -> void
    {
        property_trivial_dump_to_string(p_this, out_line);

        auto* typed_this = static_cast<FEnumProperty*>(p_this);
        out_line.append(fmt::format(STR(" [em: {:016X}]"), reinterpret_cast<UPTRINT>(ToRawPtr(typed_this->GetEnum()))));
    }

    auto boolproperty_to_string(void* p_this, StringType& out_line) -> void
    {
        property_trivial_dump_to_string(p_this, out_line);

        auto* typed_this = static_cast<FBoolProperty*>(p_this);
        if (typed_this->GetFieldMask() != 255)
        {
            out_line.append(fmt::format(STR(" [fm: {:X}] [bm: {:X}]"), typed_this->GetFieldMask(), typed_this->GetByteMask()));
        }
    }

    auto enum_to_string(void* p_this, StringType& out_line) -> void
    {
        object_trivial_dump_to_string(p_this, out_line);

        auto* typed_this = static_cast<UEnum*>(p_this);

        for (auto& Elem : typed_this->ForEachName())
        {
            out_line.append(fmt::format(STR("\n[{:016X}] {} [n: {:X}] [v: {}]"), 0, Elem.Key.ToString(), Elem.Key.GetComparisonIndex().ToUnstableInt(), Elem.Value));
        }
    }

    auto struct_to_string(void* p_this, StringType& out_line) -> void
    {
        UStruct* typed_this = static_cast<UStruct*>(p_this);

        object_trivial_dump_to_string(p_this, out_line);
        out_line.append(fmt::format(STR(" [sps: {:016X}]"), reinterpret_cast<uintptr_t>(typed_this->GetSuperStruct())));
    }

    auto function_to_string(void* p_this, StringType& out_line, std::unordered_set<UFunction*>* in_dumped_functions) -> void
    {
        auto typed_this = static_cast<UFunction*>(p_this);

        if (in_dumped_functions)
        {
            if (in_dumped_functions->contains(typed_this))
            {
                return;
            }
            else
            {
                in_dumped_functions->emplace(typed_this);
            }
        }

        object_trivial_dump_to_string(p_this, out_line, STR(":"));

        static auto as_function_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/AngelscriptCode.ASFunction"));
        if (!as_function_class || !typed_this->IsA(as_function_class))
        {
            out_line.append(fmt::format(STR(" [f: {:016X}]"), to_address(typed_this->GetFuncPtr())));
        }
        out_line.append(STR("\n"));

        for (auto param : TFieldRange<FProperty>(typed_this, EFieldIterationFlags::IncludeDeprecated))
        {
            dump_xproperty(param, out_line);
        }
    }

    auto scriptstruct_to_string_complex(void* p_this, StringType& out_line, ObjectToStringComplexDeclCallable callable) -> void
    {
        UScriptStruct* script_struct = static_cast<UScriptStruct*>(p_this);

        for (FProperty* prop : TFieldRange<FProperty>(script_struct, EFieldIterationFlags::IncludeDeprecated))
        {
            callable(prop);
        }
    }

    auto dump_xproperty(FProperty* property, StringType& out_line) -> void
    {
        auto typed_prop_class = property->GetClass().HashObject();

        if (to_string_exists(typed_prop_class))
        {
            get_to_string(typed_prop_class)(property, out_line);
            out_line.append(STR("\n"));

            if (to_string_complex_exists(typed_prop_class))
            {
                get_to_string_complex(typed_prop_class)(property, out_line, [&]([[maybe_unused]] void* prop) {
                    out_line.append(STR("\n"));
                });
            }
        }
        else
        {
            property_to_string(property, out_line);
            out_line.append(STR("\n"));
        }
    }

    auto init() -> void
    {
        object_to_string_functions[UEnum::StaticClass()->HashObject()] = &enum_to_string;
        if (UUserDefinedEnum::IsStaticClassAvailable())
        {
            object_to_string_functions[UUserDefinedEnum::StaticClass()->HashObject()] = &enum_to_string;
        }
        object_to_string_functions[UClass::StaticClass()->HashObject()] = &struct_to_string;
        if (UBlueprintGeneratedClass::IsStaticClassAvailable())
        {
            object_to_string_functions[UBlueprintGeneratedClass::StaticClass()->HashObject()] = &struct_to_string;
        }
        if (UWidgetBlueprintGeneratedClass::IsStaticClassAvailable())
        {
            object_to_string_functions[UWidgetBlueprintGeneratedClass::StaticClass()->HashObject()] = &struct_to_string;
        }
        if (UAnimBlueprintGeneratedClass::IsStaticClassAvailable())
        {
            object_to_string_functions[UAnimBlueprintGeneratedClass::StaticClass()->HashObject()] = &struct_to_string;
        }
        // The 'function_to_string' function is explicitly called, so it doesn't need to be in this map.
        // object_to_string_functions[UFunction::StaticClass()->HashObject()] = &function_to_string;
        // object_to_string_functions[UDelegateFunction::StaticClass()->HashObject()] = &function_to_string;
        // object_to_string_functions[USparseDelegateFunction::StaticClass()->HashObject()] = &function_to_string;
        object_to_string_functions[UScriptStruct::StaticClass()->HashObject()] = &struct_to_string;
        object_to_string_complex_functions[UScriptStruct::StaticClass()->HashObject()] = &scriptstruct_to_string_complex;
#define REGISTER_FIELD(Type, Function)                                                                                             \
        do                                                                                                                        \
        {                                                                                                                         \
            try                                                                                                                   \
            {                                                                                                                     \
                auto field_class = Type::StaticClass();                                                                           \
                if (field_class.IsValid())                                                                                        \
                {                                                                                                                 \
                    object_to_string_functions[field_class.HashObject()] = Function;                                               \
                }                                                                                                                 \
            }                                                                                                                     \
            catch (...)                                                                                                           \
            {                                                                                                                     \
            }                                                                                                                     \
        } while (false)
#define REGISTER_COMPLEX_FIELD(Type, Function)                                                                                     \
        do                                                                                                                        \
        {                                                                                                                         \
            try                                                                                                                   \
            {                                                                                                                     \
                auto field_class = Type::StaticClass();                                                                           \
                if (field_class.IsValid())                                                                                        \
                {                                                                                                                 \
                    object_to_string_complex_functions[field_class.HashObject()] = Function;                                       \
                }                                                                                                                 \
            }                                                                                                                     \
            catch (...)                                                                                                           \
            {                                                                                                                     \
            }                                                                                                                     \
        } while (false)
        REGISTER_FIELD(FObjectProperty, &objectproperty_to_string);
        REGISTER_FIELD(FObjectPtrProperty, &objectproperty_to_string);
        REGISTER_FIELD(FAssetObjectProperty, &objectproperty_to_string);
        REGISTER_FIELD(FAssetClassProperty, &classproperty_to_string);
        REGISTER_FIELD(FInt8Property, &property_to_string);
        REGISTER_FIELD(FInt16Property, &property_to_string);
        REGISTER_FIELD(FIntProperty, &property_to_string);
        REGISTER_FIELD(FInt64Property, &property_to_string);
        REGISTER_FIELD(FByteProperty, &property_to_string);
        REGISTER_FIELD(FUInt16Property, &property_to_string);
        REGISTER_FIELD(FUInt32Property, &property_to_string);
        REGISTER_FIELD(FUInt64Property, &property_to_string);
        REGISTER_FIELD(FNameProperty, &property_to_string);
        REGISTER_FIELD(FFloatProperty, &property_to_string);
        REGISTER_FIELD(FBoolProperty, &boolproperty_to_string);
        REGISTER_FIELD(FArrayProperty, &arrayproperty_to_string);
        REGISTER_COMPLEX_FIELD(FArrayProperty, &arrayproperty_to_string_complex);
        REGISTER_FIELD(FMapProperty, &mapproperty_to_string);
        REGISTER_COMPLEX_FIELD(FMapProperty, &mapproperty_to_string_complex);
        REGISTER_FIELD(FStructProperty, &structproperty_to_string);
        REGISTER_FIELD(FClassProperty, &classproperty_to_string);
        if (Version::IsAtLeast(4, 18))
        {
            REGISTER_FIELD(FSoftClassProperty, &classproperty_to_string);
            REGISTER_FIELD(FSoftObjectProperty, &objectproperty_to_string);
        }
        REGISTER_FIELD(FWeakObjectProperty, &objectproperty_to_string);
        REGISTER_FIELD(FLazyObjectProperty, &objectproperty_to_string);
        if (Version::IsAtLeast(4, 15))
        {
            REGISTER_FIELD(FEnumProperty, &enumproperty_to_string);
        }
        REGISTER_FIELD(FTextProperty, &property_to_string);
        REGISTER_FIELD(FStrProperty, &property_to_string);
        REGISTER_FIELD(FDelegateProperty, &delegateproperty_to_string);
        REGISTER_FIELD(FMulticastDelegateProperty, &multicastdelegateproperty_to_string);
        if (Version::IsAtLeast(4, 23))
        {
            REGISTER_FIELD(FMulticastInlineDelegateProperty, &multicastdelegateproperty_to_string);
            REGISTER_FIELD(FMulticastSparseDelegateProperty, &multicastdelegateproperty_to_string);
        }
        REGISTER_FIELD(FInterfaceProperty, &interfaceproperty_to_string);
        if (Version::IsAtLeast(4, 25))
        {
            REGISTER_FIELD(FFieldPathProperty, &fieldpathproperty_to_string);
        }
#undef REGISTER_COMPLEX_FIELD
#undef REGISTER_FIELD
    }
} // namespace RC::ObjectDumper
