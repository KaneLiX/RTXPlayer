// nvfruc: VapourSynth filter wrapping NVIDIA Optical Flow SDK's FRUC library
// (NvOFFRUC.dll, hardware optical flow + CUDA warping) for x2 frame interpolation.
//
//   core.nvfruc.Interpolate(clip YUV420P8[, dll]) -> clip of the same length where
//   frame k is the in-between of source frames k and k+1 (the last frame repeats).
//
// The caller interleaves it with the source. NvOFFRUC.dll is not redistributable,
// so it is loaded from the user's Optical Flow SDK: the `dll` argument, else the
// RTXP_FRUC_DLL environment variable (read live, the player may change it).
// The library keeps the previous input frame, so frames are fed strictly in
// order; a jump (seek) re-primes it with one extra Process call.
#include <windows.h>
#include <softpub.h>
#include <wintrust.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "NvOFFRUC.h"
#include "VapourSynth4.h"

namespace {

// ---- CUDA driver API, loaded from nvcuda.dll (no CUDA toolkit needed) -------
using CUresult = int;
using CUdevice = int;
using CUcontext = void*;
using CUdeviceptr = unsigned long long;

struct Cuda {
    HMODULE dll = nullptr;
    CUresult(__stdcall* Init)(unsigned);
    CUresult(__stdcall* DeviceGet)(CUdevice*, int);
    CUresult(__stdcall* PrimaryCtxRetain)(CUcontext*, CUdevice);
    CUresult(__stdcall* PrimaryCtxRelease)(CUdevice);
    CUresult(__stdcall* CtxPush)(CUcontext);
    CUresult(__stdcall* CtxPop)(CUcontext*);
    CUresult(__stdcall* MemAlloc)(CUdeviceptr*, size_t);
    CUresult(__stdcall* MemFree)(CUdeviceptr);
    CUresult(__stdcall* MemAllocHost)(void**, size_t);
    CUresult(__stdcall* MemFreeHost)(void*);
    CUresult(__stdcall* HtoD)(CUdeviceptr, const void*, size_t);
    CUresult(__stdcall* DtoH)(void*, CUdeviceptr, size_t);

    bool Load() {
        if (dll) return true;
        HMODULE m = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) return false;
        bool ok = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m, name));
            ok = ok && fn;
        };
        get(Init, "cuInit");
        get(DeviceGet, "cuDeviceGet");
        get(PrimaryCtxRetain, "cuDevicePrimaryCtxRetain");
        get(PrimaryCtxRelease, "cuDevicePrimaryCtxRelease_v2");
        get(CtxPush, "cuCtxPushCurrent_v2");
        get(CtxPop, "cuCtxPopCurrent_v2");
        get(MemAlloc, "cuMemAlloc_v2");
        get(MemFree, "cuMemFree_v2");
        get(MemAllocHost, "cuMemAllocHost_v2");
        get(MemFreeHost, "cuMemFreeHost");
        get(HtoD, "cuMemcpyHtoD_v2");
        get(DtoH, "cuMemcpyDtoH_v2");
        if (!ok) {
            FreeLibrary(m);
            return false;
        }
        dll = m;
        return true;
    }
};
Cuda cu;

// ---- NvOFFRUC.dll -----------------------------------------------------------
struct Fruc {
    HMODULE dll = nullptr;
    PtrToFuncNvOFFRUCCreate Create = nullptr;
    PtrToFuncNvOFFRUCRegisterResource Register = nullptr;
    PtrToFuncNvOFFRUCUnregisterResource Unregister = nullptr;
    PtrToFuncNvOFFRUCProcess Process = nullptr;
    PtrToFuncNvOFFRUCDestroy Destroy = nullptr;
};
Fruc fruc;
std::mutex g_loadMutex;

// Only load a DLL that carries a valid Authenticode signature (what the SDK's
// SecureLoadLibrary does): the path comes from user settings.
bool SignatureValid(const std::wstring& path) {
    WINTRUST_FILE_INFO file{sizeof(file)};
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA data{sizeof(data)};
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG r = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    return r == ERROR_SUCCESS;
}

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// Returns an error message, or empty on success.
std::string LoadFruc(std::wstring path) {
    std::lock_guard<std::mutex> lock(g_loadMutex);
    if (fruc.dll) return {};
    if (path.empty()) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = GetEnvironmentVariableW(L"RTXP_FRUC_DLL", buf, (DWORD)std::size(buf));
        if (n && n < std::size(buf)) path.assign(buf, n);
    }
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return "NvOFFRUC.dll not found (set the NVIDIA Optical Flow SDK folder in RTXPlayer settings)";
    if (!SignatureValid(path)) return "NvOFFRUC.dll has no valid digital signature";
    // Altered search path: its cudart64_110.dll is loaded from the same folder.
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) return "cannot load NvOFFRUC.dll (error " + std::to_string(GetLastError()) + ")";
    fruc.Create = (PtrToFuncNvOFFRUCCreate)GetProcAddress(m, CreateProcName);
    fruc.Register = (PtrToFuncNvOFFRUCRegisterResource)GetProcAddress(m, RegisterResourceProcName);
    fruc.Unregister = (PtrToFuncNvOFFRUCUnregisterResource)GetProcAddress(m, UnregisterResourceProcName);
    fruc.Process = (PtrToFuncNvOFFRUCProcess)GetProcAddress(m, ProcessProcName);
    fruc.Destroy = (PtrToFuncNvOFFRUCDestroy)GetProcAddress(m, DestroyProcName);
    if (!fruc.Create || !fruc.Register || !fruc.Unregister || !fruc.Process || !fruc.Destroy) {
        FreeLibrary(m);
        return "NvOFFRUC.dll lacks the FRUC entry points";
    }
    fruc.dll = m;  // stays loaded for the process lifetime
    return {};
}

// ---- filter instance --------------------------------------------------------
constexpr int kInputs = 2;  // FRUC caches the previous input by pointer: alternate two buffers

struct Data {
    VSNode* node = nullptr;
    VSVideoInfo vi{};
    int w = 0, h = 0;
    size_t frameBytes = 0;  // tightly packed NV12

    CUdevice dev = 0;
    CUcontext ctx = nullptr;
    bool ctxRetained = false;
    CUdeviceptr dIn[kInputs]{};
    CUdeviceptr dOut = 0;
    uint8_t* hIn = nullptr;   // pinned staging buffers
    uint8_t* hOut = nullptr;
    NvOFFRUCHandle handle = nullptr;
    bool registered = false;

    std::mutex mutex;
    int lastFed = -2;         // source index currently cached inside FRUC
    int slot = 0;             // dIn[] slot holding lastFed
    double ts = 0;            // timestamp of lastFed (synthetic, strictly increasing)
    int lastOutN = -1;        // memo: last interpolated output
    const VSFrame* lastOut = nullptr;
};

struct CtxScope {
    explicit CtxScope(CUcontext c) { cu.CtxPush(c); }
    ~CtxScope() {
        CUcontext dummy;
        cu.CtxPop(&dummy);
    }
};

void Release(Data* d, const VSAPI* vsapi) {
    if (d->lastOut) vsapi->freeFrame(d->lastOut);
    if (d->ctx) {
        CtxScope scope(d->ctx);
        if (d->handle) {
            if (d->registered) {
                NvOFFRUC_UNREGISTER_RESOURCE_PARAM up{};
                up.pArrResource[0] = &d->dOut;
                for (int i = 0; i < kInputs; ++i) up.pArrResource[1 + i] = &d->dIn[i];
                up.uiCount = kInputs + 1;
                fruc.Unregister(d->handle, &up);
            }
            fruc.Destroy(d->handle);
        }
        for (CUdeviceptr p : d->dIn)
            if (p) cu.MemFree(p);
        if (d->dOut) cu.MemFree(d->dOut);
        if (d->hIn) cu.MemFreeHost(d->hIn);
        if (d->hOut) cu.MemFreeHost(d->hOut);
    }
    if (d->ctxRetained) cu.PrimaryCtxRelease(d->dev);
    if (d->node) vsapi->freeNode(d->node);
    delete d;
}

// Returns an error message, or empty on success. Uses the device's primary
// context, the one TensorRT / CUDA runtime code in this process shares.
std::string Init(Data* d) {
    if (!cu.Load()) return "nvcuda.dll not available";
    if (cu.Init(0) || cu.DeviceGet(&d->dev, 0) || cu.PrimaryCtxRetain(&d->ctx, d->dev))
        return "CUDA initialisation failed";
    d->ctxRetained = true;
    CtxScope scope(d->ctx);

    d->frameBytes = (size_t)d->w * d->h * 3 / 2;
    for (auto& p : d->dIn)
        if (cu.MemAlloc(&p, d->frameBytes)) return "out of video memory";
    if (cu.MemAlloc(&d->dOut, d->frameBytes)) return "out of video memory";
    if (cu.MemAllocHost((void**)&d->hIn, d->frameBytes) || cu.MemAllocHost((void**)&d->hOut, d->frameBytes))
        return "out of pinned memory";

    NvOFFRUC_CREATE_PARAM cp{};
    cp.uiWidth = (uint32_t)d->w;
    cp.uiHeight = (uint32_t)d->h;
    cp.pDevice = nullptr;
    cp.eResourceType = CudaResource;
    cp.eSurfaceFormat = NV12Surface;
    cp.eCUDAResourceType = CudaResourceCuDevicePtr;
    NvOFFRUC_STATUS st = fruc.Create(&cp, &d->handle);
    if (st != NvOFFRUC_SUCCESS) {
        d->handle = nullptr;
        return "NvOFFRUCCreate failed (" + std::to_string(st) +
               (st == NvOFFRUC_ERR_NvOFFRUC_NOT_SUPPORTED ? ", GPU without optical flow hardware?)" : ")");
    }

    NvOFFRUC_REGISTER_RESOURCE_PARAM rp{};
    // Output first, then inputs: the order of NVIDIA's sample.
    rp.pArrResource[0] = &d->dOut;
    for (int i = 0; i < kInputs; ++i) rp.pArrResource[1 + i] = &d->dIn[i];
    rp.pD3D11FenceObj = nullptr;
    rp.uiCount = kInputs + 1;
    st = fruc.Register(d->handle, &rp);
    if (st != NvOFFRUC_SUCCESS) return "NvOFFRUCRegisterResource failed (" + std::to_string(st) + ")";
    d->registered = true;
    return {};
}

// YUV420P8 planes -> tightly packed NV12 in the pinned buffer.
void PackNv12(const VSFrame* f, const VSAPI* vsapi, int w, int h, uint8_t* dst) {
    const uint8_t* y = vsapi->getReadPtr(f, 0);
    ptrdiff_t ys = vsapi->getStride(f, 0);
    for (int r = 0; r < h; ++r) memcpy(dst + (size_t)r * w, y + r * ys, w);
    uint8_t* uv = dst + (size_t)w * h;
    const uint8_t* u = vsapi->getReadPtr(f, 1);
    const uint8_t* v = vsapi->getReadPtr(f, 2);
    ptrdiff_t us = vsapi->getStride(f, 1), vs = vsapi->getStride(f, 2);
    int cw = w / 2;
    for (int r = 0; r < h / 2; ++r) {
        const uint8_t* ur = u + r * us;
        const uint8_t* vr = v + r * vs;
        uint8_t* o = uv + (size_t)r * w;
        for (int c = 0; c < cw; ++c) {
            o[2 * c] = ur[c];
            o[2 * c + 1] = vr[c];
        }
    }
}

void UnpackNv12(const uint8_t* src, int w, int h, VSFrame* f, const VSAPI* vsapi) {
    uint8_t* y = vsapi->getWritePtr(f, 0);
    ptrdiff_t ys = vsapi->getStride(f, 0);
    for (int r = 0; r < h; ++r) memcpy(y + r * ys, src + (size_t)r * w, w);
    const uint8_t* uv = src + (size_t)w * h;
    uint8_t* u = vsapi->getWritePtr(f, 1);
    uint8_t* v = vsapi->getWritePtr(f, 2);
    ptrdiff_t us = vsapi->getStride(f, 1), vs = vsapi->getStride(f, 2);
    int cw = w / 2;
    for (int r = 0; r < h / 2; ++r) {
        const uint8_t* i = uv + (size_t)r * w;
        uint8_t* ur = u + r * us;
        uint8_t* vr = v + r * vs;
        for (int c = 0; c < cw; ++c) {
            ur[c] = i[2 * c];
            vr[c] = i[2 * c + 1];
        }
    }
}

// Feeds source frame idx to FRUC; when `out` is set, receives the frame halfway
// between the previously fed frame and this one. Returns an error message or empty.
std::string Feed(Data* d, int idx, const VSFrame* f, const VSAPI* vsapi, bool* repeated, bool wantOut) {
    int slot = d->lastFed == -2 ? 0 : 1 - d->slot;
    PackNv12(f, vsapi, d->w, d->h, d->hIn);
    if (cu.HtoD(d->dIn[slot], d->hIn, d->frameBytes)) return "upload failed";

    // Consecutive frames are 1.0 apart; a jump leaves a gap so FRUC never sees
    // non-increasing time.
    double ts = d->ts + (idx == d->lastFed + 1 ? 1.0 : 16.0);
    bool rep = false;
    NvOFFRUC_PROCESS_IN_PARAMS in{};
    in.stFrameDataInput.pFrame = &d->dIn[slot];
    in.stFrameDataInput.nTimeStamp = ts;
    in.stFrameDataInput.nCuSurfacePitch = 0;
    NvOFFRUC_PROCESS_OUT_PARAMS out{};
    out.stFrameDataOutput.pFrame = &d->dOut;
    out.stFrameDataOutput.nTimeStamp = ts - 0.5;
    out.stFrameDataOutput.nCuSurfacePitch = 0;
    out.stFrameDataOutput.bHasFrameRepetitionOccurred = &rep;
    NvOFFRUC_STATUS st = fruc.Process(d->handle, &in, &out);
    if (st != NvOFFRUC_SUCCESS) {
        d->lastFed = -2;  // state unknown: re-prime on the next request
        return "NvOFFRUCProcess failed (" + std::to_string(st) + ")";
    }
    d->lastFed = idx;
    d->slot = slot;
    d->ts = ts;
    if (repeated) *repeated = rep;
    if (wantOut && cu.DtoH(d->hOut, d->dOut, d->frameBytes)) return "download failed";
    return {};
}

const VSFrame* VS_CC GetFrame(int n, int activationReason, void* instanceData, void**, VSFrameContext* frameCtx,
                              VSCore* core, const VSAPI* vsapi) {
    auto* d = static_cast<Data*>(instanceData);
    const int last = d->vi.numFrames - 1;
    const int next = std::min(n + 1, last);

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
        if (next != n) vsapi->requestFrameFilter(next, d->node, frameCtx);
        return nullptr;
    }
    if (activationReason != arAllFramesReady) return nullptr;

    const VSFrame* left = vsapi->getFrameFilter(n, d->node, frameCtx);
    const VSFrame* right = vsapi->getFrameFilter(next, d->node, frameCtx);

    // Last frame, or a hard cut marked upstream: repeat the left frame rather
    // than morphing across it. FRUC is not fed, the next request re-primes it.
    const VSMap* lp = vsapi->getFramePropertiesRO(left);
    int err = 0;
    bool cut = vsapi->mapGetInt(lp, "_SceneChangeNext", 0, &err) != 0 && !err;
    if (next == n || cut) {
        vsapi->freeFrame(right);
        return left;
    }

    std::lock_guard<std::mutex> lock(d->mutex);
    if (d->lastOutN == n && d->lastOut) {
        vsapi->freeFrame(right);
        vsapi->freeFrame(left);
        return vsapi->addFrameRef(d->lastOut);
    }

    CtxScope scope(d->ctx);
    std::string e;
    if (d->lastFed != n) e = Feed(d, n, left, vsapi, nullptr, false);
    bool repeated = false;
    if (e.empty()) e = Feed(d, next, right, vsapi, &repeated, true);
    vsapi->freeFrame(right);
    if (!e.empty()) {
        vsapi->freeFrame(left);
        vsapi->setFilterError(("nvfruc: " + e).c_str(), frameCtx);
        return nullptr;
    }

    if (GetEnvironmentVariableW(L"RTXP_FRUC_DEBUG", nullptr, 0))
        fprintf(stderr, "nvfruc: n=%d fed=%d ts=%.1f repeated=%d\n", n, d->lastFed, d->ts, (int)repeated);

    VSFrame* dst = vsapi->newVideoFrame(&d->vi.format, d->w, d->h, left, core);
    vsapi->freeFrame(left);
    UnpackNv12(d->hOut, d->w, d->h, dst, vsapi);
    vsapi->mapSetInt(vsapi->getFramePropertiesRW(dst), "FrucRepeated", repeated ? 1 : 0, maReplace);

    if (d->lastOut) vsapi->freeFrame(d->lastOut);
    d->lastOut = vsapi->addFrameRef(dst);
    d->lastOutN = n;
    return dst;
}

void VS_CC Free(void* instanceData, VSCore*, const VSAPI* vsapi) { Release(static_cast<Data*>(instanceData), vsapi); }

void VS_CC Create(const VSMap* in, VSMap* out, void*, VSCore* core, const VSAPI* vsapi) {
    auto* d = new Data;
    d->node = vsapi->mapGetNode(in, "clip", 0, nullptr);
    d->vi = *vsapi->getVideoInfo(d->node);
    auto fail = [&](const std::string& msg) {
        vsapi->mapSetError(out, ("nvfruc.Interpolate: " + msg).c_str());
        Release(d, vsapi);
    };

    const VSVideoFormat& f = d->vi.format;
    if (f.colorFamily != cfYUV || f.sampleType != stInteger || f.bitsPerSample != 8 || f.subSamplingW != 1 ||
        f.subSamplingH != 1)
        return fail("clip must be YUV420P8");
    if (d->vi.width <= 0 || d->vi.height <= 0) return fail("clip must have constant dimensions");
    d->w = d->vi.width;
    d->h = d->vi.height;

    int err = 0;
    const char* dll = vsapi->mapGetData(in, "dll", 0, &err);
    std::string e = LoadFruc(err ? std::wstring() : Widen(dll));
    if (e.empty()) e = Init(d);
    if (!e.empty()) return fail(e);

    VSFilterDependency deps[] = {{d->node, rpGeneral}};
    // fmUnordered: one frame at a time through GetFrame's processing part, as
    // FRUC is a sequential state machine.
    vsapi->createVideoFilter(out, "Interpolate", &d->vi, GetFrame, Free, fmUnordered, deps, 1, d, core);
}

}  // namespace

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin* plugin, const VSPLUGINAPI* vspapi) {
    vspapi->configPlugin("com.rtxplayer.nvfruc", "nvfruc", "NVIDIA Optical Flow FRUC frame interpolation",
                         VS_MAKE_VERSION(1, 0), VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction("Interpolate", "clip:vnode;dll:data:opt;", "clip:vnode;", Create, nullptr, plugin);
}
