// RTXPlayer - a libmpv based video player with NVIDIA RTX enhancement
// (super resolution + AI frame interpolation, see enhance.cpp) and a
// PotPlayer-style UI: control bar, right-click menu and PotPlayer hotkeys.
//
// Window layout: the main window holds a video host child (mpv renders into it
// via --wid) and the docked control bar below it. In fullscreen the video fills
// the screen and an overlay bar appears when the cursor nears the bottom edge.
#include "app.h"

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>
#include <vector>

HWND g_hwnd = nullptr;
HWND g_video = nullptr;
bool g_fullscreen = false;

namespace {

const UINT WM_MPV_WAKEUP = WM_APP + 1;
enum : UINT_PTR { TIMER_CLICK = 20, TIMER_FSBAR = 21 };

HWND g_bar = nullptr;    // docked control bar
HWND g_fsBar = nullptr;  // fullscreen overlay bar
WINDOWPLACEMENT g_wp{sizeof(g_wp)};
bool g_sizedForFile = false;
ULONGLONG g_lastDblClick = 0;

void UpdateTitle() {
    std::wstring t = L"RTXPlayer";
    std::string media = GetString("media-title");
    if (!media.empty() && !GetFlag("idle-active")) t = FromUtf8(media) + L" - RTXPlayer";
    SetWindowTextW(g_hwnd, t.c_str());
}

// Fit the window to the video's display size (max 85% of the work area), once per file.
void FitWindowToVideo(int dw, int dh) {
    if (g_fullscreen || IsZoomed(g_hwnd) || dw <= 0 || dh <= 0) return;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    UINT dpi = GetDpiForWindow(g_hwnd);
    int barH = ControlBarHeight(dpi);
    int aw = mi.rcWork.right - mi.rcWork.left, ah = mi.rcWork.bottom - mi.rcWork.top;
    double s = std::min({1.0, aw * 0.85 / dw, (ah * 0.85 - barH) / dh});
    RECT rc{0, 0, (LONG)(dw * s), (LONG)(dh * s) + barH};
    AdjustWindowRectExForDpi(&rc, GetWindowLongW(g_hwnd, GWL_STYLE), FALSE, 0, dpi);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    SetWindowPos(g_hwnd, nullptr, mi.rcWork.left + (aw - ww) / 2, mi.rcWork.top + (ah - wh) / 2, ww, wh,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

std::wstring PickFile(const wchar_t* filter, bool multi, std::vector<std::wstring>& out) {
    std::vector<wchar_t> buf(65536, L'\0');
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER | (multi ? OFN_ALLOWMULTISELECT : 0);
    if (!GetOpenFileNameW(&ofn)) return {};
    std::wstring first = buf.data();
    const wchar_t* p = buf.data() + first.size() + 1;
    if (!*p) {
        out.push_back(first);
        return first;
    }
    for (; *p; p += wcslen(p) + 1) out.push_back(first + L"\\" + p);  // first = folder when several are picked
    return first;
}

// PotPlayer-style key bindings. Rewritten on every start.
void WriteInputConf(const std::wstring& path) {
    const char* conf =
        "# RTXPlayer key bindings (PotPlayer style). This file is rewritten on every start.\n"
        "SPACE cycle pause\n"
        "MBTN_LEFT script-message rtxp-click\n"
        "MBTN_LEFT_DBL script-message rtxp-dblclick\n"
        "MBTN_RIGHT script-message rtxp-menu\n"
        "MBTN_MID cycle mute\n"
        "WHEEL_UP no-osd add volume 5; show-text \"音量: ${volume}%\"\n"
        "WHEEL_DOWN no-osd add volume -5; show-text \"音量: ${volume}%\"\n"
        "UP no-osd add volume 5; show-text \"音量: ${volume}%\"\n"
        "DOWN no-osd add volume -5; show-text \"音量: ${volume}%\"\n"
        "KP_UP no-osd add volume 5; show-text \"音量: ${volume}%\"\n"
        "KP_DOWN no-osd add volume -5; show-text \"音量: ${volume}%\"\n"
        "LEFT seek -10 exact\n"
        "RIGHT seek 10 exact\n"
        "KP_LEFT seek -10 exact\n"
        "KP_RIGHT seek 10 exact\n"
        "Ctrl+LEFT seek -30 exact\n"
        "Ctrl+RIGHT seek 30 exact\n"
        "Shift+LEFT seek -60 exact\n"
        "Shift+RIGHT seek 60 exact\n"
        "HOME seek 0 absolute\n"
        "PGUP script-message rtxp-prev\n"
        "PGDWN script-message rtxp-next\n"
        "ENTER cycle fullscreen\n"
        "KP_ENTER cycle fullscreen\n"
        "ESC set fullscreen no\n"
        "c no-osd add speed 0.1; show-text \"播放速度: ${speed}×\"\n"
        "x no-osd add speed -0.1; show-text \"播放速度: ${speed}×\"\n"
        "z no-osd set speed 1.0; show-text \"播放速度: ${speed}×\"\n"
        "m cycle mute\n"
        "f frame-step\n"
        "d frame-back-step\n"
        "s screenshot\n"
        "Ctrl+o script-message rtxp-open\n"
        "Ctrl+e script-message rtxp-toggle\n"
        "Ctrl+i script-message rtxp-info\n"
        "TAB script-message rtxp-info\n"
        "F5 script-message rtxp-settings\n"
        "q ignore\n"
        "Q ignore\n";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n;
    WriteFile(f, conf, (DWORD)strlen(conf), &n, nullptr);
    CloseHandle(f);
}

void HandleClientMessage(const std::string& a) {
    if (a == "rtxp-menu") ShowMainMenu(nullptr);
    else if (a == "rtxp-open") OpenFileDialog();
    else if (a == "rtxp-toggle") ToggleEnhancement();
    else if (a == "rtxp-info") ShowEnhanceStatus();
    else if (a == "rtxp-settings") ShowSettingsDialog(g_hwnd);
    else if (a == "rtxp-prev") PlaylistStep(-1);
    else if (a == "rtxp-next") PlaylistStep(1);
    else if (a == "rtxp-click") {
        // Single click pauses, but only once it is clear the click is not half of a
        // double click. mpv reports a double click as click, double-click, click.
        if (GetTickCount64() - g_lastDblClick > GetDoubleClickTime())
            SetTimer(g_hwnd, TIMER_CLICK, GetDoubleClickTime(), nullptr);
    } else if (a == "rtxp-dblclick") {
        g_lastDblClick = GetTickCount64();
        KillTimer(g_hwnd, TIMER_CLICK);
        if (GetFlag("idle-active")) OpenFileDialog();
        else SetFullscreen(!g_fullscreen);
    }
}

void HandleMpvEvents() {
    while (g_mpv) {
        mpv_event* ev = MpvWaitEvent(0);
        if (ev->event_id == MPV_EVENT_NONE) break;
        switch (ev->event_id) {
        case MPV_EVENT_SHUTDOWN:
            PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
            return;
        case MPV_EVENT_START_FILE:
            EnhanceOnStartFile();
            g_sizedForFile = false;
            Osd("", 1);  // drop the idle hint
            break;
        case MPV_EVENT_FILE_LOADED:
            EnhanceOnFileLoaded();
            UpdateTitle();
            break;
        case MPV_EVENT_END_FILE:
            EnhanceOnEndFile();
            break;
        case MPV_EVENT_CLIENT_MESSAGE: {
            auto* msg = (mpv_event_client_message*)ev->data;
            if (msg->num_args >= 1) HandleClientMessage(msg->args[0]);
            break;
        }
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto* prop = (mpv_event_property*)ev->data;
            std::string name = prop->name;
            if (name == "fullscreen" && prop->format == MPV_FORMAT_FLAG) {
                SetFullscreen(*(int*)prop->data != 0);
            } else if (name == "media-title") {
                UpdateTitle();
            } else if (name == "video-params/dw" && prop->format == MPV_FORMAT_INT64 && !g_sizedForFile) {
                int dw = (int)GetInt("video-params/dw"), dh = (int)GetInt("video-params/dh");
                if (dw > 0 && dh > 0) {
                    g_sizedForFile = true;
                    FitWindowToVideo(dw, dh);
                }
            } else if (name == "idle-active" && prop->format == MPV_FORMAT_FLAG && *(int*)prop->data) {
                UpdateTitle();
                Osd("RTXPlayer\n双击画面或 Ctrl+O 打开文件 · 也可直接拖入视频\n右键菜单 · F5 视频增强设置", 600000);
            }
            ControlBarRefresh();
            break;
        }
        default:
            break;
        }
    }
}

// Read by the Python scripts (in-process via mpv, and by warmup.py) and by mpv
// when it loads VSScript.dll. Set in the OS block before libmpv/Python load so
// their CRTs pick them up; child processes inherit them.
void SetupEnvironment() {
    std::wstring dir = AppDir();
    std::wstring vsscript = dir + L"\\vs\\VSScript.dll";
    const std::pair<const wchar_t*, std::wstring> vars[] = {
        {L"RTXP_ROOT", dir}, {L"RTXP_DATA", DataDir()}, {L"VSSCRIPT_PATH", vsscript}};
    for (const auto& [name, value] : vars) {
        SetEnvironmentVariableW(name, value.c_str());
        _wputenv_s(name, value.c_str());
    }
}

bool InitMpv() {
    std::wstring dir = AppDir(), data = DataDir();
    if (!LoadMpv(dir + L"\\libmpv-2.dll") || !MpvCreate()) return false;

    std::wstring conf = data + L"\\portable_config";
    CreateDirectoryW(conf.c_str(), nullptr);
    CreateDirectoryW((data + L"\\logs").c_str(), nullptr);
    WriteInputConf(conf + L"\\input.conf");

    auto opt = MpvSetOption;
    opt("wid", std::to_string((int64_t)(intptr_t)g_video));
    opt("config-dir", ToUtf8(conf));
    opt("config", "yes");
    opt("input-conf", ToUtf8(conf + L"\\input.conf"));
    opt("log-file", ToUtf8(data + L"\\logs\\mpv.log"));
    opt("vo", "gpu-next");
    opt("gpu-api", "d3d11");
    opt("hwdec", "d3d11va");
    opt("osc", "no");  // replaced by our control bar
    opt("osd-on-seek", "msg");
    opt("input-default-bindings", "yes");
    opt("input-vo-keyboard", "yes");
    opt("keep-open", "yes");
    opt("idle", "yes");
    opt("force-window", "yes");
    opt("osd-font", "Microsoft YaHei UI");
    opt("osd-font-size", "30");
    opt("screenshot-directory", ToUtf8(data + L"\\screenshots"));
    opt("volume", std::to_string(g_set.volume));
    opt("volume-max", "130");
    // Opening one file plays the folder's other videos next (PgUp / PgDn), like PotPlayer.
    opt("autocreate-playlist", "filter");
    opt("directory-filter-types", "video");
    // Resume where playback stopped; only position and track choices are remembered.
    opt("save-position-on-quit", g_set.resume ? "yes" : "no");
    opt("resume-playback", g_set.resume ? "yes" : "no");
    opt("watch-later-dir", ToUtf8(data + L"\\watch_later"));
    opt("watch-later-options", "start,aid,sid,sub-delay,audio-delay");

    if (MpvInitialize() < 0) return false;

    MpvObserve("fullscreen", MPV_FORMAT_FLAG);
    MpvObserve("media-title", MPV_FORMAT_STRING);
    MpvObserve("video-params/dw", MPV_FORMAT_INT64);
    MpvObserve("idle-active", MPV_FORMAT_FLAG);
    MpvObserve("pause", MPV_FORMAT_FLAG);
    MpvObserve("mute", MPV_FORMAT_FLAG);
    MpvObserve("speed", MPV_FORMAT_DOUBLE);
    MpvObserve("volume", MPV_FORMAT_DOUBLE);
    MpvSetWakeup([](void*) { PostMessageW(g_hwnd, WM_MPV_WAKEUP, 0, 0); });
    EnhanceReadDefaults();
    return true;
}

LRESULT CALLBACK VideoProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        // Keep mpv's own window covering the host.
        if (HWND child = GetWindow(hwnd, GW_CHILD)) MoveWindow(child, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MPV_WAKEUP:
        HandleMpvEvents();
        return 0;
    case WM_TIMER:
        if (EnhanceOnTimer(wp)) return 0;
        if (wp == TIMER_CLICK) {
            KillTimer(hwnd, TIMER_CLICK);
            if (!GetFlag("idle-active")) Cmd({"cycle", "pause"});
            ControlBarRefresh();
        } else if (wp == TIMER_FSBAR) {
            UpdateFullscreenBar();
        }
        return 0;
    case WM_COMMAND:
        OnCommand(LOWORD(wp));
        return 0;
    case WM_SIZE:
        LayoutWindows();
        return 0;
    case WM_GETMINMAXINFO: {
        UINT dpi = GetDpiForWindow(hwnd);
        auto* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize = {Scale(560, dpi), Scale(360, dpi)};
        return 0;
    }
    case WM_DPICHANGED: {
        auto* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutWindows();
        return 0;
    }
    case WM_SETFOCUS:
        FocusVideo();
        return 0;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t path[MAX_PATH * 4];
            DragQueryFileW(drop, i, path, (UINT)std::size(path));
            LoadFile(path, i > 0);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    case WM_CLOSE:
        if (g_mpv) {
            SavePosition();
            g_set.volume = (int)(GetDouble("volume", g_set.volume) + 0.5);
            g_set.Save();
            MpvTerminate();
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

// ---------------------------------------------------------------------------
// Window helpers used by the other modules

void LayoutWindows() {
    if (!g_hwnd) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int barH = g_fullscreen ? 0 : ControlBarHeight(GetDpiForWindow(g_hwnd));
    if (g_video) MoveWindow(g_video, 0, 0, rc.right, std::max(0, (int)rc.bottom - barH), TRUE);
    if (g_bar) {
        if (g_fullscreen) {
            ShowWindow(g_bar, SW_HIDE);
        } else {
            MoveWindow(g_bar, 0, rc.bottom - barH, rc.right, barH, TRUE);
            ShowWindow(g_bar, SW_SHOWNA);
        }
    }
}

void SetFullscreen(bool fs) {
    if (fs == g_fullscreen) return;
    DWORD style = GetWindowLongW(g_hwnd, GWL_STYLE);
    g_fullscreen = fs;
    if (fs) {
        GetWindowPlacement(g_hwnd, &g_wp);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(g_hwnd, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        SetTimer(g_hwnd, TIMER_FSBAR, 150, nullptr);
    } else {
        KillTimer(g_hwnd, TIMER_FSBAR);
        HideFullscreenBar();
        SetWindowLongW(g_hwnd, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_hwnd, &g_wp);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    LayoutWindows();
    if (GetFlag("fullscreen") != fs) SetProp("fullscreen", fs ? "yes" : "no");
    ControlBarRefresh();
    FocusVideo();
}

bool IsTopmost() { return (GetWindowLongW(g_hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0; }

void ToggleTopmost() {
    bool on = !IsTopmost();
    SetWindowPos(g_hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    Osd(on ? "窗口置顶: 开" : "窗口置顶: 关", 1500);
}

void FocusVideo() {
    HWND target = g_video ? GetWindow(g_video, GW_CHILD) : nullptr;
    if (!target) target = g_video;
    if (target && GetForegroundWindow() == g_hwnd) SetFocus(target);
}

void SavePosition() {
    if (!g_set.resume || GetFlag("idle-active")) return;
    double pos = GetDouble("time-pos", -1), dur = GetDouble("duration", 0);
    if (dur > 0 && pos > dur - 10) Cmd({"delete-watch-later-config"});  // finished: start over next time
    else if (pos > 5) Cmd({"write-watch-later-config"});
}

void LoadFile(const std::wstring& path, bool append) {
    if (!append) SavePosition();
    std::string p = ToUtf8(path);
    Cmd({"loadfile", p.c_str(), append ? "append-play" : "replace"});
}

void PlaylistStep(int dir) {
    SavePosition();
    Cmd({dir < 0 ? "playlist-prev" : "playlist-next"});
}

void OpenFileDialog() {
    std::vector<std::wstring> files;
    PickFile(L"视频文件\0*.mkv;*.mp4;*.avi;*.mov;*.wmv;*.flv;*.webm;*.ts;*.m2ts;*.mts;*.rmvb;*.rm;*.mpg;*.mpeg;*.m4v;"
             L"*.3gp;*.vob;*.ogv\0所有文件\0*.*\0",
             true, files);
    for (size_t i = 0; i < files.size(); ++i) LoadFile(files[i], i > 0);
}

void OpenSubtitleDialog() {
    std::vector<std::wstring> files;
    PickFile(L"字幕文件\0*.srt;*.ass;*.ssa;*.sub;*.idx;*.vtt;*.sup\0所有文件\0*.*\0", false, files);
    if (!files.empty()) {
        std::string p = ToUtf8(files[0]);
        Cmd({"sub-add", p.c_str(), "select"});
    }
}

void OpenFolderDialog() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"选择要播放的文件夹");
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->Show(g_hwnd)) && SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            LoadFile(path);  // mpv plays the folder's videos as a playlist
            CoTaskMemFree(path);
        }
        item->Release();
    }
    dlg->Release();
}

// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetupEnvironment();
    g_set.Load();
    g_set.Save();  // write defaults so the file is discoverable

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"RTXPlayerWindow";
    RegisterClassExW(&wc);

    WNDCLASSEXW vc{sizeof(vc)};
    vc.lpfnWndProc = VideoProc;
    vc.hInstance = inst;
    vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    vc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    vc.lpszClassName = L"RTXPlayerVideo";
    RegisterClassExW(&vc);

    g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"RTXPlayer",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr,
                             nullptr, inst, nullptr);
    g_video = CreateWindowExW(0, vc.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0,
                              100, 100, g_hwnd, nullptr, inst, nullptr);

    if (!InitMpv()) {
        MessageBoxW(g_hwnd, L"无法初始化播放核心 (libmpv-2.dll)。", L"RTXPlayer", MB_ICONERROR);
        return 1;
    }
    g_bar = CreateControlBar(g_hwnd, false);
    g_fsBar = CreateControlBar(g_hwnd, true);
    LayoutWindows();
    ShowWindow(g_hwnd, show);
    FocusVideo();

    EnhanceStartup();

    int argc = 0;
    if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) LoadFile(argv[i], i > 1);
        LocalFree(argv);
    }

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return 0;
}
