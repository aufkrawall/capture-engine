#pragma once

// Minimal in-process host for the .NET Framework runtime plus the late-binding
// helpers CaptureEngine needs to drive a managed library from native code.
//
// Everything here goes through four frozen COM contracts that ship with every
// .NET Framework 4.x installation - `_AppDomain`, `_Object`, `_Type` and
// `IObjectHandle` - so no managed assembly of our own, no script, and no
// registered type library is required. Name-based `IDispatch` is deliberately
// not used: the CLR answers `GetIDsOfNames` with E_NOTIMPL for the mscorlib
// interfaces, and the concrete hardware classes inside LibreHardwareMonitor are
// internal and expose no class interface at all. `_Object::GetType` followed by
// `_Type::InvokeMember` reaches every public member of every managed object
// regardless of its COM visibility, which is why it is the only mechanism here.

#include <windows.h>
#include <oleauto.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace ce::clr {

// Reflection lookup flags, mirroring System.Reflection.BindingFlags. The values
// are part of the managed enum's public contract and cannot change.
enum BindingFlags : int {
    kBindInstance = 0x0004,
    kBindStatic = 0x0008,
    kBindPublic = 0x0010,
    kBindFlattenHierarchy = 0x0040,
    kBindInvokeMethod = 0x0100,
    kBindGetProperty = 0x1000,
    kBindSetProperty = 0x2000,
};

// Owns one VARIANT, including the COM reference of an object result. An empty
// Value is VT_EMPTY, which the runtime marshals as a managed null reference.
class Value {
public:
    Value();
    ~Value();
    Value(Value&& other) noexcept;
    Value& operator=(Value&& other) noexcept;
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;

    static Value FromString(const std::wstring& text);
    static Value FromBoolean(bool value);
    // Borrows the reference: the VARIANT points at an object owned elsewhere,
    // which is what reflection arguments and invocation targets need.
    static Value Borrow(IUnknown* object);
    Value Clone() const;

    bool IsEmpty() const;
    bool IsObject() const;
    bool IsArray() const;
    IUnknown* AsObject() const;
    bool AsDouble(double& out) const;
    bool AsInt32(int32_t& out) const;

    size_t ArraySize() const;
    // Only meaningful for object arrays; returns an empty Value otherwise.
    Value ArrayElement(size_t index) const;
    // Only meaningful for string arrays.
    bool ArrayString(size_t index, std::string& out) const;

    VARIANT* Address();
    const VARIANT& Raw() const;

private:
    VARIANT value_;
    bool owns_ = true;
};

// Handle to a managed System.Type used as the reflection entry point.
class Type {
public:
    Type() = default;
    ~Type();
    Type(Type&& other) noexcept;
    Type& operator=(Type&& other) noexcept;
    Type(const Type&) = delete;
    Type& operator=(const Type&) = delete;

    static Type FromValue(const Value& value);
    // The System.Type of this Type object itself, i.e. System.RuntimeType.
    Type ReflectionType() const;
    // Borrowed view of the Type object itself, for passing it as a reflection
    // target or argument.
    Value AsValue() const;

    bool Valid() const {
        return type_ != nullptr;
    }
    Value Invoke(const wchar_t* member, int flags, const Value& target, const Value* arguments,
                 size_t argumentCount, HRESULT* status = nullptr) const;

private:
    struct ManagedTypeInterface;
    ManagedTypeInterface* type_ = nullptr;
};

// A managed object together with its cached reflection type.
class Object {
public:
    Object() = default;
    explicit Object(Value value);

    bool Valid() const {
        return object_.IsObject() && type_.Valid();
    }
    const Value& Self() const {
        return object_;
    }
    const Type& ObjectType() const {
        return type_;
    }

    Value Get(const wchar_t* property, HRESULT* status = nullptr) const;
    bool Set(const wchar_t* property, const Value& value) const;
    Value Call(const wchar_t* method, const Value* arguments = nullptr, size_t argumentCount = 0,
               HRESULT* status = nullptr) const;

private:
    Value object_;
    Type type_;
};

// System.Object::ToString for any managed object, returned as UTF-8. Used for
// the value types - Identifier, Version - that never cross as a plain string.
std::string ToText(IUnknown* object);
std::string ToText(const Value& value);

std::string WideToUtf8(const wchar_t* text, size_t length);

// Loads the runtime and exposes the default application domain.
class Host {
public:
    Host() = default;
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // On failure, `failureToken` receives a stable ASCII reason token that the
    // bridge protocol can carry to the parent process.
    bool Start(std::string& failureToken);

    // AppDomain.CreateInstanceFrom: loads the assembly from an explicit path
    // (LoadFrom semantics, so sibling dependency DLLs resolve) and constructs
    // the named type with its default constructor.
    Object CreateFromFile(const std::wstring& assemblyPath, const std::wstring& typeName) const;
    // AppDomain.CreateInstance: constructs a type from an already-loadable
    // assembly such as mscorlib.
    Object CreateFromAssembly(const std::wstring& assemblyName, const std::wstring& typeName) const;
    // Type.GetType for a type reachable without an assembly-qualified name.
    Type FindType(const std::wstring& fullName) const;

    // The System.RuntimeType handle every static reflection call is made
    // through. Valid once Start succeeded.
    const Type& Reflection() const {
        return reflection_;
    }

private:
    struct ManagedAppDomainInterface;
    ManagedAppDomainInterface* appDomain_ = nullptr;
    IUnknown* runtimeHost_ = nullptr;
    Type reflection_;
};

}  // namespace ce::clr
