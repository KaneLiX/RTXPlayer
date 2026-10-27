// PotPlayer-style control bar: seek bar on top; transport buttons and time on
// the left; enhancement status, speed, volume, fullscreen and menu on the right.
// One window class serves both the docked bar (child of the main window) and the
// overlay bar shown at the bottom of the screen in fullscreen (owned popup).
#include "app.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

enum Btn : int { kNone = 0, kPrev, kPlay, kStop, kNext, kOpen, kRtx, kSpeed, kMute, kVolume, kFullscreen, kMenu, kSeek };
constexpr int kButtonCount = kMenu + 1;

const COLORREF kBg = RGB(28, 28, 30), kBgOverlay = RGB(18, 18, 20), kBorder = RGB(48, 48, 52),
               kText = RGB(228, 228, 228), kTextDim = RGB(150, 150, 155), kHover = RGB(60, 60, 64),
               kTrack = RGB(74, 74, 78), kCache = RGB(108, 108, 112), kAccent = RGB(118, 185, 0),
               kAmber = RGB(232, 170, 40), kOutline = RGB(86, 86, 92);

const wchar_t* const kTips[kButtonCount] = {
    L"", L"上一个 (PgUp)", L"播放 / 暂停 (空格、单击画面)", L"停止", L"下一个 (PgDn)", L"打开文件 (Ctrl+O)",
    L"视频增强设置：超分辨率 / 插帧算法 (F5)", L"播放速度 (C 加速 · X 减速 · Z 恢复)", L"静音 (M)",
    L"音量 (↑ ↓ 或滚轮)", L"全屏 (Enter 或双击画面)", L"菜单 (也可在画面上右键)"};

struct State {
    double pos = 0, dur = 0, cache = 0, speed = 1, volume = 100;
    bool paused = true, mute = false, hasFile = false, fullscreen = false;
    std::wstring badge;
    BadgeState badgeState = BadgeState::Idle;
    bool operator==(const State&) const = default;
};

struct Item {
    Btn id;
    RECT rc;
};

struct Bar {
    HWND hwnd = nullptr;
    bool overlay = false;
    UINT dpi = 96;
    RECT seek{};      // seek bar hit area
    RECT track{};     // seek bar as drawn
    RECT time{};
    RECT volTrack{};
    std::vector<Item> items;
    Btn hover = kNone, pressed = kNone;
    int hoverX = -1;
    bool dragSeek = false, dragVol = false, tracking = false;
    double dragFrac = 0;
    ULONGLONG lastSeekCmd = 0;
    HWND tips = nullptr;     // button tooltips
    HWND seekTip = nullptr;  // tracking tooltip: time under the cursor
};

Bar g_bars[2];  // [0] docked, [1] fullscreen overlay
State g_state;
ULONGLONG g_overlayLastNeeded = 0;

struct Fonts {
    UINT dpi = 0;
    HFONT icon = nullptr, iconBig = nullptr, text = nullptr, small = nullptr;
} g_fonts;

void EnsureFonts(UINT dpi) {
    if (g_fonts.dpi == dpi) return;
    for (HFONT f : {g_fonts.icon, g_fonts.iconBig, g_fonts.text, g_fonts.small})
        if (f) DeleteObject(f);
    auto mk = [&](int pt, const wchar_t* face) {
        return CreateFontW(-MulDiv(pt, (int)dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    g_fonts.icon = mk(11, L"Segoe MDL2 Assets");
    g_fonts.iconBig = mk(15, L"Segoe MDL2 Assets");
    g_fonts.text = mk(9, L"Microsoft YaHei UI");
    g_fonts.small = mk(8, L"Microsoft YaHei UI");
    g_fonts.dpi = dpi;
}

// --- drawing helpers --------------------------------------------------------

void Fill(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &r, br);
    DeleteObject(br);
}

void RoundFill(HDC dc, const RECT& r, COLORREF c, int radius) {
    HBRUSH br = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

void RoundOutline(HDC dc, const RECT& r, COLORREF c, int radius) {
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH)), op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

void Circle(HDC dc, int cx, int cy, int r, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

void Text(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT fmt) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

std::wstring SpeedText(double v) {
    std::wstring s = WFmt(L"%.2f", v);
    while (s.back() == L'0') s.pop_back();
    if (s.back() == L'.') s += L'0';
    return s + L"×";
}

// --- state / layout ---------------------------------------------------------

State ReadState() {
    State s;
    s.hasFile = g_mpv && !GetFlag("idle-active");
    if (s.hasFile) {
        s.pos = GetDouble("time-pos");
        s.dur = GetDouble("duration");
        s.cache = GetDouble("demuxer-cache-time");
        s.paused = GetFlag("pause");
    }
    s.speed = GetDouble("speed", 1.0);
    s.volume = GetDouble("volume", 100.0);
    s.mute = GetFlag("mute");
    s.fullscreen = g_fullscreen;
    s.badge = EnhanceBadge(s.badgeState);
    return s;
}

const Item* FindItem(const Bar& b, Btn id) {
    for (const auto& it : b.items)
        if (it.id == id) return &it;
    return nullptr;
}

void UpdateTooltips(Bar& b) {
    if (!b.tips) return;
    for (int id = kPrev; id < kButtonCount; ++id) {
        TOOLINFOW ti{sizeof(ti)};
        ti.hwnd = b.hwnd;
        ti.uId = (UINT_PTR)id;
        const Item* it = FindItem(b, (Btn)id);
        ti.rect = it ? it->rc : RECT{0, 0, 0, 0};
        SendMessageW(b.tips, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    }
}

void Layout(Bar& b) {
    RECT rc;
    GetClientRect(b.hwnd, &rc);
    b.dpi = GetDpiForWindow(b.hwnd);
    EnsureFonts(b.dpi);
    auto S = [&](int v) { return Scale(v, b.dpi); };
    int w = rc.right, seekH = S(18);
    b.seek = {S(10), 0, w - S(10), seekH};
    b.track = {b.seek.left, seekH / 2 - S(2), b.seek.right, seekH / 2 + S(2)};
    int cy = seekH + (rc.bottom - seekH) / 2, bs = S(34);
    b.items.clear();
    auto add = [&](Btn id, int x, int width) { b.items.push_back({id, {x, cy - bs / 2, x + width, cy + bs / 2}}); };

    int x = S(8);
    for (Btn id : {kPrev, kPlay, kStop, kNext, kOpen}) {
        int width = id == kPlay ? S(42) : bs;
        add(id, x, width);
        x += width + S(2);
    }
    int timeLeft = x + S(10);

    int xr = w - S(8);
    auto addRight = [&](Btn id, int width) {
        xr -= width;
        add(id, xr, width);
        xr -= S(4);
    };
    addRight(kMenu, bs);
    addRight(kFullscreen, bs);
    addRight(kVolume, S(84));
    addRight(kMute, bs);
    addRight(kSpeed, S(54));

    HDC dc = GetDC(b.hwnd);
    HGDIOBJ old = SelectObject(dc, g_fonts.small);
    SIZE sz{};
    GetTextExtentPoint32W(dc, g_state.badge.c_str(), (int)g_state.badge.size(), &sz);
    SelectObject(dc, old);
    ReleaseDC(b.hwnd, dc);
    int badgeW = std::max(S(96), (int)sz.cx + S(32));
    if (xr - badgeW > timeLeft + S(150)) addRight(kRtx, badgeW);  // dropped on very narrow windows

    b.time = {timeLeft, seekH, std::max(timeLeft, xr), rc.bottom};
    if (const Item* v = FindItem(b, kVolume))
        b.volTrack = {v->rc.left + S(6), cy - S(2), v->rc.right - S(6), cy + S(2)};
    UpdateTooltips(b);
}

Btn HitTest(const Bar& b, POINT pt) {
    if (PtInRect(&b.seek, pt)) return kSeek;
    for (const auto& it : b.items)
        if (PtInRect(&it.rc, pt)) return it.id;
    return kNone;
}

double TrackFrac(const RECT& track, int x) {
    int width = std::max(1, (int)(track.right - track.left));
    return std::clamp((x - track.left) / double(width), 0.0, 1.0);
}

// --- painting ---------------------------------------------------------------

void Paint(Bar& b, HDC hdc) {
    RECT rc;
    GetClientRect(b.hwnd, &rc);
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    EnsureFonts(b.dpi);
    auto S = [&](int v) { return Scale(v, b.dpi); };
    SetBkMode(dc, TRANSPARENT);

    Fill(dc, rc, b.overlay ? kBgOverlay : kBg);
    if (!b.overlay) Fill(dc, {0, 0, rc.right, 1}, kBorder);

    // Seek bar: track, cached range, played range, thumb on hover/drag.
    bool seekActive = (b.hover == kSeek || b.dragSeek) && g_state.dur > 0;
    RECT t = b.track;
    if (seekActive) InflateRect(&t, 0, S(1));
    RoundFill(dc, t, kTrack, S(4));
    if (g_state.dur > 0) {
        int width = t.right - t.left;
        double posFrac = b.dragSeek ? b.dragFrac : std::clamp(g_state.pos / g_state.dur, 0.0, 1.0);
        double cacheFrac = std::clamp(g_state.cache / g_state.dur, 0.0, 1.0);
        RECT c = t;
        c.right = t.left + (int)(width * cacheFrac);
        if (c.right > c.left + 1) RoundFill(dc, c, kCache, S(4));
        RECT p = t;
        p.right = t.left + (int)(width * posFrac);
        if (p.right > p.left + 1) RoundFill(dc, p, kAccent, S(4));
        if (seekActive) Circle(dc, p.right, (t.top + t.bottom) / 2, S(6), RGB(255, 255, 255));
    }

    for (const auto& it : b.items) {
        if (it.id == kVolume) {
            double frac = g_state.mute ? 0.0 : std::clamp(g_state.volume / 100.0, 0.0, 1.0);
            RoundFill(dc, b.volTrack, kTrack, S(4));
            RECT f = b.volTrack;
            f.right = f.left + (int)((f.right - f.left) * frac);
            if (f.right > f.left + 1) RoundFill(dc, f, g_state.mute ? kTextDim : kText, S(4));
            Circle(dc, f.right, (f.top + f.bottom) / 2, S(5), g_state.mute ? kTextDim : RGB(255, 255, 255));
            continue;
        }
        if (b.hover == it.id || b.pressed == it.id) RoundFill(dc, it.rc, kHover, S(6));
        switch (it.id) {
        case kPrev: Text(dc, L"\xE892", it.rc, g_fonts.icon, kText, DT_CENTER); break;
        case kPlay:
            Text(dc, g_state.hasFile && !g_state.paused ? L"\xE769" : L"\xE768", it.rc, g_fonts.iconBig, kText, DT_CENTER);
            break;
        case kStop: Text(dc, L"\xE71A", it.rc, g_fonts.icon, kText, DT_CENTER); break;
        case kNext: Text(dc, L"\xE893", it.rc, g_fonts.icon, kText, DT_CENTER); break;
        case kOpen: Text(dc, L"\xE8E5", it.rc, g_fonts.icon, kText, DT_CENTER); break;
        case kMute: Text(dc, g_state.mute ? L"\xE74F" : L"\xE767", it.rc, g_fonts.icon, kText, DT_CENTER); break;
        case kFullscreen: Text(dc, g_state.fullscreen ? L"\xE73F" : L"\xE740", it.rc, g_fonts.icon, kText, DT_CENTER); break;
        case kMenu: Text(dc, L"\xE700", it.rc, g_fonts.icon, kText, DT_CENTER); break;
        case kSpeed:
            Text(dc, SpeedText(g_state.speed), it.rc, g_fonts.text,
                 std::fabs(g_state.speed - 1.0) > 0.001 ? kAccent : kText, DT_CENTER);
            break;
        case kRtx: {
            RECT r = it.rc;
            InflateRect(&r, 0, -S(4));
            RoundOutline(dc, r, kOutline, S(8));
            COLORREF dot = g_state.badgeState == BadgeState::Active ? kAccent
                         : g_state.badgeState == BadgeState::Waiting ? kAmber : kTextDim;
            Circle(dc, r.left + S(12), (r.top + r.bottom) / 2, S(4), dot);
            RECT tr = r;
            tr.left += S(22);
            tr.right -= S(8);
            Text(dc, g_state.badge, tr, g_fonts.small,
                 g_state.badgeState == BadgeState::Active ? kText : kTextDim, DT_LEFT | DT_END_ELLIPSIS);
            break;
        }
        default: break;
        }
    }

    std::wstring tt = g_state.hasFile ? FormatTime(g_state.pos) + L" / " + FormatTime(g_state.dur) : L"--:-- / --:--";
    Text(dc, tt, b.time, g_fonts.text, g_state.hasFile ? kText : kTextDim, DT_LEFT | DT_END_ELLIPSIS);

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// --- interaction ------------------------------------------------------------

void ShowSeekTip(Bar& b, int x) {
    if (!b.seekTip || g_state.dur <= 0) return;
    static wchar_t text[32];
    wcscpy_s(text, FormatTime(TrackFrac(b.track, x) * g_state.dur).c_str());
    TOOLINFOW ti{sizeof(ti)};
    ti.hwnd = b.hwnd;
    ti.uId = 1;
    ti.lpszText = text;
    SendMessageW(b.seekTip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    POINT pt{x, b.seek.top};
    ClientToScreen(b.hwnd, &pt);
    SendMessageW(b.seekTip, TTM_TRACKPOSITION, 0, MAKELPARAM(pt.x - Scale(18, b.dpi), pt.y - Scale(30, b.dpi)));
    SendMessageW(b.seekTip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
}

void HideSeekTip(Bar& b) {
    if (!b.seekTip) return;
    TOOLINFOW ti{sizeof(ti)};
    ti.hwnd = b.hwnd;
    ti.uId = 1;
    SendMessageW(b.seekTip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
}

void SeekTo(double frac, bool exact) {
    Cmd({"seek", Fmt("%.4f", frac * 100.0).c_str(), exact ? "absolute-percent+exact" : "absolute-percent+keyframes"});
}

void SetVolumeFrac(double frac) {
    SetProp("mute", "no");
    SetProp("volume", Fmt("%.0f", frac * 100.0));
}

// Popups open above their button; the right-most ones are right-aligned to it.
void PopupAt(const Bar& b, Btn id, POINT& pt) {
    const Item* it = FindItem(b, id);
    pt = it ? POINT{id == kMenu ? it->rc.right : it->rc.left, it->rc.top} : POINT{0, 0};
    ClientToScreen(b.hwnd, &pt);
}

void Press(Bar& b, Btn id) {
    POINT pt;
    switch (id) {
    case kPrev: PlaylistStep(-1); break;
    case kNext: PlaylistStep(1); break;
    case kPlay:
        if (g_state.hasFile) Cmd({"cycle", "pause"});
        else OpenFileDialog();
        break;
    case kStop: Cmd({"stop", "keep-playlist"}); break;
    case kOpen: OpenFileDialog(); break;
    case kRtx: ShowSettingsDialog(g_hwnd); break;
    case kSpeed: PopupAt(b, kSpeed, pt); ShowSpeedMenu(pt); break;
    case kMute: Cmd({"cycle", "mute"}); break;
    case kFullscreen: SetFullscreen(!g_fullscreen); break;
    case kMenu: PopupAt(b, kMenu, pt); ShowMainMenu(&pt); break;
    default: break;
    }
    ControlBarRefresh();
    FocusVideo();
}

LRESULT CALLBACK BarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Bar* b = reinterpret_cast<Bar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        b = reinterpret_cast<Bar*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        b->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)b);
    }
    if (!b) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_CREATE: {
        b->tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0,
                                  0, 0, hwnd, nullptr, nullptr, nullptr);
        for (int id = kPrev; id < kButtonCount; ++id) {
            TOOLINFOW ti{sizeof(ti)};
            ti.uFlags = TTF_SUBCLASS;
            ti.hwnd = hwnd;
            ti.uId = (UINT_PTR)id;
            ti.lpszText = const_cast<LPWSTR>(kTips[id]);
            SendMessageW(b->tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
        b->seekTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                     0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        TOOLINFOW ti{sizeof(ti)};
        ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
        ti.hwnd = hwnd;
        ti.uId = 1;
        ti.lpszText = const_cast<LPWSTR>(L"");
        SendMessageW(b->seekTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        if (!b->overlay) SetTimer(hwnd, 1, 200, nullptr);  // the docked bar drives refreshes for both
        return 0;
    }
    case WM_SIZE:
    case WM_DPICHANGED_AFTERPARENT:
        Layout(*b);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        ControlBarRefresh();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        Paint(*b, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!b->tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            b->tracking = true;
        }
        if (b->dragSeek) {
            b->dragFrac = TrackFrac(b->track, pt.x);
            if (GetTickCount64() - b->lastSeekCmd > 80) {
                SeekTo(b->dragFrac, false);
                b->lastSeekCmd = GetTickCount64();
            }
            ShowSeekTip(*b, pt.x);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (b->dragVol) {
            SetVolumeFrac(TrackFrac(b->volTrack, pt.x));
            ControlBarRefresh();
            return 0;
        }
        Btn h = HitTest(*b, pt);
        if (h == kSeek) ShowSeekTip(*b, pt.x);
        else HideSeekTip(*b);
        if (h != b->hover) {
            b->hover = h;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        b->tracking = false;
        if (!b->dragSeek) HideSeekTip(*b);
        b->hover = kNone;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        Btn h = HitTest(*b, pt);
        if (h == kSeek && g_state.dur > 0) {
            b->dragSeek = true;
            b->dragFrac = TrackFrac(b->track, pt.x);
            SeekTo(b->dragFrac, false);
            b->lastSeekCmd = GetTickCount64();
            SetCapture(hwnd);
        } else if (h == kVolume) {
            b->dragVol = true;
            SetVolumeFrac(TrackFrac(b->volTrack, pt.x));
            SetCapture(hwnd);
        } else {
            b->pressed = h;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (b->dragSeek) {
            b->dragSeek = false;
            ReleaseCapture();
            SeekTo(TrackFrac(b->track, pt.x), true);
            HideSeekTip(*b);
            FocusVideo();
        } else if (b->dragVol) {
            b->dragVol = false;
            ReleaseCapture();
            FocusVideo();
        } else {
            Btn h = HitTest(*b, pt), was = b->pressed;
            b->pressed = kNone;
            if (h == was && h != kNone) Press(*b, h);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDBLCLK:  // fast double clicks on a button count as two presses
        SendMessageW(hwnd, WM_LBUTTONDOWN, wp, lp);
        return 0;
    case WM_RBUTTONUP:
        ShowMainMenu(nullptr);
        return 0;
    case WM_MOUSEWHEEL:
        Cmd({"add", "volume", GET_WHEEL_DELTA_WPARAM(wp) > 0 ? "5" : "-5"});
        ControlBarRefresh();
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    case WM_MOUSEACTIVATE:
        return b->overlay ? MA_NOACTIVATE : MA_ACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

// ---------------------------------------------------------------------------

int ControlBarHeight(UINT dpi) { return Scale(64, dpi); }

HWND CreateControlBar(HWND parent, bool overlay) {
    static bool registered = false;
    if (!registered) {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES};
        InitCommonControlsEx(&icc);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = BarProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"RTXPlayerBar";
        RegisterClassExW(&wc);
        registered = true;
    }
    Bar& b = g_bars[overlay ? 1 : 0];
    b.overlay = overlay;
    g_state = ReadState();
    if (overlay)
        return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"RTXPlayerBar", L"", WS_POPUP, 0, 0, 100, 64,
                               parent, nullptr, GetModuleHandleW(nullptr), &b);
    return CreateWindowExW(0, L"RTXPlayerBar", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 100, 64, parent,
                           nullptr, GetModuleHandleW(nullptr), &b);
}

void ControlBarRefresh() {
    State s = ReadState();
    if (s == g_state) return;
    bool relayout = s.badge != g_state.badge;
    g_state = s;
    for (Bar& b : g_bars) {
        if (!b.hwnd) continue;
        if (relayout) Layout(b);
        InvalidateRect(b.hwnd, nullptr, FALSE);
    }
}

void HideFullscreenBar() {
    Bar& b = g_bars[1];
    if (b.hwnd && IsWindowVisible(b.hwnd)) ShowWindow(b.hwnd, SW_HIDE);
    if (b.hwnd) HideSeekTip(b);
}

// In fullscreen, show the overlay bar while the cursor is near the bottom edge.
void UpdateFullscreenBar() {
    Bar& b = g_bars[1];
    if (!b.hwnd) return;
    if (!g_fullscreen) {
        HideFullscreenBar();
        return;
    }
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    UINT dpi = GetDpiForWindow(g_hwnd);
    int h = ControlBarHeight(dpi);
    POINT pt;
    GetCursorPos(&pt);
    bool ours = GetForegroundWindow() == g_hwnd;
    bool nearBottom = PtInRect(&mi.rcMonitor, pt) && pt.y >= mi.rcMonitor.bottom - h * 2;
    ULONGLONG now = GetTickCount64();
    if ((ours && nearBottom) || b.dragSeek || b.dragVol) g_overlayLastNeeded = now;
    bool want = now - g_overlayLastNeeded < 1500;
    bool visible = IsWindowVisible(b.hwnd) != FALSE;
    if (want && !visible) {
        g_state = ReadState();
        SetWindowPos(b.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.bottom - h, mi.rcMonitor.right - mi.rcMonitor.left,
                     h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        Layout(b);
    } else if (!want && visible) {
        HideFullscreenBar();
    }
}
