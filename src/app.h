// Shared declarations for RTXPlayer's modules.
//
//   main.cpp         window layout, mpv setup and event loop, click/fullscreen handling
//   util.cpp         string/path helpers, the runtime-loaded libmpv API, settings.ini
//   enhance.cpp      super resolution / frame interpolation planning and monitoring
//   controlbar.cpp   PotPlayer-style control bar (docked + fullscreen overlay)
//   menu.cpp         right-click menu and command dispatch
//   settingsdlg.cpp  "增强设置" dialog for choosing algorithms
#pragma once

#include <windows.h>

#include <cstdint>
#include <initializer_list>
#include <string>

#include <mpv/client.h>

// ---------------------------------------------------------------------------
// Helpers (util.cpp)

std::string ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);
std::wstring AppDir();
const std::wstring& DataDir();
bool FileExists(const std::wstring& p);
std::string Fmt(const char* fmt, ...);
std::wstring WFmt(const wchar_t* fmt, ...);
std::string MpvQuote(const std::string& v);  // mpv option-list escaping for paths
std::wstring FormatTime(double seconds);       // h:mm:ss / mm:ss
int Scale(int px, UINT dpi);

// ---------------------------------------------------------------------------
// libmpv (util.cpp). The DLL is loaded at runtime: the dev package only ships a MinGW import lib.

extern mpv_handle* g_mpv;
bool LoadMpv(const std::wstring& dllPath);
mpv_handle* MpvCreate();
int MpvInitialize();
void MpvTerminate();
void MpvSetOption(const char* name, const std::string& value);
void MpvObserve(const char* name, mpv_format format);
mpv_event* MpvWaitEvent(double timeout);
void MpvSetWakeup(void (*cb)(void*));

void Cmd(std::initializer_list<const char*> args);
void SetProp(const char* name, const std::string& value);
double GetDouble(const char* name, double def = 0.0);
int64_t GetInt(const char* name, int64_t def = 0);
bool GetFlag(const char* name);
std::string GetString(const char* name);
void Osd(const std::string& text, int ms = 4000);
void Log(const std::string& text);  // goes to logs\mpv.log

// ---------------------------------------------------------------------------
// Settings (util.cpp), stored in <DataDir>\settings.ini

enum class Mode { Auto, On, Off };
enum class SrTarget { Auto, K2, K4 };
enum class SrAlgo { Maxine, Vsr, Fsr, Nis, Anime4k, Anime4kHq, Lanczos };
constexpr int kSrAlgoCount = 7;
constexpr int kInterpAlgoCount = 5;  // see kInterpAlgoKeys

extern const wchar_t* const kSrAlgoNames[kSrAlgoCount];
extern const wchar_t* const kSrAlgoShort[kSrAlgoCount];
extern const char* const kInterpAlgoKeys[kInterpAlgoCount];  // quality, balanced, fast, fruc, blend
extern const wchar_t* const kInterpAlgoNames[kInterpAlgoCount];
extern const int kFpsOptions[6];
constexpr int kTargetDouble = -2;  // interpolation target "source frame rate x2" (30 -> 60, 24 -> 48)
extern const double kSpeedOptions[11];

struct Settings {
    // Enhancement
    Mode srMode = Mode::Auto;
    SrAlgo srAlgo = SrAlgo::Maxine;
    SrTarget srTarget = SrTarget::Auto;
    int maxineQuality = 0;               // Maxine VSR QualityLevel 1-4, 0 = automatic
    std::wstring vfxSdk;                 // NVIDIA Video Effects SDK folder ("" = search)
    Mode interpMode = Mode::Auto;
    std::string interpAlgo = "quality";  // RIFE model key, "fruc" (NVIDIA optical flow) or "blend"
    int targetFps = kTargetDouble;       // fixed fps, kTargetDouble (source x2) or 0 (display refresh)
    std::string backend = "trt";         // trt: classic TensorRT, rtx: TensorRT for RTX
    std::wstring frucDll;                // NvOFFRUC.dll from the user's Optical Flow SDK ("" = search)
    int delaySec = 3;
    // Player
    int volume = 100;
    bool resume = true;  // remember playback position per file

    bool SrToTarget() const { return srAlgo == SrAlgo::Maxine || srAlgo == SrAlgo::Vsr; }  // uses srTarget
    bool IsBlend() const { return interpAlgo == "blend"; }
    bool IsFruc() const { return interpAlgo == "fruc"; }
    bool IsRife() const { return !IsBlend() && !IsFruc(); }
    void Load();
    void Save();

private:
    std::wstring path_;
};

extern Settings g_set;

// ---------------------------------------------------------------------------
// Windows (main.cpp)

extern HWND g_hwnd;   // main window
extern HWND g_video;  // child that hosts mpv's video window
extern bool g_fullscreen;

void SetFullscreen(bool fs);
void ToggleTopmost();
bool IsTopmost();
void FocusVideo();  // give keyboard focus back to mpv
void LoadFile(const std::wstring& path, bool append = false);
void PlaylistStep(int dir);  // previous (-1) / next (+1) file, remembering the position
void SavePosition();         // write (or clear, near the end) the resume point
void OpenFileDialog();
void OpenFolderDialog();
void OpenSubtitleDialog();
void LayoutWindows();

// ---------------------------------------------------------------------------
// Enhancement (enhance.cpp)

enum : UINT_PTR { TIMER_ACTIVATE = 1, TIMER_MONITOR = 2, TIMER_WARMUP = 3 };

void EnhanceStartup();                 // first-run warm-up kick-off
void EnhanceOnStartFile();
void EnhanceOnFileLoaded();
void EnhanceOnEndFile();
bool EnhanceOnTimer(UINT_PTR id);      // true if the timer was ours
void EnhanceReadDefaults();            // remember mpv defaults we later override
void ToggleEnhancement();
bool EnhancementDisabled();
void ShowEnhanceStatus();
void OnSettingsChanged();              // save + re-plan the current file
void RebuildEngine();
std::string EngineKey(const std::string& model, const std::string& backend);
bool EngineReady(const std::string& key);
bool WarmupRunning();
bool ReadBench(const std::string& key, double& fps720, double& fps1080);
// NvOFFRUC.dll is not redistributable: it is used from the user's NVIDIA Optical
// Flow SDK. Returns the configured or discovered path, "" if none.
std::wstring FindFrucDll(bool rescan = false);
// NVIDIA Video Effects (Maxine VFX) SDK folder with the Video Super Resolution
// feature installed, from settings or a search of the local drives; "" if none.
std::wstring FindVfxSdk(bool rescan = false);

enum class BadgeState { Idle, Off, Waiting, Active };
std::wstring EnhanceBadge(BadgeState& state);

// ---------------------------------------------------------------------------
// UI (controlbar.cpp, menu.cpp, settingsdlg.cpp)

HWND CreateControlBar(HWND parent, bool overlay);
int ControlBarHeight(UINT dpi);
void ControlBarRefresh();
void UpdateFullscreenBar();            // show/hide the overlay bar from the cursor position
void HideFullscreenBar();

void ShowMainMenu(const POINT* at = nullptr);
void ShowSpeedMenu(POINT at);
void OnCommand(UINT id);
void ShowHotkeys();

bool ShowSettingsDialog(HWND owner);
