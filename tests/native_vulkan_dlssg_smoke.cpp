// Production Streamline hook bodies are extracted by run_native_vulkan_dlssg.ps1.
// This harness supplies only application/plugin services; it never loads Streamline, NGX, or a game.
#include <windows.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <future>
#include <mutex>
#include <thread>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <json.hpp>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_dlss.h>
#include <sl_pcl.h>
#include <vulkan/vulkan.h>

#define LOG_WARN(...) ((void) 0)
#define LOG_INFO(...) ((void) 0)
#define LOG_DEBUG(...) ((void) 0)
#define LOG_TRACE(...) ((void) 0)
#define LOG_FUNC() ((void) 0)
#include "../OptiScaler/OptiTypes.h"

// Use the production class declaration and its real Streamline ABI types. Private
// members are made test-visible so the original function pointers can be installed.
#define private public
#include "../OptiScaler/hooks/Streamline_Hooks.h"
#undef private

enum class FGInput : uint32_t
{
    NoFG,
    Upscaler,
    DLSSG,
    NvngxFG,
};
enum class FGOutput : uint32_t
{
    NoFG,
    FSRFG,
    DLSSG,
    XeFG,
};
enum class FGNvngxReplacement : uint32_t
{
    None,
    Nukems,
};
enum class ForceReflex : uint32_t
{
    InGame,
    ForceDisable,
    ForceEnable,
};
enum class GameQuirk : uint32_t
{
    ForceUnrealEngine,
    FixSlSimulationMarkers,
};

struct GameQuirks
{
    uint32_t bits = 0;
    GameQuirks& operator|=(GameQuirk value)
    {
        bits |= 1u << static_cast<uint32_t>(value);
        return *this;
    }
    bool operator&(GameQuirk value) const { return (bits & (1u << static_cast<uint32_t>(value))) != 0; }
};

struct FakeSlFgInputs
{
    unsigned engineReports = 0;
    unsigned constantsReports = 0;
    unsigned resourceReports = 0;
    unsigned evaluateStateCalls = 0;
    unsigned markPresentCalls = 0;
    void reportEngineType(sl::EngineType) { ++engineReports; }
    void setConstants(const sl::Constants&, uint32_t) { ++constantsReports; }
    void reportResource(const sl::ResourceTag&, ID3D12GraphicsCommandList*, uint32_t) { ++resourceReports; }
    void evaluateState() { ++evaluateStateCalls; }
    void markPresent(const sl::FrameToken&) { ++markPresentCalls; }
};

struct FakeSl1FgInputs
{
    unsigned evaluateStateCalls = 0;
    unsigned markPresentCalls = 0;
    void evaluateState() { ++evaluateStateCalls; }
    void markPresent(const sl::FrameToken&) { ++markPresentCalls; }
};

struct FakeCurrentFg
{
    bool IsActive() const { return false; }
    bool IsPaused() const { return false; }
    uint32_t GetInterpolatedFrameCount() const { return 0; }
};

struct State
{
    bool isShuttingDown = false;
    FGInput activeFgInput = FGInput::NoFG;
    FGOutput activeFgOutput = FGOutput::NoFG;
    FGNvngxReplacement activeFgNvngx = FGNvngxReplacement::None;
    bool nativeVulkanDlssg = false;
    bool nativeVulkanDlssgOptionsSeen = false;
    bool nativeVulkanDlssgRequested = false;
    std::optional<unsigned int> nativeVulkanDlssgLastResult;
    API swapchainApi = API::Vulkan;
    bool menuOverlayIsVulkan = false;
    uint32_t delayMenuRenderBy = 0;
    FakeCurrentFg* currentFG = nullptr;
    GameQuirks gameQuirks;
    FakeSlFgInputs slFGInputs;
    FakeSl1FgInputs s_sl1FGInputs;
    feature_version streamlineVersion { 2, 8, 0 };
    bool dlssgGameDMFGSupported = false;
    std::optional<int> dlssgMfgMax;
    sl::DLSSGMode dlssgLastSetMode = sl::DLSSGMode::eOff;
    int dlssgDetectedInterpolationCount = 0;

    static State& Instance()
    {
        static State state;
        return state;
    }
};

template <typename T> struct Setting
{
    std::optional<T> stored;
    T defaultValue {};
    T value_or_default() const { return stored.value_or(defaultValue); }
    bool has_value() const { return stored.has_value(); }
    const T& value() const { return stored.value(); }
    void set_volatile_value(const T& next) { stored = next; }
    void reset() { stored.reset(); }
};

struct Config
{
    std::optional<std::wstring> MainDllPath;
    Setting<bool> StreamlineSpoofing { {}, false };
    Setting<bool> FGDLSSGOverrideForceDMFG { {}, false };
    Setting<unsigned int> FGDLSSGFramerateTargetDMFG { {}, 0 };
    Setting<unsigned int> FGDLSSGOverrideInterpolationCount { {}, 0 };
    Setting<ForceReflex> FN_ForceReflex { {}, ForceReflex::InGame };

    static Config* Instance()
    {
        static Config config;
        if (!config.MainDllPath)
            config.MainDllPath = std::filesystem::temp_directory_path().wstring();
        return &config;
    }
};

namespace Util
{
std::filesystem::path ExePath() { return std::filesystem::temp_directory_path(); }
std::wstring ToLower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return std::towlower(c); });
    return value;
}
bool GetFileVersion(const std::wstring&, version_t*, version_t*) { return false; }
} // namespace Util

enum class ImGuiToastType
{
    Warning,
};
struct ImGuiToast
{
    ImGuiToastType type;
    unsigned duration;
    const char* message;
};
namespace ImGui
{
void InsertNotification(ImGuiToast) {}
} // namespace ImGui

class MenuOverlayBase
{
  public:
    static bool visible;
    static bool IsVisible() { return visible; }
};
bool MenuOverlayBase::visible = false;

class ReflexHooks
{
  public:
    static void setDlssgFrameCount(uint8_t count) { frameCount = count; }
    static inline uint8_t frameCount = 0;
};

struct GpuInformationForTest
{
    VendorId::Value vendorId = VendorId::Nvidia;
    bool dlssCapable = true;
};
class IdentifyGpu
{
  public:
    static GpuInformationForTest getPrimaryGpu() { return {}; }
};

namespace magic_enum
{
template <typename T> const char* enum_name(T) { return "test"; }
} // namespace magic_enum

void PatchSL1PluginJson(nlohmann::json&) {}

// Bodies extracted from production refer to these helpers. Their native branch
// should return before they can mutate test state; definitions keep the non-native
// branches linkable and make accidental fallthrough observable in the counters.
void StreamlineHooks::streamlineLogCallback(sl::LogType, const char*) {}
void StreamlineHooks::hookSystemCaps(sl::param::IParameters*) {}
uint32_t StreamlineHooks::getSystemCapsArch(SystemCaps*) { return 0; }
void StreamlineHooks::setArch(uint32_t, SystemCaps*) {}
void StreamlineHooks::spoofArch(uint32_t, sl::Feature, SystemCaps*) {}

static sl::Result dummy_slDLSSGGetState(const sl::ViewportHandle&, sl::DLSSGState&, const sl::DLSSGOptions*)
{
    return sl::Result::eOk;
}
static sl::Result dummy_slDLSSGSetOptions(const sl::ViewportHandle&, const sl::DLSSGOptions&)
{
    return sl::Result::eOk;
}

#include "native_vulkan_dlssg_production.inc"

namespace Mock
{
unsigned initCalls = 0;
unsigned originalCalls = 0;
unsigned constantsCalls = 0;
unsigned evaluateCalls = 0;
unsigned pluginLoadCalls = 0;
unsigned markerCalls = 0;
unsigned optionsCalls = 0;
unsigned stateCalls = 0;
unsigned featureFunctionCalls = 0;
sl::Preferences initPreferences {};
std::vector<sl::Feature> initFeatures;
std::vector<const wchar_t*> initPluginPaths;
sl::Constants const* lastConstants = nullptr;
sl::FrameToken const* lastFrame = nullptr;
sl::ViewportHandle const* lastViewport = nullptr;
void* returnedFunction = reinterpret_cast<void*>(static_cast<uintptr_t>(0x12345678));
sl::Result originalResult = sl::Result::eErrorInvalidState;
sl::PCLMarker lastMarker = sl::PCLMarker::eMaximum;
sl::FrameToken const* lastMarkerFrame = nullptr;
sl::DLSSGOptions lastOptions {};
std::array<unsigned char, sizeof(sl::DLSSGOptions)> lastOptionsBytes {};
sl::DLSSGState lastState {};
const char* originalJson = "{\"native\":true}";
bool pluginResult = false;

void Reset()
{
    initCalls = originalCalls = constantsCalls = evaluateCalls = pluginLoadCalls = optionsCalls = stateCalls =
        featureFunctionCalls = 0;
    initFeatures.clear();
    initPluginPaths.clear();
    lastConstants = nullptr;
    lastFrame = nullptr;
    lastViewport = nullptr;
    lastOptions = {};
    lastOptionsBytes.fill(0);
    lastState = {};
    originalResult = sl::Result::eErrorInvalidState;
    markerCalls = 0;
    lastMarker = sl::PCLMarker::eMaximum;
    lastMarkerFrame = nullptr;
    pluginResult = false;
}
} // namespace Mock

static sl::Result OriginalInit(const sl::Preferences& pref, uint64_t)
{
    ++Mock::initCalls;
    Mock::initPreferences = pref;
    Mock::initFeatures.assign(pref.featuresToLoad, pref.featuresToLoad + pref.numFeaturesToLoad);
    Mock::initPluginPaths.assign(pref.pathsToPlugins, pref.pathsToPlugins + pref.numPathsToPlugins);
    return Mock::originalResult;
}
static sl::Result OriginalSupported(sl::Feature, const sl::AdapterInfo&) { return Mock::originalResult; }
static sl::Result OriginalLoaded(sl::Feature, bool& loaded)
{
    loaded = false;
    return Mock::originalResult;
}
static sl::Result OriginalRequirements(sl::Feature, sl::FeatureRequirements& requirements)
{
    requirements = {};
    requirements.flags = sl::FeatureRequirementFlags::eVulkanSupported;
    return Mock::originalResult;
}
static sl::Result OriginalVersion(sl::Feature, sl::FeatureVersion& version)
{
    version.versionSL = { 9, 8, 7 };
    version.versionNGX = { 6, 5, 4 };
    return Mock::originalResult;
}
static sl::Result OriginalFunction(sl::Feature, const char* name, void*& function)
{
    ++Mock::featureFunctionCalls;
    if (name && *name)
        function = Mock::returnedFunction;
    else
        function = nullptr;
    return Mock::originalResult;
}
static sl::Result OriginalConstants(const sl::Constants& values, const sl::FrameToken& frame,
                                    const sl::ViewportHandle& viewport)
{
    ++Mock::constantsCalls;
    Mock::lastConstants = &values;
    Mock::lastFrame = &frame;
    Mock::lastViewport = &viewport;
    return Mock::originalResult;
}
static sl::Result OriginalEvaluate(sl::Feature, const sl::FrameToken& frame, const sl::BaseStructure**, uint32_t,
                                   sl::CommandBuffer*)
{
    ++Mock::evaluateCalls;
    Mock::lastFrame = &frame;
    return Mock::originalResult;
}
static sl::Result OriginalMarker(sl::PCLMarker marker, const sl::FrameToken& frame)
{
    ++Mock::markerCalls;
    Mock::lastMarker = marker;
    Mock::lastMarkerFrame = &frame;
    return Mock::originalResult;
}
static sl::Result OriginalOptions(const sl::ViewportHandle&, const sl::DLSSGOptions& options)
{
    ++Mock::optionsCalls;
    Mock::lastOptions = options;
    std::memcpy(Mock::lastOptionsBytes.data(), &options, std::min(Mock::lastOptionsBytes.size(), sizeof(options)));
    return Mock::originalResult;
}
static sl::Result OriginalState(const sl::ViewportHandle&, sl::DLSSGState& state, const sl::DLSSGOptions*)
{
    ++Mock::stateCalls;
    state = Mock::lastState;
    return Mock::originalResult;
}
static bool OriginalPluginLoad(sl::param::IParameters*, const char*, const char** json)
{
    ++Mock::pluginLoadCalls;
    *json = Mock::originalJson;
    return Mock::pluginResult;
}

struct TestFrame final : sl::FrameToken
{
    explicit TestFrame(uint32_t value) : value(value) {}
    operator uint32_t() const override { return value; }
    uint32_t value;
};

static void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

static size_t KnownOptionsBytes(uint32_t version)
{
    if (version == 1)
        return 104;
    if (version == 2 || version == 3)
        return 112;
    if (version == 4 || version == 5)
        return 120;
    return 0;
}

static void ResetState()
{
    State::Instance() = {};
    Config::Instance()->StreamlineSpoofing.reset();
    Config::Instance()->FGDLSSGOverrideForceDMFG.reset();
    Config::Instance()->FGDLSSGFramerateTargetDMFG.reset();
    Config::Instance()->FGDLSSGOverrideInterpolationCount.reset();
    MenuOverlayBase::visible = false;
    ReflexHooks::frameCount = 0;
    Mock::Reset();
    StreamlineHooks::o_slInit = &OriginalInit;
    StreamlineHooks::o_slIsFeatureSupported = &OriginalSupported;
    StreamlineHooks::o_slIsFeatureLoaded = &OriginalLoaded;
    StreamlineHooks::o_slGetFeatureRequirements = &OriginalRequirements;
    StreamlineHooks::o_slGetFeatureVersion = &OriginalVersion;
    StreamlineHooks::o_slGetFeatureFunction = &OriginalFunction;
    StreamlineHooks::o_slSetConstants = &OriginalConstants;
    StreamlineHooks::o_slEvaluateFeature = &OriginalEvaluate;
    StreamlineHooks::o_slPCLSetMarker = &OriginalMarker;
    StreamlineHooks::o_dlssg_slOnPluginLoad = &OriginalPluginLoad;
    StreamlineHooks::o_slDLSSGSetOptions = &OriginalOptions;
    StreamlineHooks::o_slDLSSGGetState = &OriginalState;
}

static void TestInitAndQueries()
{
    ResetState();
    State::Instance().activeFgInput = FGInput::DLSSG;
    State::Instance().activeFgOutput = FGOutput::DLSSG;
    sl::Feature features[] = { sl::kFeatureDLSS_G, sl::kFeatureDLSS };
    const wchar_t* paths[] = { L"game/plugins" };
    sl::Preferences preferences {};
    preferences.renderAPI = sl::RenderAPI::eVulkan;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = 2;
    preferences.pathsToPlugins = paths;
    preferences.numPathsToPlugins = 1;
    Mock::originalResult = sl::Result::eOk;
    Expect(StreamlineHooks::hkslInit(preferences, 77) == sl::Result::eOk, "native Vulkan slInit result changed");
    Expect(State::Instance().nativeVulkanDlssg && Mock::initCalls == 1, "native Vulkan mode was not recorded");
    Expect(Mock::initFeatures.size() == 2 && Mock::initFeatures[0] == sl::kFeatureDLSS_G,
           "native Vulkan slInit removed DLSS_G from the feature list");
    Expect(Mock::initPluginPaths.size() == 1 && std::wcscmp(Mock::initPluginPaths[0], paths[0]) == 0,
           "native Vulkan slInit changed plugin paths");

    sl::AdapterInfo adapter {};
    Mock::originalResult = sl::Result::eErrorFeatureNotSupported;
    Expect(StreamlineHooks::hkslIsFeatureSupported(sl::kFeatureDLSS_G, adapter) == Mock::originalResult,
           "feature support did not forward the real result");
    bool loaded = true;
    Expect(StreamlineHooks::hkslIsFeatureLoaded(sl::kFeatureDLSS_G, loaded) == Mock::originalResult && !loaded,
           "feature loaded query was not forwarded");
    sl::FeatureRequirements requirements {};
    Expect(StreamlineHooks::hkslGetFeatureRequirements(sl::kFeatureDLSS_G, requirements) == Mock::originalResult,
           "feature requirements did not forward the real result");
    sl::FeatureVersion version {};
    Expect(StreamlineHooks::hkslGetFeatureVersion(sl::kFeatureDLSS_G, version) == Mock::originalResult &&
               version.versionSL.major == 9,
           "feature version did not forward the real value");
    void* function = nullptr;
    Expect(StreamlineHooks::hkslGetFeatureFunction(sl::kFeatureDLSS_G, "otherNativeFunction", function) ==
                   Mock::originalResult &&
               function == Mock::returnedFunction && Mock::featureFunctionCalls == 1,
           "feature function lookup did not forward the real pointer/result");
    function = nullptr;
    Mock::originalResult = sl::Result::eOk;
    Expect(StreamlineHooks::hkslGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", function) ==
                   sl::Result::eOk &&
               function == reinterpret_cast<void*>(&StreamlineHooks::hkslDLSSGSetOptions),
           "native DLSSG function lookup did not install the forwarding wrapper");
}

static void TestNativeScopeGuard()
{
    ResetState();
    State::Instance().activeFgInput = FGInput::DLSSG;
    State::Instance().activeFgOutput = FGOutput::DLSSG;
    State::Instance().activeFgNvngx = FGNvngxReplacement::Nukems;
    sl::Feature features[] = { sl::kFeatureDLSS_G, sl::kFeatureDLSS };
    sl::Preferences preferences {};
    preferences.renderAPI = sl::RenderAPI::eVulkan;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = 2;
    Mock::originalResult = sl::Result::eOk;
    Expect(StreamlineHooks::hkslInit(preferences, 78) == sl::Result::eOk, "replacement Vulkan slInit failed");
    Expect(Mock::initFeatures.size() == 1 && Mock::initFeatures[0] == sl::kFeatureDLSS,
           "NVIDIA FG replacement was incorrectly treated as native Vulkan DLSSG");
    Mock::originalResult = sl::Result::eErrorFeatureNotSupported;
    sl::AdapterInfo adapter {};
    Expect(StreamlineHooks::hkslIsFeatureSupported(sl::kFeatureDLSS_G, adapter) == sl::Result::eOk,
           "replacement Vulkan path did not retain the compatibility DLSSG route");
}

static void TestVulkanResourceCalls()
{
    ResetState();
    State::Instance().nativeVulkanDlssg = true;
    sl::Constants constants {};
    TestFrame frame(42);
    sl::ViewportHandle viewport(9);
    Mock::originalResult = sl::Result::eErrorInvalidState;
    Expect(StreamlineHooks::hkslSetConstants(constants, frame, viewport) == Mock::originalResult,
           "native Vulkan SetConstants result changed");
    Expect(Mock::constantsCalls == 1 && Mock::lastConstants == &constants && Mock::lastFrame == &frame &&
               Mock::lastViewport == &viewport && State::Instance().slFGInputs.constantsReports == 0,
           "native Vulkan SetConstants touched DX12 collector or changed arguments");

    const sl::BaseStructure* inputs[] = { nullptr };
    sl::CommandBuffer* commandBuffer = nullptr;
    Expect(StreamlineHooks::hkslEvaluateFeature(sl::kFeatureDLSS_G, frame, inputs, 1, commandBuffer) ==
               Mock::originalResult,
           "native Vulkan Evaluate result changed");
    Expect(Mock::evaluateCalls == 1 && Mock::lastFrame == &frame && State::Instance().slFGInputs.resourceReports == 0,
           "native Vulkan Evaluate touched DX12 collector");
}

static void TestPluginLoadAndNativeState()
{
    ResetState();
    State::Instance().nativeVulkanDlssg = true;
    const char* json = "sentinel";
    Mock::pluginResult = true;
    Expect(StreamlineHooks::hkdlssg_slOnPluginLoad(nullptr, "loader", &json),
           "native Vulkan plugin load result changed");
    Expect(Mock::pluginLoadCalls == 1 && json == Mock::originalJson,
           "native Vulkan plugin load did not preserve the real JSON pointer/hooks");

    const auto status = StreamlineHooks::GetNativeVulkanDlssgStatus();
    Expect(status.active && !status.optionsSeen && !status.requested && !status.lastResult.has_value() &&
               status.available,
           "native Vulkan status snapshot was not read from the real state");

    sl::DLSSGState state {};
    Mock::lastState.numFramesActuallyPresented = 17;
    Mock::originalResult = sl::Result::eErrorInvalidState;
    const auto beforeCalls = Mock::stateCalls;
    Expect(StreamlineHooks::hkslDLSSGGetState(sl::ViewportHandle(1), state, nullptr) == Mock::originalResult,
           "native Vulkan GetState result changed");
    Expect(Mock::stateCalls == beforeCalls + 1 && state.numFramesActuallyPresented == 17,
           "native Vulkan GetState did not preserve the real state");
}

static void TestOptionsLockBusyIsBounded()
{
    ResetState();
    State::Instance().nativeVulkanDlssg = true;
    State::Instance().nativeVulkanDlssgOptionsSeen = true;
    State::Instance().nativeVulkanDlssgRequested = true;
    const auto optionsCalls = Mock::optionsCalls;
    const auto stateCalls = Mock::stateCalls;
    std::promise<void> acquired;
    std::promise<void> release;
    auto releaseFuture = release.get_future();
    std::thread holder(
        [&]
        {
            std::unique_lock lock(StreamlineHooks::dlssgOptionsMutex);
            acquired.set_value();
            releaseFuture.wait();
        });
    acquired.get_future().wait();
    Expect(StreamlineHooks::IsNativeVulkanDlssg(), "native atomic route changed while options lock was held");
    const auto status = StreamlineHooks::GetNativeVulkanDlssgStatus();
    Expect(!status.available, "busy options lock reported an available status snapshot");
    Expect(!StreamlineHooks::SyncNativeVulkanDlssgMenu(true, true), "busy options lock allowed a worker overlay draw");
    Expect(Mock::optionsCalls == optionsCalls && Mock::stateCalls == stateCalls,
           "busy options lock entered Streamline runtime calls");
    release.set_value();
    holder.join();
}

static void TestMarkerForwarding()
{
    ResetState();
    State::Instance().nativeVulkanDlssg = true;
    State::Instance().activeFgInput = FGInput::DLSSG;
    State::Instance().streamlineVersion = { 2, 8, 0 };
    TestFrame frame(101);
    Mock::originalResult = sl::Result::eErrorInvalidState;
    Expect(StreamlineHooks::hkslPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, frame) == Mock::originalResult,
           "native Vulkan marker result changed");
    Expect(StreamlineHooks::hkslPCLSetMarker(sl::PCLMarker::ePresentStart, frame) == Mock::originalResult,
           "native Vulkan present marker result changed");
    Expect(Mock::markerCalls == 2 && Mock::lastMarker == sl::PCLMarker::ePresentStart &&
               Mock::lastMarkerFrame == &frame && State::Instance().slFGInputs.evaluateStateCalls == 0 &&
               State::Instance().slFGInputs.markPresentCalls == 0,
           "native Vulkan marker path touched the DX12 collector or changed arguments");
}

static void TestOptionsVersionsAndMenuReplay()
{
    for (uint32_t version : { 1u, 2u, 3u, 4u, 5u })
    {
        ResetState();
        State::Instance().nativeVulkanDlssg = true;
        sl::DLSSGOptions options {};
        std::memset(&options, 0xA5, sizeof(options));
        options.structVersion = version;
        options.mode = sl::DLSSGMode::eOn;
        options.numFramesToGenerate = version + 2;
        options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
        options.next = nullptr;
        const auto bytes = KnownOptionsBytes(version);
        Mock::originalResult = sl::Result::eErrorInvalidState;
        Expect(StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(3), options) == Mock::originalResult,
               "native Vulkan options hid the original error result");
        Expect(State::Instance().nativeVulkanDlssgOptionsSeen && State::Instance().nativeVulkanDlssgRequested &&
                   State::Instance().nativeVulkanDlssgLastResult.has_value() &&
                   State::Instance().nativeVulkanDlssgLastResult.value() == static_cast<unsigned>(Mock::originalResult),
               "native Vulkan options state was not recorded");
        Expect(std::memcmp(Mock::lastOptionsBytes.data(), &options, bytes) == 0,
               "native Vulkan options changed a versioned game request");

        options.mode = sl::DLSSGMode::eAuto;
        Mock::originalResult = sl::Result::eOk;
        Expect(StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(3), options) == sl::Result::eOk,
               "native Vulkan Auto options result changed");
        Expect(Mock::lastOptions.mode == sl::DLSSGMode::eAuto &&
                   Mock::lastOptions.numFramesToGenerate == options.numFramesToGenerate &&
                   Mock::lastOptions.flags == options.flags,
               "native Vulkan Auto/count/flags request was modified");

        const auto fpsOptionsCalls = Mock::optionsCalls;
        const auto fpsStateCalls = Mock::stateCalls;
        for (unsigned frame = 0; frame < 120; ++frame)
            Expect(StreamlineHooks::SyncNativeVulkanDlssgMenu(false, false),
                   "FPS menu bookkeeping unexpectedly blocked a frame");
        for (unsigned frame = 0; frame < 120; ++frame)
            Expect(StreamlineHooks::SyncNativeVulkanDlssgMenu(true, true),
                   "verified native present boundary blocked a passive overlay");
        Expect(!StreamlineHooks::SyncNativeVulkanDlssgMenu(true, false),
               "unverified overlay was allowed while native DLSSG was active");
        Expect(Mock::optionsCalls == fpsOptionsCalls && Mock::stateCalls == fpsStateCalls,
               "FPS/passive overlay path issued extra DLSSG options or state calls");

        Mock::originalResult = sl::Result::eErrorInvalidState;

        MenuOverlayBase::visible = true;
        Mock::optionsCalls = 0;
        Expect(!StreamlineHooks::SyncNativeVulkanDlssgMenu(true, true),
               "native Vulkan menu sync hid the original Streamline error");
        Expect(Mock::optionsCalls == 1 && Mock::lastOptions.mode == sl::DLSSGMode::eOff &&
                   static_cast<uint32_t>(Mock::lastOptions.flags & sl::DLSSGFlags::eRetainResourcesWhenOff) != 0,
               "menu open did not issue temporary Off+retain");

        MenuOverlayBase::visible = false;
        Mock::originalResult = sl::Result::eOk;
        Expect(StreamlineHooks::SyncNativeVulkanDlssgMenu(true, true), "menu restore sync failed");
        Expect(Mock::optionsCalls == 2 && std::memcmp(Mock::lastOptionsBytes.data(), &options, bytes) == 0,
               "menu close did not restore the original game options");
    }

    // Unknown versions must be passed through, without cache/replay or an overlay GPU write.
    ResetState();
    State::Instance().nativeVulkanDlssg = true;
    sl::DLSSGOptions unknown {};
    unknown.structVersion = 99;
    unknown.mode = sl::DLSSGMode::eOn;
    Mock::originalResult = sl::Result::eErrorInvalidState;
    Expect(StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(4), unknown) == Mock::originalResult,
           "unknown native options result changed");
    const auto calls = Mock::optionsCalls;
    MenuOverlayBase::visible = true;
    Expect(!StreamlineHooks::SyncNativeVulkanDlssgMenu(true), "unknown options allowed an overlay GPU write");
    Expect(!StreamlineHooks::SyncNativeVulkanDlssgMenu(true, true),
           "unknown options allowed a menu overlay at the verified boundary");
    MenuOverlayBase::visible = false;
    Expect(StreamlineHooks::SyncNativeVulkanDlssgMenu(true, true),
           "unknown options blocked a verified passive native overlay");
    Expect(StreamlineHooks::SyncNativeVulkanDlssgMenu(false), "unknown options blocked menu close bookkeeping");
    Expect(Mock::optionsCalls == calls, "unknown/chained options were replayed across menu frames");

    // A known version with a non-null next chain is also opaque to the snapshot cache.
    ResetState();
    State::Instance().nativeVulkanDlssg = true;
    sl::DLSSGOptions chained {};
    chained.structVersion = 5;
    chained.mode = sl::DLSSGMode::eOn;
    chained.next = reinterpret_cast<sl::BaseStructure*>(static_cast<uintptr_t>(0x1234));
    Mock::originalResult = sl::Result::eOk;
    Expect(StreamlineHooks::hkslDLSSGSetOptions(sl::ViewportHandle(5), chained) == sl::Result::eOk,
           "chained native options result changed");
    const auto chainedCalls = Mock::optionsCalls;
    MenuOverlayBase::visible = true;
    Expect(!StreamlineHooks::SyncNativeVulkanDlssgMenu(true), "chained options allowed an overlay GPU write");
    Expect(!StreamlineHooks::SyncNativeVulkanDlssgMenu(true, true),
           "chained options allowed a menu overlay at the verified boundary");
    MenuOverlayBase::visible = false;
    Expect(StreamlineHooks::SyncNativeVulkanDlssgMenu(true, true),
           "chained options blocked a verified passive native overlay");
    Expect(StreamlineHooks::SyncNativeVulkanDlssgMenu(false), "chained options blocked menu close bookkeeping");
    Expect(Mock::optionsCalls == chainedCalls, "chained options were replayed across menu frames");
}

int main()
{
    TestInitAndQueries();
    TestNativeScopeGuard();
    TestVulkanResourceCalls();
    TestPluginLoadAndNativeState();
    TestOptionsLockBusyIsBounded();
    TestMarkerForwarding();
    TestOptionsVersionsAndMenuReplay();
    std::puts(
        "PASS: native Vulkan Streamline DLSSG/NR forwarding, ABI preservation, menu replay, and error/state handling");
    return 0;
}
