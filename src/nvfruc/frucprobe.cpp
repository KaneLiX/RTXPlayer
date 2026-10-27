// Development probe (not shipped): feeds NvOFFRUC synthetic panning frames and
// reports whether it interpolates or falls back to frame repetition.
//   frucprobe <NvOFFRUC.dll> [w h shift]
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "NvOFFRUC.h"

using CUresult = int;
using CUdeviceptr = unsigned long long;
static CUresult(__stdcall* cuInit)(unsigned);
static CUresult(__stdcall* cuDeviceGet)(int*, int);
static CUresult(__stdcall* cuCtxCreate)(void**, unsigned, int);
static CUresult(__stdcall* cuDevicePrimaryCtxRetain)(void**, int);
static CUresult(__stdcall* cuCtxSetCurrent)(void*);
static CUresult(__stdcall* cuMemAlloc)(CUdeviceptr*, size_t);
static CUresult(__stdcall* cuHtoD)(CUdeviceptr, const void*, size_t);
static CUresult(__stdcall* cuDtoH)(void*, CUdeviceptr, size_t);

static uint8_t Pattern(int x, int y) {
    // Non-periodic value noise (bilinear over a hashed 8 px lattice): unambiguous for optical flow.
    auto hash = [](int i, int j) {
        unsigned h = (unsigned)i * 374761393u + (unsigned)j * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return (double)((h ^ (h >> 16)) & 255);
    };
    int cx = (x >= 0 ? x : x - 7) / 8, cy = y / 8;
    double fx = (x - cx * 8) / 8.0, fy = (y - cy * 8) / 8.0;
    double v = hash(cx, cy) * (1 - fx) * (1 - fy) + hash(cx + 1, cy) * fx * (1 - fy) +
               hash(cx, cy + 1) * (1 - fx) * fy + hash(cx + 1, cy + 1) * fx * fy;
    return (uint8_t)std::lround(v);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    int w = argc > 3 ? _wtoi(argv[2]) : 1920, h = argc > 3 ? _wtoi(argv[3]) : 1080;
    int shift = argc > 4 ? _wtoi(argv[4]) : 8;
    bool argb = argc > 5 && wcscmp(argv[5], L"argb") == 0;
    bool primary = !(argc > 6 && wcscmp(argv[6], L"own") == 0);
    double scale = argc > 7 ? _wtof(argv[7]) : 1.0;

    HMODULE c = LoadLibraryW(L"nvcuda.dll");
    cuInit = (decltype(cuInit))GetProcAddress(c, "cuInit");
    cuDeviceGet = (decltype(cuDeviceGet))GetProcAddress(c, "cuDeviceGet");
    cuCtxCreate = (decltype(cuCtxCreate))GetProcAddress(c, "cuCtxCreate_v2");
    cuDevicePrimaryCtxRetain = (decltype(cuDevicePrimaryCtxRetain))GetProcAddress(c, "cuDevicePrimaryCtxRetain");
    cuCtxSetCurrent = (decltype(cuCtxSetCurrent))GetProcAddress(c, "cuCtxSetCurrent");
    cuMemAlloc = (decltype(cuMemAlloc))GetProcAddress(c, "cuMemAlloc_v2");
    cuHtoD = (decltype(cuHtoD))GetProcAddress(c, "cuMemcpyHtoD_v2");
    cuDtoH = (decltype(cuDtoH))GetProcAddress(c, "cuMemcpyDtoH_v2");
    int dev;
    void* ctx;
    cuInit(0);
    cuDeviceGet(&dev, 0);
    if (primary) cuDevicePrimaryCtxRetain(&ctx, dev); else cuCtxCreate(&ctx, 0, dev);
    cuCtxSetCurrent(ctx);

    HMODULE f = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    auto Create = (PtrToFuncNvOFFRUCCreate)GetProcAddress(f, CreateProcName);
    auto Register = (PtrToFuncNvOFFRUCRegisterResource)GetProcAddress(f, RegisterResourceProcName);
    auto Process = (PtrToFuncNvOFFRUCProcess)GetProcAddress(f, ProcessProcName);

    size_t bytes = argb ? (size_t)w * h * 4 : (size_t)w * h * 3 / 2;
    CUdeviceptr in[2], out;
    cuMemAlloc(&in[0], bytes);
    cuMemAlloc(&in[1], bytes);
    cuMemAlloc(&out, bytes);

    NvOFFRUC_CREATE_PARAM cp{};
    cp.uiWidth = w;
    cp.uiHeight = h;
    cp.eResourceType = CudaResource;
    cp.eSurfaceFormat = argb ? ARGBSurface : NV12Surface;
    cp.eCUDAResourceType = CudaResourceCuDevicePtr;
    NvOFFRUCHandle hf;
    printf("create %d\n", Create(&cp, &hf));
    NvOFFRUC_REGISTER_RESOURCE_PARAM rp{};
    rp.pArrResource[0] = &out;
    rp.pArrResource[1] = &in[0];
    rp.pArrResource[2] = &in[1];
    rp.uiCount = 3;
    printf("register %d\n", Register(hf, &rp));

    std::vector<uint8_t> host(bytes), res(bytes);
    for (int i = 0; i < 8; ++i) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t v = Pattern(x + i * shift, y);
                if (argb) {
                    uint8_t* p = &host[((size_t)y * w + x) * 4];
                    p[0] = v; p[1] = v; p[2] = (uint8_t)(255 - v); p[3] = 255;
                } else {
                    host[(size_t)y * w + x] = v;
                }
            }
        if (!argb) memset(&host[(size_t)w * h], 128, (size_t)w * h / 2);
        if (argc > 9) {
            // Real frames: raw I420 file, frame i, converted to NV12.
            static FILE* fp = _wfopen(argv[9], L"rb");
            std::vector<uint8_t> i420((size_t)w * h * 3 / 2);
            if (!fp || fread(i420.data(), 1, i420.size(), fp) != i420.size()) return 3;
            memcpy(host.data(), i420.data(), (size_t)w * h);
            const uint8_t* u = &i420[(size_t)w * h];
            const uint8_t* v = u + (size_t)w * h / 4;
            if (GetEnvironmentVariableW(L"PROBE_FLATUV", nullptr, 0)) u = v = nullptr;
            for (size_t k = 0; u && k < (size_t)w * h / 4; ++k) {
                host[(size_t)w * h + 2 * k] = u[k];
                host[(size_t)w * h + 2 * k + 1] = v[k];
            }
        }
        cuHtoD(in[i & 1], host.data(), bytes);

        bool rep = false;
        NvOFFRUC_PROCESS_IN_PARAMS pin{};
        pin.stFrameDataInput.pFrame = &in[i & 1];
        pin.stFrameDataInput.nTimeStamp = (i + 1) * scale;
        NvOFFRUC_PROCESS_OUT_PARAMS pout{};
        pout.stFrameDataOutput.pFrame = &out;
        pout.stFrameDataOutput.nTimeStamp = (i + 0.5) * scale;
        pout.stFrameDataOutput.bHasFrameRepetitionOccurred = &rep;
        int st = 0;
        if (argc > 8 && wcscmp(argv[8], L"threads") == 0) {
            // Each call on a fresh thread, like a VapourSynth worker pool.
            std::thread t([&] {
                cuCtxSetCurrent(ctx);
                st = Process(hf, &pin, &pout);
            });
            t.join();
        } else {
            st = Process(hf, &pin, &pout);
        }
        static auto cudaGetLastError = (int(__stdcall*)())GetProcAddress(GetModuleHandleW(L"cudart64_110.dll"), "cudaGetLastError");
        static auto cudaDeviceSynchronize = (int(__stdcall*)())GetProcAddress(GetModuleHandleW(L"cudart64_110.dll"), "cudaDeviceSynchronize");
        int ce = cudaDeviceSynchronize ? cudaDeviceSynchronize() : -1;
        int le = cudaGetLastError ? cudaGetLastError() : -1;
        printf("  cuda sync %d last %d\n", ce, le);
        cuDtoH(res.data(), out, bytes);

        // Mean abs error of the output luma/G against the expected half-shift picture.
        double eMid = 0, ePrev = 0;
        int n = 0;
        for (int y = 16; y < h - 16; y += 4)
            for (int x = 64; x < w - 64; x += 4) {
                uint8_t o = argb ? res[((size_t)y * w + x) * 4 + 1] : res[(size_t)y * w + x];
                eMid += std::abs(o - Pattern(x + (int)std::lround((i - 0.5) * shift), y));
                ePrev += std::abs(o - Pattern(x + (i - 1) * shift, y));
                ++n;
            }
        printf("frame %d: status %d repeated %d  err vs mid %.2f  vs prev %.2f\n", i, st, (int)rep, eMid / n,
               ePrev / n);
    }
    return 0;
}
