// A host NVSDK_NGX_Parameter map for providers that take the interface but do not own one.
//
// NVIDIA's static loader hands the application an SDK-owned map. ANR is loaded as a plain provider
// DLL and calls the application's map instead, so the host must implement the exact pure-virtual
// interface. Entries keep their declared type: an integer is never round-tripped through double, and
// an absent key reports failure rather than zero.
#pragma once

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params.h>

#include <cstddef>
#include <cstdint>

namespace bigscale
{
class NgxParameterMap final : public NVSDK_NGX_Parameter
{
public:
    static constexpr size_t kCapacity = 64;
    static constexpr size_t kNameCapacity = 96;

    enum class Type : int32_t
    {
        None = 0,
        Integer,
        Unsigned,
        Unsigned64,
        Float,
        Double,
        Pointer,
    };

    NgxParameterMap();

    // Typed writers used once per initialization; they never allocate.
    void SetInt(const char* name, int value);
    void SetUnsigned(const char* name, unsigned value);
    void SetUnsigned64(const char* name, unsigned long long value);
    void SetFloat(const char* name, float value);
    void SetPointer(const char* name, void* value);

    void Set(const char* InName, unsigned long long InValue) override;
    void Set(const char* InName, float InValue) override;
    void Set(const char* InName, double InValue) override;
    void Set(const char* InName, unsigned int InValue) override;
    void Set(const char* InName, int InValue) override;
    void Set(const char* InName, ID3D11Resource* InValue) override;
    void Set(const char* InName, ID3D12Resource* InValue) override;
    void Set(const char* InName, void* InValue) override;

    NVSDK_NGX_Result Get(const char* InName, unsigned long long* OutValue) const override;
    NVSDK_NGX_Result Get(const char* InName, float* OutValue) const override;
    NVSDK_NGX_Result Get(const char* InName, double* OutValue) const override;
    NVSDK_NGX_Result Get(const char* InName, unsigned int* OutValue) const override;
    NVSDK_NGX_Result Get(const char* InName, int* OutValue) const override;
    NVSDK_NGX_Result Get(const char* InName, ID3D11Resource** OutValue) const override;
    NVSDK_NGX_Result Get(const char* InName, ID3D12Resource** OutValue) const override;
    NVSDK_NGX_Result Get(const char* InName, void** OutValue) const override;

    void Reset() override;

private:
    struct Entry
    {
        char name[kNameCapacity];
        Type type = Type::None;
        union
        {
            int integer;
            unsigned unsignedValue;
            unsigned long long unsigned64;
            float floatValue;
            double doubleValue;
            void* pointer;
        } value{};
    };

    Entry* Find(const char* name);
    const Entry* Find(const char* name) const;
    void Store(const char* name, Type type, const void* value, size_t size);

    Entry entries_[kCapacity];
    size_t count_ = 0;
};
}
