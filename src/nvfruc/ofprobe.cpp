// Development probe (not shipped): runs the NVOFA optical flow engine directly on
// two synthetic frames (second = first shifted left by `shift` px) and prints the
// median flow, to tell a broken driver/OF path from a FRUC library problem.
//   ofprobe [shift] [both]
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "cuda.h"
#include "nvOpticalFlowCuda.h"

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

#define CHECK(x)                                                     \
    do {                                                             \
        int _r = (int)(x);                                           \
        if (_r) {                                                    \
            char msg[256] = {};                                      \
            uint32_t sz = sizeof(msg);                               \
            if (of.nvOFGetLastError && h) of.nvOFGetLastError(h, msg, &sz); \
            printf("FAIL %s -> %d %s\n", #x, _r, msg);               \
            return 1;                                                \
        }                                                            \
    } while (0)

int main(int argc, char** argv) {
    int shift = argc > 1 ? atoi(argv[1]) : 8;
    bool both = argc > 2 && strcmp(argv[2], "both") == 0;
    const int w = 1920, h_ = 1080, grid = 4;

    HMODULE c = LoadLibraryW(L"nvcuda.dll");
    auto cuInit_ = (CUresult(CUDAAPI*)(unsigned))GetProcAddress(c, "cuInit");
    auto cuDeviceGet_ = (CUresult(CUDAAPI*)(CUdevice*, int))GetProcAddress(c, "cuDeviceGet");
    auto cuCtxCreate_ = (CUresult(CUDAAPI*)(CUcontext*, unsigned, CUdevice))GetProcAddress(c, "cuCtxCreate_v2");
    auto cuMemcpy2D_ = (CUresult(CUDAAPI*)(const CUDA_MEMCPY2D*))GetProcAddress(c, "cuMemcpy2D_v2");
    auto cuCtxSynchronize_ = (CUresult(CUDAAPI*)())GetProcAddress(c, "cuCtxSynchronize");
    CUdevice dev;
    CUcontext ctx;
    cuInit_(0);
    cuDeviceGet_(&dev, 0);
    cuCtxCreate_(&ctx, 0, dev);

    HMODULE ofdll = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ofdll) {
        printf("nvofapi64.dll not found\n");
        return 1;
    }
    auto create = (NV_OF_STATUS(NVOFAPI*)(uint32_t, NV_OF_CUDA_API_FUNCTION_LIST*))GetProcAddress(
        ofdll, "NvOFAPICreateInstanceCuda");
    NV_OF_CUDA_API_FUNCTION_LIST of{};
    NvOFHandle h = nullptr;
    CHECK(create(NV_OF_API_VERSION, &of));
    CHECK(of.nvCreateOpticalFlowCuda(ctx, &h));

    NV_OF_INIT_PARAMS ip{};
    ip.width = w;
    ip.height = h_;
    ip.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
    ip.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
    ip.mode = NV_OF_MODE_OPTICALFLOW;
    ip.perfLevel = NV_OF_PERF_LEVEL_SLOW;
    ip.predDirection = both ? NV_OF_PRED_DIRECTION_BOTH : NV_OF_PRED_DIRECTION_FORWARD;
    ip.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
    CHECK(of.nvOFInit(h, &ip));

    NV_OF_BUFFER_DESCRIPTOR inDesc{(uint32_t)w, (uint32_t)h_, NV_OF_BUFFER_USAGE_INPUT, NV_OF_BUFFER_FORMAT_GRAYSCALE8};
    NV_OF_BUFFER_DESCRIPTOR outDesc{(uint32_t)(w / grid), (uint32_t)(h_ / grid), NV_OF_BUFFER_USAGE_OUTPUT,
                                    NV_OF_BUFFER_FORMAT_SHORT2};
    NvOFGPUBufferHandle in0, in1, out, bwd = nullptr;
    CHECK(of.nvOFCreateGPUBufferCuda(h, &inDesc, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, &in0));
    CHECK(of.nvOFCreateGPUBufferCuda(h, &inDesc, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, &in1));
    CHECK(of.nvOFCreateGPUBufferCuda(h, &outDesc, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, &out));
    if (both) CHECK(of.nvOFCreateGPUBufferCuda(h, &outDesc, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, &bwd));

    auto upload = [&](NvOFGPUBufferHandle b, int off) {
        std::vector<uint8_t> img((size_t)w * h_);
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w; ++x) img[(size_t)y * w + x] = Pattern(x + off, y);
        NV_OF_CUDA_BUFFER_STRIDE_INFO si{};
        of.nvOFGPUBufferGetStrideInfo(b, &si);
        CUDA_MEMCPY2D m{};
        m.srcMemoryType = CU_MEMORYTYPE_HOST;
        m.srcHost = img.data();
        m.srcPitch = w;
        m.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        m.dstDevice = of.nvOFGPUBufferGetCUdeviceptr(b);
        m.dstPitch = si.strideInfo[0].strideXInBytes;
        m.WidthInBytes = w;
        m.Height = h_;
        return cuMemcpy2D_(&m);
    };
    CHECK(upload(in0, 0));      // reference? see below: input = current frame
    CHECK(upload(in1, shift));  // content moves left by `shift`

    NV_OF_EXECUTE_INPUT_PARAMS ei{};
    ei.inputFrame = in1;
    ei.referenceFrame = in0;
    ei.disableTemporalHints = NV_OF_TRUE;
    NV_OF_EXECUTE_OUTPUT_PARAMS eo{};
    eo.outputBuffer = out;
    eo.bwdOutputBuffer = bwd;
    CHECK(of.nvOFExecute(h, &ei, &eo));
    CHECK(cuCtxSynchronize_());

    auto median = [&](NvOFGPUBufferHandle b, const char* name) {
        NV_OF_CUDA_BUFFER_STRIDE_INFO si{};
        of.nvOFGPUBufferGetStrideInfo(b, &si);
        int ow = w / grid, oh = h_ / grid;
        std::vector<NV_OF_FLOW_VECTOR> v((size_t)ow * oh);
        CUDA_MEMCPY2D m{};
        m.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        m.srcDevice = of.nvOFGPUBufferGetCUdeviceptr(b);
        m.srcPitch = si.strideInfo[0].strideXInBytes;
        m.dstMemoryType = CU_MEMORYTYPE_HOST;
        m.dstHost = v.data();
        m.dstPitch = ow * sizeof(NV_OF_FLOW_VECTOR);
        m.WidthInBytes = ow * sizeof(NV_OF_FLOW_VECTOR);
        m.Height = oh;
        cuMemcpy2D_(&m);
        std::vector<float> fx, fy;
        for (auto& f : v) {
            fx.push_back(f.flowx / 32.f);
            fy.push_back(f.flowy / 32.f);
        }
        std::nth_element(fx.begin(), fx.begin() + fx.size() / 2, fx.end());
        std::nth_element(fy.begin(), fy.begin() + fy.size() / 2, fy.end());
        printf("%s flow median: x %.2f  y %.2f px (expected x %+d)\n", name, fx[fx.size() / 2], fy[fy.size() / 2],
               name[0] == 'f' ? shift : -shift);
    };
    median(out, "forward");
    if (both) median(bwd, "backward");
    return 0;
}
