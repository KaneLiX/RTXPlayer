// Super resolution / frame interpolation: planning, applying and monitoring.
//
// Playback always starts untouched; enhancement is switched on a few seconds
// later so opening a file stays instant.
//
//   * Super resolution, chosen algorithm:
//       RTX VSR  - NVIDIA RTX Video Super Resolution through mpv's d3d11vpp filter;
//                  upscales the decoded frames to 2K/4K.
//       FSR/NIS/Anime4K - GLSL shaders applied while rendering to the window size.
//       Lanczos  - mpv's ewa_lanczossharp scaler.
//   * Frame interpolation, chosen algorithm:
//       RIFE 4.26 / 4.25 lite / 4.22 lite on TensorRT (VapourSynth, scripts/rtx_interp.vpy).
//         The script reports its real-time throughput via RtxpReportFps(); if the
//         GPU cannot keep up, RIFE drops to a 720p intermediate (the SR stage scales
//         it back up) and then to a lower target frame rate.
//       NVIDIA FRUC - Optical Flow SDK frame rate up-conversion (hardware optical
//         flow + warping, plugins\nvfruc.dll), always x2 at native resolution.
//         NvOFFRUC.dll comes from the user's own SDK copy (FindFrucDll).
//       Frame blending - mpv display-resample + interpolation (no AI, no new frames).
#include "app.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>

// ---------------------------------------------------------------------------
// Interpolation throughput reported by the VapourSynth script (via ctypes).

static std::atomic<double> g_reportedFps{0.0};
static std::atomic<ULONGLONG> g_reportedAt{0};

extern "C" __declspec(dllexport) void RtxpReportFps(double fps) {
    g_reportedFps.store(fps);
    g_reportedAt.store(GetTickCount64());
}

// Share of FRUC in-between frames the library replaced with the previous frame
// (its fallback when it cannot interpolate a frame pair well).
static std::atomic<double> g_frucRepeat{-1.0};

extern "C" __declspec(dllexport) void RtxpReportFruc(double share) { g_frucRepeat.store(share); }

// ---------------------------------------------------------------------------
// Engine warm-up (builds the TensorRT engine for a model/backend once, then benchmarks)

std::string EngineKey(const std::string& model, const std::string& backend) { return model + "." + backend; }

bool EngineReady(const std::string& key) {
    return FileExists(DataDir() + L"\\engines\\" + FromUtf8(key) + L".ready");
}

bool ReadBench(const std::string& key, double& fps720, double& fps1080) {
    std::wstring ini = DataDir() + L"\\engines\\bench.ini";
    std::wstring sec = FromUtf8(key);
    wchar_t a[64], b[64];
    GetPrivateProfileStringW(sec.c_str(), L"fps720", L"", a, 64, ini.c_str());
    GetPrivateProfileStringW(sec.c_str(), L"fps1080", L"", b, 64, ini.c_str());
    if (!a[0] || !b[0]) return false;
    fps720 = _wtof(a);
    fps1080 = _wtof(b);
    return fps720 > 0 && fps1080 > 0;
}

// RIFE inferences per second at a processing height, interpolated in 1/pixel-count
// space. The benchmark measures output fps for 24 -> 120, where 4 of every 5 output
// frames are inferred; source frames pass through almost for free.
static double InferenceRate(const std::string& key, int h) {
    double cap720 = 100, cap1080 = 50;
    ReadBench(key, cap720, cap1080);
    double t720 = 1.0 / cap720, t1080 = 1.0 / cap1080;
    double k = (double(h) * h - 720.0 * 720.0) / (1080.0 * 1080.0 - 720.0 * 720.0);
    double t = t720 + k * (t1080 - t720);
    return (t > 0 ? 1.0 / t : cap720 * 3) * 0.8;
}

// Can RIFE reach `target` from `fps` at processing height h? Only the new frames cost inference.
// share: the part of the GPU left to RIFE (Maxine VSR runs on every output frame too).
static bool RifeKeepsUp(const std::string& key, int h, double fps, int target, double share = 1.0) {
    const double margin = 1.08;  // headroom for decoding + SR on the same GPU
    return (target - fps) * margin <= InferenceRate(key, h) * share;
}

// GPU share left to RIFE while Maxine VSR also runs (measured on an RTX 4060 Ti:
// RIFE 4.26 at 1080p fell ~7% short of 30->60 next to Maxine at 1440p).
constexpr double kShareWithMaxine = 0.7;

// ---------------------------------------------------------------------------
// NVIDIA Optical Flow SDK FRUC

std::wstring FindFrucDll(bool rescan) {
    static std::wstring cached;
    static bool scanned = false;
    if (!g_set.frucDll.empty() && FileExists(g_set.frucDll)) return g_set.frucDll;
    if (scanned && !rescan) return cached;
    scanned = true;
    cached.clear();
    const std::wstring dll = L"\\NvOFFRUC.dll";
    const std::wstring inSdk = L"\\NvOFFRUC\\NvOFFRUCSample\\bin\\win64" + dll;
    // The SDK's bin\win64 files copied into an "nvfruc" folder next to the program or the user data.
    for (const std::wstring& d : {AppDir() + L"\\nvfruc", DataDir() + L"\\nvfruc"})
        if (FileExists(d + dll)) return cached = d + dll;
    // An unpacked SDK ("Optical_Flow_SDK_x.y.z") at a drive root or one folder below it.
    auto probe = [&](const std::wstring& dir) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\Optical_Flow_SDK*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return false;
        do {
            std::wstring p = dir + L"\\" + fd.cFileName + inSdk;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && FileExists(p)) cached = p;
        } while (cached.empty() && FindNextFileW(h, &fd));
        FindClose(h);
        return !cached.empty();
    };
    DWORD drives = GetLogicalDrives();
    for (wchar_t c = L'C'; c <= L'Z'; ++c) {
        std::wstring root = std::wstring(1, c) + L":";
        if (!(drives & (1u << (c - L'A'))) || GetDriveTypeW((root + L"\\").c_str()) != DRIVE_FIXED) continue;
        if (probe(root)) return cached;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' ||
                (fd.dwFileAttributes & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN)))
                continue;
            if (probe(root + L"\\" + fd.cFileName)) break;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (!cached.empty()) return cached;
    }
    return cached;
}

// ---------------------------------------------------------------------------
// NVIDIA Video Effects (Maxine VFX) SDK

static bool VfxSdkUsable(const std::wstring& root) {
    const std::wstring feature = L"\\nvvfxvideosuperres\\bin\\nvVFXVideoSuperRes.dll";
    return FileExists(root + L"\\bin\\NVVideoEffects.dll") &&
           (FileExists(root + L"\\features" + feature) || FileExists(root + L"\\.." + feature));
}

std::wstring FindVfxSdk(bool rescan) {
    static std::wstring cached;
    static bool scanned = false;
    if (!g_set.vfxSdk.empty() && VfxSdkUsable(g_set.vfxSdk)) return g_set.vfxSdk;
    if (scanned && !rescan) return cached;
    scanned = true;
    cached.clear();
    for (const std::wstring& d : {AppDir() + L"\\VideoFX", DataDir() + L"\\VideoFX"})
        if (VfxSdkUsable(d)) return cached = d;
    // A "VideoFX" folder (the SDK's own name) up to three levels below a drive root,
    // e.g. F:\APP\Optical_Flow_SDK_5.0.7\VideoFX.
    auto visit = [&](auto& self, const std::wstring& dir, int depth) -> bool {
        if (VfxSdkUsable(dir + L"\\VideoFX")) {
            cached = dir + L"\\VideoFX";
            return true;
        }
        if (depth >= 2) return false;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return false;
        bool found = false;
        do {
            std::wstring name = fd.cFileName;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name[0] == L'.' || name[0] == L'$' ||
                (fd.dwFileAttributes & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                (depth == 0 && (_wcsicmp(name.c_str(), L"Windows") == 0 || _wcsicmp(name.c_str(), L"ProgramData") == 0)))
                continue;
            found = self(self, dir + L"\\" + name, depth + 1);
        } while (!found && FindNextFileW(h, &fd));
        FindClose(h);
        return found;
    };
    DWORD drives = GetLogicalDrives();
    for (wchar_t c = L'C'; c <= L'Z' && cached.empty(); ++c) {
        std::wstring root = std::wstring(1, c) + L":";
        if ((drives & (1u << (c - L'A'))) && GetDriveTypeW((root + L"\\").c_str()) == DRIVE_FIXED)
            visit(visit, root, 0);
    }
    if (!cached.empty()) {  // remember it: no rescan on the next start
        g_set.vfxSdk = cached;
        g_set.Save();
    }
    return cached;
}

static HANDLE g_warmupProc = nullptr;

// True while any warmup.py runs (ours or one left over from a previous player instance).
bool WarmupRunning() {
    if (g_warmupProc) return true;
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\RTXPlayerWarmup");
    if (!m) return false;
    CloseHandle(m);
    return true;
}

// Builds + benchmarks one engine ("<model>.<backend>") in the background.
static void StartWarmup(const std::string& key, bool force) {
    if (WarmupRunning()) return;
    std::wstring dir = AppDir(), data = DataDir();
    if (!force && EngineReady(key)) return;
    if (force) DeleteFileW((data + L"\\engines\\" + FromUtf8(key) + L".ready").c_str());
    CreateDirectoryW((data + L"\\logs").c_str(), nullptr);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log = CreateFileW((data + L"\\logs\\warmup.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = log;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + dir + L"\\vs\\python.exe\" -I \"" + dir + L"\\scripts\\warmup.py\" " + FromUtf8(key);
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                       nullptr, dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        g_warmupProc = pi.hProcess;
    }
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
}

// ---------------------------------------------------------------------------
// Planning

namespace {

struct Plan {
    // Frame interpolation
    bool interp = false;   // RIFE in the VapourSynth filter
    bool blend = false;    // mpv frame blending (display-resample)
    int target = 0;        // requested target fps (RIFE)
    int procH = 0;         // RIFE processing height, 0 = native
    std::string model;
    std::string backend;
    // Super resolution
    SrAlgo srAlgo = SrAlgo::Vsr;
    bool srWanted = false; // super resolution requested (vs. only restoring a 720p intermediate)
    bool vsr = false;      // d3d11vpp RTX VSR stage present
    double vsrScale = 1.0;
    int frameH = 0;        // height leaving the filter chain before VSR
    int outH = 0;          // height after VSR
    std::string shaders;   // glsl-shaders list for FSR / NIS / Anime4K
    bool lanczos = false;
    bool maxine = false;   // Maxine VSR in the VapourSynth filter (after interpolation)
    bool maxineYields = false;  // Maxine would push RIFE below native resolution: use the driver VSR
    int maxineQ = 0;       // its QualityLevel
    std::string srNote;    // e.g. why Maxine fell back to the driver's VSR

    bool Active() const { return interp || blend || vsr || !shaders.empty() || lanczos || maxine; }
    bool UsesVapourSynth() const { return interp || maxine; }
};

struct FileState {
    bool active = false;       // a file is loaded
    bool applied = false;      // enhancement is in place
    bool userDisabled = false; // Ctrl+E
    bool waitingEngine = false;
    int w = 0, h = 0;
    double fps = 0;
    bool fullRange = false;  // source colour levels / matrix, passed to the interpolation script
    int matrix = 0;          // 1 = BT.709, 6 = BT.601, 9 = BT.2020 NCL, 0 = guess from size
    Plan plan;
    ULONGLONG appliedAt = 0;
    int slowTicks = 0;
    int fpsRetries = 0;
    std::string algoOverride;  // per-file interpolation algorithm (FRUC -> RIFE fallback)
    std::string backendOverride;
    int frucBadTicks = 0;      // consecutive seconds in which FRUC mostly repeated frames
    int maxineQCap = 0;        // Maxine quality limit after a downgrade (0 = none)
    int64_t lastDrops = -1;    // vo frame-drop counter at the previous monitor tick
};

FileState g_file;
std::string g_defaultScale = "lanczos";

// Last value we set per mpv property. Re-setting vf or hwdec to the same value
// would still rebuild the filter chain / decoder (a visible hitch), e.g. when
// only the SR shader changes, so unchanged values are skipped.
std::map<std::string, std::string> g_setValues;

bool SetIfChanged(const char* name, const std::string& value) {
    auto it = g_setValues.find(name);
    if (it != g_setValues.end() && it->second == value) return false;
    g_setValues[name] = value;
    SetProp(name, value);
    return true;
}

const wchar_t* ModelName(const std::string& m) {
    return m == "balanced" ? L"RIFE 4.25 lite"
         : m == "fast"     ? L"RIFE 4.22 lite"
         : m == "fruc"     ? L"NVIDIA FRUC"
                           : L"RIFE 4.26";
}

int MonitorHeight() {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcMonitor.bottom - mi.rcMonitor.top;
}

int ResolveTargetFps(double srcFps) {
    if (g_set.targetFps > 0) return g_set.targetFps;
    // x2 uses the nominal rate: 23.976 -> 48, 29.97 -> 60, and 30.303 (MKV ms rounding) -> 60.
    if (g_set.targetFps == kTargetDouble) return 2 * std::max(1, (int)std::lround(srcFps));
    double d = GetDouble("display-fps", 60.0);
    return std::max(60, (int)std::lround(d));
}

// Output rate the script will actually produce: NTSC sources (23.976, 29.97) run
// at 1000/1001 of the target (59.94). Other small deviations are just container
// timestamp rounding (e.g. 30.303 for 30 fps in millisecond-based MKV).
double ExpectedOutFps(const Plan& p, double srcFps) {
    if (p.model == "fruc") return 2 * srcFps;  // always exactly x2
    double nominal = std::round(srcFps);
    bool ntsc = std::fabs(srcFps - nominal) > 0.01 && std::fabs(srcFps * 1.001 - nominal) < 0.02;
    return ntsc ? p.target / 1.001 : p.target;
}

std::string ShaderList(SrAlgo algo) {
    std::string dir = ToUtf8(AppDir() + L"\\shaders\\");
    std::replace(dir.begin(), dir.end(), '\\', '/');
    auto join = [&](std::initializer_list<const char*> files) {
        std::string s;
        for (const char* f : files) s += (s.empty() ? "" : ";") + dir + f;  // ';' = path-list separator on Windows
        return s;
    };
    switch (algo) {
    case SrAlgo::Fsr: return join({"FSR.glsl"});
    case SrAlgo::Nis: return join({"NVScaler.glsl"});
    case SrAlgo::Anime4k:  // official Anime4K "Mode A (Fast)"
        return join({"Anime4K_Clamp_Highlights.glsl", "Anime4K_Restore_CNN_M.glsl", "Anime4K_Upscale_CNN_x2_M.glsl",
                     "Anime4K_AutoDownscalePre_x2.glsl", "Anime4K_AutoDownscalePre_x4.glsl",
                     "Anime4K_Upscale_CNN_x2_S.glsl"});
    case SrAlgo::Anime4kHq:  // official Anime4K "Mode A (HQ)"
        return join({"Anime4K_Clamp_Highlights.glsl", "Anime4K_Restore_CNN_VL.glsl", "Anime4K_Upscale_CNN_x2_VL.glsl",
                     "Anime4K_AutoDownscalePre_x2.glsl", "Anime4K_AutoDownscalePre_x4.glsl",
                     "Anime4K_Upscale_CNN_x2_M.glsl"});
    default: return {};
    }
}

// Decide the VSR / shader stage once the RIFE processing height is known.
void PlanSuperResolution(Plan& p, int h, int srOutH) {
    p.frameH = p.interp ? (p.procH ? p.procH : h) : h;
    p.vsr = false;
    p.vsrScale = 1.0;
    p.outH = p.frameH;
    p.shaders.clear();
    p.lanczos = false;
    p.maxine = false;
    p.srNote.clear();

    if (p.srAlgo == SrAlgo::Maxine && p.maxineYields) {
        p.srAlgo = SrAlgo::Vsr;  // falls through to the driver's VSR below
        if (p.srWanted) p.srNote = "RIFE 原分辨率插帧已用满显卡，本片超分改用驱动内置 RTX VSR";
    }
    if (p.srAlgo == SrAlgo::Maxine) {
        std::wstring sdk = FindVfxSdk();
        if (sdk.empty()) {
            p.srAlgo = SrAlgo::Vsr;  // falls through to the driver's VSR below
            if (p.srWanted) p.srNote = "未找到 NVIDIA Video Effects SDK (VSR 功能)，改用驱动内置 RTX VSR";
        } else {
            SetEnvironmentVariableW(L"RTXP_VFX_SDK", sdk.c_str());  // read by plugins\nvvfx.dll
            int finalH = p.srWanted ? srOutH : h;
            if (finalH > p.frameH) {
                p.maxine = true;
                p.outH = finalH;
                // Automatic: High, or Medium while RIFE shares the GPU (both run on every output frame).
                bool rife = p.interp && p.model != "fruc";
                p.maxineQ = g_set.maxineQuality ? g_set.maxineQuality : rife ? 2 : 3;
                if (p.maxineQ > 4) p.maxineQ = 4;
                if (g_file.maxineQCap && p.maxineQ > g_file.maxineQCap) p.maxineQ = g_file.maxineQCap;
            }
            return;
        }
    }

    if (p.srAlgo == SrAlgo::Vsr) {
        // VSR upscales to the target, or restores a 720p RIFE intermediate to the source height.
        int finalH = p.srWanted ? srOutH : h;
        double scale = double(finalH) / p.frameH;
        if (scale > 1.01) {
            p.vsr = true;
            p.vsrScale = std::min(scale, 4.0);
            p.outH = (int)std::lround(p.frameH * p.vsrScale);
        }
    } else if (p.srWanted) {
        // Render-time upscalers scale straight to the window size (also covering a 720p intermediate).
        if (p.srAlgo == SrAlgo::Lanczos) p.lanczos = true;
        else p.shaders = ShaderList(p.srAlgo);
    }
}

int SrOutHeight() {
    return g_set.srTarget == SrTarget::K2 ? 1440 : g_set.srTarget == SrTarget::K4 ? 2160
                                                 : (MonitorHeight() >= 1600 ? 2160 : 1440);
}

Plan ComputePlan(int h, double fps, std::string& note) {
    Plan p;
    const std::string algo = g_file.algoOverride.empty() ? g_set.interpAlgo : g_file.algoOverride;
    p.model = algo;
    p.backend = g_file.backendOverride.empty() ? g_set.backend : g_file.backendOverride;
    p.srAlgo = g_set.srAlgo;

    // Super resolution decision
    int srOutH = SrOutHeight();
    bool srWanted = g_set.srMode == Mode::On || (g_set.srMode == Mode::Auto && h <= 1080);
    if (g_set.SrToTarget() && srOutH <= h) srWanted = false;  // already at/above the target
    p.srWanted = srWanted;

    // Interpolation decision
    bool interp = g_set.interpMode == Mode::On || (g_set.interpMode == Mode::Auto && fps > 0 && fps <= 30.5);
    if (algo == "blend") {
        p.blend = interp && fps > 0;
        interp = false;
    }

    if (algo == "fruc") {
        // Hardware optical flow, always x2 at native resolution: no engine, no downgrade ladder.
        p.target = 2 * std::max(1, (int)std::lround(fps));
        std::wstring dll = interp ? FindFrucDll() : std::wstring();
        if (interp && fps <= 0) interp = false;
        if (interp && dll.empty()) {
            interp = false;
            note = "未找到 NVIDIA Optical Flow SDK 的 NvOFFRUC.dll，请在 视频增强设置 中指定位置";
        }
        if (interp) SetEnvironmentVariableW(L"RTXP_FRUC_DLL", dll.c_str());  // read by plugins\nvfruc.dll
        p.interp = interp;
        PlanSuperResolution(p, h, srOutH);
        return p;
    }

    int target = ResolveTargetFps(fps);
    if (interp && (fps <= 0 || target <= fps * 1.2)) interp = false;

    const std::string key = EngineKey(p.model, p.backend);
    // Hold RIFE while an engine is being built/benchmarked: it needs the GPU to itself.
    if (interp && (!EngineReady(key) || WarmupRunning())) {
        interp = false;
        g_file.waitingEngine = true;
        note = "插帧引擎正在为本机显卡优化 (约 1–2 分钟)，完成后自动启用";
        StartWarmup(key, false);
        SetTimer(g_hwnd, TIMER_WARMUP, 1000, nullptr);
    }

    if (interp) {
        bool maxine = g_set.srAlgo == SrAlgo::Maxine && srWanted && !FindVfxSdk().empty();
        double share = maxine ? kShareWithMaxine : 1.0;
        int nativeH = std::min(h, 1080);
        if (RifeKeepsUp(key, nativeH, fps, target, share)) {
            p.procH = nativeH == h ? 0 : nativeH;
        } else if (maxine && RifeKeepsUp(key, nativeH, fps, target)) {
            // RIFE fits at native resolution only without Maxine. Keeping every source
            // frame at full resolution matters more than the sharper upscaler.
            p.procH = nativeH == h ? 0 : nativeH;
            p.maxineYields = true;
        } else {
            int procH = std::min(h, 720);
            p.procH = procH == h ? 0 : procH;
            if (!RifeKeepsUp(key, procH, fps, target, share)) {
                int best = 0;
                for (int opt : kFpsOptions)
                    if (opt > fps * 1.2 && opt < target && RifeKeepsUp(key, procH, fps, opt, share))
                        best = std::max(best, opt);
                if (best == 0) {
                    interp = false;
                    note = "GPU 性能不足以插帧到 60fps 以上，已跳过插帧";
                } else {
                    note = Fmt("目标 %d fps 超出本机能力，已调整为 %d fps (720p 中间帧)", target, best);
                    target = best;
                }
            }
        }
    }
    p.interp = interp;
    p.target = target;
    PlanSuperResolution(p, h, srOutH);
    return p;
}

std::string BuildVf(const Plan& p) {
    std::string vf;
    if (p.UsesVapourSynth()) {
        // t0 = no interpolation (Maxine VSR only); u = Maxine output height (0 = none), q = its quality.
        std::string script = ToUtf8(AppDir() + L"\\scripts\\rtx_interp.vpy");
        std::replace(script.begin(), script.end(), '\\', '/');
        vf += "vapoursynth=file=" + MpvQuote(script) +
              Fmt(":user-data=t%d-m%s-p%d-b%s-s%d-r%d-x%d-u%d-q%d:buffered-frames=12:concurrent-frames=8",
                  p.interp ? p.target : 0, p.model.c_str(), p.procH, p.backend.c_str(),
                  (int)std::lround(g_file.fps * 1000), g_file.fullRange ? 1 : 0, g_file.matrix,
                  p.maxine ? p.outH : 0, p.maxineQ);
    }
    if (p.vsr) {
        if (!vf.empty()) vf += ",";
        vf += Fmt("d3d11vpp=scaling-mode=nvidia:scale=%.4f", p.vsrScale);
    }
    return vf;
}

std::string PlanText(const Plan& p, int h, double fps) {
    std::string s;
    std::string algo = ToUtf8(kSrAlgoShort[(int)p.srAlgo]);
    if (p.maxine)
        s += Fmt("超分辨率: Maxine VSR %dp → %dp · 画质 %d", p.srWanted ? h : p.frameH, p.outH, p.maxineQ);
    else if (p.vsr && p.srWanted)
        s += Fmt("超分辨率: RTX VSR %dp → %dp", h, p.outH);
    else if (p.vsr)
        s += Fmt("RTX VSR 还原: %dp → %dp", p.frameH, p.outH);
    else if (!p.shaders.empty() || p.lanczos)
        s += Fmt("超分辨率: %s %dp → 窗口尺寸", algo.c_str(), p.frameH);
    else
        s += Fmt("超分辨率: 未启用 (%dp)", h);
    s += "\n";
    if (p.interp)
        s += Fmt("插帧: %s %.3f → %.2f fps · %s · %s", ToUtf8(ModelName(p.model)).c_str(), fps,
                 ExpectedOutFps(p, fps), p.procH ? Fmt("%dp 中间帧", p.procH).c_str() : "原分辨率",
                 p.model == "fruc" ? "硬件光流" : p.backend == "rtx" ? "TensorRT-RTX" : "TensorRT");
    else if (p.blend)
        s += Fmt("插帧: 帧混合 %.3f fps → 显示器 %.0f Hz", fps, GetDouble("display-fps"));
    else
        s += Fmt("插帧: 未启用 (%.3f fps)", fps);
    return s;
}

void ClearEnhancement() {
    SetIfChanged("vf", "");
    SetIfChanged("hwdec", "d3d11va");
    SetIfChanged("glsl-shaders", "");
    SetIfChanged("scale", g_defaultScale);
    SetIfChanged("video-sync", "audio");
    SetIfChanged("interpolation", "no");
    g_file.applied = false;
    KillTimer(g_hwnd, TIMER_MONITOR);
}

void ApplyPlan(const Plan& p, const std::string& note) {
    g_file.plan = p;
    g_reportedFps = 0;
    g_frucRepeat = -1.0;
    // VapourSynth needs system-memory frames; without it keep zero-copy decoding.
    bool rebuilt = SetIfChanged("hwdec", p.UsesVapourSynth() ? "d3d11va-copy" : "d3d11va");
    rebuilt |= SetIfChanged("vf", BuildVf(p));
    // A new filter chain can take seconds to start (TensorRT-RTX compiles, Maxine loads)
    // while audio plays on; the video then trails the audio for good, as the filter
    // cannot run faster than real time. An exact seek to the current position makes
    // mpv hold the audio until the first new frame is ready: a pause instead of drift.
    if (rebuilt && p.UsesVapourSynth()) {
        double pos = GetDouble("time-pos", -1);
        if (pos >= 0) Cmd({"seek", Fmt("%.3f", pos).c_str(), "absolute+exact"});
    }
    SetIfChanged("glsl-shaders", p.shaders);
    SetIfChanged("scale", p.lanczos ? "ewa_lanczossharp" : g_defaultScale);
    if (p.blend) SetIfChanged("tscale", "oversample");
    SetIfChanged("video-sync", p.blend ? "display-resample" : "audio");
    SetIfChanged("interpolation", p.blend ? "yes" : "no");
    g_file.applied = true;
    g_file.appliedAt = GetTickCount64();
    g_file.slowTicks = 0;
    g_file.lastDrops = -1;
    if (p.interp) SetTimer(g_hwnd, TIMER_MONITOR, 1000, nullptr);
    else KillTimer(g_hwnd, TIMER_MONITOR);

    std::string msg = PlanText(p, g_file.h, g_file.fps);
    if (!p.srNote.empty()) msg += "\n" + p.srNote;
    if (!note.empty()) msg += "\n" + note;
    Osd(msg, 5000);
    Log("apply: " + msg + " | vf=" + BuildVf(p) + " | shaders=" + p.shaders);
}

void Activate() {
    KillTimer(g_hwnd, TIMER_ACTIVATE);
    if (!g_file.active || g_file.userDisabled) return;
    g_file.w = (int)GetInt("width");
    g_file.h = (int)GetInt("height");
    if (g_file.w <= 0 || g_file.h <= 0) {
        if (GetString("current-tracks/video/id").empty()) return;  // audio-only
        SetTimer(g_hwnd, TIMER_ACTIVATE, 500, nullptr);           // not decoded yet
        return;
    }
    // The source frame rate is read once per file: when re-planning later (settings
    // changed, downgrade) the decoder may be restarting and our own filters are in
    // the chain, so container-fps can be missing and estimated-vf-fps is our output.
    if (g_file.fps <= 0) {
        g_file.fps = GetDouble("container-fps");
        if (g_file.fps <= 0 && !g_file.applied) g_file.fps = GetDouble("estimated-vf-fps");
        if (g_file.fps <= 0 && ++g_file.fpsRetries < 6) {
            SetTimer(g_hwnd, TIMER_ACTIVATE, 500, nullptr);  // not known yet
            return;
        }
        // Decoder-side colour parameters (video-params is before our filters).
        g_file.fullRange = GetString("video-params/colorlevels") == "full";
        std::string m = GetString("video-params/colormatrix");
        g_file.matrix = m == "bt.709" ? 1 : m == "bt.601" ? 6 : m.rfind("bt.2020", 0) == 0 ? 9 : 0;
    }
    g_file.waitingEngine = false;
    std::string note;
    Plan p = ComputePlan(g_file.h, g_file.fps, note);
    if (!p.Active()) {
        ClearEnhancement();
        g_file.plan = p;
        Osd(PlanText(p, g_file.h, g_file.fps) + (note.empty() ? "" : "\n" + note), 3000);
        return;
    }
    ApplyPlan(p, note);
}

void ScheduleActivation(int delayMs) {
    KillTimer(g_hwnd, TIMER_ACTIVATE);
    SetTimer(g_hwnd, TIMER_ACTIVATE, std::max(delayMs, 50), nullptr);
}

// Called every second while RIFE runs: step down if it cannot keep up.
void Monitor() {
    if (!g_file.applied || !g_file.plan.interp) return;
    ULONGLONG now = GetTickCount64();
    bool paused = GetFlag("pause") || GetFlag("seeking") || GetFlag("paused-for-cache");
    // At non-1x speed the required throughput changes temporarily; don't downgrade for that.
    bool offSpeed = std::fabs(GetDouble("speed", 1.0) - 1.0) > 0.05;
    bool fruc = g_file.plan.model == "fruc";
    ULONGLONG grace = fruc ? 5000 : g_file.plan.backend == "rtx" ? 16000 : 8000;  // TensorRT-RTX JIT-compiles on load
    // While warmup.py benchmarks on the same GPU, slowness is temporary: don't downgrade.
    if (paused || offSpeed || WarmupRunning() || now - g_file.appliedAt < grace) {
        g_file.slowTicks = 0;
        return;
    }
    // FRUC copies the previous frame when it cannot interpolate a pair well. If it
    // does that most of the time (grainy/noisy video), interpolation is lost: use
    // RIFE for the rest of this file, with whichever engine is already built.
    double repeated = g_frucRepeat.load();
    g_file.frucBadTicks = fruc && repeated >= 0.5 ? g_file.frucBadTicks + 1 : 0;
    if (g_file.frucBadTicks >= 5) {
        g_file.frucBadTicks = 0;
        // Classic TensorRT first: it starts in ~0.5 s, TensorRT-RTX stalls ~7 s compiling.
        for (const char* m : {"quality", "balanced", "fast"})
            for (const char* b : {"trt", "rtx"})
                if (g_file.algoOverride.empty() && EngineReady(EngineKey(m, b))) {
                    g_file.algoOverride = m;
                    g_file.backendOverride = b;
                }
        if (g_file.algoOverride.empty()) {
            Osd(Fmt("FRUC 对此视频大多无法插帧 (%.0f%% 为重复帧)，建议改用 RIFE", repeated * 100), 6000);
            g_file.frucBadTicks = -100000;  // say it once per file
            return;
        }
        std::string note;
        Plan p = ComputePlan(g_file.h, g_file.fps, note);
        std::string why = Fmt("FRUC 对此视频大多无法插帧 (%.0f%% 为重复帧)，本文件改用 %s", repeated * 100,
                              ToUtf8(ModelName(g_file.algoOverride)).c_str());
        ApplyPlan(p, note.empty() ? why : why + "\n" + note);
        return;
    }

    double measured = (now - g_reportedAt.load() < 3000) ? g_reportedFps.load() : 0.0;
    double expected = ExpectedOutFps(g_file.plan, g_file.fps);
    // Slightly short of the target still adds up: video falls behind the audio and
    // mpv drops frames to catch up. Count that as too slow as well.
    double avsync = GetDouble("avsync");
    int64_t drops = GetInt("frame-drop-count");
    bool dropping = g_file.lastDrops >= 0 && drops - g_file.lastDrops > 3;
    g_file.lastDrops = drops;
    bool slow = measured < expected * 0.90 || std::fabs(avsync) > 0.25 || dropping;
    g_file.slowTicks = slow ? g_file.slowTicks + 1 : 0;
    Log(Fmt("monitor: measured %.1f fps, expected %.1f, slow ticks %d | avsync %.3f s, vo drops %lld, dec drops %lld"
            "%s",
            measured, expected, g_file.slowTicks, avsync, (long long)drops,
            (long long)GetInt("decoder-frame-drop-count"),
            fruc ? Fmt(" | fruc repeated %.0f%%", std::max(0.0, g_frucRepeat.load()) * 100).c_str() : ""));
    if (g_file.slowTicks < 3) return;

    Plan p = g_file.plan;
    std::string note;
    if (p.maxine && p.maxineQ > 1) {
        // Cheapest visible step first: Maxine VSR runs on every output frame.
        g_file.maxineQCap = 1;
        note = Fmt("实时处理跟不上 (%.0f/%.0f fps)，Maxine 超分画质降为 1", measured, expected);
    } else if (fruc) {
        // FRUC has no smaller setting to fall back to.
        p.interp = false;
        note = Fmt("FRUC 插帧跟不上 (%.0f/%.0f fps)，已关闭插帧", measured, expected);
    } else if (p.procH != 720 && g_file.h > 720) {
        p.procH = 720;
        note = Fmt("实时插帧跟不上 (%.0f/%.0f fps)，改用 720p 中间帧", measured, expected);
    } else {
        // Jump straight to the highest rate the measured throughput supports;
        // fall back to one step down if even that guess is unavailable.
        int lower = 0, stepDown = 0;
        for (int opt : kFpsOptions) {
            if (opt >= p.target || opt <= g_file.fps * 1.2) continue;
            stepDown = std::max(stepDown, opt);
            if (opt <= measured * 0.97) lower = std::max(lower, opt);
        }
        if (!lower && measured >= 0.9 * kFpsOptions[0]) lower = stepDown;
        if (lower) {
            note = Fmt("实时插帧跟不上 (%.0f/%.0f fps)，目标降为 %d fps", measured, expected, lower);
            p.target = lower;
        } else {
            p.interp = false;
            note = "GPU 负载过高，已关闭插帧";
        }
    }
    PlanSuperResolution(p, g_file.h, SrOutHeight());
    ApplyPlan(p, note);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points

void EnhanceReadDefaults() {
    std::string s = GetString("scale");
    if (!s.empty()) g_defaultScale = s;
}

void EnhanceStartup() {
    if (!g_set.IsRife()) return;
    StartWarmup(EngineKey(g_set.interpAlgo, g_set.backend), false);
    if (WarmupRunning()) {
        SetTimer(g_hwnd, TIMER_WARMUP, 1000, nullptr);
        Osd("首次运行：正在为本机显卡优化 RTX 插帧引擎 (约 2 分钟后插帧可用)…\n期间可正常播放，超分辨率已可用", 8000);
    }
}

void EnhanceOnStartFile() {
    // Every file starts untouched; enhancement is decided after the delay.
    KillTimer(g_hwnd, TIMER_ACTIVATE);
    ClearEnhancement();
    g_file = FileState{false, false, g_file.userDisabled};
}

void EnhanceOnFileLoaded() {
    g_file.active = true;
    if (!g_file.userDisabled) ScheduleActivation(g_set.delaySec * 1000);
}

void EnhanceOnEndFile() {
    KillTimer(g_hwnd, TIMER_ACTIVATE);
    KillTimer(g_hwnd, TIMER_MONITOR);
}

bool EnhanceOnTimer(UINT_PTR id) {
    switch (id) {
    case TIMER_ACTIVATE: Activate(); return true;
    case TIMER_MONITOR: Monitor(); return true;
    case TIMER_WARMUP:
        // Reap our warm-up process first so WarmupRunning() is accurate below.
        if (g_warmupProc && WaitForSingleObject(g_warmupProc, 0) == WAIT_OBJECT_0) {
            DWORD code = 1;
            GetExitCodeProcess(g_warmupProc, &code);
            CloseHandle(g_warmupProc);
            g_warmupProc = nullptr;
            if (code != 0) {
                g_file.waitingEngine = false;
                Osd("插帧引擎优化失败，详见 logs\\warmup.log");
            }
        }
        if (g_file.waitingEngine && EngineReady(EngineKey(g_set.interpAlgo, g_set.backend)) && !WarmupRunning()) {
            g_file.waitingEngine = false;
            Osd("RTX 插帧引擎已就绪");
            if (g_file.active && !g_file.userDisabled) ScheduleActivation(0);
        }
        if (!WarmupRunning() && !g_file.waitingEngine) KillTimer(g_hwnd, TIMER_WARMUP);
        return true;
    }
    return false;
}

void ToggleEnhancement() {
    g_file.userDisabled = !g_file.userDisabled;
    if (g_file.userDisabled) {
        KillTimer(g_hwnd, TIMER_ACTIVATE);
        ClearEnhancement();
        Osd("视频增强: 已关闭 (Ctrl+E 重新开启)");
    } else if (g_file.active) {
        Osd("视频增强: 开启中…", 1500);
        ScheduleActivation(0);
    }
}

bool EnhancementDisabled() { return g_file.userDisabled; }

void ShowEnhanceStatus() {
    if (!g_file.active) {
        Osd("未打开文件");
        return;
    }
    std::string s;
    if (g_file.applied) {
        s = PlanText(g_file.plan, g_file.h, g_file.fps);
        if (g_file.plan.interp) {
            double m = (GetTickCount64() - g_reportedAt.load() < 3000) ? g_reportedFps.load() : 0.0;
            s += Fmt("\n实时输出: %.1f fps", m);
            if (g_file.plan.model == "fruc" && g_frucRepeat.load() >= 0)
                s += Fmt(" · FRUC 无法插帧而重复上一帧: %.0f%%", g_frucRepeat.load() * 100);
        }
    } else if (g_file.userDisabled) {
        s = "视频增强: 已手动关闭 (Ctrl+E 开启)";
    } else if (g_file.waitingEngine) {
        s = "插帧引擎优化中，完成后自动启用";
    } else {
        s = "视频增强: 等待启用…";
    }
    s += Fmt("\n源: %dx%d @ %.3f fps · 显示器刷新 %.0f Hz", g_file.w, g_file.h, g_file.fps, GetDouble("display-fps"));
    Osd(s, 6000);
}

void OnSettingsChanged() {
    g_set.Save();
    g_file.algoOverride.clear();  // an explicit choice replaces the automatic FRUC fallback
    g_file.backendOverride.clear();
    g_file.maxineQCap = 0;
    if (g_file.active && !g_file.userDisabled) ScheduleActivation(0);
}

void RebuildEngine() {
    if (!g_set.IsRife()) {
        Osd("当前插帧算法不使用 TensorRT 引擎，无需优化");
        return;
    }
    // Interpolation pauses while the engine is rebuilt so the benchmark gets the GPU alone.
    ClearEnhancement();
    StartWarmup(EngineKey(g_set.interpAlgo, g_set.backend), true);
    g_file.waitingEngine = g_file.active;
    SetTimer(g_hwnd, TIMER_WARMUP, 1000, nullptr);
    if (g_file.active && !g_file.userDisabled) ScheduleActivation(0);  // keeps SR on meanwhile
    Osd("正在重新优化插帧引擎并测速 (约 1–2 分钟，期间可正常播放)…");
}

std::wstring EnhanceBadge(BadgeState& state) {
    if (!g_file.active) {
        state = BadgeState::Idle;
        return L"视频增强";
    }
    if (g_file.userDisabled) {
        state = BadgeState::Off;
        return L"增强: 关";
    }
    const Plan& p = g_file.plan;
    std::wstring sr, fi;
    if (g_file.applied) {
        if (p.vsr || !p.shaders.empty() || p.lanczos || p.maxine) sr = kSrAlgoShort[(int)p.srAlgo];
        if (p.interp) fi = WFmt(L"%.0f fps", ExpectedOutFps(p, g_file.fps));
        else if (p.blend) fi = L"帧混合";
    }
    if (g_file.waitingEngine && fi.empty()) fi = L"引擎优化中";
    if (sr.empty() && fi.empty()) {
        state = g_file.applied ? BadgeState::Idle : BadgeState::Waiting;
        return g_file.applied ? L"增强: 未启用" : L"增强: 等待启用";
    }
    state = g_file.waitingEngine ? BadgeState::Waiting : BadgeState::Active;
    if (!sr.empty() && !fi.empty()) return sr + L" · " + fi;
    return sr.empty() ? L"插帧 " + fi : L"超分 " + sr;
}
