#include "daemon/overlay/overlay.h"

#include <d2d1.h>
#include <dwrite.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "common/log.h"
#include "common/strings.h"
#include "daemon/system/inject.h"
#include "daemon/system/target.h"

namespace wa {
namespace {

constexpr wchar_t kClassName[] = L"WinAssistOverlay";
constexpr UINT_PTR kAnimTimer = 1;
constexpr float kFadeMs = 140.0f;

template <class T>
void safe_release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

D2D1_COLOR_F rgba(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

struct Palette {
    uint32_t a;
    uint32_t b;
};

Palette palette_for(OverlayView::Mode mode) {
    switch (mode) {
        case OverlayView::Mode::Listening: return {0xFF4D6D, 0xB5179E};
        case OverlayView::Mode::Input: return {0x4CC9F0, 0x4361EE};
        case OverlayView::Mode::Processing: return {0x7B61FF, 0x4CC9F0};
        case OverlayView::Mode::Response: return {0x2EC4B6, 0x4CC9F0};
        case OverlayView::Mode::Error: return {0xFFB703, 0xFB8500};
        case OverlayView::Mode::Confirm: return {0xFF4D6D, 0xFFB703};
    }
    return {0x4CC9F0, 0x4361EE};
}

IDWriteTextFormat* make_format(IDWriteFactory* dw, float size, DWRITE_FONT_WEIGHT weight) {
    IDWriteTextFormat* format = nullptr;
    dw->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                         DWRITE_FONT_STRETCH_NORMAL, size, L"", &format);
    if (!format) {
        dw->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                             L"", &format);
    }
    if (format) format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    return format;
}

bool mode_has_composer(OverlayView::Mode mode) {
    return mode == OverlayView::Mode::Input || mode == OverlayView::Mode::Response ||
           mode == OverlayView::Mode::Error || mode == OverlayView::Mode::Processing;
}

// Confirm takes keyboard focus but shows buttons instead of a text field.
bool mode_is_modal(OverlayView::Mode mode) { return mode == OverlayView::Mode::Confirm; }

}  // namespace

Overlay::~Overlay() { destroy(); }

bool Overlay::composer_active() const {
    return visible_ && mode_has_composer(view_.mode) && view_.mode != OverlayView::Mode::Processing;
}

bool Overlay::create(HINSTANCE instance, Callbacks callbacks) {
    callbacks_ = std::move(callbacks);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Overlay::wndproc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                            kClassName, L"win-assist", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    return hwnd_ != nullptr;
}

void Overlay::destroy() {
    release_resources();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void Overlay::configure(int width_dip, bool at_top) {
    width_dip_ = std::clamp(width_dip, 320, 1200);
    at_top_ = at_top;
}

bool Overlay::hidden_for(std::chrono::milliseconds d) const {
    return !visible_ && fade_ <= 0.0f && std::chrono::steady_clock::now() - hidden_at_ >= d;
}

void Overlay::release_resources() {
    safe_release(heading_format_);
    safe_release(body_format_);
    safe_release(hint_format_);
    safe_release(target_);
    safe_release(dwrite_);
    safe_release(d2d_);
    format_scale_ = 0.0f;
    if (mem_dc_) {
        SelectObject(mem_dc_, old_bitmap_);
        DeleteDC(mem_dc_);
        mem_dc_ = nullptr;
    }
    if (bitmap_) {
        DeleteObject(bitmap_);
        bitmap_ = nullptr;
    }
    bitmap_w_ = bitmap_h_ = 0;
}

float Overlay::scale() const {
    HMONITOR monitor = MonitorFromWindow(anchor_ ? anchor_ : hwnd_, MONITOR_DEFAULTTONEAREST);
    UINT dpi_x = 96, dpi_y = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) dpi_x = 96;
    return dpi_x / 96.0f;
}

bool Overlay::ensure_resources() {
    if (!d2d_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_))) return false;
    if (!dwrite_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                               reinterpret_cast<IUnknown**>(&dwrite_)))) {
        return false;
    }
    if (!target_) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        if (FAILED(d2d_->CreateDCRenderTarget(&props, &target_))) return false;
        target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    const float s = scale();
    if (format_scale_ != s) {
        safe_release(heading_format_);
        safe_release(body_format_);
        safe_release(hint_format_);
        heading_format_ = make_format(dwrite_, 12.5f * s, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        body_format_ = make_format(dwrite_, 15.0f * s, DWRITE_FONT_WEIGHT_NORMAL);
        hint_format_ = make_format(dwrite_, 11.5f * s, DWRITE_FONT_WEIGHT_NORMAL);
        format_scale_ = s;
    }
    return heading_format_ && body_format_ && hint_format_;
}

void Overlay::stick_chat_to_bottom() {
    stick_bottom_ = true;
    chat_scroll_ = 0.0f;
}

void Overlay::scroll_by(float delta_px) {
    if (chat_content_h_ <= chat_viewport_h_) {
        chat_scroll_ = 0.0f;
        stick_bottom_ = true;
        return;
    }
    const float max_scroll = chat_content_h_ - chat_viewport_h_;
    chat_scroll_ = std::clamp(chat_scroll_ + delta_px, 0.0f, max_scroll);
    stick_bottom_ = chat_scroll_ <= 1.0f;
}

void Overlay::show(const OverlayView& view, HWND anchor) {
    anchor_ = anchor;
    view_ = view;
    if (!visible_) {
        shown_at_ = std::chrono::steady_clock::now();
        fade_ = std::max(fade_, 0.0f);
    }
    visible_ = true;
    fade_target_ = 1.0f;
    focus_reclaimed_ = false;
    stick_chat_to_bottom();
    set_interactive(mode_has_composer(view.mode) || mode_is_modal(view.mode));
    last_tick_ = std::chrono::steady_clock::now();
    render();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (composer_active() || confirm_active()) {
        force_foreground(hwnd_);
        SetFocus(hwnd_);
    }
    SetTimer(hwnd_, kAnimTimer, 16, nullptr);
}

void Overlay::update(const OverlayView& view) {
    if (!visible_) return;
    const bool stick = stick_bottom_;
    const bool was_modal = mode_is_modal(view_.mode);
    view_ = view;
    set_interactive(mode_has_composer(view.mode) || mode_is_modal(view.mode));
    if (stick) stick_chat_to_bottom();
    render();
    // A confirmation must be able to receive Enter/Esc the moment it appears.
    if (!was_modal && mode_is_modal(view.mode)) {
        force_foreground(hwnd_);
        SetFocus(hwnd_);
    }
}

void Overlay::append_chat(std::wstring_view text) {
    view_.chat.append(text);
    if (stick_bottom_) chat_scroll_ = 0.0f;
    if (visible_) render();
}

void Overlay::hide() {
    if (!visible_) return;
    visible_ = false;
    fade_target_ = 0.0f;
    set_interactive(false);
    last_tick_ = std::chrono::steady_clock::now();
    SetTimer(hwnd_, kAnimTimer, 16, nullptr);
}

void Overlay::set_interactive(bool interactive) {
    if (interactive_ == interactive) return;
    interactive_ = interactive;
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (interactive) ex &= ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE | WS_EX_TRANSPARENT);
    else ex |= WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
}

void Overlay::tick() {
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float, std::milli>(now - last_tick_).count();
    last_tick_ = now;
    if (fade_ < fade_target_) fade_ = std::min(fade_target_, fade_ + dt / kFadeMs);
    else if (fade_ > fade_target_) fade_ = std::max(fade_target_, fade_ - dt / kFadeMs);
    level_smoothed_ += (level_ - level_smoothed_) * std::min(1.0f, dt / 60.0f);

    if (!visible_ && fade_ <= 0.0f) {
        KillTimer(hwnd_, kAnimTimer);
        ShowWindow(hwnd_, SW_HIDE);
        hidden_at_ = now;
        return;
    }
    render();
}

void Overlay::place(int width, int height) {
    HMONITOR monitor = MonitorFromWindow(anchor_ && IsWindow(anchor_) ? anchor_ : GetForegroundWindow(),
                                         MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor, &mi);
    const RECT& work = mi.rcWork;
    const int margin = static_cast<int>(56 * scale());
    const int x = work.left + (work.right - work.left - width) / 2;
    const int y = at_top_ ? work.top + margin : work.bottom - margin - height;
    POINT dst{x, y};
    SIZE size{width, height};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(std::clamp(fade_, 0.0f, 1.0f) * 255.0f), AC_SRC_ALPHA};
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(hwnd_, screen, &dst, &size, mem_dc_, &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

void Overlay::render() {
    if (!hwnd_ || !ensure_resources()) return;
    const float s = scale();
    const float width = std::round(width_dip_ * s);
    const float pad = 16.0f * s;
    const float orb = 34.0f * s;
    const float text_x = pad + orb + 14.0f * s;
    const float text_w = width - text_x - pad;
    const float max_chat_h = 360.0f * s;
    const float prompt_min_h = 28.0f * s;
    const bool composer = mode_has_composer(view_.mode);
    const bool listening = view_.mode == OverlayView::Mode::Listening;

    IDWriteTextLayout* heading = nullptr;
    IDWriteTextLayout* chat_layout = nullptr;
    IDWriteTextLayout* prompt_layout = nullptr;
    IDWriteTextLayout* hint = nullptr;

    dwrite_->CreateTextLayout(view_.heading.c_str(), static_cast<UINT32>(view_.heading.size()), heading_format_, text_w,
                              80.0f * s, &heading);

    const std::wstring& chat_text = view_.chat;
    dwrite_->CreateTextLayout(chat_text.c_str(), static_cast<UINT32>(chat_text.size()), body_format_, text_w,
                              4000.0f * s, &chat_layout);

    const bool prompt_placeholder = composer && view_.prompt.empty() && view_.mode != OverlayView::Mode::Processing;
    const std::wstring prompt_draw =
        prompt_placeholder ? L"Message win-assist\u2026" : view_.prompt;
    if (composer) {
        dwrite_->CreateTextLayout(prompt_draw.c_str(), static_cast<UINT32>(prompt_draw.size()), body_format_, text_w,
                                  120.0f * s, &prompt_layout);
    }
    if (!view_.hint.empty()) {
        dwrite_->CreateTextLayout(view_.hint.c_str(), static_cast<UINT32>(view_.hint.size()), hint_format_, text_w,
                                  60.0f * s, &hint);
    }

    DWRITE_TEXT_METRICS hm{}, cm{}, pm{}, tm{};
    if (heading) heading->GetMetrics(&hm);
    if (chat_layout) chat_layout->GetMetrics(&cm);
    if (prompt_layout) prompt_layout->GetMetrics(&pm);
    if (hint) hint->GetMetrics(&tm);

    const float heading_h = view_.heading.empty() ? 0.0f : hm.height;
    chat_content_h_ = chat_text.empty() ? 0.0f : cm.height;
    chat_viewport_h_ = std::min(max_chat_h, std::max(chat_content_h_, listening ? 0.0f : 48.0f * s));
    if (chat_content_h_ <= chat_viewport_h_) {
        chat_scroll_ = 0.0f;
        stick_bottom_ = true;
    } else if (stick_bottom_) {
        chat_scroll_ = 0.0f;
    } else {
        chat_scroll_ = std::clamp(chat_scroll_, 0.0f, chat_content_h_ - chat_viewport_h_);
    }

    const bool confirming = view_.mode == OverlayView::Mode::Confirm;
    const float button_h = confirming ? 30.0f * s : 0.0f;
    const float prompt_h = composer ? std::max(prompt_min_h, pm.height + 10.0f * s) : 0.0f;
    const float hint_h = hint ? tm.height + 6.0f * s : 0.0f;
    const float gap = 6.0f * s;
    float content_h = heading_h;
    if (chat_viewport_h_ > 0) content_h += (content_h > 0 ? gap : 0) + chat_viewport_h_;
    if (prompt_h > 0) content_h += gap + prompt_h + 2.0f * s;  // divider + field
    if (button_h > 0) content_h += gap + button_h;
    if (hint_h > 0) content_h += hint_h;
    const float height = std::round(std::max(orb + 2 * pad, content_h + 2 * pad));
    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);

    if (w != bitmap_w_ || h != bitmap_h_ || !mem_dc_) {
        if (mem_dc_) {
            SelectObject(mem_dc_, old_bitmap_);
            DeleteDC(mem_dc_);
        }
        if (bitmap_) DeleteObject(bitmap_);
        HDC screen = GetDC(nullptr);
        mem_dc_ = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bitmap_ = CreateDIBSection(mem_dc_, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        old_bitmap_ = SelectObject(mem_dc_, bitmap_);
        bitmap_w_ = w;
        bitmap_h_ = h;
    }

    RECT bind{0, 0, w, h};
    if (FAILED(target_->BindDC(mem_dc_, &bind))) {
        safe_release(target_);
        return;
    }
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - shown_at_).count();
    target_->BeginDraw();
    target_->Clear(D2D1::ColorF(0, 0, 0, 0));

    ID2D1SolidColorBrush* brush = nullptr;
    target_->CreateSolidColorBrush(rgba(0x17171C, 0.95f), &brush);
    const float radius = 20.0f * s;
    D2D1_ROUNDED_RECT panel = {D2D1::RectF(0.5f, 0.5f, width - 0.5f, height - 0.5f), radius, radius};
    target_->FillRoundedRectangle(panel, brush);
    brush->SetColor(rgba(0xFFFFFF, 0.09f));
    target_->DrawRoundedRectangle(panel, brush, 1.0f);

    const Palette pal = palette_for(view_.mode);
    const D2D1_POINT_2F center = D2D1::Point2F(pad + orb / 2, pad + orb / 2);
    float pulse = 0.0f;
    if (view_.mode == OverlayView::Mode::Listening) pulse = level_smoothed_;
    else if (view_.mode == OverlayView::Mode::Processing) pulse = 0.25f + 0.15f * std::sin(t * 5.0f);
    ID2D1GradientStopCollection* stops = nullptr;
    D2D1_GRADIENT_STOP gs[2] = {{0.0f, rgba(pal.a)}, {1.0f, rgba(pal.b)}};
    target_->CreateGradientStopCollection(gs, 2, &stops);
    ID2D1RadialGradientBrush* radial = nullptr;
    if (stops) {
        target_->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(D2D1::Point2F(center.x - orb * 0.15f, center.y - orb * 0.2f),
                                                D2D1::Point2F(0, 0), orb * 0.7f, orb * 0.7f),
            stops, &radial);
    }
    const float glow = orb / 2 + (3.0f + 10.0f * pulse) * s;
    brush->SetColor(rgba(pal.a, 0.18f + 0.25f * pulse));
    target_->FillEllipse(D2D1::Ellipse(center, glow, glow), brush);
    if (radial) target_->FillEllipse(D2D1::Ellipse(center, orb / 2, orb / 2), radial);
    if (view_.mode == OverlayView::Mode::Processing) {
        brush->SetColor(rgba(0xFFFFFF, 0.9f));
        const float r = orb / 2 - 6.0f * s;
        for (int i = 0; i < 3; ++i) {
            float a = t * 4.0f + i * 2.0f * std::numbers::pi_v<float> / 3.0f;
            target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(center.x + r * std::cos(a), center.y + r * std::sin(a)),
                                               2.2f * s, 2.2f * s),
                                 brush);
        }
    } else if (view_.mode == OverlayView::Mode::Listening) {
        brush->SetColor(rgba(0xFFFFFF, 0.92f));
        for (int i = 0; i < 4; ++i) {
            float phase = std::sin(t * 9.0f + i * 1.3f) * 0.5f + 0.5f;
            float bar = (3.0f + (4.0f + 9.0f * level_smoothed_) * phase) * s;
            float x = center.x + (i - 1.5f) * 4.5f * s;
            target_->FillRectangle(D2D1::RectF(x - 1.2f * s, center.y - bar, x + 1.2f * s, center.y + bar), brush);
        }
    }

    float y = pad;
    if (heading && heading_h > 0) {
        brush->SetColor(rgba(0xA9ADBB));
        target_->DrawTextLayout(D2D1::Point2F(text_x, y), heading, brush);
        y += heading_h + gap;
    }

    if (chat_layout && chat_viewport_h_ > 0) {
        const float chat_top = y;
        const float chat_bottom = y + chat_viewport_h_;
        // Draw from bottom of content upward by scroll_offset.
        const float draw_y = chat_bottom - chat_content_h_ + chat_scroll_;
        target_->PushAxisAlignedClip(D2D1::RectF(text_x, chat_top, text_x + text_w, chat_bottom),
                                     D2D1_ANTIALIAS_MODE_ALIASED);
        brush->SetColor(rgba(0xF3F4F7));
        target_->DrawTextLayout(D2D1::Point2F(text_x, draw_y), chat_layout, brush);
        target_->PopAxisAlignedClip();

        if (chat_content_h_ > chat_viewport_h_ + 1.0f) {
            const float track_h = chat_viewport_h_;
            const float thumb_h = std::max(18.0f * s, track_h * (chat_viewport_h_ / chat_content_h_));
            const float max_scroll = chat_content_h_ - chat_viewport_h_;
            const float thumb_y = chat_top + (max_scroll > 0 ? (1.0f - chat_scroll_ / max_scroll) * (track_h - thumb_h) : 0);
            brush->SetColor(rgba(0xFFFFFF, 0.18f));
            target_->FillRectangle(D2D1::RectF(width - pad - 3.0f * s, thumb_y, width - pad, thumb_y + thumb_h), brush);
        }
        y = chat_bottom + gap;
    }

    if (confirming) {
        const float bw = 104.0f * s;
        const float bh = button_h;
        const float bx = text_x;
        auto draw_button = [&](float x, uint32_t fill, const wchar_t* label, float* box) {
            D2D1_ROUNDED_RECT r = {D2D1::RectF(x, y, x + bw, y + bh), 8.0f * s, 8.0f * s};
            brush->SetColor(rgba(fill, 0.22f));
            target_->FillRoundedRectangle(r, brush);
            brush->SetColor(rgba(fill, 0.85f));
            target_->DrawRoundedRectangle(r, brush, 1.2f);
            IDWriteTextLayout* text = nullptr;
            dwrite_->CreateTextLayout(label, static_cast<UINT32>(wcslen(label)), hint_format_, bw, bh, &text);
            if (text) {
                text->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                DWRITE_TEXT_METRICS lm{};
                text->GetMetrics(&lm);
                brush->SetColor(rgba(0xFFFFFF, 0.95f));
                target_->DrawTextLayout(D2D1::Point2F(x, y + (bh - lm.height) / 2), text, brush);
                safe_release(text);
            }
            box[0] = x;
            box[1] = y;
            box[2] = x + bw;
            box[3] = y + bh;
        };
        draw_button(bx, 0x2EC4B6, L"Allow  \u21B5", allow_button_);
        draw_button(bx + bw + 10.0f * s, 0xFF4D6D, L"Deny  Esc", deny_button_);
        y += bh + gap;
    }

    if (composer && prompt_layout) {
        brush->SetColor(rgba(0xFFFFFF, 0.08f));
        target_->FillRectangle(D2D1::RectF(text_x, y, text_x + text_w, y + 1.0f * s), brush);
        y += 4.0f * s;
        const float field_top = y;
        brush->SetColor(rgba(0xFFFFFF, 0.06f));
        D2D1_ROUNDED_RECT field = {D2D1::RectF(text_x - 4.0f * s, field_top, text_x + text_w + 4.0f * s, field_top + prompt_h),
                                   8.0f * s, 8.0f * s};
        target_->FillRoundedRectangle(field, brush);
        brush->SetColor(prompt_placeholder ? rgba(0x6E7280) : rgba(0xF3F4F7));
        target_->DrawTextLayout(D2D1::Point2F(text_x, field_top + 5.0f * s), prompt_layout, brush);
        if (composer_active() && !prompt_placeholder && std::fmod(t, 1.0f) < 0.55f) {
            FLOAT cx = 0, cy = 0;
            DWRITE_HIT_TEST_METRICS hit{};
            const UINT32 pos = static_cast<UINT32>(view_.prompt.size());
            prompt_layout->HitTestTextPosition(pos, FALSE, &cx, &cy, &hit);
            brush->SetColor(rgba(0x4CC9F0));
            target_->FillRectangle(
                D2D1::RectF(text_x + cx, field_top + 5.0f * s + cy + 2 * s, text_x + cx + 1.6f * s,
                            field_top + 5.0f * s + cy + hit.height - 2 * s),
                brush);
        }
        y = field_top + prompt_h + 4.0f * s;
    }

    if (hint) {
        brush->SetColor(rgba(0x7C8090));
        target_->DrawTextLayout(D2D1::Point2F(text_x, y), hint, brush);
    }

    HRESULT hr = target_->EndDraw();
    safe_release(radial);
    safe_release(stops);
    safe_release(brush);
    safe_release(heading);
    safe_release(chat_layout);
    safe_release(prompt_layout);
    safe_release(hint);
    if (hr == D2DERR_RECREATE_TARGET) {
        safe_release(target_);
        return;
    }
    place(w, h);
}

LRESULT CALLBACK Overlay::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        static_cast<Overlay*>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Overlay::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == kAnimTimer) tick();
            return 0;
        case WM_MOUSEACTIVATE:
            return interactive_ ? MA_ACTIVATE : MA_NOACTIVATE;
        case WM_MOUSEWHEEL: {
            if (!visible_) break;
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            scroll_by(static_cast<float>(delta) * 0.4f * scale());
            render();
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && input_active()) {
                const bool fresh = std::chrono::steady_clock::now() - shown_at_ < std::chrono::milliseconds(1500);
                const bool clicked = (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) |
                                      GetAsyncKeyState(VK_MBUTTON)) &
                                     0x8000;
                if (fresh && !clicked && !focus_reclaimed_) {
                    focus_reclaimed_ = true;
                    wchar_t cls[64] = L"";
                    if (lp) GetClassNameW(reinterpret_cast<HWND>(lp), cls, 64);
                    log::info("overlay: focus taken by a '{}' window right after opening, reclaiming", to_utf8(cls));
                    PostMessageW(hwnd_, WM_APP, 0, 0);
                } else if (callbacks_.on_cancel) {
                    log::info("overlay: input lost focus, closing");
                    callbacks_.on_cancel();
                }
            }
            return 0;
        case WM_APP:
            if (composer_active() || confirm_active()) force_foreground(hwnd_);
            return 0;
        case WM_LBUTTONUP: {
            if (!confirm_active() || !callbacks_.on_confirm) break;
            const float x = static_cast<float>(GET_X_LPARAM(lp));
            const float y = static_cast<float>(GET_Y_LPARAM(lp));
            auto inside = [x, y](const float* b) {
                return x >= b[0] && x <= b[2] && y >= b[1] && y <= b[3];
            };
            if (inside(allow_button_)) callbacks_.on_confirm(true);
            else if (inside(deny_button_)) callbacks_.on_confirm(false);
            return 0;
        }
        case WM_KEYDOWN:
            if (confirm_active()) {
                if (!callbacks_.on_confirm) return 0;
                if (wp == VK_RETURN || wp == 'Y') callbacks_.on_confirm(true);
                else if (wp == VK_ESCAPE || wp == 'N') callbacks_.on_confirm(false);
                return 0;
            }
            if (!composer_active()) break;
            if (wp == VK_ESCAPE) {
                if (callbacks_.on_cancel) callbacks_.on_cancel();
            } else if (wp == VK_RETURN) {
                std::wstring text = view_.prompt;
                if (!text.empty() && callbacks_.on_submit) {
                    view_.prompt.clear();
                    callbacks_.on_submit(std::move(text));
                }
            } else if (wp == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                std::wstring paste = get_clipboard_text();
                for (wchar_t& c : paste) {
                    if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
                }
                view_.prompt += paste;
                render();
            } else if (wp == VK_BACK && (GetKeyState(VK_CONTROL) & 0x8000)) {
                auto& b = view_.prompt;
                while (!b.empty() && b.back() == L' ') b.pop_back();
                while (!b.empty() && b.back() != L' ') b.pop_back();
                render();
            } else if (wp == VK_PRIOR) {
                scroll_by(chat_viewport_h_ * 0.8f);
                render();
            } else if (wp == VK_NEXT) {
                scroll_by(-chat_viewport_h_ * 0.8f);
                render();
            }
            return 0;
        case WM_CHAR:
            if (!composer_active()) break;
            if (wp == VK_BACK) {
                if (GetKeyState(VK_CONTROL) & 0x8000) return 0;
                if (!view_.prompt.empty()) {
                    view_.prompt.pop_back();
                    if (!view_.prompt.empty() && IS_HIGH_SURROGATE(view_.prompt.back())) view_.prompt.pop_back();
                }
                render();
            } else if (wp >= 0x20 && wp != 0x7F) {
                view_.prompt += static_cast<wchar_t>(wp);
                render();
            }
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, confirm_active() ? IDC_HAND : (interactive_ ? IDC_IBEAM : IDC_ARROW)));
            return TRUE;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace wa
