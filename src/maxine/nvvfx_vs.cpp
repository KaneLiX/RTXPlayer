// nvvfx: VapourSynth filters on the NVIDIA Video Effects (Maxine VFX) SDK.
//
//   core.nvvfx.SuperRes(clip YUV420P8, width, height[, quality, strength, matrix, full, sdk])
//     AI video super resolution ("VideoSuperRes" feature, NGX VSR model).
//
// The SDK is not redistributed with the player: it is loaded from the user's copy,
// the `sdk` argument or the RTXP_VFX_SDK environment variable (the folder holding
// bin\NVVideoEffects.dll). Features live in <sdk>\features\<name>\bin (the layout
// of NVIDIA's install_feature script) or next to the SDK folder (<sdk>\..\<name>\bin).
// Frames stay YUV on the CPU side; the YUV <-> BGRA conversion runs on the GPU.
#include <windows.h>
#include <softpub.h>
#include <wintrust.h>

#include <mutex>
#include <string>

#include "VapourSynth4.h"
#include "nvVideoEffects.h"

char* g_nvVFXSDKPath = nullptr;  // nvVideoEffectsProxy.cpp: unset, the DLLs are preloaded instead

namespace {

std::wstring Widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// The SDK path comes from user settings: only load NVIDIA-signed binaries.
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

std::mutex g_loadMutex;
std::wstring g_sdkRoot;  // loaded SDK (one per process)

std::wstring SdkRoot(const char* arg) {
    if (arg && *arg) return Widen(arg);
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"RTXP_VFX_SDK", buf, (DWORD)std::size(buf));
    return n && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
}

HMODULE LoadSigned(const std::wstring& path, std::string& err) {
    if (!SignatureValid(path)) {
        err = "no valid signature: " + Narrow(path);
        return nullptr;
    }
    // Altered search path: dependencies resolve from the DLL's own folder.
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) err = "cannot load " + Narrow(path) + " (error " + std::to_string(GetLastError()) + ")";
    return m;
}

// Loads the SDK core and one feature library. The core loads features by bare
// name, which then resolves to the module already loaded here.
std::string LoadSdk(const std::wstring& root, const wchar_t* feature, const wchar_t* featureDll) {
    std::lock_guard<std::mutex> lock(g_loadMutex);
    std::string err;
    if (g_sdkRoot.empty()) {
        if (root.empty() || !Exists(root + L"\\bin\\NVVideoEffects.dll"))
            return "NVIDIA Video Effects SDK not found (set its folder in RTXPlayer settings)";
        // The proxies read this to skip their Program Files search (no SetDllDirectory).
        SetEnvironmentVariableW(L"NV_VIDEO_EFFECTS_PATH", L"USE_APP_PATH");
        for (const wchar_t* d : {L"\\bin\\NVCVImage.dll", L"\\bin\\NVVideoEffects.dll", L"\\bin\\nvngxruntime.dll"})
            if (Exists(root + d) && !LoadSigned(root + d, err)) return err;
        g_sdkRoot = root;
    }
    if (GetModuleHandleW(featureDll)) return {};
    std::wstring rel = std::wstring(L"\\") + feature + L"\\bin\\" + featureDll;
    for (const std::wstring& p : {g_sdkRoot + L"\\features" + rel, g_sdkRoot + L"\\.." + rel})
        if (Exists(p)) return LoadSigned(p, err) ? std::string() : err;
    return "feature " + Narrow(feature) + " is not installed in the Video Effects SDK";
}

std::string Status(NvCV_Status s) { return std::to_string((int)s) + " " + NvCV_GetErrorStringFromCode(s); }

unsigned ColorSpace(int matrix, bool full) {
    unsigned cs = matrix == 6 ? NVCV_601 : matrix == 9 ? NVCV_2020 : NVCV_709;
    return cs | (full ? NVCV_FULL_RANGE : NVCV_VIDEO_RANGE) | NVCV_CHROMA_MPEG2;
}

// Planar YUV420 8-bit frame in GPU memory. Plane copies between VapourSynth frames
// and these buffers are plain 2D memcpys; NvCVImage_TransferFrom/ToYUV then convert
// on the GPU (given CPU-side YUV they convert on the CPU, which is far too slow).
struct GpuYuv {
    NvCVImage plane[3];

    NvCV_Status Alloc(int w, int h) {
        NvCV_Status s = NvCVImage_Alloc(&plane[0], w, h, NVCV_Y, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 256);
        for (int p = 1; p < 3 && s == NVCV_SUCCESS; ++p)
            s = NvCVImage_Alloc(&plane[p], w / 2, h / 2, NVCV_Y, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 256);
        if (s == NVCV_SUCCESS && plane[1].pitch != plane[2].pitch) s = NVCV_ERR_CUDA_PITCH;  // one uvPitch
        return s;
    }

    NvCV_Status Upload(const VSFrame* f, const VSAPI* vsapi, CUstream stream) {
        NvCV_Status s = NVCV_SUCCESS;
        for (int p = 0; p < 3 && s == NVCV_SUCCESS; ++p) {
            NvCVImage cpu;
            s = NvCVImage_Init(&cpu, plane[p].width, plane[p].height, (int)vsapi->getStride(f, p),
                               (void*)vsapi->getReadPtr(f, p), NVCV_Y, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
            if (s == NVCV_SUCCESS) s = NvCVImage_Transfer(&cpu, &plane[p], 1.f, stream, nullptr);
            cpu.deletePtr = nullptr;  // wrapper only
        }
        return s;
    }

    NvCV_Status Download(VSFrame* f, const VSAPI* vsapi, CUstream stream) {
        NvCV_Status s = NVCV_SUCCESS;
        for (int p = 0; p < 3 && s == NVCV_SUCCESS; ++p) {
            NvCVImage cpu;
            s = NvCVImage_Init(&cpu, plane[p].width, plane[p].height, (int)vsapi->getStride(f, p),
                               vsapi->getWritePtr(f, p), NVCV_Y, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
            if (s == NVCV_SUCCESS) s = NvCVImage_Transfer(&plane[p], &cpu, 1.f, stream, nullptr);
            cpu.deletePtr = nullptr;
        }
        return s;
    }

    NvCV_Status ToBgra(NvCVImage* dst, unsigned cs, CUstream stream) {
        return NvCVImage_TransferFromYUV(plane[0].pixels, 1, plane[0].pitch, plane[1].pixels, plane[2].pixels, 1,
                                         plane[1].pitch, NVCV_YUV420, NVCV_U8, cs, NVCV_GPU, dst, nullptr, 1.f,
                                         stream, nullptr);
    }

    NvCV_Status FromBgra(const NvCVImage* src, unsigned cs, CUstream stream) {
        return NvCVImage_TransferToYUV(src, nullptr, plane[0].pixels, 1, plane[0].pitch, plane[1].pixels,
                                       plane[2].pixels, 1, plane[1].pitch, NVCV_YUV420, NVCV_U8, cs, NVCV_GPU, 1.f,
                                       stream, nullptr);
    }
};

// ---- SuperRes ---------------------------------------------------------------

struct SrData {
    VSNode* node = nullptr;
    VSVideoInfo vi{};  // output
    int inW = 0, inH = 0;
    unsigned colorspace = 0;
    NvVFX_Handle effect = nullptr;
    CUstream stream = nullptr;
    NvCVImage srcGpu, dstGpu;  // BGRA, the effect's input/output
    GpuYuv yuvIn, yuvOut;
    std::mutex mutex;  // one effect instance: frames go through it one at a time
};

void Release(SrData* d, const VSAPI* vsapi) {
    if (d->effect) NvVFX_DestroyEffect(d->effect);
    if (d->stream) NvVFX_CudaStreamDestroy(d->stream);
    if (d->node) vsapi->freeNode(d->node);
    delete d;  // NvCVImage destructors free the GPU buffers
}

const VSFrame* VS_CC SrGetFrame(int n, int activationReason, void* instanceData, void**, VSFrameContext* frameCtx,
                                VSCore* core, const VSAPI* vsapi) {
    auto* d = static_cast<SrData*>(instanceData);
    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
        return nullptr;
    }
    if (activationReason != arAllFramesReady) return nullptr;

    const VSFrame* src = vsapi->getFrameFilter(n, d->node, frameCtx);
    VSFrame* dst = vsapi->newVideoFrame(&d->vi.format, d->vi.width, d->vi.height, src, core);
    NvCV_Status s;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        s = d->yuvIn.Upload(src, vsapi, d->stream);
        if (s == NVCV_SUCCESS) s = d->yuvIn.ToBgra(&d->srcGpu, d->colorspace, d->stream);
        if (s == NVCV_SUCCESS) s = NvVFX_Run(d->effect, 1);
        if (s == NVCV_SUCCESS) s = d->yuvOut.FromBgra(&d->dstGpu, d->colorspace, d->stream);
        if (s == NVCV_SUCCESS) s = d->yuvOut.Download(dst, vsapi, d->stream);
        if (s == NVCV_SUCCESS) s = NvVFX_CudaStreamSynchronize(d->stream);
    }
    vsapi->freeFrame(src);
    if (s != NVCV_SUCCESS) {
        vsapi->freeFrame(dst);
        vsapi->setFilterError(("nvvfx.SuperRes: " + Status(s)).c_str(), frameCtx);
        return nullptr;
    }
    return dst;
}

void VS_CC SrFree(void* instanceData, VSCore*, const VSAPI* vsapi) { Release(static_cast<SrData*>(instanceData), vsapi); }

void VS_CC SrCreate(const VSMap* in, VSMap* out, void*, VSCore* core, const VSAPI* vsapi) {
    // Load the SDK before any NvCVImage exists: its constructor/destructor call into
    // the proxy, which would otherwise look for the DLLs elsewhere and cache a failure.
    int err = 0;
    std::string e = LoadSdk(SdkRoot(vsapi->mapGetData(in, "sdk", 0, &err)), L"nvvfxvideosuperres",
                            L"nvVFXVideoSuperRes.dll");
    if (!e.empty()) {
        vsapi->mapSetError(out, ("nvvfx.SuperRes: " + e).c_str());
        return;
    }
    auto* d = new SrData;
    d->node = vsapi->mapGetNode(in, "clip", 0, nullptr);
    d->vi = *vsapi->getVideoInfo(d->node);
    auto fail = [&](const std::string& msg) {
        vsapi->mapSetError(out, ("nvvfx.SuperRes: " + msg).c_str());
        Release(d, vsapi);
    };
    const VSVideoFormat& f = d->vi.format;
    if (f.colorFamily != cfYUV || f.sampleType != stInteger || f.bitsPerSample != 8 || f.subSamplingW != 1 ||
        f.subSamplingH != 1)
        return fail("clip must be YUV420P8");
    if (d->vi.width <= 0 || d->vi.height <= 0) return fail("clip must have constant dimensions");
    d->inW = d->vi.width;
    d->inH = d->vi.height;
    int outW = (int)vsapi->mapGetInt(in, "width", 0, nullptr);
    int outH = (int)vsapi->mapGetInt(in, "height", 0, nullptr);
    if (outW < d->inW || outH < d->inH || (outW | outH) & 1) return fail("width/height must be even and >= the input");
    int quality = vsapi->mapGetIntSaturated(in, "quality", 0, &err);
    if (err) quality = 2;
    double strength = vsapi->mapGetFloat(in, "strength", 0, &err);
    bool hasStrength = !err;
    int matrix = vsapi->mapGetIntSaturated(in, "matrix", 0, &err);
    if (err) matrix = 1;
    bool full = vsapi->mapGetInt(in, "full", 0, &err) != 0 && !err;
    d->colorspace = ColorSpace(matrix, full);

    NvCV_Status s;
    auto ck = [&](NvCV_Status st, const char* what) {
        s = st;
        if (st != NVCV_SUCCESS) e = std::string(what) + " failed: " + Status(st);
        return st == NVCV_SUCCESS;
    };
    if (!ck(NvVFX_CreateEffect("VideoSuperRes", &d->effect), "CreateEffect") ||
        !ck(NvVFX_CudaStreamCreate(&d->stream), "CudaStreamCreate") ||
        !ck(NvVFX_SetCudaStream(d->effect, NVVFX_CUDA_STREAM, d->stream), "SetCudaStream") ||
        !ck(NvVFX_SetU32(d->effect, "QualityLevel", (unsigned)quality), "QualityLevel") ||
        (hasStrength && !ck(NvVFX_SetF32(d->effect, NVVFX_STRENGTH, (float)strength), "Strength")) ||
        !ck(NvCVImage_Alloc(&d->srcGpu, d->inW, d->inH, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 32), "Alloc") ||
        !ck(NvCVImage_Alloc(&d->dstGpu, outW, outH, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 32), "Alloc") ||
        !ck(d->yuvIn.Alloc(d->inW, d->inH), "Alloc") || !ck(d->yuvOut.Alloc(outW, outH), "Alloc") ||
        !ck(NvVFX_SetImage(d->effect, NVVFX_INPUT_IMAGE, &d->srcGpu), "SetImage") ||
        !ck(NvVFX_SetImage(d->effect, NVVFX_OUTPUT_IMAGE, &d->dstGpu), "SetImage") ||
        !ck(NvVFX_Load(d->effect), "Load"))
        return fail(e);

    d->vi.width = outW;
    d->vi.height = outH;
    VSFilterDependency deps[] = {{d->node, rpStrictSpatial}};
    vsapi->createVideoFilter(out, "SuperRes", &d->vi, SrGetFrame, SrFree, fmParallelRequests, deps, 1, d, core);
}

}  // namespace

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin* plugin, const VSPLUGINAPI* vspapi) {
    vspapi->configPlugin("com.rtxplayer.nvvfx", "nvvfx", "NVIDIA Video Effects SDK filters", VS_MAKE_VERSION(1, 0),
                         VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction("SuperRes",
                             "clip:vnode;width:int;height:int;quality:int:opt;strength:float:opt;matrix:int:opt;"
                             "full:int:opt;sdk:data:opt;",
                             "clip:vnode;", SrCreate, nullptr, plugin);
}
