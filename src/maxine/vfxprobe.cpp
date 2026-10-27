// Development probe (not shipped): runs NVIDIA VFX SDK Video Super Resolution on
// one raw BGRA frame, times it, and writes the upscaled BGRA frame.
//   vfxprobe <sdk\bin> <in.bgra> <w> <h> <outW> <outH> <qualityLevel> <out.bgra> [strength]
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "nvVideoEffects.h"

char* g_nvVFXSDKPath = nullptr;  // used by nvVideoEffectsProxy.cpp; DLLs are preloaded instead

#define CK(x)                                                                         \
    do {                                                                              \
        NvCV_Status _s = (x);                                                         \
        if (_s != NVCV_SUCCESS) {                                                     \
            printf("FAIL %s -> %d %s\n", #x, (int)_s, NvCV_GetErrorStringFromCode(_s)); \
            return 1;                                                                 \
        }                                                                             \
    } while (0)

int wmain(int argc, wchar_t** argv) {
    if (argc < 9) return 2;
    std::wstring bin = argv[1];
    int w = _wtoi(argv[3]), h = _wtoi(argv[4]), ow = _wtoi(argv[5]), oh = _wtoi(argv[6]);
    unsigned q = (unsigned)_wtoi(argv[7]);
    float strength = argc > 9 ? (float)_wtof(argv[9]) : -1.f;

    // Preload by full path (dependencies resolve from the SDK's bin); the proxies' own
    // LoadLibrary then returns these modules without touching the DLL search path.
    SetEnvironmentVariableW(L"NV_VIDEO_EFFECTS_PATH", L"USE_APP_PATH");
    for (const wchar_t* d : {L"\\NVCVImage.dll", L"\\NVVideoEffects.dll", L"\\nvngxruntime.dll",
                             L"\\..\\features\\nvvfxvideosuperres\\bin\\nvVFXVideoSuperRes.dll"})
        if (!LoadLibraryExW((bin + d).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
            printf("cannot load %ls (%lu)\n", d, GetLastError());
            return 1;
        }
    unsigned ver = 0;
    NvVFX_GetVersion(&ver);
    printf("VFX SDK %u.%u.%u\n", ver >> 24, (ver >> 16) & 255, (ver >> 8) & 255);
    NvVFX_ConfigureLogger(NVCV_LOG_WARNING, "stderr", nullptr, nullptr);

    std::vector<unsigned char> in((size_t)w * h * 4), out((size_t)ow * oh * 4);
    FILE* f = _wfopen(argv[2], L"rb");
    if (!f || fread(in.data(), 1, in.size(), f) != in.size()) return 3;
    fclose(f);

    NvVFX_Handle eff = nullptr;
    CK(NvVFX_CreateEffect("VideoSuperRes", &eff));
    CUstream stream = nullptr;
    CK(NvVFX_CudaStreamCreate(&stream));
    CK(NvVFX_SetCudaStream(eff, NVVFX_CUDA_STREAM, stream));
    CK(NvVFX_SetU32(eff, "QualityLevel", q));
    if (strength >= 0) CK(NvVFX_SetF32(eff, NVVFX_STRENGTH, strength));

    NvCVImage srcCpu, dstCpu, srcGpu, dstGpu, tmp;
    CK(NvCVImage_Init(&srcCpu, w, h, w * 4, in.data(), NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU));
    CK(NvCVImage_Init(&dstCpu, ow, oh, ow * 4, out.data(), NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU));
    CK(NvCVImage_Alloc(&srcGpu, w, h, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 32));
    CK(NvCVImage_Alloc(&dstGpu, ow, oh, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 32));
    CK(NvVFX_SetImage(eff, NVVFX_INPUT_IMAGE, &srcGpu));
    CK(NvVFX_SetImage(eff, NVVFX_OUTPUT_IMAGE, &dstGpu));

    auto t0 = std::chrono::steady_clock::now();
    CK(NvVFX_Load(eff));
    auto t1 = std::chrono::steady_clock::now();
    printf("load %.0f ms\n", std::chrono::duration<double, std::milli>(t1 - t0).count());

    CK(NvCVImage_Transfer(&srcCpu, &srcGpu, 1.f, stream, &tmp));
    CK(NvVFX_Run(eff, 0));  // warm-up
    CK(NvVFX_CudaStreamSynchronize(stream));
    const int runs = 60;
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < runs; ++i) CK(NvVFX_Run(eff, 1));
    CK(NvVFX_CudaStreamSynchronize(stream));
    t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / runs;
    printf("run %dx%d -> %dx%d q%u: %.2f ms/frame (%.0f fps)\n", w, h, ow, oh, q, ms, 1000 / ms);

    CK(NvCVImage_Transfer(&dstGpu, &dstCpu, 1.f, stream, &tmp));
    CK(NvVFX_CudaStreamSynchronize(stream));
    f = _wfopen(argv[8], L"wb");
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    NvVFX_DestroyEffect(eff);
    NvVFX_CudaStreamDestroy(stream);
    return 0;
}
