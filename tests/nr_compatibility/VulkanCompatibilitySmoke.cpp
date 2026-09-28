#include <windows.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <variant>

#include <vulkan/vulkan.h>
#include <nvsdk_ngx_vk.h>

#include "../../OptiScaler/dlssnr/DlssNr_CompatibilityRuntime.h"

namespace
{
struct Parameters final : NVSDK_NGX_Parameter
{
    using Value = std::variant<unsigned long long, float, double, unsigned, int, void*>;
    std::map<std::string, Value> values;

    void Set(const char* name, unsigned long long value) override { values[name] = value; }
    void Set(const char* name, float value) override { values[name] = value; }
    void Set(const char* name, double value) override { values[name] = value; }
    void Set(const char* name, unsigned value) override { values[name] = value; }
    void Set(const char* name, int value) override { values[name] = value; }
    void Set(const char* name, ID3D11Resource* value) override { values[name] = value; }
    void Set(const char* name, ID3D12Resource* value) override { values[name] = value; }
    void Set(const char* name, void* value) override { values[name] = value; }

    NVSDK_NGX_Result Get(const char* name, unsigned long long* out) const override { return GetValue(name, out); }
    NVSDK_NGX_Result Get(const char* name, float* out) const override { return GetValue(name, out); }
    NVSDK_NGX_Result Get(const char* name, double* out) const override { return GetValue(name, out); }
    NVSDK_NGX_Result Get(const char* name, unsigned* out) const override { return GetValue(name, out); }
    NVSDK_NGX_Result Get(const char* name, int* out) const override { return GetValue(name, out); }
    NVSDK_NGX_Result Get(const char* name, ID3D11Resource** out) const override { return GetValue(name, out); }
    NVSDK_NGX_Result Get(const char* name, ID3D12Resource** out) const override { return GetValue(name, out); }
    NVSDK_NGX_Result Get(const char* name, void** out) const override { return GetValue(name, out); }
    void Reset() override { values.clear(); }

  private:
    template <typename T> NVSDK_NGX_Result GetValue(const char* name, T* out) const
    {
        const auto it = values.find(name);
        if (it == values.end())
            return NVSDK_NGX_Result_FAIL_FeatureNotFound;
        if constexpr (std::is_pointer_v<T>)
        {
            if (const auto* value = std::get_if<void*>(&it->second))
            {
                *out = reinterpret_cast<T>(*value);
                return NVSDK_NGX_Result_Success;
            }
        }
        else if (const auto* value = std::get_if<T>(&it->second))
        {
            *out = *value;
            return NVSDK_NGX_Result_Success;
        }
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
};

unsigned g_allocateCalls = 0;
unsigned g_destroyCalls = 0;

NVSDK_NGX_Result Allocate(NVSDK_NGX_Parameter** out)
{
    ++g_allocateCalls;
    *out = new Parameters;
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result Destroy(NVSDK_NGX_Parameter* parameters)
{
    ++g_destroyCalls;
    delete static_cast<Parameters*>(parameters);
    return NVSDK_NGX_Result_Success;
}

struct FakeStats
{
    unsigned init;
    unsigned create;
    unsigned evaluate;
    unsigned release;
    unsigned shutdown;
    unsigned long long applicationId;
    unsigned version;
    VkInstance instance;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    const NVSDK_NGX_FeatureCommonInfo* featureInfo;
    VkCommandBuffer commandBuffer;
    NVSDK_NGX_Parameter* initParameters;
    NVSDK_NGX_Parameter* createParameters;
    NVSDK_NGX_Parameter* evaluateParameters;
    NVSDK_NGX_Handle* feature;
    NVSDK_NGX_Feature featureId;
};

using GetStats = FakeStats (*)();
[[noreturn]] void Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

void Expect(bool value, const char* message)
{
    if (!value)
        Fail(message);
}

template <typename T> T Proc(HMODULE module, const char* name)
{
    auto address = GetProcAddress(module, name);
    Expect(address != nullptr, name);
    return reinterpret_cast<T>(address);
}

void ExpectNoModule(const wchar_t* name, const char* message) { Expect(GetModuleHandleW(name) == nullptr, message); }
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 5)
        Fail("usage: VulkanCompatibilitySmoke.exe <complete DLL> <missing-evaluate DLL> <missing-shutdown DLL> "
             "<alternate DLL>");

    const std::filesystem::path completePath = argv[1];
    const std::filesystem::path missingEvaluatePath = argv[2];
    const std::filesystem::path missingShutdownPath = argv[3];
    const std::filesystem::path alternatePath = argv[4];
    Expect(!GetModuleHandleW(L"nvngx_dlssnr.dll"), "complete fixture unexpectedly preloaded");

    const auto instance = reinterpret_cast<VkInstance>(static_cast<uintptr_t>(0x1100));
    const auto physicalDevice = reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(0x2200));
    const auto device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(0x3300));
    const auto commandBuffer = reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(0x4400));
    const std::filesystem::path dataPath = std::filesystem::temp_directory_path() / L"nr-vulkan-compatibility-data";

    g_allocateCalls = g_destroyCalls = 0;
    Parameters parameters;
    std::shared_ptr<DlssNr::CompatibilityRuntime> first;
    {
        first = DlssNr::CompatibilityRuntime::OpenVulkan(completePath, instance, physicalDevice, device, &Allocate,
                                                         &Destroy, dataPath);
        Expect(first != nullptr, "OpenVulkan rejected the complete fake runtime");

        const auto module = GetModuleHandleW(L"nvngx_dlssnr.dll");
        Expect(module != nullptr, "complete fake runtime was not loaded");
        auto getStats = Proc<GetStats>(module, "FakeGetStats");

        auto stats = getStats();
        // The compatibility DLL's private forwarder ABI places feature-info before
        // the SDK version; this is intentionally different from the public SDK header.
        Expect(stats.init == 1, "Vulkan Init_Ext was not called exactly once");
        Expect(stats.applicationId == 0ull, "Vulkan Init_Ext application id was changed");
        Expect(stats.version == static_cast<unsigned>(NVSDK_NGX_Version_API),
               "Vulkan Init_Ext SDK version argument is in the wrong ABI slot");
        Expect(stats.featureInfo == nullptr, "Vulkan Init_Ext feature-info argument was not null");
        Expect(stats.instance == instance && stats.physicalDevice == physicalDevice && stats.device == device,
               "Vulkan Init_Ext device handles were not preserved");

        // Re-open a second owner and verify that same-device ownership is reused without Init.
        auto second = DlssNr::CompatibilityRuntime::OpenVulkan(completePath, instance, physicalDevice, device,
                                                               &Allocate, &Destroy, dataPath);
        Expect(second == first, "same Vulkan device did not reuse its runtime owner");
        stats = getStats();
        Expect(stats.init == 1, "same-device owner unexpectedly reinitialized the fake runtime");

        NVSDK_NGX_Handle* feature = nullptr;
        Expect(first->Create(commandBuffer, &parameters, &feature) == NVSDK_NGX_Result_Success,
               "Vulkan Create did not return success");
        Expect(feature != nullptr, "Vulkan Create returned no feature handle");
        stats = getStats();
        Expect(stats.create == 1 && stats.createParameters == &parameters, "Create ABI arguments were not preserved");
        Expect(stats.commandBuffer == commandBuffer && stats.featureId == static_cast<NVSDK_NGX_Feature>(18),
               "Create did not receive the expected command buffer/feature id");

        Expect(first->Evaluate(commandBuffer, feature, &parameters) == NVSDK_NGX_Result_Success,
               "Vulkan Evaluate did not return success");
        stats = getStats();
        Expect(stats.evaluate == 1 && stats.evaluateParameters == &parameters,
               "Evaluate ABI arguments were not preserved");

        Expect(first->Release(feature) == NVSDK_NGX_Result_Success, "Vulkan Release did not return success");
        stats = getStats();
        Expect(stats.release == 1 && stats.feature == feature, "Release ABI argument was not preserved");
        second.reset();
        first.reset();
    }

    Expect(g_allocateCalls == 1 && g_destroyCalls == 1, "runtime capability parameter ownership was not balanced");
    ExpectNoModule(L"nvngx_dlssnr.dll", "owned Vulkan runtime was not unloaded after its last owner retired");

    const auto deviceA = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(0x3300));
    const auto deviceB = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(0x5500));
    g_allocateCalls = g_destroyCalls = 0;
    {
        auto ownerA = DlssNr::CompatibilityRuntime::OpenVulkan(completePath, instance, physicalDevice, deviceA,
                                                               &Allocate, &Destroy, dataPath);
        Expect(ownerA != nullptr, "device A could not open the complete fake runtime");
        const auto module = GetModuleHandleW(L"nvngx_dlssnr.dll");
        Expect(module != nullptr, "multi-owner fake runtime was not loaded");
        auto getStats = Proc<GetStats>(module, "FakeGetStats");

        // A different candidate path must not return the cached owner for device A.
        auto wrongPath = DlssNr::CompatibilityRuntime::OpenVulkan(alternatePath, instance, physicalDevice, deviceA,
                                                                  &Allocate, &Destroy, dataPath);
        Expect(!wrongPath, "same-device OpenVulkan returned an owner for a different candidate path");

        auto ownerB = DlssNr::CompatibilityRuntime::OpenVulkan(completePath, instance, physicalDevice, deviceB,
                                                               &Allocate, &Destroy, dataPath);
        Expect(ownerB != nullptr && ownerB != ownerA, "second Vulkan device did not receive an independent owner");
        auto stats = getStats();
        Expect(stats.init == 2, "second Vulkan device did not initialize the shared module");

        ownerB.reset();
        auto reopenedB = DlssNr::CompatibilityRuntime::OpenVulkan(completePath, instance, physicalDevice, deviceB,
                                                                  &Allocate, &Destroy, dataPath);
        Expect(reopenedB != nullptr && reopenedB != ownerA,
               "retired Vulkan device could not reacquire an owner while device A remained live");
        stats = getStats();
        Expect(stats.init == 3, "reopened Vulkan device did not reinitialize the shared module");
        reopenedB.reset();
        ownerA.reset();
    }
    Expect(g_allocateCalls == 3 && g_destroyCalls == 3, "multi-device capability ownership was not balanced");
    ExpectNoModule(L"nvngx_dlssnr.dll", "shared Vulkan runtime was not unloaded after all owners retired");

    // Missing exports must fail before a usable owner escapes and must unload each
    // rejected DLL, so a later valid OpenVulkan is not blocked by module state.
    auto rejectedEvaluate = DlssNr::CompatibilityRuntime::OpenVulkan(missingEvaluatePath, instance, physicalDevice,
                                                                     device, &Allocate, &Destroy, dataPath);
    Expect(!rejectedEvaluate, "runtime with missing Vulkan EvaluateFeature export was accepted");
    ExpectNoModule(L"missing_eval_nvngx_dlssnr.dll", "missing-evaluate Vulkan runtime remained loaded");

    auto rejectedShutdown = DlssNr::CompatibilityRuntime::OpenVulkan(missingShutdownPath, instance, physicalDevice,
                                                                     device, &Allocate, &Destroy, dataPath);
    Expect(!rejectedShutdown, "runtime with missing Vulkan Shutdown1 export was accepted");
    ExpectNoModule(L"missing_shutdown_nvngx_dlssnr.dll", "missing-shutdown Vulkan runtime remained loaded");

    auto reopened = DlssNr::CompatibilityRuntime::OpenVulkan(completePath, instance, physicalDevice, device, &Allocate,
                                                             &Destroy, dataPath);
    Expect(reopened != nullptr, "valid Vulkan runtime could not reopen after rejected module");
    const auto module = GetModuleHandleW(L"nvngx_dlssnr.dll");
    Expect(module != nullptr, "valid Vulkan runtime was not loaded after rejection");
    auto getStats = Proc<GetStats>(module, "FakeGetStats");
    Expect(getStats().init == 1, "reopened runtime did not execute its Vulkan init ABI");
    reopened.reset();
    ExpectNoModule(L"nvngx_dlssnr.dll", "reopened Vulkan runtime was not unloaded");

    std::puts("PASS: fake Vulkan NGX ABI/calls, owner reuse/reopen, path isolation, rejection cleanup, and unload");
    return 0;
}
