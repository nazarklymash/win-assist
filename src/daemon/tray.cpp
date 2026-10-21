#include "daemon/tray.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>

namespace wa {
namespace {

constexpr UINT kIconId = 1;

struct Rgb {
    float r, g, b;
};

Rgb lerp(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

// Gradient ring with a state-coloured core, drawn at the system small-icon size.
HICON draw_icon(Rgb top, Rgb bottom, Rgb core) {
    const int size = std::max(16, GetSystemMetrics(SM_CXSMICON) * 2);
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);

    auto* px = static_cast<uint32_t*>(bits);
    const float c = (size - 1) / 2.0f;
    const float outer = size / 2.0f - 0.5f;
    const float ring = outer * 0.70f;
    const float inner = outer * 0.38f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float d = std::hypot(x - c, y - c);
            float a_outer = std::clamp(outer - d + 0.5f, 0.0f, 1.0f);
            float a_hole = std::clamp(d - ring + 0.5f, 0.0f, 1.0f);
            float a_core = std::clamp(inner - d + 0.5f, 0.0f, 1.0f);
            Rgb ring_color = lerp(top, bottom, static_cast<float>(y) / size);
            float ring_alpha = a_outer * a_hole;
            float alpha = std::max(ring_alpha, a_core);
            Rgb col = a_core > ring_alpha ? core : ring_color;
            auto ch = [&](float v) { return static_cast<uint32_t>(std::clamp(v * alpha, 0.0f, 1.0f) * 255.0f); };
            px[y * size + x] = (static_cast<uint32_t>(alpha * 255.0f) << 24) | (ch(col.r) << 16) | (ch(col.g) << 8) | ch(col.b);
        }
    }
    ICONINFO info{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

}  // namespace

Tray::~Tray() { destroy(); }

HICON Tray::icon_for(TrayState state) {
    int i = static_cast<int>(state);
    if (!icons_[i]) {
        const Rgb top{0.30f, 0.79f, 0.94f}, bottom{0.48f, 0.38f, 1.0f};
        const Rgb core = state == TrayState::Listening ? Rgb{1.0f, 0.30f, 0.43f}
                         : state == TrayState::Busy    ? Rgb{0.48f, 0.38f, 1.0f}
                         : state == TrayState::Error   ? Rgb{1.0f, 0.72f, 0.01f}
                                                       : Rgb{0.93f, 0.95f, 0.97f};
        icons_[i] = draw_icon(top, bottom, core);
    }
    return icons_[i];
}

void Tray::apply(DWORD message) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner_;
    nid.uID = kIconId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = callback_;
    nid.hIcon = icon_for(state_);
    wcsncpy_s(nid.szTip, tooltip_.c_str(), _TRUNCATE);
    Shell_NotifyIconW(message, &nid);
    if (message == NIM_ADD) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
}

bool Tray::create(HWND owner, UINT callback_message) {
    owner_ = owner;
    callback_ = callback_message;
    apply(NIM_ADD);
    added_ = true;
    return true;
}

void Tray::recreate() {
    if (!owner_) return;
    apply(NIM_ADD);
    added_ = true;
}

void Tray::destroy() {
    if (added_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = owner_;
        nid.uID = kIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        added_ = false;
    }
    for (auto& icon : icons_) {
        if (icon) DestroyIcon(icon);
        icon = nullptr;
    }
}

void Tray::set_state(TrayState state) {
    if (state == state_) return;
    state_ = state;
    if (added_) apply(NIM_MODIFY);
}

void Tray::set_tooltip(const std::wstring& text) {
    tooltip_ = text;
    if (added_) apply(NIM_MODIFY);
}

void Tray::notify(const std::wstring& title, const std::wstring& text) {
    if (!added_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner_;
    nid.uID = kIconId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

}  // namespace wa
