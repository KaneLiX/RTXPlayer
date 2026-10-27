// String/path helpers, the runtime-loaded libmpv API and settings.ini.
#include "app.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <type_traits>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring AppDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    PathRemoveFileSpecW(buf);
    return buf;
}

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Where settings, the TensorRT engine cache, logs and screenshots live.
// A "portable" marker file next to the exe keeps everything in the program
// folder; installed copies use %LOCALAPPDATA%\RTXPlayer, since the install
// folder (e.g. Program Files) is not writable for the user.
const std::wstring& DataDir() {
    static const std::wstring dir = [] {
        std::wstring app = AppDir();
        if (FileExists(app + L"\\portable")) return app;
        std::wstring d = app;
        PWSTR base = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) d = std::wstring(base) + L"\\RTXPlayer";
        CoTaskMemFree(base);
        CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }();
    return dir;
}

std::string Fmt(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

std::wstring WFmt(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

std::string MpvQuote(const std::string& v) { return "%" + std::to_string(v.size()) + "%" + v; }

std::wstring FormatTime(double seconds) {
    if (!(seconds >= 0)) seconds = 0;
    long long t = (long long)seconds;
    int h = (int)(t / 3600), m = (int)(t / 60 % 60), s = (int)(t % 60);
    return h ? WFmt(L"%d:%02d:%02d", h, m, s) : WFmt(L"%02d:%02d", m, s);
}

int Scale(int px, UINT dpi) { return MulDiv(px, (int)dpi, 96); }

// ---------------------------------------------------------------------------
// libmpv

namespace {

struct MpvApi {
    HMODULE dll = nullptr;
    decltype(&mpv_create) create;
    decltype(&mpv_initialize) initialize;
    decltype(&mpv_terminate_destroy) terminate_destroy;
    decltype(&mpv_set_option_string) set_option_string;
    decltype(&mpv_set_property_string) set_property_string;
    decltype(&mpv_get_property) get_property;
    decltype(&mpv_get_property_string) get_property_string;
    decltype(&mpv_observe_property) observe_property;
    decltype(&mpv_command) command;
    decltype(&mpv_wait_event) wait_event;
    decltype(&mpv_set_wakeup_callback) set_wakeup_callback;
    decltype(&mpv_free) free;
};

MpvApi api;

}  // namespace

mpv_handle* g_mpv = nullptr;

bool LoadMpv(const std::wstring& dllPath) {
    api.dll = LoadLibraryW(dllPath.c_str());
    if (!api.dll) return false;
    bool ok = true;
    auto get = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(api.dll, name));
        ok = ok && fn;
    };
    get(api.create, "mpv_create");
    get(api.initialize, "mpv_initialize");
    get(api.terminate_destroy, "mpv_terminate_destroy");
    get(api.set_option_string, "mpv_set_option_string");
    get(api.set_property_string, "mpv_set_property_string");
    get(api.get_property, "mpv_get_property");
    get(api.get_property_string, "mpv_get_property_string");
    get(api.observe_property, "mpv_observe_property");
    get(api.command, "mpv_command");
    get(api.wait_event, "mpv_wait_event");
    get(api.set_wakeup_callback, "mpv_set_wakeup_callback");
    get(api.free, "mpv_free");
    return ok;
}

mpv_handle* MpvCreate() { return g_mpv = api.create(); }
int MpvInitialize() { return api.initialize(g_mpv); }

void MpvTerminate() {
    if (!g_mpv) return;
    mpv_handle* h = g_mpv;
    g_mpv = nullptr;
    api.terminate_destroy(h);
}

void MpvSetOption(const char* name, const std::string& value) { api.set_option_string(g_mpv, name, value.c_str()); }
void MpvObserve(const char* name, mpv_format format) { api.observe_property(g_mpv, 0, name, format); }
mpv_event* MpvWaitEvent(double timeout) { return api.wait_event(g_mpv, timeout); }
void MpvSetWakeup(void (*cb)(void*)) { api.set_wakeup_callback(g_mpv, cb, nullptr); }

void Cmd(std::initializer_list<const char*> args) {
    if (!g_mpv) return;
    std::vector<const char*> v(args);
    v.push_back(nullptr);
    api.command(g_mpv, v.data());
}

void SetProp(const char* name, const std::string& value) {
    if (g_mpv) api.set_property_string(g_mpv, name, value.c_str());
}

double GetDouble(const char* name, double def) {
    double v = def;
    if (!g_mpv || api.get_property(g_mpv, name, MPV_FORMAT_DOUBLE, &v) < 0) return def;
    return v;
}

int64_t GetInt(const char* name, int64_t def) {
    int64_t v = def;
    if (!g_mpv || api.get_property(g_mpv, name, MPV_FORMAT_INT64, &v) < 0) return def;
    return v;
}

bool GetFlag(const char* name) {
    int v = 0;
    if (g_mpv) api.get_property(g_mpv, name, MPV_FORMAT_FLAG, &v);
    return v != 0;
}

std::string GetString(const char* name) {
    if (!g_mpv) return {};
    char* s = api.get_property_string(g_mpv, name);
    if (!s) return {};
    std::string r = s;
    api.free(s);
    return r;
}

void Osd(const std::string& text, int ms) { Cmd({"show-text", text.c_str(), std::to_string(ms).c_str()}); }

void Log(const std::string& text) { Cmd({"print-text", ("[rtxp] " + text).c_str()}); }

// ---------------------------------------------------------------------------
// Settings

const wchar_t* const kSrAlgoNames[kSrAlgoCount] = {
    L"NVIDIA Maxine VSR · AI 超分 (推荐，需 Video Effects SDK)",
    L"NVIDIA RTX Video Super Resolution (驱动内置)",
    L"AMD FidelityFX Super Resolution (FSR 1.0)",
    L"NVIDIA Image Scaling (NIS)",
    L"Anime4K 动漫 (快速)",
    L"Anime4K 动漫 (高画质)",
    L"传统缩放 EWA Lanczos",
};
const wchar_t* const kSrAlgoShort[kSrAlgoCount] = {L"Maxine VSR", L"RTX VSR", L"FSR", L"NIS", L"Anime4K", L"Anime4K HQ", L"Lanczos"};
static const wchar_t* const kSrAlgoKeys[kSrAlgoCount] = {L"maxine", L"vsr", L"fsr", L"nis", L"anime4k", L"anime4k_hq", L"lanczos"};

const char* const kInterpAlgoKeys[kInterpAlgoCount] = {"quality", "balanced", "fast", "fruc", "blend"};
const wchar_t* const kInterpAlgoNames[kInterpAlgoCount] = {
    L"RIFE 4.26 · AI 插帧 (画质优先)",
    L"RIFE 4.25 lite · AI 插帧 (均衡)",
    L"RIFE 4.22 lite · AI 插帧 (速度优先)",
    L"NVIDIA 光流 FRUC · 硬件光流插帧 (极快，固定 ×2)",
    L"帧混合 (不生成新画面，最省资源)",
};

const int kFpsOptions[6] = {60, 90, 120, 144, 165, 240};
const double kSpeedOptions[11] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 4.0};

Settings g_set;

namespace {

Mode ParseMode(const std::wstring& v) { return v == L"on" ? Mode::On : v == L"off" ? Mode::Off : Mode::Auto; }
const wchar_t* ModeStr(Mode m) { return m == Mode::On ? L"on" : m == Mode::Off ? L"off" : L"auto"; }

std::wstring ReadIni(const std::wstring& path, const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[1024];
    GetPrivateProfileStringW(sec, key, def, buf, 1024, path.c_str());
    return buf;
}

}  // namespace

void Settings::Load() {
    path_ = DataDir() + L"\\settings.ini";
    auto r = [&](const wchar_t* key, const wchar_t* def) { return ReadIni(path_, L"enhance", key, def); };

    srMode = ParseMode(r(L"sr_mode", L"auto"));
    std::wstring a = r(L"sr_algo", L"maxine");
    srAlgo = SrAlgo::Maxine;
    for (int i = 0; i < kSrAlgoCount; ++i)
        if (a == kSrAlgoKeys[i]) srAlgo = (SrAlgo)i;
    std::wstring t = r(L"sr_target", L"auto");
    srTarget = t == L"2k" ? SrTarget::K2 : t == L"4k" ? SrTarget::K4 : SrTarget::Auto;

    interpMode = ParseMode(r(L"interp_mode", L"auto"));
    // interp_algo replaced the older "model" key (quality/fast).
    std::string ia = ToUtf8(r(L"interp_algo", r(L"model", L"quality").c_str()));
    interpAlgo = "quality";
    for (const char* k : kInterpAlgoKeys)
        if (ia == k) interpAlgo = k;
    std::wstring f = r(L"target_fps", L"x2");
    targetFps = f == L"display" ? 0 : f == L"x2" ? kTargetDouble : std::clamp(_wtoi(f.c_str()), 30, 480);
    backend = r(L"backend", L"trt") == L"rtx" ? "rtx" : "trt";
    delaySec = std::clamp(_wtoi(r(L"delay_sec", L"3").c_str()), 0, 60);
    frucDll = r(L"fruc_dll", L"");
    maxineQuality = std::clamp(_wtoi(r(L"maxine_quality", L"0").c_str()), 0, 4);
    vfxSdk = r(L"vfx_sdk", L"");

    volume = std::clamp(_wtoi(ReadIni(path_, L"player", L"volume", L"100").c_str()), 0, 130);
    resume = ReadIni(path_, L"player", L"resume", L"1") != L"0";
}

void Settings::Save() {
    auto w = [&](const wchar_t* sec, const wchar_t* k, const std::wstring& v) {
        WritePrivateProfileStringW(sec, k, v.c_str(), path_.c_str());
    };
    w(L"enhance", L"sr_mode", ModeStr(srMode));
    w(L"enhance", L"sr_algo", kSrAlgoKeys[(int)srAlgo]);
    w(L"enhance", L"sr_target", srTarget == SrTarget::K2 ? L"2k" : srTarget == SrTarget::K4 ? L"4k" : L"auto");
    w(L"enhance", L"interp_mode", ModeStr(interpMode));
    w(L"enhance", L"interp_algo", FromUtf8(interpAlgo));
    w(L"enhance", L"target_fps", targetFps == 0              ? std::wstring(L"display")
                                 : targetFps == kTargetDouble ? std::wstring(L"x2")
                                                              : std::to_wstring(targetFps));
    w(L"enhance", L"backend", FromUtf8(backend));
    w(L"enhance", L"delay_sec", std::to_wstring(delaySec));
    w(L"enhance", L"fruc_dll", frucDll);
    w(L"enhance", L"maxine_quality", std::to_wstring(maxineQuality));
    w(L"enhance", L"vfx_sdk", vfxSdk);
    WritePrivateProfileStringW(L"enhance", L"model", nullptr, path_.c_str());  // superseded by interp_algo
    w(L"player", L"volume", std::to_wstring(volume));
    w(L"player", L"resume", resume ? L"1" : L"0");
}
