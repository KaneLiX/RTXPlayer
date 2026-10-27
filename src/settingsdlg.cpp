// "视频增强设置" dialog: choose the super resolution and frame interpolation
// algorithms and their options. Built from plain Win32 controls and run as a
// modal window over the player.
#include "app.h"

#include <commctrl.h>
#include <commdlg.h>

#include <iterator>
#include <string>
#include <vector>

namespace {

enum : int {
    IDC_SR_MODE = 101, IDC_SR_ALGO, IDC_SR_TARGET, IDC_FI_MODE, IDC_FI_ALGO, IDC_FI_FPS, IDC_FI_BACKEND, IDC_DELAY,
    IDC_INFO, IDC_APPLY, IDC_FRUC_DLL, IDC_SR_QUALITY, IDC_VFX_SDK,
};

const int kDelays[] = {0, 2, 3, 5, 10};

struct Dialog {
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    UINT dpi = 96;
    bool done = false;
};
Dialog g_dlg;

int S(int v) { return Scale(v, g_dlg.dpi); }

HWND Ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_dlg.hwnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_dlg.font, TRUE);
    return c;
}

void Row(int y, const wchar_t* label, int id, const std::vector<std::wstring>& items, int sel) {
    Ctl(WC_STATICW, label, SS_LEFT | SS_CENTERIMAGE, -1, 30, y, 130, 26);
    HWND c = Ctl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id, 165, y, 375, 300);
    for (const auto& s : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    SendMessageW(c, CB_SETCURSEL, sel, 0);
}

int Sel(int id) { return (int)SendDlgItemMessageW(g_dlg.hwnd, id, CB_GETCURSEL, 0, 0); }

// Combo order: [0] source x2, [1..n] kFpsOptions, [n+1] follow display.
int FpsIndex(int fps) {
    if (fps == kTargetDouble) return 0;
    for (int i = 0; i < (int)std::size(kFpsOptions); ++i)
        if (kFpsOptions[i] == fps) return i + 1;
    return (int)std::size(kFpsOptions) + 1;
}

int FpsFromIndex(int i) {
    if (i == 0) return kTargetDouble;
    return i <= (int)std::size(kFpsOptions) ? kFpsOptions[i - 1] : 0;
}

int DelayIndex(int d) {
    for (int i = 0; i < (int)std::size(kDelays); ++i)
        if (kDelays[i] == d) return i;
    return 2;
}

std::wstring g_frucDll;  // edited copies of g_set.frucDll / vfxSdk
std::wstring g_vfxSdk;

// Grey out options that do not apply and describe what this machine can do.
void UpdateState() {
    bool maxine = Sel(IDC_SR_ALGO) == (int)SrAlgo::Maxine;
    bool vsr = maxine || Sel(IDC_SR_ALGO) == (int)SrAlgo::Vsr;  // scale to the target resolution
    std::string algo = kInterpAlgoKeys[Sel(IDC_FI_ALGO)];
    bool fruc = algo == "fruc";
    bool rife = algo != "blend" && !fruc;
    EnableWindow(GetDlgItem(g_dlg.hwnd, IDC_SR_TARGET), vsr);
    EnableWindow(GetDlgItem(g_dlg.hwnd, IDC_SR_QUALITY), maxine);
    ShowWindow(GetDlgItem(g_dlg.hwnd, IDC_VFX_SDK), maxine ? SW_SHOW : SW_HIDE);
    EnableWindow(GetDlgItem(g_dlg.hwnd, IDC_FI_FPS), rife);
    EnableWindow(GetDlgItem(g_dlg.hwnd, IDC_FI_BACKEND), rife);
    ShowWindow(GetDlgItem(g_dlg.hwnd, IDC_FRUC_DLL), fruc ? SW_SHOW : SW_HIDE);

    std::wstring info;
    if (maxine) {
        std::wstring sdk = g_vfxSdk.empty() ? FindVfxSdk() : g_vfxSdk;
        info = sdk.empty() ? L"Maxine VSR：未找到 Video Effects SDK (含 VSR 功能)，将改用驱动内置 RTX VSR；"
                             L"点 \"Maxine SDK…\" 选择 VideoFX\\bin\\NVVideoEffects.dll。\n"
                           : L"Maxine VSR：" + sdk + L"\n";
    } else if (!vsr) {
        info = L"超分：着色器 / 传统算法在渲染时直接放大到窗口尺寸，\"目标分辨率\"只对 RTX / Maxine VSR 有效。\n";
    }
    if (fruc) {
        std::wstring dll = g_frucDll.empty() ? FindFrucDll() : g_frucDll;
        info += L"NVIDIA 光流 FRUC：固定 ×2 (30→60、24→48)，原分辨率，几乎不占显卡。";
        info += dll.empty() ? L"\n未找到 NvOFFRUC.dll：点 \"FRUC 位置…\" 选择 Optical Flow SDK 里的 NvOFFRUC.dll。"
                            : L"\n使用: " + dll;
    } else if (rife) {
        std::string model = algo;
        std::string backend = Sel(IDC_FI_BACKEND) == 1 ? "rtx" : "trt";
        double f720 = 0, f1080 = 0;
        if (ReadBench(EngineKey(model, backend), f720, f1080))
            info += WFmt(L"本机实测插帧能力：720p 中间帧最高约 %.0f fps，1080p 约 %.0f fps；超出时自动降级。", f720, f1080);
        else
            info += L"该算法 / 后端组合尚未在本机测速：首次使用时后台自动优化引擎 (约 1–2 分钟)。";
    } else {
        info += L"帧混合：按显示器刷新率混合相邻帧，不生成新画面，几乎不占显卡。";
    }
    SetDlgItemTextW(g_dlg.hwnd, IDC_INFO, info.c_str());
}

void Apply() {
    g_set.srMode = (Mode)Sel(IDC_SR_MODE);
    g_set.srAlgo = (SrAlgo)Sel(IDC_SR_ALGO);
    g_set.srTarget = (SrTarget)Sel(IDC_SR_TARGET);
    g_set.maxineQuality = Sel(IDC_SR_QUALITY);
    g_set.vfxSdk = g_vfxSdk;
    g_set.interpMode = (Mode)Sel(IDC_FI_MODE);
    g_set.interpAlgo = kInterpAlgoKeys[Sel(IDC_FI_ALGO)];
    g_set.targetFps = FpsFromIndex(Sel(IDC_FI_FPS));
    g_set.backend = Sel(IDC_FI_BACKEND) == 1 ? "rtx" : "trt";
    g_set.delaySec = kDelays[Sel(IDC_DELAY)];
    g_set.frucDll = g_frucDll;
    OnSettingsChanged();
}

// Lets the user point at NvOFFRUC.dll inside their NVIDIA Optical Flow SDK.
void BrowseFrucDll() {
    wchar_t path[MAX_PATH * 2] = {};
    std::wstring cur = g_frucDll.empty() ? FindFrucDll() : g_frucDll;
    wcsncpy_s(path, cur.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g_dlg.hwnd;
    ofn.lpstrFilter = L"NvOFFRUC.dll\0NvOFFRUC.dll\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = (DWORD)std::size(path);
    ofn.lpstrTitle = L"选择 Optical Flow SDK 中的 NvOFFRUC.dll (NvOFFRUC\\NvOFFRUCSample\\bin\\win64)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        g_frucDll = path;
        UpdateState();
    }
}

// Lets the user point at NVVideoEffects.dll in their NVIDIA Video Effects SDK (VideoFX\bin).
void BrowseVfxSdk() {
    wchar_t path[MAX_PATH * 2] = {};
    std::wstring cur = g_vfxSdk.empty() ? FindVfxSdk() : g_vfxSdk;
    if (!cur.empty()) wcsncpy_s(path, (cur + L"\\bin\\NVVideoEffects.dll").c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g_dlg.hwnd;
    ofn.lpstrFilter = L"NVVideoEffects.dll\0NVVideoEffects.dll\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = (DWORD)std::size(path);
    ofn.lpstrTitle = L"选择 Video Effects SDK 中的 NVVideoEffects.dll (VideoFX\\bin)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        std::wstring p = path;
        p = p.substr(0, p.find_last_of(L'\\'));                 // ...\VideoFX\bin
        g_vfxSdk = p.substr(0, p.find_last_of(L'\\'));          // ...\VideoFX
        UpdateState();
    }
}

void Build() {
    std::vector<std::wstring> modesSr = {L"自动 (源 ≤1080p 时开启)", L"始终开启", L"关闭"};
    std::vector<std::wstring> modesFi = {L"自动 (源 ≤30fps 时开启)", L"始终开启", L"关闭"};
    std::vector<std::wstring> srAlgos(kSrAlgoNames, kSrAlgoNames + kSrAlgoCount);
    std::vector<std::wstring> fiAlgos(kInterpAlgoNames, kInterpAlgoNames + kInterpAlgoCount);
    std::vector<std::wstring> targets = {L"自动 (按显示器选 2K / 4K)", L"2K (2560×1440)", L"4K (3840×2160)"};
    std::vector<std::wstring> qualities = {L"自动 (单独超分用 3 高，配合 RIFE 用 2 中)", L"1 低 (最快)", L"2 中",
                                           L"3 高", L"4 极高 (最慢)"};
    std::vector<std::wstring> fps = {L"原帧率 ×2 (30→60、24→48，推荐)"};
    for (int v : kFpsOptions) fps.push_back(WFmt(L"%d fps", v));
    fps.push_back(WFmt(L"跟随显示器刷新率 (%.0f Hz)", GetDouble("display-fps", 60)));
    std::vector<std::wstring> backends = {L"TensorRT (启用时几乎无停顿，推荐)", L"TensorRT-RTX (快约 25%，启用时停顿约 6 秒)"};
    std::vector<std::wstring> delays = {L"立即", L"2 秒后", L"3 秒后", L"5 秒后", L"10 秒后"};

    g_frucDll = g_set.frucDll;
    g_vfxSdk = g_set.vfxSdk;
    int interpIdx = 0;
    for (int i = 0; i < kInterpAlgoCount; ++i)
        if (g_set.interpAlgo == kInterpAlgoKeys[i]) interpIdx = i;

    Ctl(WC_BUTTONW, L"超分辨率", BS_GROUPBOX, -1, 12, 8, 544, 166);
    Row(32, L"开启条件", IDC_SR_MODE, modesSr, (int)g_set.srMode);
    Row(66, L"算法", IDC_SR_ALGO, srAlgos, (int)g_set.srAlgo);
    Row(100, L"目标分辨率", IDC_SR_TARGET, targets, (int)g_set.srTarget);
    Row(134, L"Maxine 画质", IDC_SR_QUALITY, qualities, g_set.maxineQuality);

    Ctl(WC_BUTTONW, L"插帧", BS_GROUPBOX, -1, 12, 184, 544, 166);
    Row(208, L"开启条件", IDC_FI_MODE, modesFi, (int)g_set.interpMode);
    Row(242, L"算法", IDC_FI_ALGO, fiAlgos, interpIdx);
    Row(276, L"目标帧率", IDC_FI_FPS, fps, FpsIndex(g_set.targetFps));
    Row(310, L"AI 推理后端", IDC_FI_BACKEND, backends, g_set.backend == "rtx" ? 1 : 0);

    Ctl(WC_BUTTONW, L"通用", BS_GROUPBOX, -1, 12, 360, 544, 64);
    Row(384, L"播放后启用增强", IDC_DELAY, delays, DelayIndex(g_set.delaySec));

    Ctl(WC_STATICW, L"", SS_LEFT, IDC_INFO, 16, 434, 536, 60);
    Ctl(WC_BUTTONW, L"Maxine SDK…", BS_PUSHBUTTON | WS_TABSTOP, IDC_VFX_SDK, 16, 502, 110, 28);
    Ctl(WC_BUTTONW, L"FRUC 位置…", BS_PUSHBUTTON | WS_TABSTOP, IDC_FRUC_DLL, 132, 502, 110, 28);
    Ctl(WC_BUTTONW, L"确定", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, 272, 502, 88, 28);
    Ctl(WC_BUTTONW, L"取消", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, 368, 502, 88, 28);
    Ctl(WC_BUTTONW, L"应用", BS_PUSHBUTTON | WS_TABSTOP, IDC_APPLY, 464, 502, 88, 28);
    UpdateState();
}

LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (HIWORD(wp) == CBN_SELCHANGE) {
            UpdateState();
            return 0;
        }
        if (id == IDOK) {
            Apply();
            g_dlg.done = true;
        } else if (id == IDCANCEL) {
            g_dlg.done = true;
        } else if (id == IDC_APPLY) {
            Apply();
        } else if (id == IDC_FRUC_DLL) {
            BrowseFrucDll();
        } else if (id == IDC_VFX_SDK) {
            BrowseVfxSdk();
        }
        return 0;
    }
    case WM_CLOSE:
        g_dlg.done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool ShowSettingsDialog(HWND owner) {
    if (g_dlg.hwnd) return false;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"RTXPlayerSettings";
        RegisterClassExW(&wc);
        registered = true;
    }
    if (g_fullscreen) SetFullscreen(false);  // keep the dialog visible above a windowed player

    g_dlg = Dialog{};
    g_dlg.dpi = GetDpiForWindow(owner);
    g_dlg.font = CreateFontW(-MulDiv(9, (int)g_dlg.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                             L"Microsoft YaHei UI");
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, exStyle = WS_EX_DLGMODALFRAME;
    RECT rc{0, 0, S(568), S(544)};
    AdjustWindowRectExForDpi(&rc, style, FALSE, exStyle, g_dlg.dpi);
    RECT orc;
    GetWindowRect(owner, &orc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = orc.left + (orc.right - orc.left - w) / 2, y = orc.top + (orc.bottom - orc.top - h) / 2;
    g_dlg.hwnd = CreateWindowExW(exStyle, L"RTXPlayerSettings", L"视频增强设置 — 超分辨率 / 插帧", style, x, y, w, h,
                                 owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    Build();
    ShowWindow(g_dlg.hwnd, SW_SHOW);
    EnableWindow(owner, FALSE);

    MSG msg;
    while (!g_dlg.done) {
        BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r <= 0) {
            PostQuitMessage((int)msg.wParam);
            break;
        }
        if (!IsDialogMessageW(g_dlg.hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    DestroyWindow(g_dlg.hwnd);
    DeleteObject(g_dlg.font);
    g_dlg.hwnd = nullptr;
    FocusVideo();
    return true;
}
