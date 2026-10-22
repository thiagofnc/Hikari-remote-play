#pragma once
// Hikari's tiny Direct2D UI kit: one theme, a painter with cached text
// formats/brushes, and immediate-mode widgets with simple hit testing.
// Used by the client lobby (drawn into the swap chain), the setup wizard,
// settings and the host status window, so every screen looks the same.

#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <map>
#include <string>
#include <vector>

namespace hikari::ui {

using Microsoft::WRL::ComPtr;

D2D1_COLOR_F rgb(uint32_t hex, float a = 1.0f);

namespace theme {
constexpr uint32_t kBg = 0x191A1D;
constexpr uint32_t kBg2 = 0x202226;
constexpr uint32_t kSurface = 0x282A2F;
constexpr uint32_t kSurface2 = 0x303339;
constexpr uint32_t kBorder = 0x41444B;
constexpr uint32_t kText = 0xF1F2F3;
constexpr uint32_t kText2 = 0xB7BAC0;
constexpr uint32_t kText3 = 0xA0A4AC;
constexpr uint32_t kAccent = 0xFBBB04;   // Hikari amber (logo colour)
constexpr uint32_t kAccentHover = 0xFFCB33;
constexpr uint32_t kOnAccent = 0x1A1400;
constexpr uint32_t kGood = 0x3DDC84;
constexpr uint32_t kWarn = 0xF5A524;
constexpr uint32_t kBad = 0xFF5C5C;
constexpr uint32_t kNeonBlue = 0x00B8D9;  // Joy-Con (L)
constexpr uint32_t kNeonRed = 0xFF5F52;   // Joy-Con (R)
}  // namespace theme

enum class Align { Leading, Center, Trailing };
enum class Weight { Regular = 400, Medium = 500, SemiBold = 600, Bold = 700 };
enum class ButtonStyle { Primary, Secondary, Ghost, Danger };
enum class ConsoleModel { Switch, Switch2 };

struct Rect {
    float l = 0, t = 0, r = 0, b = 0;
    float w() const { return r - l; }
    float h() const { return b - t; }
    float cx() const { return (l + r) / 2; }
    float cy() const { return (t + b) / 2; }
    bool contains(float x, float y) const { return x >= l && x < r && y >= t && y < b; }
    Rect inset(float d) const { return {l + d, t + d, r - d, b - d}; }
    Rect inset(float dx, float dy) const { return {l + dx, t + dy, r - dx, b - dy}; }
    D2D1_RECT_F d2d() const { return D2D1::RectF(l, t, r, b); }
};

// Immediate-mode interaction state for one window.
struct Interaction {
    float mouse_x = -1, mouse_y = -1;
    bool mouse_down = false;
    int pressed_id = -1;   // widget under the cursor when the button went down
    int clicked_id = -1;   // set for one frame on release over the same widget
    int focus_id = -1;     // keyboard/gamepad focus
    std::vector<std::pair<int, Rect>> hits;

    void begin_frame() { hits.clear(); }
    bool hovered(int id, const Rect& r) const { return r.contains(mouse_x, mouse_y) && (pressed_id < 0 || pressed_id == id); }
    int hit(float x, float y) const;
    int take_click()
    {
        const int c = clicked_id;
        clicked_id = -1;
        return c;
    }
};

class Painter {
public:
    bool init();  // factories (call once per thread/window)
    // Binds to a render target for this frame. `scale` = DPI / 96.
    void begin(ID2D1RenderTarget* rt, float scale, double time_s);
    void end() { rt_ = nullptr; }

    ID2D1RenderTarget* rt() const { return rt_; }
    IDWriteFactory* dwrite() const { return dwrite_.Get(); }
    float s(float v) const { return v * scale_; }  // DIP -> pixels
    double time() const { return time_; }

    void clear(uint32_t color);
    void background(const Rect& r);  // matte application background
    void fill(const Rect& r, D2D1_COLOR_F c, float radius = 0);
    void stroke(const Rect& r, D2D1_COLOR_F c, float width, float radius = 0);
    void line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width);
    void circle(float cx, float cy, float rad, D2D1_COLOR_F c, bool filled = true, float width = 1);

    void text(const std::string& utf8, const Rect& r, float size, D2D1_COLOR_F c, Weight w = Weight::Regular,
              Align h = Align::Leading, Align v = Align::Center, bool wrap = false);
    float text_width(const std::string& utf8, float size, Weight w = Weight::Regular);

    // Widgets. Returns true when clicked this frame.
    bool button(Interaction& ui, int id, const Rect& r, const std::string& label, ButtonStyle style = ButtonStyle::Secondary,
                bool enabled = true);
    float pill(float x, float y, const std::string& label, uint32_t dot_color, bool right_align = false);
    void spinner(float cx, float cy, float radius, D2D1_COLOR_F c);
    void progress(const Rect& r, float frac, D2D1_COLOR_F c);
    bool toggle(Interaction& ui, int id, const Rect& r, bool on);
    void logo(const Rect& r, float opacity = 1.0f);
    // Stylised Switch in its dock. `screen` shows the awake display.
    void console(const Rect& r, uint32_t status_color, bool screen_on, ConsoleModel model = ConsoleModel::Switch);

private:
    IDWriteTextFormat* format(float size, Weight w, Align h, Align v, bool wrap);
    ID2D1SolidColorBrush* brush(D2D1_COLOR_F c);
    void load_logo();

    ComPtr<ID2D1Factory1> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<IWICFormatConverter> logo_src_;
    ID2D1RenderTarget* rt_ = nullptr;
    ID2D1RenderTarget* bound_rt_ = nullptr;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1Bitmap> logo_;
    std::map<uint64_t, ComPtr<IDWriteTextFormat>> formats_;
    float scale_ = 1.0f;
    double time_ = 0;
};

std::wstring wide(const std::string& s);

}  // namespace hikari::ui
