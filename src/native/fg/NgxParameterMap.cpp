#include "NgxParameterMap.h"

#include <cstring>

namespace bigscale
{
namespace
{
constexpr NVSDK_NGX_Result kMissing = NVSDK_NGX_Result_FAIL_InvalidParameter;
}

NgxParameterMap::NgxParameterMap() { Reset(); }

void NgxParameterMap::Reset()
{
    count_ = 0;
    std::memset(entries_, 0, sizeof(entries_));
}

NgxParameterMap::Entry* NgxParameterMap::Find(const char* name)
{
    if (name == nullptr) return nullptr;
    for (size_t i = 0; i < count_; i++)
        if (std::strncmp(entries_[i].name, name, kNameCapacity) == 0) return &entries_[i];
    return nullptr;
}

const NgxParameterMap::Entry* NgxParameterMap::Find(const char* name) const
{
    return const_cast<NgxParameterMap*>(this)->Find(name);
}

void NgxParameterMap::Store(const char* name, Type type, const void* value, size_t size)
{
    if (name == nullptr || value == nullptr) return;
    Entry* entry = Find(name);
    if (entry == nullptr)
    {
        if (count_ >= kCapacity) return;
        entry = &entries_[count_++];
        std::strncpy(entry->name, name, kNameCapacity - 1);
        entry->name[kNameCapacity - 1] = 0;
    }
    entry->type = type;
    std::memcpy(&entry->value, value, size);
}

void NgxParameterMap::SetInt(const char* name, int value) { Store(name, Type::Integer, &value, sizeof(value)); }
void NgxParameterMap::SetUnsigned(const char* name, unsigned value)
{
    Store(name, Type::Unsigned, &value, sizeof(value));
}
void NgxParameterMap::SetUnsigned64(const char* name, unsigned long long value)
{
    Store(name, Type::Unsigned64, &value, sizeof(value));
}
void NgxParameterMap::SetFloat(const char* name, float value) { Store(name, Type::Float, &value, sizeof(value)); }
void NgxParameterMap::SetPointer(const char* name, void* value) { Store(name, Type::Pointer, &value, sizeof(value)); }

void NgxParameterMap::Set(const char* InName, unsigned long long InValue)
{
    Store(InName, Type::Unsigned64, &InValue, sizeof(InValue));
}

void NgxParameterMap::Set(const char* InName, float InValue) { Store(InName, Type::Float, &InValue, sizeof(InValue)); }

void NgxParameterMap::Set(const char* InName, double InValue)
{
    Store(InName, Type::Double, &InValue, sizeof(InValue));
}

void NgxParameterMap::Set(const char* InName, unsigned int InValue)
{
    Store(InName, Type::Unsigned, &InValue, sizeof(InValue));
}

void NgxParameterMap::Set(const char* InName, int InValue) { Store(InName, Type::Integer, &InValue, sizeof(InValue)); }

void NgxParameterMap::Set(const char* InName, ID3D11Resource* InValue)
{
    void* pointer = InValue;
    Store(InName, Type::Pointer, &pointer, sizeof(pointer));
}

void NgxParameterMap::Set(const char* InName, ID3D12Resource* InValue)
{
    void* pointer = InValue;
    Store(InName, Type::Pointer, &pointer, sizeof(pointer));
}

void NgxParameterMap::Set(const char* InName, void* InValue)
{
    Store(InName, Type::Pointer, &InValue, sizeof(InValue));
}

NVSDK_NGX_Result NgxParameterMap::Get(const char* InName, unsigned long long* OutValue) const
{
    const Entry* entry = Find(InName);
    if (entry == nullptr || OutValue == nullptr) return kMissing;
    switch (entry->type)
    {
        case Type::Unsigned64: *OutValue = entry->value.unsigned64; return NVSDK_NGX_Result_Success;
        case Type::Unsigned: *OutValue = entry->value.unsignedValue; return NVSDK_NGX_Result_Success;
        case Type::Integer:
            if (entry->value.integer < 0) return kMissing;
            *OutValue = static_cast<unsigned long long>(entry->value.integer);
            return NVSDK_NGX_Result_Success;
        default: return kMissing;
    }
}

NVSDK_NGX_Result NgxParameterMap::Get(const char* InName, float* OutValue) const
{
    const Entry* entry = Find(InName);
    if (entry == nullptr || OutValue == nullptr) return kMissing;
    switch (entry->type)
    {
        case Type::Float: *OutValue = entry->value.floatValue; return NVSDK_NGX_Result_Success;
        case Type::Double: *OutValue = static_cast<float>(entry->value.doubleValue); return NVSDK_NGX_Result_Success;
        default: return kMissing;
    }
}

NVSDK_NGX_Result NgxParameterMap::Get(const char* InName, double* OutValue) const
{
    const Entry* entry = Find(InName);
    if (entry == nullptr || OutValue == nullptr) return kMissing;
    switch (entry->type)
    {
        case Type::Double: *OutValue = entry->value.doubleValue; return NVSDK_NGX_Result_Success;
        case Type::Float: *OutValue = entry->value.floatValue; return NVSDK_NGX_Result_Success;
        default: return kMissing;
    }
}

NVSDK_NGX_Result NgxParameterMap::Get(const char* InName, unsigned int* OutValue) const
{
    const Entry* entry = Find(InName);
    if (entry == nullptr || OutValue == nullptr) return kMissing;
    switch (entry->type)
    {
        case Type::Unsigned: *OutValue = entry->value.unsignedValue; return NVSDK_NGX_Result_Success;
        case Type::Integer:
            if (entry->value.integer < 0) return kMissing;
            *OutValue = static_cast<unsigned int>(entry->value.integer);
            return NVSDK_NGX_Result_Success;
        case Type::Unsigned64:
            if (entry->value.unsigned64 > 0xffffffffull) return kMissing;
            *OutValue = static_cast<unsigned int>(entry->value.unsigned64);
            return NVSDK_NGX_Result_Success;
        default: return kMissing;
    }
}

NVSDK_NGX_Result NgxParameterMap::Get(const char* InName, int* OutValue) const
{
    const Entry* entry = Find(InName);
    if (entry == nullptr || OutValue == nullptr) return kMissing;
    switch (entry->type)
    {
        case Type::Integer: *OutValue = entry->value.integer; return NVSDK_NGX_Result_Success;
        case Type::Unsigned:
            if (entry->value.unsignedValue > 0x7fffffffu) return kMissing;
            *OutValue = static_cast<int>(entry->value.unsignedValue);
            return NVSDK_NGX_Result_Success;
        case Type::Unsigned64:
            if (entry->value.unsigned64 > 0x7fffffffull) return kMissing;
            *OutValue = static_cast<int>(entry->value.unsigned64);
            return NVSDK_NGX_Result_Success;
        default: return kMissing;
    }
}

NVSDK_NGX_Result NgxParameterMap::Get(const char* InName, ID3D11Resource** OutValue) const
{
    const Entry* entry = Find(InName);
    if (entry == nullptr || OutValue == nullptr || entry->type != Type::Pointer) return kMissing;
    *OutValue = static_cast<ID3D11Resource*>(entry->value.pointer);
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NgxParameterMap::Get(const char* InName, ID3D12Resource** OutValue) const
{
    const Entry* entry = Find(InName);
    if (entry == nullptr || OutValue == nullptr || entry->type != Type::Pointer) return kMissing;
    *OutValue = static_cast<ID3D12Resource*>(entry->value.pointer);
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NgxParameterMap::Get(const char* InName, void** OutValue) const
{
    const Entry* entry = Find(InName);
    if (entry == nullptr || OutValue == nullptr) return kMissing;
    if (entry->type != Type::Pointer) return kMissing;
    *OutValue = entry->value.pointer;
    return NVSDK_NGX_Result_Success;
}
}
