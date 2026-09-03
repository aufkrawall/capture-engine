#include "clr_interop.h"

#include <mscoree.h>

#include <utility>

namespace ce::clr {
namespace {

// Hosting CLSIDs/IIDs. metahost.h is absent from the MinGW headers, so the two
// hosting interfaces are declared here with exactly the slots that are used.
constexpr CLSID kClsidClrMetaHost = {0x9280188d, 0x0e8e, 0x4867, {0xb3, 0x0c, 0x7f, 0xa8, 0x38, 0x84, 0xe8, 0xde}};
constexpr IID kIidClrMetaHost = {0xd332db9e, 0xb9b3, 0x4125, {0x82, 0x07, 0xa1, 0x48, 0x84, 0xf5, 0x32, 0x16}};
constexpr IID kIidClrRuntimeInfo = {0xbd39d1d2, 0xba2f, 0x486a, {0x89, 0xb0, 0xb4, 0xb0, 0xcb, 0x46, 0x68, 0x91}};
constexpr CLSID kClsidCorRuntimeHost = {0xcb2f6723, 0xab3a, 0x11d2, {0x9c, 0x40, 0x00, 0xc0, 0x4f, 0xa3, 0x0a, 0x3e}};
constexpr IID kIidCorRuntimeHost = {0xcb2f6722, 0xab3a, 0x11d2, {0x9c, 0x40, 0x00, 0xc0, 0x4f, 0xa3, 0x0a, 0x3e}};

// mscorlib COM contracts. These interfaces are frozen: they have been byte
// compatible since .NET Framework 1.1 because every native host and every VB6
// client in existence binds to their vtable layout.
constexpr IID kIidAppDomain = {0x05f696dc, 0x2b29, 0x3663, {0xad, 0x8b, 0xc4, 0x38, 0x9c, 0xf2, 0xa7, 0x13}};
constexpr IID kIidObject = {0x65074f7f, 0x63c0, 0x304e, {0xaf, 0x0a, 0xd5, 0x17, 0x41, 0xcb, 0x4a, 0x8d}};
constexpr IID kIidType = {0xbca8b44d, 0xaad6, 0x3a86, {0x8a, 0xb7, 0x03, 0x34, 0x9f, 0x4f, 0x2d, 0xa2}};
constexpr IID kIidObjectHandle = {0xc460e2b4, 0xe199, 0x412a, {0x84, 0x56, 0x84, 0xdc, 0x3e, 0x48, 0x38, 0xc3}};

// Placeholder for a vtable slot this code never calls. Only the slot positions
// of the members below it matter, and those are fixed by the contracts above.
#define CE_CLR_RESERVED_SLOT(index) virtual HRESULT STDMETHODCALLTYPE ReservedSlot##index(void) = 0;

struct IClrMetaHostSubset : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetRuntime(LPCWSTR version, REFIID riid, void** runtime) = 0;
};

struct IClrRuntimeInfoSubset : public IUnknown {
    CE_CLR_RESERVED_SLOT(3) CE_CLR_RESERVED_SLOT(4) CE_CLR_RESERVED_SLOT(5) CE_CLR_RESERVED_SLOT(6)
    CE_CLR_RESERVED_SLOT(7) CE_CLR_RESERVED_SLOT(8)
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFCLSID classId, REFIID riid, void** result) = 0;
};

// _Object slot 7 is ToString, slot 10 is GetType.
struct IManagedObjectSubset : public IUnknown {
    CE_CLR_RESERVED_SLOT(3) CE_CLR_RESERVED_SLOT(4) CE_CLR_RESERVED_SLOT(5) CE_CLR_RESERVED_SLOT(6)
    virtual HRESULT STDMETHODCALLTYPE ToStringValue(BSTR* text) = 0;
    CE_CLR_RESERVED_SLOT(8) CE_CLR_RESERVED_SLOT(9)
    virtual HRESULT STDMETHODCALLTYPE GetObjectType(IUnknown** type) = 0;
};

// _ObjectHandle slot 3 is Unwrap; AppDomain.CreateInstance* returns one.
struct IManagedObjectHandleSubset : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Unwrap(VARIANT* value) = 0;
};

typedef HRESULT(WINAPI* ClrCreateInstanceFunction)(REFCLSID, REFIID, void**);

IUnknown* QueryManagedInterface(IUnknown* object, REFIID riid) {
    if (!object)
        return nullptr;
    IUnknown* result = nullptr;
    if (FAILED(object->QueryInterface(riid, reinterpret_cast<void**>(&result))))
        return nullptr;
    return result;
}

// Drains the thread's COM error state so a failure here cannot be misread as
// the outcome of the next call.
void DiscardErrorInfo() {
    IErrorInfo* errorInfo = nullptr;
    if (GetErrorInfo(0, &errorInfo) == S_OK && errorInfo)
        errorInfo->Release();
}

}  // namespace

// _AppDomain slot 37 is CreateInstance, slot 38 is CreateInstanceFrom.
struct Host::ManagedAppDomainInterface : public IUnknown {
    CE_CLR_RESERVED_SLOT(3) CE_CLR_RESERVED_SLOT(4) CE_CLR_RESERVED_SLOT(5) CE_CLR_RESERVED_SLOT(6)
    CE_CLR_RESERVED_SLOT(7) CE_CLR_RESERVED_SLOT(8) CE_CLR_RESERVED_SLOT(9) CE_CLR_RESERVED_SLOT(10)
    CE_CLR_RESERVED_SLOT(11) CE_CLR_RESERVED_SLOT(12) CE_CLR_RESERVED_SLOT(13) CE_CLR_RESERVED_SLOT(14)
    CE_CLR_RESERVED_SLOT(15) CE_CLR_RESERVED_SLOT(16) CE_CLR_RESERVED_SLOT(17) CE_CLR_RESERVED_SLOT(18)
    CE_CLR_RESERVED_SLOT(19) CE_CLR_RESERVED_SLOT(20) CE_CLR_RESERVED_SLOT(21) CE_CLR_RESERVED_SLOT(22)
    CE_CLR_RESERVED_SLOT(23) CE_CLR_RESERVED_SLOT(24) CE_CLR_RESERVED_SLOT(25) CE_CLR_RESERVED_SLOT(26)
    CE_CLR_RESERVED_SLOT(27) CE_CLR_RESERVED_SLOT(28) CE_CLR_RESERVED_SLOT(29) CE_CLR_RESERVED_SLOT(30)
    CE_CLR_RESERVED_SLOT(31) CE_CLR_RESERVED_SLOT(32) CE_CLR_RESERVED_SLOT(33) CE_CLR_RESERVED_SLOT(34)
    CE_CLR_RESERVED_SLOT(35) CE_CLR_RESERVED_SLOT(36)
    virtual HRESULT STDMETHODCALLTYPE CreateInstance(BSTR assemblyName, BSTR typeName, IUnknown** handle) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateInstanceFrom(BSTR assemblyFile, BSTR typeName, IUnknown** handle) = 0;
};

// _Type slot 57 is InvokeMember_3, the five-argument reflection entry point.
struct Type::ManagedTypeInterface : public IUnknown {
    CE_CLR_RESERVED_SLOT(3) CE_CLR_RESERVED_SLOT(4) CE_CLR_RESERVED_SLOT(5) CE_CLR_RESERVED_SLOT(6)
    CE_CLR_RESERVED_SLOT(7) CE_CLR_RESERVED_SLOT(8) CE_CLR_RESERVED_SLOT(9)
    virtual HRESULT STDMETHODCALLTYPE GetObjectType(IUnknown** type) = 0;
    CE_CLR_RESERVED_SLOT(11) CE_CLR_RESERVED_SLOT(12) CE_CLR_RESERVED_SLOT(13) CE_CLR_RESERVED_SLOT(14)
    CE_CLR_RESERVED_SLOT(15) CE_CLR_RESERVED_SLOT(16) CE_CLR_RESERVED_SLOT(17) CE_CLR_RESERVED_SLOT(18)
    CE_CLR_RESERVED_SLOT(19) CE_CLR_RESERVED_SLOT(20) CE_CLR_RESERVED_SLOT(21) CE_CLR_RESERVED_SLOT(22)
    CE_CLR_RESERVED_SLOT(23) CE_CLR_RESERVED_SLOT(24) CE_CLR_RESERVED_SLOT(25) CE_CLR_RESERVED_SLOT(26)
    CE_CLR_RESERVED_SLOT(27) CE_CLR_RESERVED_SLOT(28) CE_CLR_RESERVED_SLOT(29) CE_CLR_RESERVED_SLOT(30)
    CE_CLR_RESERVED_SLOT(31) CE_CLR_RESERVED_SLOT(32) CE_CLR_RESERVED_SLOT(33) CE_CLR_RESERVED_SLOT(34)
    CE_CLR_RESERVED_SLOT(35) CE_CLR_RESERVED_SLOT(36) CE_CLR_RESERVED_SLOT(37) CE_CLR_RESERVED_SLOT(38)
    CE_CLR_RESERVED_SLOT(39) CE_CLR_RESERVED_SLOT(40) CE_CLR_RESERVED_SLOT(41) CE_CLR_RESERVED_SLOT(42)
    CE_CLR_RESERVED_SLOT(43) CE_CLR_RESERVED_SLOT(44) CE_CLR_RESERVED_SLOT(45) CE_CLR_RESERVED_SLOT(46)
    CE_CLR_RESERVED_SLOT(47) CE_CLR_RESERVED_SLOT(48) CE_CLR_RESERVED_SLOT(49) CE_CLR_RESERVED_SLOT(50)
    CE_CLR_RESERVED_SLOT(51) CE_CLR_RESERVED_SLOT(52) CE_CLR_RESERVED_SLOT(53) CE_CLR_RESERVED_SLOT(54)
    CE_CLR_RESERVED_SLOT(55) CE_CLR_RESERVED_SLOT(56)
    virtual HRESULT STDMETHODCALLTYPE InvokeMember(BSTR name, int invokeAttribute, IUnknown* binder, VARIANT target,
                                                   SAFEARRAY* arguments, VARIANT* result) = 0;
};

std::string WideToUtf8(const wchar_t* text, size_t length) {
    if (!text || length == 0 || length > INT_MAX)
        return {};
    const int required =
        WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (required <= 0)
        return {};
    std::string result(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), result.data(), required, nullptr,
                            nullptr) != required) {
        return {};
    }
    return result;
}

Value::Value() {
    VariantInit(&value_);
}

Value::~Value() {
    if (owns_)
        VariantClear(&value_);
}

Value::Value(Value&& other) noexcept : value_(other.value_), owns_(other.owns_) {
    VariantInit(&other.value_);
    other.owns_ = true;
}

Value& Value::operator=(Value&& other) noexcept {
    if (this != &other) {
        if (owns_)
            VariantClear(&value_);
        value_ = other.value_;
        owns_ = other.owns_;
        VariantInit(&other.value_);
        other.owns_ = true;
    }
    return *this;
}

Value Value::FromString(const std::wstring& text) {
    Value result;
    result.value_.vt = VT_BSTR;
    result.value_.bstrVal = SysAllocStringLen(text.c_str(), static_cast<UINT>(text.size()));
    return result;
}

Value Value::FromBoolean(bool value) {
    Value result;
    result.value_.vt = VT_BOOL;
    result.value_.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    return result;
}

Value Value::Borrow(IUnknown* object) {
    Value result;
    result.value_.vt = VT_UNKNOWN;
    result.value_.punkVal = object;
    result.owns_ = false;
    return result;
}

Value Value::Clone() const {
    Value result;
    if (FAILED(VariantCopy(&result.value_, const_cast<VARIANT*>(&value_))))
        VariantInit(&result.value_);
    return result;
}

bool Value::IsEmpty() const {
    return value_.vt == VT_EMPTY || value_.vt == VT_NULL;
}

bool Value::IsObject() const {
    return (value_.vt == VT_UNKNOWN || value_.vt == VT_DISPATCH) && value_.punkVal != nullptr;
}

bool Value::IsArray() const {
    return (value_.vt & VT_ARRAY) != 0 && value_.parray != nullptr;
}

IUnknown* Value::AsObject() const {
    return IsObject() ? value_.punkVal : nullptr;
}

bool Value::AsDouble(double& out) const {
    VARIANT converted;
    VariantInit(&converted);
    if (FAILED(VariantChangeType(&converted, const_cast<VARIANT*>(&value_), 0, VT_R8)))
        return false;
    out = converted.dblVal;
    VariantClear(&converted);
    return true;
}

bool Value::AsInt32(int32_t& out) const {
    VARIANT converted;
    VariantInit(&converted);
    if (FAILED(VariantChangeType(&converted, const_cast<VARIANT*>(&value_), 0, VT_I4)))
        return false;
    out = converted.lVal;
    VariantClear(&converted);
    return true;
}

size_t Value::ArraySize() const {
    LONG lower = 0;
    LONG upper = -1;
    // The runtime only ever hands out zero-based single-dimension arrays; the
    // non-negative lower bound is asserted here so the count can be computed in
    // the wide type without relying on signed wraparound.
    if (!IsArray() || SafeArrayGetDim(value_.parray) != 1 ||
        FAILED(SafeArrayGetLBound(value_.parray, 1, &lower)) ||
        FAILED(SafeArrayGetUBound(value_.parray, 1, &upper)) || lower < 0 || upper < lower) {
        return 0;
    }
    return static_cast<size_t>(upper) - static_cast<size_t>(lower) + 1;
}

Value Value::ArrayElement(size_t index) const {
    Value result;
    LONG lower = 0;
    if (index >= ArraySize() || FAILED(SafeArrayGetLBound(value_.parray, 1, &lower)))
        return result;
    LONG position = lower + static_cast<LONG>(index);
    IUnknown* element = nullptr;
    if (FAILED(SafeArrayGetElement(value_.parray, &position, static_cast<void*>(&element))) || !element)
        return result;
    result.value_.vt = VT_UNKNOWN;
    result.value_.punkVal = element;
    return result;
}

bool Value::ArrayString(size_t index, std::string& out) const {
    LONG lower = 0;
    if (index >= ArraySize() || FAILED(SafeArrayGetLBound(value_.parray, 1, &lower)))
        return false;
    LONG position = lower + static_cast<LONG>(index);
    BSTR text = nullptr;
    if (FAILED(SafeArrayGetElement(value_.parray, &position, static_cast<void*>(&text))) || !text)
        return false;
    out = WideToUtf8(text, SysStringLen(text));
    SysFreeString(text);
    return true;
}

VARIANT* Value::Address() {
    return &value_;
}

const VARIANT& Value::Raw() const {
    return value_;
}

Type::~Type() {
    if (type_)
        type_->Release();
}

Type::Type(Type&& other) noexcept : type_(other.type_) {
    other.type_ = nullptr;
}

Type& Type::operator=(Type&& other) noexcept {
    if (this != &other) {
        if (type_)
            type_->Release();
        type_ = other.type_;
        other.type_ = nullptr;
    }
    return *this;
}

Type Type::FromValue(const Value& value) {
    Type result;
    result.type_ = reinterpret_cast<ManagedTypeInterface*>(QueryManagedInterface(value.AsObject(), kIidType));
    return result;
}

Type Type::ReflectionType() const {
    Type result;
    if (!type_)
        return result;
    IUnknown* reflection = nullptr;
    if (SUCCEEDED(type_->GetObjectType(&reflection)) && reflection) {
        result.type_ = reinterpret_cast<ManagedTypeInterface*>(QueryManagedInterface(reflection, kIidType));
        reflection->Release();
    }
    return result;
}

Value Type::AsValue() const {
    return Value::Borrow(static_cast<IUnknown*>(type_));
}

Value Type::Invoke(const wchar_t* member, int flags, const Value& target, const Value* arguments,
                   size_t argumentCount, HRESULT* status) const {
    Value result;
    if (!type_) {
        if (status)
            *status = E_POINTER;
        return result;
    }
    SAFEARRAY* argumentArray = SafeArrayCreateVector(VT_VARIANT, 0, static_cast<ULONG>(argumentCount));
    if (!argumentArray) {
        if (status)
            *status = E_OUTOFMEMORY;
        return result;
    }
    for (size_t index = 0; index < argumentCount; ++index) {
        LONG position = static_cast<LONG>(index);
        SafeArrayPutElement(argumentArray, &position, const_cast<VARIANT*>(&arguments[index].Raw()));
    }
    BSTR memberName = SysAllocString(member);
    const HRESULT hr =
        type_->InvokeMember(memberName, flags, nullptr, target.Raw(), argumentArray, result.Address());
    SysFreeString(memberName);
    SafeArrayDestroy(argumentArray);
    if (FAILED(hr))
        DiscardErrorInfo();
    if (status)
        *status = hr;
    return result;
}

Object::Object(Value value) : object_(std::move(value)) {
    if (!object_.IsObject())
        return;
    IUnknown* managed = QueryManagedInterface(object_.AsObject(), kIidObject);
    if (!managed)
        return;
    IUnknown* objectType = nullptr;
    if (SUCCEEDED(static_cast<IManagedObjectSubset*>(managed)->GetObjectType(&objectType)) && objectType) {
        Value typeValue;
        typeValue.Address()->vt = VT_UNKNOWN;
        typeValue.Address()->punkVal = objectType;
        type_ = Type::FromValue(typeValue);
    }
    managed->Release();
}

Value Object::Get(const wchar_t* property, HRESULT* status) const {
    return type_.Invoke(property, kBindGetProperty | kBindPublic | kBindInstance, object_, nullptr, 0, status);
}

bool Object::Set(const wchar_t* property, const Value& value) const {
    HRESULT status = E_FAIL;
    type_.Invoke(property, kBindSetProperty | kBindPublic | kBindInstance, object_, &value, 1, &status);
    return SUCCEEDED(status);
}

Value Object::Call(const wchar_t* method, const Value* arguments, size_t argumentCount, HRESULT* status) const {
    return type_.Invoke(method, kBindInvokeMethod | kBindPublic | kBindInstance, object_, arguments, argumentCount,
                        status);
}

std::string ToText(IUnknown* object) {
    IUnknown* managed = QueryManagedInterface(object, kIidObject);
    if (!managed)
        return {};
    BSTR text = nullptr;
    std::string result;
    if (SUCCEEDED(static_cast<IManagedObjectSubset*>(managed)->ToStringValue(&text)) && text) {
        result = WideToUtf8(text, SysStringLen(text));
        SysFreeString(text);
    }
    managed->Release();
    return result;
}

std::string ToText(const Value& value) {
    if (value.IsObject())
        return ToText(value.AsObject());
    if (value.Raw().vt == VT_BSTR && value.Raw().bstrVal)
        return WideToUtf8(value.Raw().bstrVal, SysStringLen(value.Raw().bstrVal));
    return {};
}

Host::~Host() {
    if (appDomain_)
        appDomain_->Release();
    if (runtimeHost_)
        runtimeHost_->Release();
}

bool Host::Start(std::string& failureToken) {
    HMODULE runtimeLoader = LoadLibraryW(L"mscoree.dll");
    if (!runtimeLoader) {
        failureToken = "ClrLoaderUnavailable";
        return false;
    }
    auto createInstance =
        reinterpret_cast<ClrCreateInstanceFunction>(GetProcAddress(runtimeLoader, "CLRCreateInstance"));
    if (!createInstance) {
        failureToken = "ClrLoaderUnsupported";
        return false;
    }
    IClrMetaHostSubset* metaHost = nullptr;
    if (FAILED(createInstance(kClsidClrMetaHost, kIidClrMetaHost, reinterpret_cast<void**>(&metaHost))) ||
        !metaHost) {
        failureToken = "ClrMetaHostUnavailable";
        return false;
    }
    IClrRuntimeInfoSubset* runtimeInfo = nullptr;
    const HRESULT runtimeStatus =
        metaHost->GetRuntime(L"v4.0.30319", kIidClrRuntimeInfo, reinterpret_cast<void**>(&runtimeInfo));
    metaHost->Release();
    if (FAILED(runtimeStatus) || !runtimeInfo) {
        failureToken = "ClrRuntimeMissing";
        return false;
    }
    ICorRuntimeHost* runtimeHost = nullptr;
    const HRESULT hostStatus =
        runtimeInfo->GetInterface(kClsidCorRuntimeHost, kIidCorRuntimeHost, reinterpret_cast<void**>(&runtimeHost));
    runtimeInfo->Release();
    if (FAILED(hostStatus) || !runtimeHost) {
        failureToken = "ClrHostUnavailable";
        return false;
    }
    runtimeHost_ = runtimeHost;
    if (FAILED(runtimeHost->Start())) {
        failureToken = "ClrStartFailed";
        return false;
    }
    IUnknown* domain = nullptr;
    if (FAILED(runtimeHost->GetDefaultDomain(&domain)) || !domain) {
        failureToken = "ClrDomainUnavailable";
        return false;
    }
    appDomain_ = reinterpret_cast<ManagedAppDomainInterface*>(QueryManagedInterface(domain, kIidAppDomain));
    domain->Release();
    if (!appDomain_) {
        failureToken = "ClrDomainInterfaceMissing";
        return false;
    }

    // Any managed instance bootstraps reflection: its System.Type, and then
    // that Type's own type, which is the System.RuntimeType every static call
    // below is dispatched through.
    const Object probe = CreateFromAssembly(L"mscorlib", L"System.Object");
    if (!probe.Valid()) {
        failureToken = "ClrReflectionUnavailable";
        return false;
    }
    reflection_ = probe.ObjectType().ReflectionType();
    if (!reflection_.Valid()) {
        failureToken = "ClrReflectionTypeMissing";
        return false;
    }
    return true;
}

Object Host::CreateFromFile(const std::wstring& assemblyPath, const std::wstring& typeName) const {
    Object empty;
    if (!appDomain_)
        return empty;
    BSTR assembly = SysAllocStringLen(assemblyPath.c_str(), static_cast<UINT>(assemblyPath.size()));
    BSTR type = SysAllocStringLen(typeName.c_str(), static_cast<UINT>(typeName.size()));
    IUnknown* handle = nullptr;
    const HRESULT status = appDomain_->CreateInstanceFrom(assembly, type, &handle);
    SysFreeString(assembly);
    SysFreeString(type);
    if (FAILED(status)) {
        DiscardErrorInfo();
        return empty;
    }
    IUnknown* objectHandle = QueryManagedInterface(handle, kIidObjectHandle);
    if (handle)
        handle->Release();
    if (!objectHandle)
        return empty;
    Value instance;
    const HRESULT unwrapStatus = static_cast<IManagedObjectHandleSubset*>(objectHandle)->Unwrap(instance.Address());
    objectHandle->Release();
    if (FAILED(unwrapStatus)) {
        DiscardErrorInfo();
        return empty;
    }
    return Object(std::move(instance));
}

Object Host::CreateFromAssembly(const std::wstring& assemblyName, const std::wstring& typeName) const {
    Object empty;
    if (!appDomain_)
        return empty;
    BSTR assembly = SysAllocStringLen(assemblyName.c_str(), static_cast<UINT>(assemblyName.size()));
    BSTR type = SysAllocStringLen(typeName.c_str(), static_cast<UINT>(typeName.size()));
    IUnknown* handle = nullptr;
    const HRESULT status = appDomain_->CreateInstance(assembly, type, &handle);
    SysFreeString(assembly);
    SysFreeString(type);
    if (FAILED(status)) {
        DiscardErrorInfo();
        return empty;
    }
    IUnknown* objectHandle = QueryManagedInterface(handle, kIidObjectHandle);
    if (handle)
        handle->Release();
    if (!objectHandle)
        return empty;
    Value instance;
    const HRESULT unwrapStatus = static_cast<IManagedObjectHandleSubset*>(objectHandle)->Unwrap(instance.Address());
    objectHandle->Release();
    if (FAILED(unwrapStatus)) {
        DiscardErrorInfo();
        return empty;
    }
    return Object(std::move(instance));
}

Type Host::FindType(const std::wstring& fullName) const {
    const Value name = Value::FromString(fullName);
    const Value target;
    // Type.GetType is a static declared on System.Type, so reaching it from
    // System.RuntimeType requires FlattenHierarchy.
    const Value found = reflection_.Invoke(L"GetType",
                                           kBindInvokeMethod | kBindPublic | kBindStatic | kBindFlattenHierarchy,
                                           target, &name, 1);
    return Type::FromValue(found);
}

}  // namespace ce::clr
