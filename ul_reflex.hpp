#pragma once
// Ultra Limiter — NVIDIA Reflex hook layer
// Clean-room implementation from public documentation:
//   - NVIDIA NVAPI SDK (MIT): nvapi.h struct definitions, nvapi_interface.h IDs
//   - MinHook API: MH_CreateHook / MH_EnableHook / MH_DisableHook / MH_RemoveHook
//   - Windows: LoadLibraryA, GetProcAddress
// Hooks: NvAPI_D3D_SetSleepMode, NvAPI_D3D_Sleep, NvAPI_D3D_SetLatencyMarker
// Also resolves NvAPI_D3D_GetLatency (not hooked, called directly).

#include <atomic>
#include <cstdint>
#include <windows.h>

struct IUnknown;
struct IDXGISwapChain;

// NVAPI status codes (from nvapi.h, MIT)
using NvStatus = int;
using NvU32 = unsigned int;
constexpr NvStatus NV_OK = 0;
constexpr NvStatus NV_NO_IMPL = -4;
constexpr NvStatus NV_BAD_ARG = -5;

// Latency marker types (from nvapi.h, MIT)
enum LatencyMarker {
    SIM_START = 0,
    SIM_END = 1,
    RENDER_SUBMIT_START = 2,
    RENDER_SUBMIT_END = 3,
    PRESENT_BEGIN = 4,
    PRESENT_FINISH = 5,
    INPUT_SAMPLE = 6,
    TRIGGER_FLASH = 7,
    PC_LATENCY_PING = 8,
    OOB_RENDER_SUBMIT_START = 9,
    OOB_RENDER_SUBMIT_END = 10,
    OOB_PRESENT_START = 11,
    OOB_PRESENT_END = 12,
};
constexpr int kMarkerCount = 13;

// --- NVAPI structs (minimal copies from nvapi.h, MIT licensed) ---

#pragma pack(push, 8)

struct NvSleepParams {
    NvU32 version;
    NvU32 bLowLatencyMode : 1;
    NvU32 bLowLatencyBoost : 1;
    NvU32 bUseMarkersToOptimize : 1;
    NvU32 bReservedBits : 29;
    NvU32 minimumIntervalUs;
    NvU32 reserved[4];
};
#define NV_SLEEP_PARAMS_VER ((NvU32)(sizeof(NvSleepParams) | (1 << 16)))

struct NvMarkerParams {
    NvU32 version;
    uint64_t frameID;
    LatencyMarker markerType;
    NvU32 reserved[4];
};
#define NV_MARKER_PARAMS_VER ((NvU32)(sizeof(NvMarkerParams) | (1 << 16)))

struct NvLatencyFrameReport {
    uint64_t frameID;
    uint64_t inputSampleTime;
    uint64_t simStartTime;
    uint64_t simEndTime;
    uint64_t renderSubmitStartTime;
    uint64_t renderSubmitEndTime;
    uint64_t presentStartTime;
    uint64_t presentEndTime;
    uint64_t driverStartTime;
    uint64_t driverEndTime;
    uint64_t osRenderQueueStartTime;
    uint64_t osRenderQueueEndTime;
    uint64_t gpuRenderStartTime;
    uint64_t gpuRenderEndTime;
    NvU32 gpuActiveRenderTimeUs;
    NvU32 gpuFrameTimeUs;
    unsigned char rsvd[120];
};

struct NvLatencyResult {
    NvU32 version;
    NvLatencyFrameReport frameReport[64];
    unsigned char rsvd[32];
};
#define NV_LATENCY_RESULT_VER ((NvU32)(sizeof(NvLatencyResult) | (1 << 16)))

#pragma pack(pop)

// --- Function pointer types ---

using NvQueryInterface_fn = void*(__cdecl*)(NvU32);
using NvInitialize_fn = NvStatus(__cdecl*)();
using NvSetSleepMode_fn = NvStatus(__cdecl*)(IUnknown*, NvSleepParams*);
using NvSleep_fn = NvStatus(__cdecl*)(IUnknown*);
using NvSetMarker_fn = NvStatus(__cdecl*)(IUnknown*, NvMarkerParams*);
using NvGetLatency_fn = NvStatus(__cdecl*)(IUnknown*, NvLatencyResult*);

// --- Captured game Reflex state ---

struct GameReflexState {
    std::atomic<bool> captured{false};
    std::atomic<bool> low_latency{false};
    std::atomic<bool> boost{false};
    std::atomic<bool> use_markers{false};
};

// --- Per-frame marker timestamp ring buffer ---

constexpr size_t kRingSize = 64;

struct MarkerSlot {
    std::atomic<uint64_t> frame_id{0};
    std::atomic<int64_t> timestamp_ns[kMarkerCount]{};
    std::atomic<uint64_t> seen_frame[kMarkerCount]{};  // dedup: last frame_id per marker type
};

extern MarkerSlot g_ring[kRingSize];
extern std::atomic<bool> g_game_uses_reflex;  // true once we see a SetLatencyMarker call

// --- Public API ---

bool SetupReflexHooks();
void TeardownReflexHooks();
bool ReflexActive();

NvStatus InvokeSetSleepMode(IUnknown* dev, NvSleepParams* p);
NvStatus InvokeSleep(IUnknown* dev);
NvStatus InvokeSetMarker(IUnknown* dev, NvMarkerParams* p);
NvStatus InvokeGetLatency(IUnknown* dev, NvLatencyResult* p);

const GameReflexState& GetGameState();

// --- Streamline proxy swapchain hook ---

bool HookSLProxy(IDXGISwapChain* sc);

// --- VSync override swapchain hook ---

bool HookVSyncPresent(IDXGISwapChain* sc);

using SLPresentCb = void(*)();
void SetSLPresentCb(SLPresentCb cb);

// Marker callback — fired when game calls SetLatencyMarker
using MarkerCb = void(*)(int marker_type, uint64_t frame_id);
void SetMarkerCb(MarkerCb cb);
