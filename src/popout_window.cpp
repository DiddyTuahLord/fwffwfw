#include "stdafx.h"
#include "popout_window.h"

// Popout Mini Player
//
// A borderless layered window that floats above other windows. Each frame is
// drawn with GDI+ into a 32-bit premultiplied-alpha bitmap and handed to
// UpdateLayeredWindow, which gives true per-pixel transparency: the desktop
// shows through the card, and anti-aliased text and icons blend cleanly.

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

namespace popout {
namespace {

namespace gp = Gdiplus;

// ---------------------------------------------------------------------------
// Settings, stored in foobar2000's configuration
// ---------------------------------------------------------------------------

// {76F7230A-1B5F-492E-8F3D-F901EDF59B0F}
constexpr GUID guid_cfg_visible = { 0x76f7230a, 0x1b5f, 0x492e, { 0x8f, 0x3d, 0xf9, 0x01, 0xed, 0xf5, 0x9b, 0x0f } };
// {CA703264-AD51-4552-9270-9508DA2C4548}
constexpr GUID guid_cfg_pos_x = { 0xca703264, 0xad51, 0x4552, { 0x92, 0x70, 0x95, 0x08, 0xda, 0x2c, 0x45, 0x48 } };
// {6C469888-5DA5-4E8C-8252-B9F90FF2D159}
constexpr GUID guid_cfg_pos_y = { 0x6c469888, 0x5da5, 0x4e8c, { 0x82, 0x52, 0xb9, 0xf9, 0x0f, 0xf2, 0xd1, 0x59 } };
// {81AEE990-A26E-455D-A91D-36B6D63CC7A2}
constexpr GUID guid_cfg_bg_alpha = { 0x81aee990, 0xa26e, 0x455d, { 0xa9, 0x1d, 0x36, 0xb6, 0xd6, 0x3c, 0xc7, 0xa2 } };
// {EB86857F-AF32-4A5C-A470-3560D4561B31}
constexpr GUID guid_cfg_topmost = { 0xeb86857f, 0xaf32, 0x4a5c, { 0xa4, 0x70, 0x35, 0x60, 0xd4, 0x56, 0x1b, 0x31 } };
// {04082535-B731-41D5-8DF8-1B0D1833E357}
constexpr GUID guid_cfg_autohide = { 0x04082535, 0xb731, 0x41d5, { 0x8d, 0xf8, 0x1b, 0x0d, 0x18, 0x33, 0xe3, 0x57 } };

constexpr int kNoPosition = INT32_MIN;

cfg_bool cfg_visible(guid_cfg_visible, true);       // show at startup (when auto-hide is off)
cfg_int cfg_pos_x(guid_cfg_pos_x, kNoPosition);     // top-left corner, in screen pixels
cfg_int cfg_pos_y(guid_cfg_pos_y, kNoPosition);
cfg_int cfg_bg_alpha(guid_cfg_bg_alpha, 128);       // card tint: 0 = clear ... 255 = solid
cfg_bool cfg_topmost(guid_cfg_topmost, true);
cfg_bool cfg_autohide(guid_cfg_autohide, false);    // pop up on track change, fade out when idle

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr wchar_t kClassName[] = L"foo_popout_player_window";

constexpr UINT_PTR kTimerProgress = 1;
constexpr UINT_PTR kTimerFade = 2;
constexpr UINT_PTR kTimerAutoHide = 3;
constexpr UINT kProgressMs = 250;
constexpr UINT kFadeMs = 15;
constexpr UINT kAutoHideMs = 5000;
constexpr int kFadeStep = 24;   // about 160 ms from invisible to opaque

enum Command : UINT {
    CmdBackground0 = 1,
    CmdBackground25,
    CmdBackground50,
    CmdBackground75,
    CmdTopmost,
    CmdAutoHide,
    CmdShowFoobar,
    CmdHide,
};

struct BackgroundLevel {
    UINT command;
    const wchar_t* label;
    int alpha;
};

constexpr BackgroundLevel kBackgroundLevels[] = {
    { CmdBackground0, L"Fully transparent", 0 },
    { CmdBackground25, L"25% tint", 64 },
    { CmdBackground50, L"50% tint", 128 },
    { CmdBackground75, L"75% tint", 192 },
};

// Layout in device-independent pixels; the whole frame is scaled by DPI.
namespace layout {
    constexpr float W = 340.f, H = 124.f, Radius = 16.f, Pad = 18.f;
    constexpr float TitleY = 13.f, TitleH = 24.f;
    constexpr float ArtistY = 37.f, ArtistH = 20.f;
    constexpr float BarY = 66.f, BarThin = 4.f, BarThick = 6.f, BarHitHalf = 9.f, KnobR = 6.f;
    constexpr float ButtonsY = 96.f, PlayR = 18.f, SideR = 16.f, SideOffset = 54.f;
    constexpr float CloseX = W - 22.f, CloseY = 22.f, CloseR = 10.f;
    constexpr float TitleW = CloseX - CloseR - 6.f - Pad;   // stays clear of the close button
    constexpr float TimeW = 72.f, TimeH = 16.f;
}

enum class Part { None, Background, Seek, Prev, Play, Next, Close };

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Warm white used for all text and icons.
gp::Color Ink(int alpha) {
    return gp::Color(static_cast<BYTE>(std::clamp(alpha, 0, 255)), 245, 245, 242);
}

std::wstring Utf8ToWide(const char* s) {
    if (s == nullptr || *s == 0) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    w.resize(static_cast<size_t>(n - 1));
    return w;
}

// Tags can contain line breaks; each field is shown on one line.
std::wstring OneLine(const char* utf8) {
    std::wstring w = Utf8ToWide(utf8);
    for (wchar_t& ch : w) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
    }
    return w;
}

std::wstring TwoDigits(long long v) {
    std::wstring s(2, L'0');
    s[0] = static_cast<wchar_t>(L'0' + (v / 10) % 10);
    s[1] = static_cast<wchar_t>(L'0' + v % 10);
    return s;
}

// m:ss, or h:mm:ss for long tracks. Elapsed time rounds down, length rounds to nearest.
std::wstring FormatTime(double seconds, bool roundToNearest) {
    if (!(seconds > 0)) seconds = 0;   // also catches NaN
    const long long t = roundToNearest ? std::llround(seconds) : static_cast<long long>(std::floor(seconds));
    if (t >= 3600) {
        return std::to_wstring(t / 3600) + L":" + TwoDigits((t / 60) % 60) + L":" + TwoDigits(t % 60);
    }
    return std::to_wstring(t / 60) + L":" + TwoDigits(t % 60);
}

// GetDpiForWindow only exists on Windows 10 1607+, so it is looked up at runtime.
UINT QueryDpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    if (hwnd != nullptr && getDpiForWindow != nullptr) {
        if (const UINT dpi = getDpiForWindow(hwnd)) return dpi;
    }
    HDC screen = GetDC(nullptr);
    const int dpi = screen != nullptr ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen != nullptr) ReleaseDC(nullptr, screen);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

// Keeps a window of the given size fully inside the work area of the nearest monitor.
POINT ClampToWorkArea(POINT pt, SIZE size) {
    RECT rc = { pt.x, pt.y, pt.x + size.cx, pt.y + size.cy };
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST), &mi)) return pt;
    const RECT& wa = mi.rcWork;
    pt.x = std::clamp(pt.x, wa.left, std::max(wa.left, wa.right - size.cx));
    pt.y = std::clamp(pt.y, wa.top, std::max(wa.top, wa.bottom - size.cy));
    return pt;
}

void AddRoundRect(gp::GraphicsPath& path, const gp::RectF& r, float radius) {
    const float d = std::min({ radius * 2.f, r.Width, r.Height });
    if (d < 0.5f) {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180.f, 90.f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.f, 90.f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.f, 90.f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.f, 90.f);
    path.CloseFigure();
}

void FillRoundRect(gp::Graphics& g, const gp::RectF& r, float radius, const gp::Color& color) {
    gp::GraphicsPath path;
    AddRoundRect(path, r, radius);
    gp::SolidBrush brush(color);
    g.FillPath(&brush, &path);
}

void FillCircle(gp::Graphics& g, float cx, float cy, float r, const gp::Color& color) {
    gp::SolidBrush brush(color);
    g.FillEllipse(&brush, cx - r, cy - r, r * 2.f, r * 2.f);
}

// Text gets a soft drop shadow so it stays legible over any wallpaper.
void DrawTextWithShadow(gp::Graphics& g, const std::wstring& text, const gp::Font& font, const gp::RectF& rect,
                        const gp::StringFormat& format, const gp::Color& color) {
    if (text.empty()) return;
    const INT length = static_cast<INT>(text.size());
    gp::RectF shadowRect = rect;
    shadowRect.Offset(0.f, 1.f);
    gp::SolidBrush shadow(gp::Color(static_cast<BYTE>(color.GetA() / 2), 0, 0, 0));
    g.DrawString(text.c_str(), length, &font, shadowRect, &format, &shadow);
    gp::SolidBrush brush(color);
    g.DrawString(text.c_str(), length, &font, rect, &format, &brush);
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

class PopoutWindow final : private play_callback_impl_base {
public:
    PopoutWindow()
        : play_callback_impl_base(play_callback::flag_on_playback_new_track | play_callback::flag_on_playback_stop |
                                  play_callback::flag_on_playback_pause | play_callback::flag_on_playback_seek |
                                  play_callback::flag_on_playback_edited |
                                  play_callback::flag_on_playback_dynamic_info_track) {
        auto compiler = titleformat_compiler::get();
        compiler->compile_safe(m_scriptTitle, "%title%");
        compiler->compile_safe(m_scriptArtist, "[%artist%]");
    }

    ~PopoutWindow() { Destroy(); }

    bool Create() {
        const HINSTANCE instance = core_api::get_my_instance();

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &PopoutWindow::WndProcThunk;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

        m_dpi = QueryDpi(nullptr);
        CreateFonts();

        const SIZE size = PixelSize();
        const POINT pos = InitialPosition(size);
        DWORD exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        if (cfg_topmost.get()) exStyle |= WS_EX_TOPMOST;

        const HWND hwnd = CreateWindowExW(exStyle, kClassName, L"Popout Mini Player", WS_POPUP, pos.x, pos.y,
                                          size.cx, size.cy, nullptr, nullptr, instance, this);
        if (hwnd == nullptr) return false;

        // The window may have landed on a monitor with a different scale factor.
        m_dpi = QueryDpi(m_hwnd);
        RefreshTrackInfo();
        return true;
    }

    void Destroy() {
        if (m_hwnd != nullptr) DestroyWindow(m_hwnd);   // WM_NCDESTROY clears m_hwnd
        m_hwnd = nullptr;
        FreeSurface();
        UnregisterClassW(kClassName, core_api::get_my_instance());
    }

    bool IsShown() const { return m_hwnd != nullptr && m_fadeTarget > 0; }

    void Show(bool byUser) {
        if (m_hwnd == nullptr) return;
        if (byUser) cfg_visible.set(true);
        m_autoHiding = false;
        m_fadeTarget = 255;
        if (!IsWindowVisible(m_hwnd)) {
            m_fade = 0;
            m_hover = false;
            m_hot = Part::None;
            RefreshTrackInfo();
            Render();   // prime the layered surface while still invisible
            ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        }
        if (m_fade != m_fadeTarget) SetTimer(m_hwnd, kTimerFade, kFadeMs, nullptr);
        UpdateProgressTimer();
        RestartAutoHide();
    }

    void Hide(bool byUser) {
        if (m_hwnd == nullptr) return;
        if (byUser) cfg_visible.set(false);
        m_autoHiding = !byUser;
        m_fadeTarget = 0;
        KillTimer(m_hwnd, kTimerAutoHide);
        if (m_fade > 0 && IsWindowVisible(m_hwnd)) {
            SetTimer(m_hwnd, kTimerFade, kFadeMs, nullptr);
        } else {
            m_fade = 0;
            ShowWindow(m_hwnd, SW_HIDE);
        }
        UpdateProgressTimer();
    }

private:
    // ----- Playback notifications (always on the main thread) ---------------

    void on_playback_new_track(metadb_handle_ptr) override {
        if (m_seeking) {
            m_seeking = false;
            if (m_hwnd != nullptr && GetCapture() == m_hwnd) ReleaseCapture();
        }
        RefreshTrackInfo();
        if (cfg_autohide.get()) Show(false);
        Render();
    }

    void on_playback_stop(play_control::t_stop_reason reason) override {
        // Switching tracks reports a stop right before the next track starts;
        // skipping it avoids a flash of "Nothing playing".
        if (reason == playback_control::stop_reason_starting_another ||
            reason == playback_control::stop_reason_shutting_down) {
            return;
        }
        RefreshTrackInfo();
        Render();
    }

    void on_playback_pause(bool paused) override {
        m_paused = paused;
        RefreshPosition();
        UpdateProgressTimer();
        Render();
    }

    void on_playback_seek(double time) override {
        m_position = time;
        Render();
    }

    void on_playback_edited(metadb_handle_ptr) override {
        RefreshTrackInfo();
        Render();
    }

    void on_playback_dynamic_info_track(const file_info&) override {
        RefreshTrackInfo();   // e.g. an internet radio stream announced its next song
        if (cfg_autohide.get()) Show(false);
        Render();
    }

    // ----- Playback state ---------------------------------------------------

    void RefreshTrackInfo() {
        auto pc = playback_control::get();
        m_playing = pc->is_playing();
        m_paused = m_playing && pc->is_paused();
        if (m_playing) {
            pfc::string8 title, artist;
            pc->playback_format_title(nullptr, title, m_scriptTitle, nullptr, playback_control::display_level_titles);
            pc->playback_format_title(nullptr, artist, m_scriptArtist, nullptr, playback_control::display_level_titles);
            m_title = OneLine(title.c_str());
            m_artist = OneLine(artist.c_str());
            m_length = pc->playback_get_length_ex();
            m_seekable = pc->playback_can_seek();
            m_position = pc->playback_get_position();
        } else {
            m_title = L"Nothing playing";
            m_artist = L"Press play to start";
            m_length = 0;
            m_position = 0;
            m_seekable = false;
        }
        UpdateProgressTimer();
    }

    void RefreshPosition() {
        auto pc = playback_control::get();
        if (pc->is_playing()) m_position = pc->playback_get_position();
    }

    bool CanSeek() const { return m_playing && m_seekable && m_length > 0; }

    // ----- Timers and animation ---------------------------------------------

    void UpdateProgressTimer() {
        if (m_hwnd == nullptr) {
            m_progressTimerOn = false;
            return;
        }
        const bool want = m_fadeTarget > 0 && m_playing && !m_paused;
        if (want == m_progressTimerOn) return;
        m_progressTimerOn = want;
        if (want) SetTimer(m_hwnd, kTimerProgress, kProgressMs, nullptr);
        else KillTimer(m_hwnd, kTimerProgress);
    }

    void RestartAutoHide() {
        if (m_hwnd == nullptr) return;
        KillTimer(m_hwnd, kTimerAutoHide);
        if (cfg_autohide.get() && m_fadeTarget > 0 && !m_hover && !m_menuOpen && !m_seeking &&
            m_pressed == Part::None) {
            SetTimer(m_hwnd, kTimerAutoHide, kAutoHideMs, nullptr);
        }
    }

    void OnTimer(UINT_PTR id) {
        switch (id) {
        case kTimerProgress:
            RefreshPosition();
            Render();
            break;
        case kTimerFade:
            OnFadeTick();
            break;
        case kTimerAutoHide:
            KillTimer(m_hwnd, kTimerAutoHide);
            if (cfg_autohide.get() && !m_hover && !m_menuOpen && !m_seeking) Hide(false);
            break;
        default:
            break;
        }
    }

    void OnFadeTick() {
        if (m_fade < m_fadeTarget) m_fade = std::min(m_fadeTarget, m_fade + kFadeStep);
        else if (m_fade > m_fadeTarget) m_fade = std::max(m_fadeTarget, m_fade - kFadeStep);

        if (m_fade == m_fadeTarget) {
            KillTimer(m_hwnd, kTimerFade);
            if (m_fade == 0) {
                ShowWindow(m_hwnd, SW_HIDE);
                m_hover = false;
                m_hot = Part::None;
                m_pressed = Part::None;
                m_seeking = false;
                if (GetCapture() == m_hwnd) ReleaseCapture();
                return;
            }
        }
        Present();
    }

    // ----- Mouse ------------------------------------------------------------

    float Scale() const { return static_cast<float>(m_dpi) / 96.f; }

    Part HitTest(float x, float y) const {
        using namespace layout;
        if (x < 0 || y < 0 || x > W || y > H) return Part::None;
        const auto inCircle = [x, y](float cx, float cy, float r) {
            const float dx = x - cx, dy = y - cy;
            return dx * dx + dy * dy <= r * r;
        };
        if (m_hover && inCircle(CloseX, CloseY, CloseR + 3.f)) return Part::Close;
        if (inCircle(W / 2.f, ButtonsY, PlayR + 2.f)) return Part::Play;
        if (inCircle(W / 2.f - SideOffset, ButtonsY, SideR)) return Part::Prev;
        if (inCircle(W / 2.f + SideOffset, ButtonsY, SideR)) return Part::Next;
        if (CanSeek() && x >= Pad - 6.f && x <= W - Pad + 6.f && std::fabs(y - BarY) <= BarHitHalf) return Part::Seek;
        return Part::Background;
    }

    float SeekFraction(float x) const {
        return std::clamp((x - layout::Pad) / (layout::W - 2.f * layout::Pad), 0.f, 1.f);
    }

    void TrackLeave() {
        if (m_trackingLeave) return;
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = m_hwnd;
        m_trackingLeave = TrackMouseEvent(&tme) != FALSE;
    }

    void OnMouseMove(int px, int py) {
        TrackLeave();
        bool dirty = false;
        if (!m_hover) {
            m_hover = true;
            dirty = true;
            KillTimer(m_hwnd, kTimerAutoHide);
            if (m_autoHiding && m_fadeTarget == 0) Show(false);   // reached for it while it was fading out
        }
        const float x = static_cast<float>(px) / Scale(), y = static_cast<float>(py) / Scale();
        if (m_seeking) {
            m_seekFraction = SeekFraction(x);
            Render();
            return;
        }
        const Part hot = HitTest(x, y);
        if (hot != m_hot) {
            m_hot = hot;
            dirty = true;
        }
        if (dirty) Render();
    }

    void OnMouseLeave() {
        m_trackingLeave = false;
        if (m_seeking || m_pressed != Part::None) return;   // still dragging; tidied up on button release
        m_hover = false;
        m_hot = Part::None;
        Render();
        RestartAutoHide();
    }

    void OnLButtonDown(int px, int py) {
        const float x = static_cast<float>(px) / Scale(), y = static_cast<float>(py) / Scale();
        const Part part = HitTest(x, y);
        switch (part) {
        case Part::Seek:
            m_seeking = true;
            m_seekFraction = SeekFraction(x);
            SetCapture(m_hwnd);
            Render();
            break;
        case Part::Prev:
        case Part::Play:
        case Part::Next:
        case Part::Close:
            m_pressed = part;
            m_hot = part;
            SetCapture(m_hwnd);
            Render();
            break;
        default:
            BeginWindowDrag();
            break;
        }
    }

    void OnLButtonUp(int px, int py) {
        const float x = static_cast<float>(px) / Scale(), y = static_cast<float>(py) / Scale();
        if (m_seeking) {
            m_seeking = false;
            ReleaseCapture();
            auto pc = playback_control::get();
            if (pc->is_playing() && pc->playback_can_seek() && m_length > 0) {
                const double target = static_cast<double>(SeekFraction(x)) * m_length;
                pc->playback_seek(target);
                m_position = target;
            }
        } else if (m_pressed != Part::None) {
            const Part pressed = m_pressed;
            m_pressed = Part::None;
            ReleaseCapture();
            if (HitTest(x, y) == pressed) Activate(pressed);
        }
        SyncHoverWithCursor();
    }

    // Capture was taken away mid-drag (Alt+Tab and similar): cancel quietly.
    void OnCaptureChanged() {
        if (!m_seeking && m_pressed == Part::None) return;
        m_seeking = false;
        m_pressed = Part::None;
        SyncHoverWithCursor();
    }

    bool OnSetCursor() {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(m_hwnd, &pt);
        const Part part = m_seeking ? Part::Seek
                                    : HitTest(static_cast<float>(pt.x) / Scale(), static_cast<float>(pt.y) / Scale());
        const bool hand = part == Part::Seek || part == Part::Prev || part == Part::Play || part == Part::Next ||
                          part == Part::Close;
        SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
        return true;
    }

    // Lets Windows run its normal move loop, as if the card were a title bar.
    void BeginWindowDrag() {
        POINT pt{};
        GetCursorPos(&pt);
        ReleaseCapture();
        SendMessageW(m_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(pt.x, pt.y));
    }

    void Activate(Part part) {
        auto pc = playback_control::get();
        switch (part) {
        case Part::Prev: pc->previous(); break;
        case Part::Play: pc->play_or_pause(); break;
        case Part::Next: pc->next(); break;
        case Part::Close: Hide(true); break;
        default: break;
        }
    }

    // Re-reads hover state from the real cursor position after menus, drags and captures.
    void SyncHoverWithCursor() {
        if (m_hwnd == nullptr) return;
        POINT pt{};
        RECT rc{};
        GetCursorPos(&pt);
        GetWindowRect(m_hwnd, &rc);
        if (IsWindowVisible(m_hwnd) && PtInRect(&rc, pt)) {
            ScreenToClient(m_hwnd, &pt);
            m_hover = true;
            m_hot = HitTest(static_cast<float>(pt.x) / Scale(), static_cast<float>(pt.y) / Scale());
            m_trackingLeave = false;
            TrackLeave();
            KillTimer(m_hwnd, kTimerAutoHide);
        } else {
            m_hover = false;
            m_hot = Part::None;
            RestartAutoHide();
        }
        Render();
    }

    void ShowContextMenu(POINT screenPt) {
        HMENU menu = CreatePopupMenu();
        HMENU background = CreatePopupMenu();
        if (menu == nullptr || background == nullptr) {
            if (menu != nullptr) DestroyMenu(menu);
            if (background != nullptr) DestroyMenu(background);
            return;
        }

        const int currentAlpha = static_cast<int>(cfg_bg_alpha.get());
        UINT checked = CmdBackground0;
        int bestDistance = 1000;
        for (const auto& level : kBackgroundLevels) {
            AppendMenuW(background, MF_STRING, level.command, level.label);
            const int distance = std::abs(level.alpha - currentAlpha);
            if (distance < bestDistance) {
                bestDistance = distance;
                checked = level.command;
            }
        }
        CheckMenuRadioItem(background, CmdBackground0, CmdBackground75, checked, MF_BYCOMMAND);

        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(background), L"Background");
        AppendMenuW(menu, MF_STRING | (cfg_topmost.get() ? MF_CHECKED : MF_UNCHECKED), CmdTopmost, L"Always on top");
        AppendMenuW(menu, MF_STRING | (cfg_autohide.get() ? MF_CHECKED : MF_UNCHECKED), CmdAutoHide,
                    L"Auto-hide, pop up on track change");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, CmdShowFoobar, L"Show foobar2000");
        AppendMenuW(menu, MF_STRING, CmdHide, L"Hide");

        m_menuOpen = true;
        KillTimer(m_hwnd, kTimerAutoHide);
        SetForegroundWindow(m_hwnd);   // lets the menu close when clicking elsewhere
        const UINT command = static_cast<UINT>(TrackPopupMenu(
            menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screenPt.x, screenPt.y, 0, m_hwnd, nullptr));
        PostMessageW(m_hwnd, WM_NULL, 0, 0);
        m_menuOpen = false;
        DestroyMenu(menu);   // also destroys the submenu

        switch (command) {
        case CmdBackground0:
        case CmdBackground25:
        case CmdBackground50:
        case CmdBackground75:
            for (const auto& level : kBackgroundLevels) {
                if (level.command == command) cfg_bg_alpha.set(level.alpha);
            }
            break;
        case CmdTopmost:
            SetTopmost(!cfg_topmost.get());
            break;
        case CmdAutoHide:
            cfg_autohide.set(!cfg_autohide.get());
            if (!cfg_autohide.get()) cfg_visible.set(true);   // it is showing now, so keep it that way
            break;
        case CmdShowFoobar:
            ui_control::get()->activate();
            break;
        case CmdHide:
            Hide(true);
            break;
        default:
            break;
        }
        SyncHoverWithCursor();   // re-renders and re-arms auto-hide
    }

    // ----- Placement --------------------------------------------------------

    SIZE PixelSize() const {
        const float s = Scale();
        return { static_cast<LONG>(std::lround(layout::W * s)), static_cast<LONG>(std::lround(layout::H * s)) };
    }

    POINT InitialPosition(SIZE size) const {
        const int64_t x = cfg_pos_x.get();
        const int64_t y = cfg_pos_y.get();
        if (x != kNoPosition && y != kNoPosition) {
            return ClampToWorkArea({ static_cast<LONG>(x), static_cast<LONG>(y) }, size);
        }
        // First run: bottom-right corner of the primary work area.
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const LONG margin = MulDiv(24, static_cast<int>(m_dpi), 96);
        return { work.right - size.cx - margin, work.bottom - size.cy - margin };
    }

    // Called while dragging: sticks the window to work-area edges when it gets close.
    void SnapToWorkArea(RECT* rc) const {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(MonitorFromRect(rc, MONITOR_DEFAULTTONEAREST), &mi)) return;
        const RECT& wa = mi.rcWork;
        const LONG snap = MulDiv(12, static_cast<int>(m_dpi), 96);
        const LONG w = rc->right - rc->left;
        const LONG h = rc->bottom - rc->top;
        if (std::abs(rc->left - wa.left) < snap) rc->left = wa.left;
        else if (std::abs(rc->right - wa.right) < snap) rc->left = wa.right - w;
        if (std::abs(rc->top - wa.top) < snap) rc->top = wa.top;
        else if (std::abs(rc->bottom - wa.bottom) < snap) rc->top = wa.bottom - h;
        rc->right = rc->left + w;
        rc->bottom = rc->top + h;
    }

    void SavePosition() {
        RECT rc{};
        if (m_hwnd != nullptr && GetWindowRect(m_hwnd, &rc)) {
            cfg_pos_x.set(rc.left);
            cfg_pos_y.set(rc.top);
        }
    }

    void SetTopmost(bool topmost) {
        cfg_topmost.set(topmost);
        SetWindowPos(m_hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // A monitor went away or changed resolution: pull the window back into view.
    void EnsureOnScreen() {
        RECT rc{};
        if (!GetWindowRect(m_hwnd, &rc)) return;
        const SIZE size = { rc.right - rc.left, rc.bottom - rc.top };
        const POINT pt = ClampToWorkArea({ rc.left, rc.top }, size);
        if (pt.x != rc.left || pt.y != rc.top) {
            SetWindowPos(m_hwnd, nullptr, pt.x, pt.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    void OnDpiChanged(UINT dpi, const RECT* suggested) {
        m_dpi = dpi != 0 ? dpi : 96;
        if (suggested != nullptr) {
            SetWindowPos(m_hwnd, nullptr, suggested->left, suggested->top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        Render();   // UpdateLayeredWindow picks up the new pixel size
    }

    // ----- Rendering --------------------------------------------------------

    void CreateFonts() {
        m_familyText = std::make_unique<gp::FontFamily>(L"Segoe UI");
        if (m_familyText->GetLastStatus() != gp::Ok) m_familyText.reset();
        m_familyStrong = std::make_unique<gp::FontFamily>(L"Segoe UI Semibold");
        if (m_familyStrong->GetLastStatus() != gp::Ok) m_familyStrong.reset();

        const gp::FontFamily* text = m_familyText ? m_familyText.get() : gp::FontFamily::GenericSansSerif();
        if (m_familyStrong) {
            m_fontTitle = std::make_unique<gp::Font>(m_familyStrong.get(), 15.f, gp::FontStyleRegular, gp::UnitPixel);
        } else {
            m_fontTitle = std::make_unique<gp::Font>(text, 15.f, gp::FontStyleBold, gp::UnitPixel);
        }
        m_fontArtist = std::make_unique<gp::Font>(text, 12.5f, gp::FontStyleRegular, gp::UnitPixel);
        m_fontTime = std::make_unique<gp::Font>(text, 11.f, gp::FontStyleRegular, gp::UnitPixel);
    }

    bool EnsureSurface(int width, int height) {
        if (m_memDC != nullptr && width == m_surfaceW && height == m_surfaceH) return true;
        FreeSurface();

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height;   // top-down rows
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        HDC screen = GetDC(nullptr);
        m_memDC = CreateCompatibleDC(screen);
        m_dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &m_bits, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (m_memDC == nullptr || m_dib == nullptr || m_bits == nullptr) {
            FreeSurface();
            return false;
        }
        m_oldBitmap = SelectObject(m_memDC, m_dib);
        m_surfaceW = width;
        m_surfaceH = height;
        return true;
    }

    void FreeSurface() {
        if (m_memDC != nullptr) {
            if (m_oldBitmap != nullptr) SelectObject(m_memDC, m_oldBitmap);
            DeleteDC(m_memDC);
        }
        if (m_dib != nullptr) DeleteObject(m_dib);
        m_memDC = nullptr;
        m_dib = nullptr;
        m_oldBitmap = nullptr;
        m_bits = nullptr;
        m_surfaceW = 0;
        m_surfaceH = 0;
    }

    void Render() {
        if (m_hwnd == nullptr) return;
        if (m_fadeTarget == 0 && !IsWindowVisible(m_hwnd)) return;   // hidden: nothing to draw

        const SIZE size = PixelSize();
        if (!EnsureSurface(size.cx, size.cy)) return;
        GdiFlush();
        {
            // Draw straight into the DIB's memory in premultiplied ARGB, the format UpdateLayeredWindow expects.
            gp::Bitmap bitmap(size.cx, size.cy, size.cx * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(m_bits));
            gp::Graphics g(&bitmap);
            g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
            g.SetTextRenderingHint(gp::TextRenderingHintAntiAliasGridFit);   // grayscale AA: correct on alpha
            g.Clear(gp::Color(0, 0, 0, 0));
            g.ScaleTransform(Scale(), Scale());
            Paint(g);
        }
        Present();
    }

    void Present() {
        if (m_hwnd == nullptr || m_memDC == nullptr) return;
        SIZE size = { m_surfaceW, m_surfaceH };
        POINT origin = { 0, 0 };
        BLENDFUNCTION blend = { AC_SRC_OVER, 0, static_cast<BYTE>(m_fade), AC_SRC_ALPHA };
        UpdateLayeredWindow(m_hwnd, nullptr, nullptr, &size, m_memDC, &origin, 0, &blend, ULW_ALPHA);
    }

    void Paint(gp::Graphics& g) {
        using namespace layout;
        const bool chrome = m_hover || m_seeking;   // extra detail while the mouse is over the window

        // Card. Never fully transparent, so clicks land on the window instead of falling through.
        int cardAlpha = static_cast<int>(std::clamp<int64_t>(cfg_bg_alpha.get(), 0, 255));
        if (chrome) cardAlpha = std::max(cardAlpha, 96);   // reveal the card's edges while interacting
        cardAlpha = std::max(cardAlpha, 1);
        {
            gp::GraphicsPath card;
            AddRoundRect(card, gp::RectF(0.5f, 0.5f, W - 1.f, H - 1.f), Radius);
            gp::SolidBrush fill(gp::Color(static_cast<BYTE>(cardAlpha), 18, 19, 23));
            g.FillPath(&fill, &card);
            if (cardAlpha >= 24) {
                gp::Pen edge(Ink(cardAlpha / 4 + 8), 1.f);
                g.DrawPath(&edge, &card);
            }
        }

        // Title and artist, truncated with an ellipsis.
        gp::StringFormat line(gp::StringFormatFlagsNoWrap);
        line.SetTrimming(gp::StringTrimmingEllipsisCharacter);
        line.SetLineAlignment(gp::StringAlignmentCenter);
        DrawTextWithShadow(g, m_title, *m_fontTitle, gp::RectF(Pad, TitleY, TitleW, TitleH), line, Ink(255));
        DrawTextWithShadow(g, m_artist, *m_fontArtist, gp::RectF(Pad, ArtistY, W - 2.f * Pad, ArtistH), line, Ink(185));

        // Progress bar. Thickens and grows a knob when it can be dragged.
        const double shown = m_seeking ? static_cast<double>(m_seekFraction) * m_length : m_position;
        const float progress = m_length > 0 ? static_cast<float>(std::clamp(shown / m_length, 0.0, 1.0)) : 0.f;
        const bool barHot = CanSeek() && (m_seeking || m_hot == Part::Seek);
        const float thickness = barHot ? BarThick : BarThin;
        const gp::RectF rail(Pad, BarY - thickness / 2.f, W - 2.f * Pad, thickness);
        FillRoundRect(g, rail, thickness / 2.f, Ink(60));
        if (progress > 0.f) {
            const float filled = std::max(thickness, rail.Width * progress);
            FillRoundRect(g, gp::RectF(rail.X, rail.Y, filled, thickness), thickness / 2.f, Ink(240));
        }
        if (barHot) {
            const float knobX = rail.X + rail.Width * progress;
            FillCircle(g, knobX, BarY + 1.f, KnobR, gp::Color(70, 0, 0, 0));
            FillCircle(g, knobX, BarY, KnobR, gp::Color(255, 255, 255, 255));
        }

        // Elapsed and total time, either side of the buttons.
        std::wstring elapsed, total;
        if (m_playing) {
            elapsed = FormatTime(shown, false);
            total = m_length > 0 ? FormatTime(m_length, true) : std::wstring(L"live");
        }
        gp::StringFormat left(gp::StringFormatFlagsNoWrap);
        left.SetLineAlignment(gp::StringAlignmentCenter);
        gp::StringFormat right(gp::StringFormatFlagsNoWrap);
        right.SetAlignment(gp::StringAlignmentFar);
        right.SetLineAlignment(gp::StringAlignmentCenter);
        const gp::Color timeInk = Ink(chrome ? 225 : 165);
        const float timeY = ButtonsY - TimeH / 2.f;
        DrawTextWithShadow(g, elapsed, *m_fontTime, gp::RectF(Pad, timeY, TimeW, TimeH), left, timeInk);
        DrawTextWithShadow(g, total, *m_fontTime, gp::RectF(W - Pad - TimeW, timeY, TimeW, TimeH), right, timeInk);

        // Transport buttons; "down" only while the pressed button is still under the cursor.
        PaintSideButton(g, W / 2.f - SideOffset, false, m_hot == Part::Prev, m_pressed == Part::Prev && m_hot == Part::Prev);
        PaintPlayButton(g, m_hot == Part::Play, m_pressed == Part::Play && m_hot == Part::Play);
        PaintSideButton(g, W / 2.f + SideOffset, true, m_hot == Part::Next, m_pressed == Part::Next && m_hot == Part::Next);
        if (chrome) PaintCloseButton(g, m_hot == Part::Close, m_pressed == Part::Close && m_hot == Part::Close);
    }

    // The play button is the one solid element: a white disc that reads on any background.
    void PaintPlayButton(gp::Graphics& g, bool hot, bool down) {
        const float cx = layout::W / 2.f, cy = layout::ButtonsY, r = layout::PlayR;
        FillCircle(g, cx, cy + 1.5f, r, gp::Color(70, 0, 0, 0));
        const gp::Color disc = down  ? gp::Color(255, 205, 205, 202)
                               : hot ? gp::Color(255, 255, 255, 255)
                                     : gp::Color(255, 238, 238, 235);
        FillCircle(g, cx, cy, r, disc);

        const gp::Color ink(255, 22, 23, 27);
        if (m_playing && !m_paused) {
            FillRoundRect(g, gp::RectF(cx - 6.f, cy - 7.f, 4.f, 14.f), 1.2f, ink);
            FillRoundRect(g, gp::RectF(cx + 2.f, cy - 7.f, 4.f, 14.f), 1.2f, ink);
        } else {
            gp::SolidBrush brush(ink);
            const gp::PointF triangle[3] = {
                gp::PointF(cx - 4.f, cy - 7.5f),
                gp::PointF(cx - 4.f, cy + 7.5f),
                gp::PointF(cx + 8.f, cy),
            };
            g.FillPolygon(&brush, triangle, 3);
        }
    }

    void PaintSideButton(gp::Graphics& g, float cx, bool next, bool hot, bool down) {
        const float cy = layout::ButtonsY;
        if (hot || down) FillCircle(g, cx, cy, layout::SideR, Ink(down ? 70 : 38));
        const float dir = next ? 1.f : -1.f;
        const auto shape = [&](const gp::Color& color, float dy) {
            gp::SolidBrush brush(color);
            const gp::PointF triangle[3] = {
                gp::PointF(cx - dir * 6.f, cy - 7.f + dy),
                gp::PointF(cx - dir * 6.f, cy + 7.f + dy),
                gp::PointF(cx + dir * 4.5f, cy + dy),
            };
            g.FillPolygon(&brush, triangle, 3);
            const float barX = next ? cx + 4.5f : cx - 7.f;
            g.FillRectangle(&brush, gp::RectF(barX, cy - 7.f + dy, 2.5f, 14.f));
        };
        shape(gp::Color(90, 0, 0, 0), 1.f);   // shadow
        shape(Ink(hot ? 255 : 235), 0.f);
    }

    void PaintCloseButton(gp::Graphics& g, bool hot, bool down) {
        const float cx = layout::CloseX, cy = layout::CloseY;
        FillCircle(g, cx, cy, layout::CloseR, hot ? gp::Color(down ? 255 : 230, 196, 43, 28) : Ink(45));
        gp::Pen pen(Ink(245), 1.6f);
        pen.SetStartCap(gp::LineCapRound);
        pen.SetEndCap(gp::LineCapRound);
        const float k = 3.5f;
        g.DrawLine(&pen, cx - k, cy - k, cx + k, cy + k);
        g.DrawLine(&pen, cx - k, cy + k, cx + k, cy - k);
    }

    // ----- Window procedure -------------------------------------------------

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        PopoutWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<PopoutWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->m_hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<PopoutWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (self == nullptr) return DefWindowProcW(hwnd, msg, wp, lp);
        try {
            return self->WndProc(msg, wp, lp);
        } catch (const std::exception& e) {
            FB2K_console_formatter() << "Popout Mini Player: " << e.what();
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_MOUSEMOVE:
            OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSELEAVE:
            OnMouseLeave();
            return 0;
        case WM_LBUTTONDOWN:
            OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_LBUTTONUP:
            OnLButtonUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_RBUTTONUP: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(m_hwnd, &pt);
            ShowContextMenu(pt);
            return 0;
        }
        case WM_CAPTURECHANGED:
            OnCaptureChanged();
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && OnSetCursor()) return TRUE;
            break;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;   // clicking the mini player never steals focus
        case WM_TIMER:
            OnTimer(wp);
            return 0;
        case WM_MOVING:
            SnapToWorkArea(reinterpret_cast<RECT*>(lp));
            return TRUE;
        case WM_EXITSIZEMOVE:
            SavePosition();
            return 0;
        case WM_DPICHANGED:
            OnDpiChanged(HIWORD(wp), reinterpret_cast<const RECT*>(lp));
            return 0;
        case WM_DISPLAYCHANGE:
            EnsureOnScreen();
            break;
        case WM_CLOSE:   // e.g. Alt+F4 right after using the context menu
            Hide(true);
            return 0;
        case WM_NCDESTROY: {
            const LRESULT result = DefWindowProcW(m_hwnd, msg, wp, lp);
            SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
            m_hwnd = nullptr;
            m_progressTimerOn = false;
            return result;
        }
        default:
            break;
        }
        return DefWindowProcW(m_hwnd, msg, wp, lp);
    }

    // ----- Members ----------------------------------------------------------

    HWND m_hwnd = nullptr;
    UINT m_dpi = 96;

    // What's playing
    titleformat_object::ptr m_scriptTitle, m_scriptArtist;
    std::wstring m_title, m_artist;
    bool m_playing = false, m_paused = false, m_seekable = false;
    double m_position = 0, m_length = 0;

    // Interaction
    Part m_hot = Part::None, m_pressed = Part::None;
    bool m_hover = false, m_trackingLeave = false, m_seeking = false, m_menuOpen = false;
    float m_seekFraction = 0;

    // Animation
    int m_fade = 0, m_fadeTarget = 0;   // current and target window opacity, 0..255
    bool m_autoHiding = false;          // the current fade-out was started by auto-hide
    bool m_progressTimerOn = false;

    // Drawing surface
    HDC m_memDC = nullptr;
    HBITMAP m_dib = nullptr;
    HGDIOBJ m_oldBitmap = nullptr;
    void* m_bits = nullptr;
    int m_surfaceW = 0, m_surfaceH = 0;

    std::unique_ptr<gp::FontFamily> m_familyText, m_familyStrong;
    std::unique_ptr<gp::Font> m_fontTitle, m_fontArtist, m_fontTime;
};

ULONG_PTR g_gdiplusToken = 0;
PopoutWindow* g_window = nullptr;

}   // namespace

// ---------------------------------------------------------------------------
// Public entry points (main thread)
// ---------------------------------------------------------------------------

void startup() {
    gp::GdiplusStartupInput input;
    if (gp::GdiplusStartup(&g_gdiplusToken, &input, nullptr) != gp::Ok) {
        g_gdiplusToken = 0;
        console::print("Popout Mini Player: GDI+ could not start, so the mini player is unavailable.");
        return;
    }

    auto window = std::make_unique<PopoutWindow>();
    if (!window->Create()) {
        console::print("Popout Mini Player: could not create its window.");
        return;
    }
    g_window = window.release();

    if (cfg_autohide.get()) {
        if (playback_control::get()->is_playing()) g_window->Show(false);
    } else if (cfg_visible.get()) {
        g_window->Show(false);
    }
}

void shutdown() {
    delete g_window;   // destroys the window and its GDI+ objects before GDI+ shuts down
    g_window = nullptr;
    if (g_gdiplusToken != 0) {
        gp::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

bool is_visible() {
    return g_window != nullptr && g_window->IsShown();
}

void toggle() {
    if (g_window == nullptr) return;
    if (g_window->IsShown()) g_window->Hide(true);
    else g_window->Show(true);
}

}   // namespace popout
