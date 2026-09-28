// Actual production model functions are extracted by run_nr_vulkan_model.ps1.
// Dependencies below count backend calls; no GPU or NVIDIA runtime is loaded.
#include <cassert>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#define LOG_INFO(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)
#define LOG_ERROR(...) ((void) 0)
using VkCommandBuffer = void*;
using VkDevice = void*;
using VkInstance = void*;
using VkPhysicalDevice = void*;
using NVSDK_NGX_Result = unsigned;
using NVSDK_NGX_Feature = unsigned;
constexpr unsigned NVSDK_NGX_Result_Success = 1, NVSDK_NGX_Result_Fail = 0xBAD00000;
#define NVSDK_NGX_FAILED(r) (((r) & 0xFFF00000u) == NVSDK_NGX_Result_Fail)
struct NVSDK_NGX_Handle
{
    unsigned Id;
};
struct NVSDK_NGX_Resource_VK
{
    int image;
};
struct NVSDK_NGX_Parameter
{
    std::map<std::string, std::variant<unsigned, float, void*>> values;
    template <class T> void Set(const char* key, T value) { values[key] = value; }
};
struct Config
{
};
namespace DlssNr
{
constexpr unsigned MaxPassCount = 3;
struct ModelSettings
{
    unsigned preset = 4;
};
namespace Profiles
{
ModelSettings PassSettings(const Config&, unsigned) { return {}; }
} // namespace Profiles
struct GuideRegions
{
};
struct Size
{
    unsigned width, height;
};
void SetModelTuning(NVSDK_NGX_Parameter* p, const ModelSettings&) { p->Set("tuning", 1u); }
void SetModelRegions(NVSDK_NGX_Parameter* p, Size s, const GuideRegions&) { p->Set("guideWidth", s.width); }
} // namespace DlssNr
namespace Mock
{
unsigned driverCreates, directCreates, opens, allocated, destroyed, driverEvals, directEvals;
unsigned driverResult, directResult;
bool available, driverHandle, directHandle;
NVSDK_NGX_Handle driverHandles[] = { { 1 }, { 2 }, { 3 } }, directHandles[] = { { 1 }, { 2 }, { 3 } };
NVSDK_NGX_Handle* forcedDriver = nullptr;
NVSDK_NGX_Handle* forcedDirect = nullptr;
std::map<NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*> driverLive, directLive;
std::vector<NVSDK_NGX_Parameter*> maps;
unsigned Allocate(NVSDK_NGX_Parameter** p)
{
    ++allocated;
    *p = new NVSDK_NGX_Parameter;
    maps.push_back(*p);
    return 1;
}
unsigned Destroy(NVSDK_NGX_Parameter* p)
{
    for (const auto& [handle, params] : driverLive)
        assert(params != p);
    for (const auto& [handle, params] : directLive)
        assert(params != p);
    ++destroyed;
    delete p;
    return 1;
}
unsigned DriverCreate(VkDevice, VkCommandBuffer, unsigned feature, NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** out)
{
    assert(feature == 18);
    *out = driverHandle ? (forcedDriver ? forcedDriver : &driverHandles[driverCreates % 3]) : nullptr;
    ++driverCreates;
    if (*out)
        driverLive.emplace(*out, p);
    return driverResult;
}
unsigned DriverRelease(NVSDK_NGX_Handle* p)
{
    assert(driverLive.erase(p) == 1);
    return 1;
}
unsigned DriverEvaluate(VkCommandBuffer, NVSDK_NGX_Handle* p, NVSDK_NGX_Parameter* params, void*)
{
    assert(driverLive.at(p) == params);
    ++driverEvals;
    return 1;
}
void Reset()
{
    assert(driverLive.empty() && directLive.empty());
    driverCreates = directCreates = opens = allocated = destroyed = driverEvals = directEvals = 0;
    driverResult = directResult = 1;
    available = driverHandle = directHandle = true;
    forcedDriver = forcedDirect = nullptr;
    maps.clear();
}
} // namespace Mock
struct NVNGXProxy
{
    static auto VULKAN_GetCapabilityParameters() { return &Mock::Allocate; }
    static auto VULKAN_DestroyParameters() { return &Mock::Destroy; }
    static auto VULKAN_CreateFeature1() { return &Mock::DriverCreate; }
    static auto VULKAN_EvaluateFeature() { return &Mock::DriverEvaluate; }
    static auto VULKAN_ReleaseFeature() { return &Mock::DriverRelease; }
};
namespace DlssNr
{
struct CompatibilityRuntime
{
    static std::shared_ptr<CompatibilityRuntime> TryOpenVulkan(VkInstance, VkPhysicalDevice, VkDevice)
    {
        ++Mock::opens;
        return Mock::available ? std::make_shared<CompatibilityRuntime>() : nullptr;
    }
    unsigned Create(VkCommandBuffer, NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** out)
    {
        *out = Mock::directHandle
                   ? (Mock::forcedDirect ? Mock::forcedDirect : &Mock::directHandles[Mock::directCreates % 3])
                   : nullptr;
        ++Mock::directCreates;
        if (*out)
            Mock::directLive.emplace(*out, p);
        return Mock::directResult;
    }
    unsigned Evaluate(VkCommandBuffer, NVSDK_NGX_Handle* p, NVSDK_NGX_Parameter* params)
    {
        assert(Mock::directLive.at(p) == params);
        ++Mock::directEvals;
        return 1;
    }
    unsigned Release(NVSDK_NGX_Handle* p)
    {
        assert(Mock::directLive.erase(p) == 1);
        return 1;
    }
};
struct Pass
{
    NVSDK_NGX_Handle* feature = nullptr;
    NVSDK_NGX_Parameter* parameters = nullptr;
    std::shared_ptr<CompatibilityRuntime> compatibility;
};
struct ModelVk
{
    struct Impl
    {
        struct State
        {
            bool failed = false, reset = true;
            const char* reason = "";
            VkDevice device = nullptr;
            VkInstance instance = nullptr;
            VkPhysicalDevice physicalDevice = nullptr;
            Pass models[MaxPassCount];
            std::shared_ptr<CompatibilityRuntime> compatibility;
        } state;
        void Fail(const char*);
        void ReleaseModels();
        bool CreateModel(VkCommandBuffer, unsigned, unsigned, unsigned, const Config&);
        unsigned EvaluateModel(VkCommandBuffer, unsigned, NVSDK_NGX_Resource_VK*, NVSDK_NGX_Resource_VK*,
                               NVSDK_NGX_Resource_VK*, NVSDK_NGX_Resource_VK*, unsigned, unsigned, const GuideRegions&,
                               bool, float, float, const Config&);
        ~Impl() { ReleaseModels(); }
    };
};
#include "vulkan_model_production.inc"
} // namespace DlssNr
int main()
{
    using DlssNr::ModelVk;
    Config config;
    Mock::Reset();
    {
        ModelVk::Impl model;
        assert(model.CreateModel(nullptr, 0, 1280, 720, config));
        assert(Mock::opens == 0 && Mock::directCreates == 0);
        NVSDK_NGX_Resource_VK color { 1 }, depth { 2 }, mv { 3 }, output { 4 };
        model.EvaluateModel(nullptr, 0, &color, &depth, &mv, &output, 1280, 720, {}, true, -1.f, 1.f, config);
        assert(Mock::driverEvals == 1 && Mock::directEvals == 0);
    }
    assert(Mock::allocated == Mock::destroyed);
    Mock::Reset();
    {
        ModelVk::Impl model;
        Mock::driverResult = NVSDK_NGX_Result_Fail | 11;
        Mock::driverHandle = false;
        for (unsigned pass = 0; pass < 3; ++pass)
            assert(model.CreateModel(nullptr, pass, 1280, 720, config));
        assert(Mock::driverCreates == 1 && Mock::opens == 1 && Mock::directCreates == 3);
        assert(Mock::maps[0] != Mock::maps[1] && Mock::maps[1] != Mock::maps[2]);
        NVSDK_NGX_Resource_VK color { 1 }, depth { 2 }, mv { 3 }, output { 4 };
        for (unsigned pass = 0; pass < 3; ++pass)
        {
            model.EvaluateModel(nullptr, pass, &color, &depth, &mv, &output, 1280, 720, {}, true, -1.f, 1.f, config);
            const auto& values = Mock::maps[pass]->values;
            assert(std::get<unsigned>(values.at("DLSSNR.Width")) == 1280);
            assert(std::get<void*>(values.at("DLSSNR.Color")) == &color);
            assert(std::get<void*>(values.at("DLSSNR.Depth")) == &depth);
            assert(std::get<void*>(values.at("DLSSNR.MVec")) == &mv);
            assert(std::get<float>(values.at("DLSSNR.MVecScaleX")) == -1.f);
            assert(std::get<unsigned>(values.at("DLSSNR.Reset")) == 1);
        }
        assert(Mock::directEvals == 3 && Mock::driverEvals == 0);
        model.ReleaseModels();
        assert(model.state.compatibility && Mock::directLive.empty());
        assert(model.CreateModel(nullptr, 0, 960, 540, config));
        assert(Mock::opens == 1); // retain initialized runtime through a fenced resize
    }
    assert(Mock::allocated == Mock::destroyed);
    for (bool returnsHandle : { false, true })
    {
        Mock::Reset();
        ModelVk::Impl model;
        Mock::available = false;
        Mock::driverResult = NVSDK_NGX_Result_Fail | 3;
        Mock::driverHandle = returnsHandle;
        assert(!model.CreateModel(nullptr, 0, 1280, 720, config));
        assert(Mock::opens == (returnsHandle ? 0u : 1u));
    }
    Mock::Reset();
    {
        ModelVk::Impl model;
        Mock::driverHandle = false; // success with no handle is not a valid feature
        assert(!model.CreateModel(nullptr, 0, 1280, 720, config));
        assert(Mock::opens == 0);
    }
    Mock::Reset();
    {
        ModelVk::Impl model;
        Mock::driverHandle = false;
        Mock::driverResult = NVSDK_NGX_Result_Fail;
        Mock::directResult = NVSDK_NGX_Result_Fail;
        assert(!model.CreateModel(nullptr, 0, 1280, 720, config));
        assert(model.state.models[0].compatibility); // failed direct call returned an owned handle
    }
    Mock::Reset();
    {
        ModelVk::Impl model;
        Mock::driverHandle = false;
        Mock::driverResult = NVSDK_NGX_Result_Fail;
        Mock::forcedDirect = &Mock::directHandles[0];
        assert(model.CreateModel(nullptr, 0, 1280, 720, config));
        assert(!model.CreateModel(nullptr, 1, 1280, 720, config));
        assert(!model.state.models[1].feature); // duplicate pointer must not be double-released
    }
    Mock::Reset();
    {
        ModelVk::Impl model;
        assert(model.CreateModel(nullptr, 0, 1280, 720, config));
        Mock::driverHandle = false;
        Mock::driverResult = NVSDK_NGX_Result_Fail;
        assert(model.CreateModel(nullptr, 1, 1280, 720, config));
        assert(model.state.models[0].feature->Id == model.state.models[1].feature->Id);
        assert(model.state.models[0].feature != model.state.models[1].feature);
        // Driver and direct numeric IDs are distinct namespaces; teardown uses each owner.
    }
    assert(Mock::driverLive.empty() && Mock::directLive.empty() && Mock::allocated == Mock::destroyed);
    std::puts(
        "PASS: Vulkan driver/direct routing, independent passes, rebuild, failure ownership and parameter handoff");
}
