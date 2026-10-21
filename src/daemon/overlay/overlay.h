#pragma once

#include <windows.h>

#include <chrono>
#include <functional>
#include <string>

struct ID2D1Factory;
struct ID2D1DCRenderTarget;
struct IDWriteFactory;
struct IDWriteTextFormat;

namespace wa {

struct OverlayView {
    enum class Mode { Listening, Input, Processing, Response, Error };
    Mode mode = Mode::Listening;
    std::wstring heading;
    std::wstring body;
    std::wstring hint;
};

// Frameless, top-most, click-through panel rendered with Direct2D into a
// per-pixel-alpha layered window. It never takes focus except in Input mode.
class Overlay {
public:
    struct Callbacks {
        std::function<void(std::wstring)> on_submit;
        std::function<void()> on_cancel;
    };

    Overlay() = default;
    ~Overlay();

    bool create(HINSTANCE instance, Callbacks callbacks);
    void destroy();

    void configure(int width_dip, bool at_top);
    void show(const OverlayView& view, HWND anchor);
    void update(const OverlayView& view);
    void append_body(std::wstring_view text);
    void set_level(float level) { level_ = level; }
    void hide();

    bool visible() const { return visible_; }
    bool input_active() const { return view_.mode == OverlayView::Mode::Input && visible_; }
    HWND hwnd() const { return hwnd_; }
    // True when hidden for at least `d` (used to drop GPU/text resources).
    bool hidden_for(std::chrono::milliseconds d) const;
    void release_resources();

private:
    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    bool ensure_resources();
    void set_interactive(bool interactive);
    void render();
    void tick();
    void place(int width, int height);
    float scale() const;

    HWND hwnd_ = nullptr;
    HWND anchor_ = nullptr;
    Callbacks callbacks_;
    OverlayView view_;
    bool visible_ = false;
    bool interactive_ = false;
    bool focus_reclaimed_ = false;
    bool at_top_ = false;
    int width_dip_ = 620;
    float fade_ = 0.0f;
    float fade_target_ = 0.0f;
    float level_ = 0.0f;
    float level_smoothed_ = 0.0f;
    std::chrono::steady_clock::time_point shown_at_{};
    std::chrono::steady_clock::time_point last_tick_{};
    std::chrono::steady_clock::time_point hidden_at_{};

    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    ID2D1DCRenderTarget* target_ = nullptr;
    IDWriteTextFormat* heading_format_ = nullptr;
    IDWriteTextFormat* body_format_ = nullptr;
    IDWriteTextFormat* hint_format_ = nullptr;
    float format_scale_ = 0.0f;

    HDC mem_dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ old_bitmap_ = nullptr;
    int bitmap_w_ = 0;
    int bitmap_h_ = 0;
};

}  // namespace wa
