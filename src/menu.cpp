// Right-click menu (PotPlayer-like layout), speed popup and command dispatch.
#include "app.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

enum : UINT {
    ID_OPEN = 100, ID_OPEN_FOLDER, ID_OPEN_SUB, ID_PLAYPAUSE, ID_STOP, ID_PREV, ID_NEXT, ID_BACK10, ID_FWD10,
    ID_SPEED_UP, ID_SPEED_DOWN, ID_SPEED_RESET, ID_MUTE, ID_VOL_UP, ID_VOL_DOWN, ID_SUB_OFF, ID_FULLSCREEN,
    ID_TOPMOST, ID_SCREENSHOT, ID_SETTINGS, ID_TOGGLE, ID_INFO, ID_REBUILD, ID_RESUME, ID_HOTKEYS, ID_EXIT,
    ID_SR_AUTO = 200, ID_SR_ON, ID_SR_OFF, ID_SRT_AUTO, ID_SRT_2K, ID_SRT_4K,
    ID_SRALGO_BASE = 220,  // + SrAlgo
    ID_SRQ_BASE = 240,     // + Maxine quality (0 = automatic)
    ID_FI_AUTO = 300, ID_FI_ON, ID_FI_OFF, ID_BE_TRT, ID_BE_RTX,
    ID_FIALGO_BASE = 320,  // + index into kInterpAlgoKeys
    ID_FPS_BASE = 400,     // + fps; ID_FPS_BASE + 0 = follow display
    ID_FPS_X2 = 890,
    ID_DELAY_BASE = 900,   // + seconds
    ID_SPEED_BASE = 1000,  // + index into kSpeedOptions
    ID_ASPECT_BASE = 1100, // + index into kAspects
    ID_AID_BASE = 2000,    // + track id
    ID_SID_BASE = 3000,    // + track id
};

struct Aspect {
    const char* value;
    const wchar_t* label;
};
const Aspect kAspects[] = {{"no", L"默认"}, {"16:9", L"16:9"}, {"4:3", L"4:3"}, {"2.35:1", L"2.35:1 (宽银幕)"},
                           {"21:9", L"21:9"}, {"1:1", L"1:1"}};

const int kDelays[] = {0, 2, 3, 5, 10};

struct Track {
    int64_t id = 0;
    bool selected = false;
    std::wstring label;
};

std::vector<Track> Tracks(const char* type) {
    std::vector<Track> out;
    int n = (int)GetInt("track-list/count");
    for (int i = 0; i < n; ++i) {
        if (GetString(Fmt("track-list/%d/type", i).c_str()) != type) continue;
        Track t;
        t.id = GetInt(Fmt("track-list/%d/id", i).c_str());
        t.selected = GetFlag(Fmt("track-list/%d/selected", i).c_str());
        std::string title = GetString(Fmt("track-list/%d/title", i).c_str());
        std::string lang = GetString(Fmt("track-list/%d/lang", i).c_str());
        std::string codec = GetString(Fmt("track-list/%d/codec", i).c_str());
        bool external = GetFlag(Fmt("track-list/%d/external", i).c_str());
        std::wstring label = WFmt(L"#%lld", (long long)t.id);
        if (!title.empty()) label += L"  " + FromUtf8(title);
        if (!lang.empty()) label += L"  [" + FromUtf8(lang) + L"]";
        if (!codec.empty()) label += L"  (" + FromUtf8(codec) + L")";
        if (external) label += L"  外部";
        t.label = label;
        out.push_back(t);
    }
    return out;
}

UINT Check(bool b) { return MF_STRING | (b ? MF_CHECKED : MF_UNCHECKED); }
UINT Radio(bool b) { return MF_STRING | MFT_RADIOCHECK | (b ? MF_CHECKED : MF_UNCHECKED); }

void AddSpeedItems(HMENU m) {
    double cur = GetDouble("speed", 1.0);
    for (int i = 0; i < (int)std::size(kSpeedOptions); ++i) {
        double v = kSpeedOptions[i];
        std::wstring label = WFmt(L"%g×", v) + (v == 1.0 ? L"  正常速度" : L"");
        AppendMenuW(m, Radio(std::fabs(cur - v) < 0.001), ID_SPEED_BASE + i, label.c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_SPEED_UP, L"加速 0.1×\tC");
    AppendMenuW(m, MF_STRING, ID_SPEED_DOWN, L"减速 0.1×\tX");
    AppendMenuW(m, MF_STRING, ID_SPEED_RESET, L"恢复正常速度\tZ");
}

HMENU BuildEnhanceMenu() {
    HMENU m = CreatePopupMenu();

    HMENU srAlgo = CreatePopupMenu();
    for (int i = 0; i < kSrAlgoCount; ++i)
        AppendMenuW(srAlgo, Radio(g_set.srAlgo == (SrAlgo)i), ID_SRALGO_BASE + i, kSrAlgoNames[i]);
    HMENU srMode = CreatePopupMenu();
    AppendMenuW(srMode, Radio(g_set.srMode == Mode::Auto), ID_SR_AUTO, L"自动 (源 ≤1080p 时开启)");
    AppendMenuW(srMode, Radio(g_set.srMode == Mode::On), ID_SR_ON, L"始终开启");
    AppendMenuW(srMode, Radio(g_set.srMode == Mode::Off), ID_SR_OFF, L"关闭");
    HMENU srTarget = CreatePopupMenu();
    AppendMenuW(srTarget, Radio(g_set.srTarget == SrTarget::Auto), ID_SRT_AUTO, L"自动 (按显示器选 2K / 4K)");
    AppendMenuW(srTarget, Radio(g_set.srTarget == SrTarget::K2), ID_SRT_2K, L"2K (2560×1440)");
    AppendMenuW(srTarget, Radio(g_set.srTarget == SrTarget::K4), ID_SRT_4K, L"4K (3840×2160)");
    HMENU srQuality = CreatePopupMenu();
    const wchar_t* qualities[] = {L"自动", L"1 低 (最快)", L"2 中", L"3 高", L"4 极高 (最慢)"};
    for (int q = 0; q < 5; ++q) AppendMenuW(srQuality, Radio(g_set.maxineQuality == q), ID_SRQ_BASE + q, qualities[q]);

    HMENU fiAlgo = CreatePopupMenu();
    for (int i = 0; i < kInterpAlgoCount; ++i)
        AppendMenuW(fiAlgo, Radio(g_set.interpAlgo == kInterpAlgoKeys[i]), ID_FIALGO_BASE + i, kInterpAlgoNames[i]);
    HMENU fiMode = CreatePopupMenu();
    AppendMenuW(fiMode, Radio(g_set.interpMode == Mode::Auto), ID_FI_AUTO, L"自动 (源 ≤30fps 时开启)");
    AppendMenuW(fiMode, Radio(g_set.interpMode == Mode::On), ID_FI_ON, L"始终开启");
    AppendMenuW(fiMode, Radio(g_set.interpMode == Mode::Off), ID_FI_OFF, L"关闭");
    HMENU fps = CreatePopupMenu();
    AppendMenuW(fps, Radio(g_set.targetFps == kTargetDouble), ID_FPS_X2, L"原帧率 ×2 (30→60、24→48)");
    for (int f : kFpsOptions)
        AppendMenuW(fps, Radio(g_set.targetFps == f), ID_FPS_BASE + f, WFmt(L"%d fps", f).c_str());
    AppendMenuW(fps, Radio(g_set.targetFps == 0), ID_FPS_BASE,
                WFmt(L"跟随显示器 (%.0f Hz)", GetDouble("display-fps", 60)).c_str());
    HMENU backend = CreatePopupMenu();
    AppendMenuW(backend, Radio(g_set.backend == "trt"), ID_BE_TRT, L"TensorRT (启用时几乎无停顿)");
    AppendMenuW(backend, Radio(g_set.backend == "rtx"), ID_BE_RTX, L"TensorRT-RTX (快约 25%，启用时停顿约 6 秒)");

    HMENU delay = CreatePopupMenu();
    for (int d : kDelays)
        AppendMenuW(delay, Radio(g_set.delaySec == d), ID_DELAY_BASE + d, d ? WFmt(L"%d 秒后", d).c_str() : L"立即");

    UINT rifeOnly = g_set.IsRife() ? 0 : MF_GRAYED;
    AppendMenuW(m, MF_POPUP, (UINT_PTR)srAlgo, L"超分辨率算法");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)srMode, L"超分辨率开启条件");
    AppendMenuW(m, MF_POPUP | (g_set.SrToTarget() ? 0 : MF_GRAYED), (UINT_PTR)srTarget, L"超分目标分辨率");
    AppendMenuW(m, MF_POPUP | (g_set.srAlgo == SrAlgo::Maxine ? 0 : MF_GRAYED), (UINT_PTR)srQuality, L"Maxine 超分画质");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)fiAlgo, L"插帧算法");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)fiMode, L"插帧开启条件");
    AppendMenuW(m, MF_POPUP | rifeOnly, (UINT_PTR)fps, L"插帧目标帧率");
    AppendMenuW(m, MF_POPUP | rifeOnly, (UINT_PTR)backend, L"AI 推理后端");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)delay, L"开始播放后启用增强");
    AppendMenuW(m, Check(EnhancementDisabled()), ID_TOGGLE, L"暂时关闭视频增强\tCtrl+E");
    AppendMenuW(m, MF_STRING, ID_INFO, L"显示增强状态\tTab");
    bool warming = WarmupRunning();
    AppendMenuW(m, MF_STRING | (warming || !g_set.IsRife() ? MF_GRAYED : 0), ID_REBUILD,
                warming ? L"插帧引擎优化中…" : L"重新优化插帧引擎 / 测速");
    return m;
}

UINT RunMenu(HMENU m, POINT pt, UINT flags) {
    SetForegroundWindow(g_hwnd);
    UINT id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | flags, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);  // destroys the submenus as well
    return id;
}

void SetSpeed(double v) {
    v = std::clamp(v, 0.1, 8.0);
    SetProp("speed", Fmt("%.2f", v));
    Osd(Fmt("播放速度: %.2f×", v), 1500);
}

}  // namespace

void ShowSpeedMenu(POINT at) {
    HMENU m = CreatePopupMenu();
    AddSpeedItems(m);
    if (UINT id = RunMenu(m, at, TPM_BOTTOMALIGN)) OnCommand(id);
}

void ShowMainMenu(const POINT* at) {
    bool hasFile = !GetFlag("idle-active");
    UINT fileOnly = hasFile ? 0 : MF_GRAYED;

    HMENU play = CreatePopupMenu();
    AppendMenuW(play, MF_STRING | fileOnly, ID_PLAYPAUSE, L"播放 / 暂停\t空格");
    AppendMenuW(play, MF_STRING | fileOnly, ID_STOP, L"停止");
    AppendMenuW(play, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(play, MF_STRING | fileOnly, ID_BACK10, L"快退 10 秒\t←");
    AppendMenuW(play, MF_STRING | fileOnly, ID_FWD10, L"快进 10 秒\t→");
    AppendMenuW(play, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(play, MF_STRING, ID_PREV, L"上一个\tPgUp");
    AppendMenuW(play, MF_STRING, ID_NEXT, L"下一个\tPgDn");
    AppendMenuW(play, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(play, Check(g_set.resume), ID_RESUME, L"记住播放位置");

    HMENU speed = CreatePopupMenu();
    AddSpeedItems(speed);

    HMENU audio = CreatePopupMenu();
    for (const Track& t : Tracks("audio"))
        AppendMenuW(audio, Radio(t.selected), ID_AID_BASE + (UINT)t.id, t.label.c_str());
    if (GetMenuItemCount(audio)) AppendMenuW(audio, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(audio, Check(GetFlag("mute")), ID_MUTE, L"静音\tM");
    AppendMenuW(audio, MF_STRING, ID_VOL_UP, L"音量 +5\t↑");
    AppendMenuW(audio, MF_STRING, ID_VOL_DOWN, L"音量 -5\t↓");

    HMENU subs = CreatePopupMenu();
    std::vector<Track> subTracks = Tracks("sub");
    bool anySub = std::any_of(subTracks.begin(), subTracks.end(), [](const Track& t) { return t.selected; });
    AppendMenuW(subs, Radio(!anySub), ID_SUB_OFF, L"不显示字幕");
    for (const Track& t : subTracks) AppendMenuW(subs, Radio(t.selected), ID_SID_BASE + (UINT)t.id, t.label.c_str());
    AppendMenuW(subs, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(subs, MF_STRING | fileOnly, ID_OPEN_SUB, L"加载字幕文件…");

    HMENU aspect = CreatePopupMenu();
    std::string curAspect = GetString("video-aspect-override");
    for (int i = 0; i < (int)std::size(kAspects); ++i) {
        bool sel = i == 0 ? (curAspect == "-1" || curAspect == "no" || curAspect.empty())
                          : curAspect == kAspects[i].value;
        AppendMenuW(aspect, Radio(sel), ID_ASPECT_BASE + i, kAspects[i].label);
    }
    HMENU video = CreatePopupMenu();
    AppendMenuW(video, Check(g_fullscreen), ID_FULLSCREEN, L"全屏\tEnter");
    AppendMenuW(video, Check(IsTopmost()), ID_TOPMOST, L"窗口置顶");
    AppendMenuW(video, MF_POPUP, (UINT_PTR)aspect, L"画面比例");
    AppendMenuW(video, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(video, MF_STRING | fileOnly, ID_SCREENSHOT, L"截图\tS");

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_OPEN, L"打开文件…\tCtrl+O");
    AppendMenuW(m, MF_STRING, ID_OPEN_FOLDER, L"打开文件夹…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)play, L"播放");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)speed, WFmt(L"播放速度 (当前 %g×)", GetDouble("speed", 1.0)).c_str());
    AppendMenuW(m, MF_POPUP, (UINT_PTR)audio, L"声音");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)subs, L"字幕");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)video, L"画面");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)BuildEnhanceMenu(), L"视频增强 (超分 / 插帧)");
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"视频增强设置…\tF5");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_HOTKEYS, L"快捷键一览");
    AppendMenuW(m, MF_STRING, ID_EXIT, L"退出");

    POINT pt;
    if (at) pt = *at;
    else GetCursorPos(&pt);
    // From the bar's menu button (bottom-right corner) the menu opens up and to the left.
    if (UINT id = RunMenu(m, pt, at ? TPM_BOTTOMALIGN | TPM_RIGHTALIGN : 0)) PostMessageW(g_hwnd, WM_COMMAND, id, 0);
}

void ShowHotkeys() {
    MessageBoxW(g_hwnd,
                L"播放控制\n"
                L"  空格 / 单击画面\t播放 / 暂停\n"
                L"  ← / →\t\t快退 / 快进 10 秒\n"
                L"  Ctrl + ← / →\t快退 / 快进 30 秒\n"
                L"  Shift + ← / →\t快退 / 快进 60 秒\n"
                L"  Home\t\t回到开头\n"
                L"  PgUp / PgDn\t上一个 / 下一个文件\n"
                L"  D / F\t\t上一帧 / 下一帧\n\n"
                L"速度与声音\n"
                L"  C / X / Z\t\t加速 / 减速 / 恢复正常速度\n"
                L"  ↑ / ↓、滚轮\t音量 + / -\n"
                L"  M\t\t静音\n\n"
                L"画面\n"
                L"  Enter / 双击画面\t全屏\n"
                L"  Esc\t\t退出全屏\n"
                L"  S\t\t截图\n\n"
                L"视频增强\n"
                L"  F5\t\t视频增强设置 (选择超分 / 插帧算法)\n"
                L"  Ctrl + E\t\t暂时关闭 / 开启增强\n"
                L"  Tab\t\t显示增强状态\n\n"
                L"其他\n"
                L"  Ctrl + O\t\t打开文件\n"
                L"  右键\t\t菜单",
                L"RTXPlayer 快捷键", MB_OK | MB_ICONINFORMATION);
}

void OnCommand(UINT id) {
    switch (id) {
    case ID_OPEN: OpenFileDialog(); return;
    case ID_OPEN_FOLDER: OpenFolderDialog(); return;
    case ID_OPEN_SUB: OpenSubtitleDialog(); return;
    case ID_PLAYPAUSE: Cmd({"cycle", "pause"}); return;
    case ID_STOP: SavePosition(); Cmd({"stop", "keep-playlist"}); return;
    case ID_PREV: PlaylistStep(-1); return;
    case ID_NEXT: PlaylistStep(1); return;
    case ID_BACK10: Cmd({"seek", "-10", "exact"}); return;
    case ID_FWD10: Cmd({"seek", "10", "exact"}); return;
    case ID_SPEED_UP: SetSpeed(GetDouble("speed", 1.0) + 0.1); return;
    case ID_SPEED_DOWN: SetSpeed(GetDouble("speed", 1.0) - 0.1); return;
    case ID_SPEED_RESET: SetSpeed(1.0); return;
    case ID_MUTE: Cmd({"cycle", "mute"}); return;
    case ID_VOL_UP: Cmd({"add", "volume", "5"}); return;
    case ID_VOL_DOWN: Cmd({"add", "volume", "-5"}); return;
    case ID_SUB_OFF: SetProp("sid", "no"); return;
    case ID_FULLSCREEN: SetFullscreen(!g_fullscreen); return;
    case ID_TOPMOST: ToggleTopmost(); return;
    case ID_SCREENSHOT: Cmd({"screenshot"}); return;
    case ID_SETTINGS: ShowSettingsDialog(g_hwnd); return;
    case ID_TOGGLE: ToggleEnhancement(); return;
    case ID_INFO: ShowEnhanceStatus(); return;
    case ID_REBUILD: RebuildEngine(); return;
    case ID_HOTKEYS: ShowHotkeys(); return;
    case ID_EXIT: PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return;
    case ID_RESUME:
        g_set.resume = !g_set.resume;
        g_set.Save();
        SetProp("save-position-on-quit", g_set.resume ? "yes" : "no");
        SetProp("resume-playback", g_set.resume ? "yes" : "no");
        return;
    // Enhancement settings
    case ID_SR_AUTO: g_set.srMode = Mode::Auto; break;
    case ID_SR_ON: g_set.srMode = Mode::On; break;
    case ID_SR_OFF: g_set.srMode = Mode::Off; break;
    case ID_SRT_AUTO: g_set.srTarget = SrTarget::Auto; break;
    case ID_SRT_2K: g_set.srTarget = SrTarget::K2; break;
    case ID_SRT_4K: g_set.srTarget = SrTarget::K4; break;
    case ID_FI_AUTO: g_set.interpMode = Mode::Auto; break;
    case ID_FI_ON: g_set.interpMode = Mode::On; break;
    case ID_FI_OFF: g_set.interpMode = Mode::Off; break;
    case ID_BE_TRT: g_set.backend = "trt"; break;
    case ID_BE_RTX: g_set.backend = "rtx"; break;
    case ID_FPS_X2: g_set.targetFps = kTargetDouble; break;
    default:
        if (id >= ID_SRQ_BASE && id < ID_SRQ_BASE + 5) {
            g_set.maxineQuality = (int)(id - ID_SRQ_BASE);
            break;
        }
        if (id >= ID_SRALGO_BASE && id < ID_SRALGO_BASE + kSrAlgoCount) {
            g_set.srAlgo = (SrAlgo)(id - ID_SRALGO_BASE);
            break;
        }
        if (id >= ID_FIALGO_BASE && id < ID_FIALGO_BASE + kInterpAlgoCount) {
            g_set.interpAlgo = kInterpAlgoKeys[id - ID_FIALGO_BASE];
            break;
        }
        if (id >= ID_FPS_BASE && id <= ID_FPS_BASE + 480) {
            g_set.targetFps = (int)(id - ID_FPS_BASE);
            break;
        }
        if (id >= ID_DELAY_BASE && id <= ID_DELAY_BASE + 60) {
            g_set.delaySec = (int)(id - ID_DELAY_BASE);
            g_set.Save();
            return;  // only affects the next file
        }
        if (id >= ID_SPEED_BASE && id < ID_SPEED_BASE + std::size(kSpeedOptions)) {
            SetSpeed(kSpeedOptions[id - ID_SPEED_BASE]);
            return;
        }
        if (id >= ID_ASPECT_BASE && id < ID_ASPECT_BASE + std::size(kAspects)) {
            SetProp("video-aspect-override", kAspects[id - ID_ASPECT_BASE].value);
            return;
        }
        if (id >= ID_AID_BASE && id < ID_SID_BASE) {
            SetProp("aid", std::to_string(id - ID_AID_BASE));
            return;
        }
        if (id >= ID_SID_BASE && id < ID_SID_BASE + 1000) {
            SetProp("sid", std::to_string(id - ID_SID_BASE));
            return;
        }
        return;
    }
    OnSettingsChanged();
}
