#pragma once
// Base class for Hikari's 2D windows (wizard, settings, host status).
// Everything is custom-drawn with the shared UI kit; the only native
// controls are single-line edit boxes, styled dark, for text entry.

#include <windows.h>

#include <string>
#include <vector>

#include "hikari/ui_kit.h"

namespace hikari {

class D2DWindow {
public:
    virtual ~D2DWindow();

    bool create(const wchar_t* title, int w_dip, int h_dip, HWND owner = nullptr, bool resizable = false);
    HWND hwnd() const { return hwnd_; }
    void close() { DestroyWindow(hwnd_); }
    // Runs a modal loop (owner disabled) until the window is destroyed.
    void run_modal();
    void invalidate() { InvalidateRect(hwnd_, nullptr, FALSE); }
    float scale() const { return scale_; }

protected:
    virtual void draw(ui::Painter& p, const ui::Rect& r) = 0;
    virtual void on_action(int /*id*/) {}
    virtual void on_timer() {}
    virtual bool on_close() { return true; }  // false = keep window (e.g. hide to tray)
    virtual LRESULT on_message(UINT msg, WPARAM wp, LPARAM lp, bool& handled);
    virtual void on_key(UINT /*vk*/) {}

    // Text boxes: created once, positioned every frame by layout code.
    HWND add_edit(const std::string& text, bool password = false, bool upper = false);
    void place(HWND edit, const ui::Rect& r);
    std::string text_of(HWND edit) const;
    void set_text(HWND edit, const std::string& s);

    ui::Interaction ui_;
    bool animate_ = true;  // repaint continuously (spinners, glow)

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void paint();
    void make_font();

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> rt_;
    ui::Painter painter_;
    float scale_ = 1.0f;
    HFONT font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    std::vector<HWND> edits_;
    uint64_t start_ = 0;
};

}  // namespace hikari
