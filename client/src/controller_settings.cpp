#include "controller_settings.h"

#include <d2d1.h>
#include <dwrite.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#include "hikari/log.h"
#include "hikari/system.h"
#include "hk_link.h"
#include "joycon_art.h"
#include "hikari/ui_kit.h"

namespace hikari {

namespace {

using Microsoft::WRL::ComPtr;

// ---- palette -----------------------------------------------------------------

constexpr uint32_t kBg = ui::theme::kBg;
constexpr uint32_t kPanel = ui::theme::kBg2;
constexpr uint32_t kPanelBorder = ui::theme::kBorder;
constexpr uint32_t kControlBorderHover = 0x474A52;
constexpr uint32_t kControl = ui::theme::kSurface;
constexpr uint32_t kControlHover = ui::theme::kSurface2;
constexpr uint32_t kControlBorder = ui::theme::kBorderStrong;
constexpr uint32_t kText = ui::theme::kText;
constexpr uint32_t kText2 = ui::theme::kText2;
constexpr uint32_t kText3 = ui::theme::kText3;
constexpr uint32_t kAmber = ui::theme::kAccent;
constexpr uint32_t kInk = ui::theme::kOnAccent;
constexpr uint32_t kGreen = ui::theme::kGood;
constexpr uint32_t kTrack = 0x33363D;
constexpr uint32_t kDanger = 0xFF8A80;

constexpr float kPi = 3.14159265f;
constexpr double kCaptureTimeoutS = 5.0;
constexpr float kDesignW = 1200.0f, kDesignH = 760.0f;  // default client size (DIPs)
constexpr float kMinW = 1040.0f, kMinH = 700.0f;

D2D1_COLOR_F col(uint32_t rgb, float a = 1.0f) { return D2D1::ColorF(rgb, a); }

double now_s()
{
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

bool contains(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

D2D1_RECT_F inset(const D2D1_RECT_F& r, float dx, float dy) { return {r.left + dx, r.top + dy, r.right - dx, r.bottom - dy}; }

constexpr size_t sidx(SwitchInput s) { return static_cast<size_t>(s); }

// HK_BTN_* bit per SwitchInput (0 = stick direction), for the live preview.
constexpr uint32_t kSwitchBit[kSwitchInputCount] = {
    HK_BTN_A,          HK_BTN_B,         HK_BTN_X,         HK_BTN_Y,          HK_BTN_L,      HK_BTN_R,
    HK_BTN_ZL,         HK_BTN_ZR,        HK_BTN_PLUS,      HK_BTN_MINUS,      HK_BTN_HOME,   HK_BTN_CAPTURE,
    HK_BTN_LSTICK,     HK_BTN_RSTICK,    HK_BTN_DPAD_UP,   HK_BTN_DPAD_DOWN,  HK_BTN_DPAD_LEFT,
    HK_BTN_DPAD_RIGHT, 0,                0,                0,                 0,             0,
    0,                 0,                0};

// ---- binding grid definition ------------------------------------------------

struct RowDef {
    SwitchInput input;
    const wchar_t* label;
};
struct GroupDef {
    const wchar_t* title;
    int column;
    int count;
    RowDef rows[8];
};

const GroupDef kGroups[] = {
    {L"Buttons",
     0,
     8,
     {{SwitchInput::A, L"A"},
      {SwitchInput::B, L"B"},
      {SwitchInput::X, L"X"},
      {SwitchInput::Y, L"Y"},
      {SwitchInput::Plus, L"Plus"},
      {SwitchInput::Minus, L"Minus"},
      {SwitchInput::Home, L"Home"},
      {SwitchInput::Capture, L"Capture"}}},
    {L"Left stick",
     1,
     5,
     {{SwitchInput::LStickClick, L"Button"},
      {SwitchInput::LStickUp, L"Up"},
      {SwitchInput::LStickDown, L"Down"},
      {SwitchInput::LStickLeft, L"Left"},
      {SwitchInput::LStickRight, L"Right"}}},
    {L"Directional pad",
     1,
     4,
     {{SwitchInput::DpadUp, L"Up"},
      {SwitchInput::DpadDown, L"Down"},
      {SwitchInput::DpadLeft, L"Left"},
      {SwitchInput::DpadRight, L"Right"}}},
    {L"Right stick",
     2,
     5,
     {{SwitchInput::RStickClick, L"Button"},
      {SwitchInput::RStickUp, L"Up"},
      {SwitchInput::RStickDown, L"Down"},
      {SwitchInput::RStickLeft, L"Left"},
      {SwitchInput::RStickRight, L"Right"}}},
    {L"Triggers",
     2,
     4,
     {{SwitchInput::L, L"L"}, {SwitchInput::R, L"R"}, {SwitchInput::ZL, L"ZL"}, {SwitchInput::ZR, L"ZR"}}},
};
constexpr int kGroupCount = static_cast<int>(sizeof(kGroups) / sizeof(kGroups[0]));
constexpr int kColumns = 3;

// Navigation order (column-major, matches the grid).
std::array<int, kSwitchInputCount> nav_order()
{
    std::array<int, kSwitchInputCount> o{};
    size_t n = 0;
    for (int c = 0; c < kColumns; ++c)
        for (const auto& g : kGroups)
            if (g.column == c)
                for (int r = 0; r < g.count; ++r) o[n++] = static_cast<int>(g.rows[r].input);
    return o;
}

// Inputs considered "pressed" while capturing a new binding. Stick directions
// need 60% and triggers 50% so resting drift never gets captured.
using PadBits = std::bitset<static_cast<size_t>(PhysicalInput::PadCount)>;
PadBits capture_bits(const RawPadState& p)
{
    PadBits b;
    if (!p.connected) return b;
    b = p.pressed;
    const auto bit = [](PhysicalInput i) { return static_cast<size_t>(i); };
    for (int i = static_cast<int>(PhysicalInput::PadLStickUp); i <= static_cast<int>(PhysicalInput::PadRStickRight); ++i)
        b.reset(static_cast<size_t>(i));
    b.reset(bit(PhysicalInput::PadLT));
    b.reset(bit(PhysicalInput::PadRT));
    const int t = 19660;  // 60% of full scale
    if (p.ly > t) b.set(bit(PhysicalInput::PadLStickUp));
    if (p.ly < -t) b.set(bit(PhysicalInput::PadLStickDown));
    if (p.lx < -t) b.set(bit(PhysicalInput::PadLStickLeft));
    if (p.lx > t) b.set(bit(PhysicalInput::PadLStickRight));
    if (p.ry > t) b.set(bit(PhysicalInput::PadRStickUp));
    if (p.ry < -t) b.set(bit(PhysicalInput::PadRStickDown));
    if (p.rx < -t) b.set(bit(PhysicalInput::PadRStickLeft));
    if (p.rx > t) b.set(bit(PhysicalInput::PadRStickRight));
    if (p.lt > 127) b.set(bit(PhysicalInput::PadLT));
    if (p.rt > 127) b.set(bit(PhysicalInput::PadRT));
    return b;
}

// ---- hit ids -------------------------------------------------------------------

enum class El : uint8_t {
    None,
    Binding,
    Art,
    DevDrop,
    LayoutDrop,
    ProfileDrop,
    SaveAs,
    Delete,
    TabPad,
    TabKb,
    Toggle,
    Slider,
    Reset,
    Save,
    Close,
    DropItem,
    Backdrop,
    DlgOk,
    DlgCancel,
    DlgField,
    DlgBody,
};

struct Hit {
    El el = El::None;
    int idx = 0;
    bool operator==(const Hit& o) const { return el == o.el && idx == o.idx; }
    bool operator!=(const Hit& o) const { return !(*this == o); }
};

enum class Tab { Gamepad, Keyboard };
enum class Drop { None, Device, Layout, Profile };
enum class DlgKind { None, Confirm, TextEntry };
enum class BtnStyle { Primary, Secondary, Danger };

struct Dialog {
    DlgKind kind = DlgKind::None;
    std::wstring title, message, ok_text;
    bool danger = false;
    std::wstring text;   // text entry contents
    std::wstring error;  // validation message under the field
    std::function<bool(const std::wstring&)> on_ok;  // return false to keep the dialog open
};

struct Layout {
    float w = 0, h = 0;
    D2D1_RECT_F dev{}, layout{}, profile{}, save_as{}, del{};
    D2D1_POINT_2F cap_dev{}, cap_layout{}, cap_profile{};
    D2D1_RECT_F left_card{}, art_card{}, tab_pad{}, tab_kb{}, tabs{}, hint{};
    D2D1_RECT_F toggles[3]{};
    D2D1_RECT_F slider_label{}, slider_track{}, slider_hit{}, slider_value{};
    float options_divider = 0;
    D2D1_RECT_F group_hdr[kGroupCount]{};
    std::array<D2D1_RECT_F, kSwitchInputCount> row_label{}, row_btn{};
    std::array<int, kSwitchInputCount> row_col{};
    D2D1_RECT_F art_area{}, art_hint{}, pill_anchor{};
    D2D1_RECT_F reset{}, save{}, close{}, status{};
};

// ---- the window ----------------------------------------------------------------

class SettingsWindow {
public:
    SettingsWindow(Config& cfg, InputMapping& mapping, const ControllerSettingsDeps& deps)
        : cfg_(cfg), mapping_(mapping), deps_(deps), work_(mapping), committed_(mapping)
    {
        order_ = nav_order();
    }
    bool run(HWND parent);

private:
    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    // resources
    bool create_factories();
    bool create_device();
    void release_device();
    ComPtr<IDWriteTextFormat> make_format(const wchar_t* family, DWRITE_FONT_WEIGHT weight, float size, bool wrap = false);

    // state
    void tick();
    void compute_layout();
    Hit hit_test(float x, float y) const;
    bool clickable(const Hit& h) const;
    void activate(const Hit& h);
    void on_key(WPARAM vk, LPARAM lp, bool sys, bool* handled);
    void on_char(wchar_t ch);
    void refresh_devices();
    void refresh_profiles();
    void select_device(int idx);
    void set_tab(Tab t);
    void start_capture(int input);
    void cancel_capture();
    void bind(PhysicalInput p);
    void move_focus(int dir_row, int dir_col);
    void request_close();
    void commit_save();
    void load_profile(const std::string& name);
    void confirm(std::wstring title, std::wstring message, std::wstring ok, bool danger, std::function<void()> on_ok);
    void toast(std::wstring text);
    std::array<PhysicalInput, kSwitchInputCount>& bindings() { return tab_ == Tab::Gamepad ? work_.pad : work_.keyboard; }
    int current_layout() const;
    bool dirty() const { return !mapping_equal(work_, committed_); }
    std::vector<std::wstring> drop_items(Drop d) const;
    int drop_selected(Drop d) const;
    D2D1_RECT_F drop_anchor(Drop d) const;
    D2D1_RECT_F popup_rect() const;
    D2D1_RECT_F dialog_rect() const;
    D2D1_RECT_F dialog_ok_rect() const;
    D2D1_RECT_F dialog_cancel_rect() const;
    D2D1_RECT_F dialog_field_rect() const;
    void set_deadzone_from_x(float x);
    float dip(int px) const { return static_cast<float>(px) * 96.0f / static_cast<float>(dpi_); }

    // drawing
    void render();
    void draw_header();
    void draw_toolbar();
    void draw_bindings();
    void draw_binding(int i);
    void draw_options();
    void draw_art_card();
    void draw_footer();
    void draw_popup();
    void draw_dialog();
    void fill_rr(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c);
    void stroke_rr(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c, float width);
    void text(const std::wstring& s, const D2D1_RECT_F& r, IDWriteTextFormat* f, const D2D1_COLOR_F& c,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT para = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    float text_width(const std::wstring& s, IDWriteTextFormat* f);
    void chevron(float cx, float cy, float size, bool up, const D2D1_COLOR_F& c);
    void button(const D2D1_RECT_F& r, const wchar_t* label, Hit h, BtnStyle style, bool enabled = true);
    void dropdown(const D2D1_RECT_F& r, const std::wstring& value, Hit h, bool open);
    void shadow(const D2D1_RECT_F& r, float radius, float strength);
    void fill_gradient(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& top, const D2D1_COLOR_F& bottom);
    void card(const D2D1_RECT_F& r);
    void highlight(const D2D1_RECT_F& r, float radius, float alpha);
    float flash_amount(int i) const;

    Config& cfg_;
    InputMapping& mapping_;
    const ControllerSettingsDeps& deps_;
    InputMapping work_, committed_;
    bool saved_any_ = false;
    bool done_ = false;

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> round_;
    ComPtr<IDWriteTextFormat> f_title_, f_sub_, f_caption_, f_body_, f_body_sb_, f_small_, f_key_, f_art_, f_wrap_,
        f_dlg_title_;
    joycon::JoyConArt art_;
    Layout lay_;

    // live data
    RawPadState raw_;
    KeyboardState kb_;
    proto::PadState preview_;
    uint32_t active_ = 0;
    double t_ = 0;

    // ui state
    Tab tab_ = Tab::Gamepad;
    std::vector<std::string> devices_;
    std::vector<std::wstring> device_items_;
    int device_sel_ = 0;
    double last_device_refresh_ = 0;
    std::vector<std::string> profiles_;
    int layout_pref_ = 0;
    std::array<int, kSwitchInputCount> order_{};
    int focus_ = 0;
    bool focus_visible_ = false;
    Hit hover_, pressed_;
    bool tracking_mouse_ = false;
    bool dragging_slider_ = false;
    Drop drop_ = Drop::None;
    Dialog dlg_;
    int cap_target_ = -1;
    double cap_start_ = 0;
    PadBits cap_base_;
    std::array<double, kSwitchInputCount> flash_t_{};
    int last_flash_ = -1;
    std::wstring toast_;
    double toast_t_ = -100;
    float toggle_anim_[3] = {0, 0, 0};
};

// ---- resources ---------------------------------------------------------------

ComPtr<IDWriteTextFormat> SettingsWindow::make_format(const wchar_t* family, DWRITE_FONT_WEIGHT weight, float size,
                                                      bool wrap)
{
    ComPtr<IDWriteTextFormat> f;
    if (FAILED(dwrite_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         size, L"en-us", &f)))
        return nullptr;
    if (!wrap) {
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        // Long device / profile names end in an ellipsis instead of spilling.
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(f.Get(), &ellipsis))) f->SetTrimming(&trim, ellipsis.Get());
    }
    return f;
}

bool SettingsWindow::create_factories()
{
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;
    d2d_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                            nullptr, 0, &round_);

    // Windows 11 ships Segoe UI Variable; fall back to Segoe UI on Windows 10.
    const wchar_t* text_family = L"Segoe UI";
    const wchar_t* display_family = L"Segoe UI";
    ComPtr<IDWriteFontCollection> fonts;
    if (SUCCEEDED(dwrite_->GetSystemFontCollection(&fonts))) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(fonts->FindFamilyName(L"Segoe UI Variable Text", &index, &exists)) && exists)
            text_family = L"Segoe UI Variable Text";
        if (SUCCEEDED(fonts->FindFamilyName(L"Segoe UI Variable Display", &index, &exists)) && exists)
            display_family = L"Segoe UI Variable Display";
    }
    f_title_ = make_format(display_family, DWRITE_FONT_WEIGHT_SEMI_BOLD, 22.0f);
    f_dlg_title_ = make_format(display_family, DWRITE_FONT_WEIGHT_SEMI_BOLD, 17.0f);
    f_sub_ = make_format(text_family, DWRITE_FONT_WEIGHT_NORMAL, 13.0f);
    f_caption_ = make_format(text_family, DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.0f);
    f_body_ = make_format(text_family, DWRITE_FONT_WEIGHT_NORMAL, 13.5f);
    f_body_sb_ = make_format(text_family, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.5f);
    f_small_ = make_format(text_family, DWRITE_FONT_WEIGHT_NORMAL, 12.0f);
    f_key_ = make_format(text_family, DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.0f);
    f_art_ = make_format(text_family, DWRITE_FONT_WEIGHT_BOLD, 12.0f);
    f_wrap_ = make_format(text_family, DWRITE_FONT_WEIGHT_NORMAL, 13.5f, true);
    if (!f_title_ || !f_dlg_title_ || !f_sub_ || !f_caption_ || !f_body_ || !f_body_sb_ || !f_small_ || !f_key_ ||
        !f_art_ || !f_wrap_)
        return false;
    return art_.create(d2d_.Get(), deps_.model);
}

bool SettingsWindow::create_device()
{
    if (rt_) return true;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const auto fdpi = static_cast<float>(dpi_);
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        fdpi, fdpi);
    const D2D1_HWND_RENDER_TARGET_PROPERTIES hprops = D2D1::HwndRenderTargetProperties(
        hwnd_, D2D1::SizeU(static_cast<UINT32>(std::max<LONG>(1, rc.right - rc.left)),
                           static_cast<UINT32>(std::max<LONG>(1, rc.bottom - rc.top))));
    HRESULT hr = d2d_->CreateHwndRenderTarget(props, hprops, &rt_);
    if (FAILED(hr)) {
        HK_LOGE("controller settings: CreateHwndRenderTarget failed: %s", hresult_string(hr).c_str());
        return false;
    }
    hr = rt_->CreateSolidColorBrush(col(kText), &brush_);
    if (FAILED(hr) || !art_.create_device(rt_.Get())) {
        release_device();
        return false;
    }
    return true;
}

void SettingsWindow::release_device()
{
    art_.release_device();
    brush_.Reset();
    rt_.Reset();
}

// ---- state helpers -------------------------------------------------------------

int SettingsWindow::current_layout() const
{
    if (work_.pad == InputMapping::defaults_positional().pad) return 0;
    if (work_.pad == InputMapping::defaults_by_label().pad) return 1;
    return -1;
}

void SettingsWindow::toast(std::wstring text)
{
    toast_ = std::move(text);
    toast_t_ = now_s();
}

void SettingsWindow::refresh_devices()
{
    last_device_refresh_ = now_s();
    std::string previous;
    if (tab_ == Tab::Gamepad && device_sel_ >= 0 && device_sel_ < static_cast<int>(devices_.size()))
        previous = devices_[static_cast<size_t>(device_sel_)];
    devices_ = deps_.list_devices ? deps_.list_devices() : std::vector<std::string>{};
    device_items_.clear();
    for (const auto& d : devices_) device_items_.push_back(widen(d));
    if (devices_.empty()) device_items_.push_back(L"No controller detected");
    device_items_.push_back(L"Keyboard");

    if (tab_ == Tab::Keyboard) {
        device_sel_ = static_cast<int>(device_items_.size()) - 1;
        return;
    }
    if (previous.empty()) previous = raw_.device_name;
    device_sel_ = 0;
    for (size_t i = 0; i < devices_.size(); ++i)
        if (devices_[i] == previous) device_sel_ = static_cast<int>(i);
}

void SettingsWindow::refresh_profiles()
{
    profiles_ = list_mapping_profiles(cfg_);
    if (std::find(profiles_.begin(), profiles_.end(), work_.profile) == profiles_.end()) profiles_.push_back(work_.profile);
    std::sort(profiles_.begin(), profiles_.end(), [](const std::string& a, const std::string& b) {
        if (a == "Default" || b == "Default") return a == "Default" && b != "Default";
        return _stricmp(a.c_str(), b.c_str()) < 0;
    });
}

void SettingsWindow::select_device(int idx)
{
    const int keyboard_idx = static_cast<int>(device_items_.size()) - 1;
    if (idx == keyboard_idx) {
        set_tab(Tab::Keyboard);
        return;
    }
    set_tab(Tab::Gamepad);
    device_sel_ = idx;
    if (idx >= 0 && idx < static_cast<int>(devices_.size()) && deps_.select_device)
        deps_.select_device(devices_[static_cast<size_t>(idx)]);
}

void SettingsWindow::set_tab(Tab t)
{
    if (t == tab_) return;
    cancel_capture();
    tab_ = t;
    if (t == Tab::Keyboard) {
        device_sel_ = static_cast<int>(device_items_.size()) - 1;
    } else {
        device_sel_ = 0;
        for (size_t i = 0; i < devices_.size(); ++i)
            if (devices_[i] == raw_.device_name) device_sel_ = static_cast<int>(i);
    }
}

void SettingsWindow::start_capture(int input)
{
    cap_target_ = input;
    cap_start_ = now_s();
    cap_base_ = capture_bits(raw_);
    focus_ = input;
}

void SettingsWindow::cancel_capture() { cap_target_ = -1; }

void SettingsWindow::bind(PhysicalInput p)
{
    if (cap_target_ < 0) return;
    auto& arr = bindings();
    const auto t = static_cast<size_t>(cap_target_);
    const double now = now_s();
    int swapped = -1;
    if (p != PhysicalInput::None) {
        // One physical input drives one Switch input: swap with the previous owner.
        for (size_t j = 0; j < kSwitchInputCount; ++j) {
            if (j != t && arr[j] == p) {
                arr[j] = arr[t];
                flash_t_[j] = now;
                swapped = static_cast<int>(j);
            }
        }
    }
    arr[t] = p;
    flash_t_[t] = now;
    // The art flashes the input that moved, so a swap is visible at a glance.
    last_flash_ = swapped >= 0 ? swapped : cap_target_;
    cap_target_ = -1;
}

float SettingsWindow::flash_amount(int i) const
{
    if (i < 0) return 0;
    const double age = t_ - flash_t_[static_cast<size_t>(i)];
    if (age < 0 || age > 0.9) return 0;
    return static_cast<float>(1.0 - age / 0.9);
}

void SettingsWindow::move_focus(int dir_row, int dir_col)
{
    focus_visible_ = true;
    int pos = 0;
    for (size_t i = 0; i < order_.size(); ++i)
        if (order_[i] == focus_) pos = static_cast<int>(i);
    const int n = static_cast<int>(order_.size());
    if (dir_col == 0) {
        const int next = pos + dir_row;
        if (next >= 0 && next < n) focus_ = order_[static_cast<size_t>(next)];
        return;
    }
    // Left/right: nearest row (by y) in the neighbouring column.
    const int col = lay_.row_col[static_cast<size_t>(focus_)] + dir_col;
    if (col < 0 || col >= kColumns) return;
    const float y = lay_.row_btn[static_cast<size_t>(focus_)].top;
    float best = 1e9f;
    for (size_t i = 0; i < kSwitchInputCount; ++i) {
        if (lay_.row_col[i] != col) continue;
        const float d = std::fabs(lay_.row_btn[i].top - y);
        if (d < best) {
            best = d;
            focus_ = static_cast<int>(i);
        }
    }
}

void SettingsWindow::confirm(std::wstring title, std::wstring message, std::wstring ok, bool danger,
                             std::function<void()> on_ok)
{
    dlg_ = Dialog{};
    dlg_.kind = DlgKind::Confirm;
    dlg_.title = std::move(title);
    dlg_.message = std::move(message);
    dlg_.ok_text = std::move(ok);
    dlg_.danger = danger;
    dlg_.on_ok = [fn = std::move(on_ok)](const std::wstring&) {
        fn();
        return true;
    };
    drop_ = Drop::None;
    cancel_capture();
}

void SettingsWindow::commit_save()
{
    work_.save(cfg_, "mapping." + work_.profile);
    cfg_.set("client", "mapping_profile", work_.profile);
    if (!cfg_.save()) {
        HK_LOGE("controller settings: could not write %s", cfg_.path().c_str());
        toast(L"Couldn’t write the settings file");
    } else {
        HK_LOGI("controller settings: saved profile '%s'", work_.profile.c_str());
    }
    committed_ = work_;
    mapping_ = work_;
    saved_any_ = true;
    refresh_profiles();
}

void SettingsWindow::load_profile(const std::string& name)
{
    InputMapping m = InputMapping::defaults_positional();
    if (cfg_.has_section("mapping." + name)) m.load(cfg_, "mapping." + name);
    m.profile = name;
    work_ = m;
    cancel_capture();
    refresh_profiles();
}

void SettingsWindow::request_close()
{
    if (dirty()) {
        confirm(L"Discard unsaved changes?", L"Your changes to this profile haven’t been saved.", L"Discard", true,
                [this] { done_ = true; });
        return;
    }
    done_ = true;
}

void SettingsWindow::set_deadzone_from_x(float x)
{
    const D2D1_RECT_F& t = lay_.slider_track;
    const float v = std::clamp((x - t.left) / std::max(1.0f, t.right - t.left), 0.0f, 1.0f);
    work_.stick_deadzone = std::round(v * 30.0f) / 100.0f;
}

void SettingsWindow::tick()
{
    t_ = now_s();
    raw_ = deps_.poll_pad ? deps_.poll_pad() : RawPadState{};
    kb_.down.reset();
    if (GetForegroundWindow() == hwnd_) {
        for (int vk = 0x08; vk <= 0xFE; ++vk)
            if (GetAsyncKeyState(vk) & 0x8000) kb_.down.set(static_cast<size_t>(vk));
    }
    preview_ = work_.apply(raw_, kb_);

    active_ = 0;
    for (size_t i = 0; i < kSwitchInputCount; ++i)
        if (kSwitchBit[i] & preview_.buttons) active_ |= 1u << i;
    const int16_t half = 16384;
    const auto dir = [&](SwitchInput s, bool on) {
        if (on) active_ |= 1u << sidx(s);
    };
    dir(SwitchInput::LStickUp, preview_.ly > half);
    dir(SwitchInput::LStickDown, preview_.ly < -half);
    dir(SwitchInput::LStickLeft, preview_.lx < -half);
    dir(SwitchInput::LStickRight, preview_.lx > half);
    dir(SwitchInput::RStickUp, preview_.ry > half);
    dir(SwitchInput::RStickDown, preview_.ry < -half);
    dir(SwitchInput::RStickLeft, preview_.rx < -half);
    dir(SwitchInput::RStickRight, preview_.rx > half);

    if (t_ - last_device_refresh_ > 1.0 && drop_ != Drop::Device) refresh_devices();

    if (cap_target_ >= 0) {
        if (t_ - cap_start_ > kCaptureTimeoutS) {
            cancel_capture();
            toast(L"Rebind timed out");
        } else if (tab_ == Tab::Gamepad) {
            const PadBits now = capture_bits(raw_);
            const PadBits fresh = now & ~cap_base_;
            cap_base_ &= now;  // a button held at start can be captured after release + press
            for (size_t i = 1; i < fresh.size(); ++i) {
                if (fresh.test(i)) {
                    bind(static_cast<PhysicalInput>(i));
                    break;
                }
            }
        }
    }

    const bool toggles[3] = {work_.analog_sticks, work_.analog_triggers, work_.keyboard_enabled};
    for (int i = 0; i < 3; ++i) toggle_anim_[i] += ((toggles[i] ? 1.0f : 0.0f) - toggle_anim_[i]) * 0.35f;
}

// ---- layout & hit testing -----------------------------------------------------

void SettingsWindow::compute_layout()
{
    RECT rc;
    GetClientRect(hwnd_, &rc);
    Layout& L = lay_;
    L.w = dip(rc.right - rc.left);
    L.h = dip(rc.bottom - rc.top);
    const float M = 24.0f;

    // Toolbar: captions above the controls.
    const float cap_y = 78.0f, ctl_y = 98.0f, ctl_h = 36.0f;
    float x = M;
    L.cap_dev = {x, cap_y};
    L.dev = {x, ctl_y, x + 300, ctl_y + ctl_h};
    x = L.dev.right + 16;
    L.cap_layout = {x, cap_y};
    L.layout = {x, ctl_y, x + 170, ctl_y + ctl_h};
    x = L.layout.right + 16;
    L.cap_profile = {x, cap_y};
    L.profile = {x, ctl_y, x + 210, ctl_y + ctl_h};
    L.save_as = {L.profile.right + 8, ctl_y, L.profile.right + 8 + 96, ctl_y + ctl_h};
    L.del = {L.save_as.right + 8, ctl_y, L.save_as.right + 8 + 84, ctl_y + ctl_h};

    // Content cards.
    const float top = 148.0f, bottom = L.h - 92.0f;
    const float left_w = std::clamp(L.w * 0.57f, 600.0f, 800.0f);
    L.left_card = {M, top, M + left_w, bottom};
    L.art_card = {L.left_card.right + 16, top, L.w - M, bottom};

    const float pad = 18.0f;
    const D2D1_RECT_F in = inset(L.left_card, pad, pad);
    L.tabs = {in.left, in.top, in.left + 236, in.top + 34};
    L.tab_pad = {L.tabs.left + 3, L.tabs.top + 3, L.tabs.left + 118, L.tabs.bottom - 3};
    L.tab_kb = {L.tab_pad.right, L.tabs.top + 3, L.tabs.right - 3, L.tabs.bottom - 3};
    L.hint = {L.tabs.right + 16, in.top, in.right, in.top + 34};

    // Options block at the bottom of the left card.
    const float opt_top = in.bottom - 74.0f;
    L.options_divider = opt_top - 12.0f;
    const float tw = (in.right - in.left) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        const float tx = in.left + tw * static_cast<float>(i);
        L.toggles[i] = {tx, opt_top, tx + tw - 8, opt_top + 30};
    }
    const float sy = opt_top + 44.0f;
    L.slider_label = {in.left, sy, in.left + 120, sy + 30};
    L.slider_track = {in.left + 130, sy + 13, in.left + 130 + std::min(260.0f, in.right - in.left - 260), sy + 17};
    L.slider_hit = {L.slider_track.left - 10, sy, L.slider_track.right + 10, sy + 30};
    L.slider_value = {L.slider_track.right + 16, sy, L.slider_track.right + 72, sy + 30};

    // Binding columns.
    const float grid_top = L.tabs.bottom + 18.0f, grid_bottom = L.options_divider - 14.0f;
    const float gap = 22.0f;
    const float col_w = ((in.right - in.left) - gap * (kColumns - 1)) / kColumns;
    const float hdr_h = 26.0f, group_gap = 14.0f;
    for (int c = 0; c < kColumns; ++c) {
        int rows = 0, groups = 0;
        for (const auto& g : kGroups)
            if (g.column == c) {
                rows += g.count;
                ++groups;
            }
        const float avail = grid_bottom - grid_top - static_cast<float>(groups) * hdr_h -
                            static_cast<float>(std::max(0, groups - 1)) * group_gap;
        const float pitch = std::clamp(avail / static_cast<float>(std::max(1, rows)), 24.0f, 40.0f);
        const float row_h = std::min(32.0f, pitch - 5.0f);
        const float cx = in.left + (col_w + gap) * static_cast<float>(c);
        const float label_w = std::clamp(col_w * 0.30f, 52.0f, 76.0f);
        float y = grid_top;
        for (int gi = 0; gi < kGroupCount; ++gi) {
            const GroupDef& g = kGroups[gi];
            if (g.column != c) continue;
            L.group_hdr[gi] = {cx, y, cx + col_w, y + hdr_h - 6};
            y += hdr_h;
            for (int r = 0; r < g.count; ++r) {
                const auto i = sidx(g.rows[r].input);
                L.row_label[i] = {cx, y, cx + label_w, y + row_h};
                L.row_btn[i] = {cx + label_w + 6, y, cx + col_w, y + row_h};
                L.row_col[i] = c;
                y += pitch;
            }
            y += group_gap;
        }
    }

    // Art card.
    const D2D1_RECT_F ain = inset(L.art_card, 20, 18);
    L.pill_anchor = {ain.left, ain.top, ain.right, ain.top + 28};
    L.art_area = {ain.left, ain.top + 44, ain.right, ain.bottom - 34};
    L.art_hint = {ain.left, ain.bottom - 24, ain.right, ain.bottom};

    // Footer.
    const float fy = L.h - 55.0f, fh = 36.0f;
    L.close = {L.w - M - 104, fy, L.w - M, fy + fh};
    L.save = {L.close.left - 10 - 120, fy, L.close.left - 10, fy + fh};
    L.reset = {L.save.left - 22 - 156, fy, L.save.left - 22, fy + fh};
    L.status = {M, fy, L.reset.left - 16, fy + fh};
}

D2D1_RECT_F SettingsWindow::drop_anchor(Drop d) const
{
    switch (d) {
    case Drop::Device: return lay_.dev;
    case Drop::Layout: return lay_.layout;
    case Drop::Profile: return lay_.profile;
    default: return {};
    }
}

std::vector<std::wstring> SettingsWindow::drop_items(Drop d) const
{
    switch (d) {
    case Drop::Device: return device_items_;
    case Drop::Layout:
        return {L"Positional  ·  Xbox A → Switch B", L"By label  ·  Xbox A → Switch A"};
    case Drop::Profile: {
        std::vector<std::wstring> out;
        for (const auto& p : profiles_) out.push_back(widen(p));
        return out;
    }
    default: return {};
    }
}

int SettingsWindow::drop_selected(Drop d) const
{
    switch (d) {
    case Drop::Device: return device_sel_;
    case Drop::Layout: return current_layout();
    case Drop::Profile:
        for (size_t i = 0; i < profiles_.size(); ++i)
            if (profiles_[i] == work_.profile) return static_cast<int>(i);
        return -1;
    default: return -1;
    }
}

constexpr float kItemH = 34.0f;

D2D1_RECT_F SettingsWindow::popup_rect() const
{
    const D2D1_RECT_F a = drop_anchor(drop_);
    const auto n = static_cast<float>(drop_items(drop_).size());
    const float w = std::max(a.right - a.left, drop_ == Drop::Layout ? 300.0f : 0.0f);
    const float h = n * kItemH + 8.0f;
    D2D1_RECT_F r = {a.left, a.bottom + 6, a.left + w, a.bottom + 6 + h};
    if (r.bottom > lay_.h - 8) r = {a.left, a.top - 6 - h, a.left + w, a.top - 6};
    return r;
}

D2D1_RECT_F SettingsWindow::dialog_rect() const
{
    const float w = 460.0f, h = dlg_.kind == DlgKind::TextEntry ? 232.0f : 196.0f;
    const float x = std::floor((lay_.w - w) * 0.5f), y = std::floor((lay_.h - h) * 0.5f);
    return {x, y, x + w, y + h};
}

D2D1_RECT_F SettingsWindow::dialog_ok_rect() const
{
    const D2D1_RECT_F d = dialog_rect();
    return {d.right - 22 - 124, d.bottom - 22 - 36, d.right - 22, d.bottom - 22};
}

D2D1_RECT_F SettingsWindow::dialog_cancel_rect() const
{
    const D2D1_RECT_F ok = dialog_ok_rect();
    return {ok.left - 10 - 100, ok.top, ok.left - 10, ok.bottom};
}

D2D1_RECT_F SettingsWindow::dialog_field_rect() const
{
    const D2D1_RECT_F d = dialog_rect();
    return {d.left + 24, d.top + 92, d.right - 24, d.top + 130};
}

Hit SettingsWindow::hit_test(float x, float y) const
{
    if (dlg_.kind != DlgKind::None) {
        if (contains(dialog_ok_rect(), x, y)) return {El::DlgOk, 0};
        if (contains(dialog_cancel_rect(), x, y)) return {El::DlgCancel, 0};
        if (dlg_.kind == DlgKind::TextEntry && contains(dialog_field_rect(), x, y)) return {El::DlgField, 0};
        if (contains(dialog_rect(), x, y)) return {El::DlgBody, 0};
        return {El::Backdrop, 0};
    }
    if (drop_ != Drop::None) {
        const D2D1_RECT_F p = popup_rect();
        if (contains(p, x, y)) {
            const int i = static_cast<int>((y - p.top - 4) / kItemH);
            const int n = static_cast<int>(drop_items(drop_).size());
            return i >= 0 && i < n ? Hit{El::DropItem, i} : Hit{El::DlgBody, 0};
        }
        if (contains(drop_anchor(drop_), x, y))
            return {drop_ == Drop::Device ? El::DevDrop : drop_ == Drop::Layout ? El::LayoutDrop : El::ProfileDrop, 0};
        return {El::Backdrop, 0};
    }
    const Layout& L = lay_;
    if (contains(L.dev, x, y)) return {El::DevDrop, 0};
    if (contains(L.layout, x, y)) return {El::LayoutDrop, 0};
    if (contains(L.profile, x, y)) return {El::ProfileDrop, 0};
    if (contains(L.save_as, x, y)) return {El::SaveAs, 0};
    if (contains(L.del, x, y)) return {El::Delete, 0};
    if (contains(L.tab_pad, x, y)) return {El::TabPad, 0};
    if (contains(L.tab_kb, x, y)) return {El::TabKb, 0};
    for (int i = 0; i < 3; ++i)
        if (contains(L.toggles[i], x, y)) return {El::Toggle, i};
    if (contains(L.slider_hit, x, y)) return {El::Slider, 0};
    if (contains(L.reset, x, y)) return {El::Reset, 0};
    if (contains(L.save, x, y)) return {El::Save, 0};
    if (contains(L.close, x, y)) return {El::Close, 0};
    for (size_t i = 0; i < kSwitchInputCount; ++i)
        if (contains(L.row_btn[i], x, y) || contains(L.row_label[i], x, y)) return {El::Binding, static_cast<int>(i)};
    if (contains(L.art_area, x, y)) {
        const int a = art_.hit_test(x, y);
        if (a >= 0) return {El::Art, a};
    }
    return {};
}

bool SettingsWindow::clickable(const Hit& h) const
{
    return h.el != El::None && h.el != El::DlgBody && h.el != El::Backdrop && h.el != El::DlgField;
}

// ---- actions ----------------------------------------------------------------------

void SettingsWindow::activate(const Hit& h)
{
    // Any click outside the binding being captured ends the capture.
    if (cap_target_ >= 0 && !((h.el == El::Binding || h.el == El::Art) && h.idx == cap_target_)) cancel_capture();

    switch (h.el) {
    case El::Binding:
    case El::Art:
        focus_ = h.idx;
        if (cap_target_ == h.idx) cancel_capture();
        else start_capture(h.idx);
        break;
    case El::DevDrop: drop_ = drop_ == Drop::Device ? Drop::None : Drop::Device; break;
    case El::LayoutDrop: drop_ = drop_ == Drop::Layout ? Drop::None : Drop::Layout; break;
    case El::ProfileDrop:
        if (drop_ != Drop::Profile) refresh_profiles();
        drop_ = drop_ == Drop::Profile ? Drop::None : Drop::Profile;
        break;
    case El::Backdrop: drop_ = Drop::None; break;
    case El::DropItem: {
        const Drop d = drop_;
        drop_ = Drop::None;
        if (d == Drop::Device) {
            select_device(h.idx);
        } else if (d == Drop::Layout) {
            if (h.idx == current_layout()) break;
            const bool positional = h.idx == 0;
            const std::wstring name = positional ? L"Positional" : L"By label";
            confirm(L"Switch to the " + name + L" layout?",
                    positional ? L"Gamepad bindings in this profile are reset so buttons keep their Switch position "
                                 L"(Xbox A → Switch B). Keyboard bindings are kept."
                               : L"Gamepad bindings in this profile are reset so buttons match their printed label "
                                 L"(Xbox A → Switch A). Keyboard bindings are kept.",
                    L"Reset bindings", false, [this, positional] {
                        work_.pad = (positional ? InputMapping::defaults_positional() : InputMapping::defaults_by_label()).pad;
                        layout_pref_ = positional ? 0 : 1;
                        toast(positional ? L"Positional layout applied" : L"By-label layout applied");
                    });
        } else if (d == Drop::Profile && h.idx >= 0 && h.idx < static_cast<int>(profiles_.size())) {
            const std::string name = profiles_[static_cast<size_t>(h.idx)];
            if (name == work_.profile) break;
            if (dirty()) {
                confirm(L"Switch profile?", L"Unsaved changes to “" + widen(work_.profile) + L"” will be lost.",
                        L"Switch", true, [this, name] { load_profile(name); });
            } else {
                load_profile(name);
            }
        }
        break;
    }
    case El::SaveAs:
        dlg_ = Dialog{};
        dlg_.kind = DlgKind::TextEntry;
        dlg_.title = L"Save profile as";
        dlg_.message = L"Profiles keep separate bindings, e.g. one per game.";
        dlg_.ok_text = L"Save";
        dlg_.on_ok = [this](const std::wstring& text) {
            std::wstring t = text;
            while (!t.empty() && iswspace(t.back())) t.pop_back();
            while (!t.empty() && iswspace(t.front())) t.erase(t.begin());
            if (t.empty()) {
                dlg_.error = L"Enter a name";
                return false;
            }
            const std::string name = utf8(t.c_str());
            const auto finish = [this, name] {
                work_.profile = name;
                commit_save();
                toast(L"Saved profile “" + widen(name) + L"”");
            };
            if (name != work_.profile && cfg_.has_section("mapping." + name)) {
                confirm(L"Replace “" + widen(name) + L"”?", L"A profile with this name already exists.",
                        L"Replace", true, finish);
                return false;  // confirm() replaced this dialog
            }
            finish();
            return true;
        };
        break;
    case El::Delete:
        if (work_.profile == "Default") {
            toast(L"The Default profile can’t be deleted");
            break;
        }
        confirm(L"Delete “" + widen(work_.profile) + L"”?", L"This profile and its bindings are removed permanently.",
                L"Delete", true, [this] {
                    const std::string gone = work_.profile;
                    cfg_.remove_section("mapping." + gone);
                    load_profile("Default");
                    commit_save();
                    toast(L"Deleted “" + widen(gone) + L"”");
                });
        break;
    case El::TabPad: set_tab(Tab::Gamepad); break;
    case El::TabKb: set_tab(Tab::Keyboard); break;
    case El::Toggle:
        if (h.idx == 0) work_.analog_sticks = !work_.analog_sticks;
        if (h.idx == 1) work_.analog_triggers = !work_.analog_triggers;
        if (h.idx == 2) work_.keyboard_enabled = !work_.keyboard_enabled;
        break;
    case El::Reset:
        confirm(L"Reset to defaults?",
                L"All gamepad and keyboard bindings and options in “" + widen(work_.profile) +
                    L"” go back to their defaults.",
                L"Reset", true, [this] {
                    const std::string name = work_.profile;
                    const int lay = current_layout() >= 0 ? current_layout() : layout_pref_;
                    work_ = lay == 1 ? InputMapping::defaults_by_label() : InputMapping::defaults_positional();
                    work_.profile = name;
                    toast(L"Defaults restored");
                });
        break;
    case El::Save:
        commit_save();
        done_ = true;
        break;
    case El::Close: request_close(); break;
    case El::DlgOk: {
        if (dlg_.kind == DlgKind::None) break;
        // Copies: on_ok may replace dlg_ with a follow-up dialog and return
        // false, or return false to keep this one open with an error.
        const auto fn = dlg_.on_ok;
        const std::wstring entered = dlg_.text;
        if (!fn || fn(entered)) dlg_ = Dialog{};
        break;
    }
    case El::DlgCancel: dlg_ = Dialog{}; break;
    default: break;
    }
}

void SettingsWindow::on_key(WPARAM vk, LPARAM lp, bool sys, bool* handled)
{
    *handled = true;
    const bool repeat = (lp & (1 << 30)) != 0;
    if (dlg_.kind != DlgKind::None) {
        if (vk == VK_ESCAPE) dlg_ = Dialog{};
        else if (vk == VK_RETURN) activate({El::DlgOk, 0});
        else if (vk == VK_BACK && dlg_.kind == DlgKind::TextEntry && !dlg_.text.empty()) dlg_.text.pop_back();
        else *handled = !sys;
        return;
    }
    if (drop_ != Drop::None) {
        if (vk == VK_ESCAPE) drop_ = Drop::None;
        else *handled = !sys;
        return;
    }
    if (cap_target_ >= 0) {
        if (repeat) return;
        if (vk == VK_ESCAPE) {
            cancel_capture();
        } else if (vk == VK_DELETE || vk == VK_BACK) {
            bind(PhysicalInput::None);
        } else if (tab_ == Tab::Keyboard && vk > 0 && vk < 0xFF) {
            bind(key_input(static_cast<unsigned>(vk)));
        } else {
            *handled = !sys;
        }
        return;
    }
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    switch (vk) {
    case VK_TAB: move_focus(shift ? -1 : 1, 0); break;
    case VK_UP: move_focus(-1, 0); break;
    case VK_DOWN: move_focus(1, 0); break;
    case VK_LEFT: move_focus(0, -1); break;
    case VK_RIGHT: move_focus(0, 1); break;
    case VK_RETURN:
    case VK_SPACE:
        if (!repeat) {
            focus_visible_ = true;
            start_capture(focus_);
        }
        break;
    case VK_DELETE: {
        focus_visible_ = true;
        cap_target_ = focus_;
        bind(PhysicalInput::None);
        break;
    }
    case VK_ESCAPE: request_close(); break;
    default: *handled = !sys; break;
    }
}

void SettingsWindow::on_char(wchar_t ch)
{
    if (dlg_.kind != DlgKind::TextEntry) return;
    // Characters that would break the INI section header are refused.
    if (ch < 32 || ch == L'[' || ch == L']' || ch == L'=' || ch == L';' || ch == L'#' || ch == 127) return;
    if (dlg_.text.size() >= 32) return;
    dlg_.text.push_back(ch);
    dlg_.error.clear();
}

// ---- drawing primitives ---------------------------------------------------------

void SettingsWindow::fill_rr(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c)
{
    brush_->SetColor(c);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get());
}

void SettingsWindow::stroke_rr(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c, float width)
{
    brush_->SetColor(c);
    const float h = width * 0.5f;
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(inset(r, h, h), radius, radius), brush_.Get(), width);
}

void SettingsWindow::shadow(const D2D1_RECT_F& r, float radius, float strength)
{
    // Matches ui::Painter::shadow: stacked layers approximate a soft, offset blur.
    constexpr int kLayers = 6;
    for (int i = 1; i <= kLayers; ++i) {
        const float g = 1.6f * static_cast<float>(i), dy = 2.0f + 0.7f * static_cast<float>(i);
        fill_rr({r.left - g, r.top - g + dy, r.right + g, r.bottom + g + dy}, radius + g,
                col(0x000000, strength * (1.0f - static_cast<float>(i) / (kLayers + 1)) / 3.0f));
    }
}

void SettingsWindow::fill_gradient(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& top, const D2D1_COLOR_F& bottom)
{
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, top}, {1.0f, bottom}};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> gradient;
    if (FAILED(rt_->CreateGradientStopCollection(stops, 2, &collection)) ||
        FAILED(rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({r.left, r.top}, {r.left, r.bottom}),
                                              collection.Get(), &gradient))) {
        fill_rr(r, radius, bottom);
        return;
    }
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), gradient.Get());
}

void SettingsWindow::card(const D2D1_RECT_F& r)
{
    shadow(r, 12, 0.32f);
    fill_rr(r, 12, col(kPanel));
    stroke_rr(r, 12, col(0xFFFFFF, 0.065f), 1.0f);
    brush_->SetColor(col(0xFFFFFF, 0.035f));
    rt_->DrawLine({r.left + 12, r.top + 1.5f}, {r.right - 12, r.top + 1.5f}, brush_.Get(), 1.0f);
}

void SettingsWindow::highlight(const D2D1_RECT_F& r, float radius, float alpha)
{
    brush_->SetColor(col(0xFFFFFF, alpha));
    rt_->DrawLine({r.left + radius, r.top + 1.5f}, {r.right - radius, r.top + 1.5f}, brush_.Get(), 1.0f);
}

void SettingsWindow::text(const std::wstring& s, const D2D1_RECT_F& r, IDWriteTextFormat* f, const D2D1_COLOR_F& c,
                          DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT para)
{
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(para);
    brush_->SetColor(c);
    rt_->DrawText(s.c_str(), static_cast<UINT32>(s.size()), f, r, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float SettingsWindow::text_width(const std::wstring& s, IDWriteTextFormat* f)
{
    ComPtr<IDWriteTextLayout> layout;
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, 4096.0f, 100.0f, &layout))) return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void SettingsWindow::chevron(float cx, float cy, float size, bool up, const D2D1_COLOR_F& c)
{
    brush_->SetColor(c);
    const float d = up ? -size * 0.5f : size * 0.5f;
    rt_->DrawLine({cx - size, cy - d}, {cx, cy + d}, brush_.Get(), 1.6f, round_.Get());
    rt_->DrawLine({cx, cy + d}, {cx + size, cy - d}, brush_.Get(), 1.6f, round_.Get());
}

void SettingsWindow::button(const D2D1_RECT_F& r, const wchar_t* label, Hit h, BtnStyle style, bool enabled)
{
    const bool hov = enabled && hover_ == h;
    const bool down = hov && pressed_ == h;
    D2D1_COLOR_F fg;
    if (!enabled) {
        fill_rr(r, 8, col(kControl, 0.5f));
        stroke_rr(r, 8, col(kControlBorder, 0.5f), 1.0f);
        fg = col(kText3);
    } else if (style == BtnStyle::Primary) {
        if (!down) shadow(r, 8, 0.30f);
        if (down) fill_rr(r, 8, col(ui::theme::kAccentDown));
        else fill_gradient(r, 8, col(hov ? 0xFFD452 : 0xFFC52A), col(hov ? 0xFFC21A : kAmber));
        stroke_rr(r, 8, col(0x000000, 0.22f), 1.0f);
        if (!down) highlight(r, 8, 0.38f);
        fg = col(kInk);
    } else if (style == BtnStyle::Danger) {
        fill_rr(r, 8, col(ui::theme::kBad, down ? 0.30f : hov ? 0.22f : 0.12f));
        stroke_rr(r, 8, col(ui::theme::kBad, 0.35f), 1.0f);
        fg = col(kDanger);
    } else {
        if (down) fill_rr(r, 8, col(0x1B1C20));
        else fill_gradient(r, 8, col(hov ? 0x2D2F35 : 0x25272C), col(hov ? 0x26282D : 0x1F2024));
        stroke_rr(r, 8, col(hov ? kControlBorderHover : kControlBorder), 1.0f);
        if (!down) highlight(r, 8, 0.05f);
        fg = col(kText);
    }
    text(label, down ? D2D1_RECT_F{r.left, r.top + 1, r.right, r.bottom + 1} : r, f_body_sb_.Get(), fg, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void SettingsWindow::dropdown(const D2D1_RECT_F& r, const std::wstring& value, Hit h, bool open)
{
    const bool hov = hover_ == h;
    if (open) fill_rr({r.left - 3, r.top - 3, r.right + 3, r.bottom + 3}, 11, col(kAmber, 0.16f));
    fill_gradient(r, 8, col(hov || open ? 0x2D2F35 : 0x25272C), col(hov || open ? 0x26282D : 0x1F2024));
    stroke_rr(r, 8, open ? col(kAmber) : col(hov ? kControlBorderHover : kControlBorder), 1.0f);
    if (!open) highlight(r, 8, 0.05f);
    text(value, {r.left + 12, r.top, r.right - 34, r.bottom}, f_body_.Get(), col(kText));
    chevron(r.right - 18, (r.top + r.bottom) * 0.5f, 4.5f, open, col(kText2));
}

// ---- drawing: sections -----------------------------------------------------------

void SettingsWindow::draw_header()
{
    text(L"Controller settings", {24, 16, lay_.w - 300, 46}, f_title_.Get(), col(kText));
    text(L"Choose a binding, then press the button or key to use.", {24, 48, lay_.w - 100, 70}, f_sub_.Get(), col(kText2));

}

void SettingsWindow::draw_toolbar()
{
    const auto caption = [&](D2D1_POINT_2F p, const wchar_t* s) {
        text(s, {p.x + 1, p.y - 1, p.x + 300, p.y + 17}, f_caption_.Get(), col(kText2));
    };
    caption(lay_.cap_dev, L"Input device");
    caption(lay_.cap_layout, L"Layout");
    caption(lay_.cap_profile, L"Profile");

    const std::wstring dev = device_sel_ >= 0 && device_sel_ < static_cast<int>(device_items_.size())
                                 ? device_items_[static_cast<size_t>(device_sel_)]
                                 : L"";
    dropdown(lay_.dev, dev, {El::DevDrop, 0}, drop_ == Drop::Device);
    const int lay = current_layout();
    dropdown(lay_.layout, lay == 0 ? L"Positional" : lay == 1 ? L"By label" : L"Custom", {El::LayoutDrop, 0},
             drop_ == Drop::Layout);
    dropdown(lay_.profile, widen(work_.profile), {El::ProfileDrop, 0}, drop_ == Drop::Profile);
    button(lay_.save_as, L"Save as…", {El::SaveAs, 0}, BtnStyle::Secondary);
    button(lay_.del, L"Delete", {El::Delete, 0}, BtnStyle::Danger, work_.profile != "Default");
}

void SettingsWindow::draw_binding(int i)
{
    const auto si = static_cast<size_t>(i);
    const D2D1_RECT_F r = lay_.row_btn[si];
    const bool cap = cap_target_ == i;
    const bool hov = (hover_.el == El::Binding || hover_.el == El::Art) && hover_.idx == i;
    const bool foc = focus_visible_ && focus_ == i;
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(t_) * 2.0f * kPi * 1.3f);

    fill_rr(r, 7, cap ? col(kAmber, 0.10f) : col(hov ? kControlHover : kControl));
    if (!cap) highlight(r, 7, 0.035f);
    const float fl = flash_amount(i);
    if (fl > 0) fill_rr(r, 7, col(kAmber, 0.35f * fl));
    if (cap) {
        stroke_rr(r, 7, col(kAmber, 0.45f + 0.55f * pulse), 1.8f);
    } else if (foc) {
        stroke_rr(r, 7, col(kAmber), 1.5f);
    } else {
        stroke_rr(r, 7, col(hov ? kControlBorderHover : kControlBorder), 1.0f);
    }

    const D2D1_RECT_F content = {r.left + 8, r.top, r.right - 8, r.bottom};
    if (cap) {
        text(tab_ == Tab::Gamepad ? L"Press a button…" : L"Press a key…", content, f_body_sb_.Get(),
             col(kAmber), DWRITE_TEXT_ALIGNMENT_CENTER);
        // Remaining time before the capture gives up.
        const float left = 1.0f - static_cast<float>((t_ - cap_start_) / kCaptureTimeoutS);
        const float w = (r.right - r.left - 14) * std::clamp(left, 0.0f, 1.0f);
        fill_rr({r.left + 7, r.bottom - 4, r.left + 7 + w, r.bottom - 2}, 1, col(kAmber, 0.8f));
        return;
    }

    const PhysicalInput p = (tab_ == Tab::Gamepad ? work_.pad : work_.keyboard)[si];
    if (p == PhysicalInput::None) {
        text(L"Unbound", content, f_body_.Get(), col(kText3), DWRITE_TEXT_ALIGNMENT_CENTER);
    } else if (is_key(p)) {
        // Keyboard bindings render as a keycap.
        std::wstring name = widen(to_string(p));
        if (name.rfind(L"Key ", 0) == 0) name = name.substr(4);
        const float tw = std::min(text_width(name, f_key_.Get()), r.right - r.left - 34);
        const float cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f;
        const float kw = std::max(tw + 18.0f, 26.0f);
        const D2D1_RECT_F key = {cx - kw * 0.5f, cy - 11, cx + kw * 0.5f, cy + 10};
        fill_rr({key.left, key.top + 2, key.right, key.bottom + 2}, 5, col(0x050506));
        fill_gradient(key, 5, col(0x3A3D45), col(0x2E3036));
        stroke_rr(key, 5, col(0xFFFFFF, 0.09f), 1.0f);
        text(name, {key.left + 4, key.top, key.right - 4, key.bottom}, f_key_.Get(), col(kText),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        text(widen(to_string(p)), content, f_body_.Get(), col(kText), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // Live dot: this Switch input is currently pressed.
    if ((active_ >> i) & 1u) {
        brush_->SetColor(col(kAmber));
        rt_->FillEllipse(D2D1::Ellipse({r.right - 11, (r.top + r.bottom) * 0.5f}, 3.5f, 3.5f), brush_.Get());
    }
}

void SettingsWindow::draw_bindings()
{
    card(lay_.left_card);

    // Gamepad / Keyboard segmented tabs.
    fill_rr(lay_.tabs, 9, col(kBg));
    stroke_rr(lay_.tabs, 9, col(kPanelBorder), 1.0f);
    const auto tab = [&](const D2D1_RECT_F& r, const wchar_t* label, bool sel, Hit h) {
        if (sel) {
            fill_gradient(r, 7, col(0x2D2F35), col(0x25272C));
            stroke_rr(r, 7, col(0xFFFFFF, 0.08f), 1.0f);
            fill_rr({(r.left + r.right) * 0.5f - 14, r.bottom - 4, (r.left + r.right) * 0.5f + 14, r.bottom - 2}, 1,
                    col(kAmber));
        } else if (hover_ == h) {
            fill_rr(r, 7, col(kControl));
        }
        text(label, {r.left, r.top, r.right, r.bottom - 2}, sel ? f_body_sb_.Get() : f_body_.Get(),
             col(sel ? kText : kText2), DWRITE_TEXT_ALIGNMENT_CENTER);
    };
    tab(lay_.tab_pad, L"Gamepad", tab_ == Tab::Gamepad, {El::TabPad, 0});
    tab(lay_.tab_kb, L"Keyboard", tab_ == Tab::Keyboard, {El::TabKb, 0});

    std::wstring hint;
    D2D1_COLOR_F hint_col = col(kText3);
    if (cap_target_ >= 0) {
        hint = L"Esc cancels  ·  Delete clears";
        hint_col = col(kAmber);
    } else if (tab_ == Tab::Keyboard && !work_.keyboard_enabled) {
        hint = L"Keyboard input is turned off below";
        hint_col = col(kDanger);
    } else {
        hint = L"Click a binding, then press the input to use";
    }
    text(hint, lay_.hint, f_small_.Get(), hint_col, DWRITE_TEXT_ALIGNMENT_TRAILING);

    for (int gi = 0; gi < kGroupCount; ++gi) {
        const GroupDef& g = kGroups[gi];
        const D2D1_RECT_F h = lay_.group_hdr[gi];
        text(g.title, h, f_caption_.Get(), col(kText2));
        const float tw = text_width(g.title, f_caption_.Get());
        brush_->SetColor(col(0xFFFFFF, 0.06f));
        const float ly = std::floor((h.top + h.bottom) * 0.5f) + 0.5f;
        if (h.left + tw + 10 < h.right) rt_->DrawLine({h.left + tw + 10, ly}, {h.right, ly}, brush_.Get(), 1.0f);
        for (int r = 0; r < g.count; ++r) {
            const int i = static_cast<int>(g.rows[r].input);
            const auto si = static_cast<size_t>(i);
            const bool emph = cap_target_ == i || ((hover_.el == El::Binding || hover_.el == El::Art) && hover_.idx == i) ||
                              (focus_visible_ && focus_ == i);
            text(g.rows[r].label, lay_.row_label[si], f_body_.Get(), col(emph ? kText : kText2));
            draw_binding(i);
        }
    }
    draw_options();
}

void SettingsWindow::draw_options()
{
    brush_->SetColor(col(0xFFFFFF, 0.06f));
    const float y = std::floor(lay_.options_divider) + 0.5f;
    rt_->DrawLine({lay_.left_card.left + 18, y}, {lay_.left_card.right - 18, y}, brush_.Get(), 1.0f);

    static const wchar_t* const kLabels[3] = {L"Analog sticks", L"Analog triggers", L"Keyboard enabled"};
    for (int i = 0; i < 3; ++i) {
        const D2D1_RECT_F r = lay_.toggles[i];
        const bool hov = hover_ == Hit{El::Toggle, i};
        const float a = toggle_anim_[i];
        const float cy = (r.top + r.bottom) * 0.5f;
        const D2D1_RECT_F track = {r.left, cy - 10, r.left + 38, cy + 10};
        // Track colour blends from grey to amber as the knob travels.
        const D2D1_COLOR_F off = col(kTrack), on = col(kAmber);
        const D2D1_COLOR_F tc = {off.r + (on.r - off.r) * a, off.g + (on.g - off.g) * a, off.b + (on.b - off.b) * a, 1};
        fill_rr(track, 10, tc);
        if (hov) stroke_rr(track, 10, col(0xFFFFFF, 0.25f), 1.0f);
        const float kx = track.left + 10 + a * 18.0f;
        brush_->SetColor(a > 0.5f ? col(kInk) : col(0xC9CDD2));
        rt_->FillEllipse(D2D1::Ellipse({kx, cy}, 7, 7), brush_.Get());
        text(kLabels[i], {track.right + 10, r.top, r.right, r.bottom}, f_body_.Get(), col(hov ? kText : 0xC8CCD1));
    }

    const bool hov = hover_.el == El::Slider || dragging_slider_;
    text(L"Stick deadzone", lay_.slider_label, f_body_.Get(), col(0xC8CCD1));
    const D2D1_RECT_F t = lay_.slider_track;
    const float v = std::clamp(work_.stick_deadzone / 0.3f, 0.0f, 1.0f);
    const float kx = t.left + (t.right - t.left) * v;
    fill_rr(t, 2, col(kTrack));
    fill_rr({t.left, t.top, kx, t.bottom}, 2, col(kAmber));
    const float cy = (t.top + t.bottom) * 0.5f;
    if (hov) {
        brush_->SetColor(col(kAmber, 0.25f));
        rt_->FillEllipse(D2D1::Ellipse({kx, cy}, 13, 13), brush_.Get());
    }
    brush_->SetColor(col(kText));
    rt_->FillEllipse(D2D1::Ellipse({kx, cy}, 8, 8), brush_.Get());
    brush_->SetColor(col(kAmber));
    rt_->DrawEllipse(D2D1::Ellipse({kx, cy}, 7.5f, 7.5f), brush_.Get(), 1.5f);
    wchar_t buf[48];
    swprintf(buf, 48, L"%d%%", static_cast<int>(std::lround(work_.stick_deadzone * 100.0f)));
    text(buf, {lay_.slider_value.left, lay_.slider_value.top, lay_.slider_value.left + 44, lay_.slider_value.bottom},
         f_body_sb_.Get(), col(kText));

}

void SettingsWindow::draw_art_card()
{
    card(lay_.art_card);

    const D2D1_RECT_F a = lay_.pill_anchor;
    text(deps_.model == joycon::Model::Switch2 ? L"Switch 2" : L"Controller", {a.left + 2, a.top, a.right, a.bottom}, f_caption_.Get(), col(kText2));

    // Status pill, right-aligned.
    std::wstring status;
    uint32_t dot = kText3;
    bool live = false;
    if (tab_ == Tab::Keyboard) {
        status = work_.keyboard_enabled ? L"Keyboard" : L"Keyboard (disabled)";
        dot = work_.keyboard_enabled ? kAmber : kText3;
        live = work_.keyboard_enabled;
    } else if (raw_.connected) {
        status = L"Connected: " + widen(raw_.device_name.empty() ? std::string("Controller") : raw_.device_name);
        dot = kGreen;
        live = true;
    } else {
        status = L"No controller detected — press any button";
    }
    const float max_w = (a.right - a.left) - 110;
    const float tw = std::min(text_width(status, f_small_.Get()), max_w);
    const D2D1_RECT_F pill = {a.right - tw - 36, a.top, a.right, a.bottom};
    fill_rr(pill, 14, live ? col(dot, 0.10f) : col(kControl));
    stroke_rr(pill, 14, live ? col(dot, 0.35f) : col(kControlBorder), 1.0f);
    const float pulse = live ? 1.0f : 0.55f + 0.45f * std::sin(static_cast<float>(t_) * 3.0f);
    brush_->SetColor(col(dot, pulse));
    rt_->FillEllipse(D2D1::Ellipse({pill.left + 14, (pill.top + pill.bottom) * 0.5f}, 4, 4), brush_.Get());
    text(status, {pill.left + 24, pill.top, pill.right - 10, pill.bottom}, f_small_.Get(), col(live ? kText : kText2));

    // Joy-Cons.
    joycon::ArtState s;
    s.active = active_;
    s.lx = preview_.lx / 32767.0f;
    s.ly = preview_.ly / 32767.0f;
    s.rx = preview_.rx / 32767.0f;
    s.ry = preview_.ry / 32767.0f;
    s.lt = preview_.lt / 255.0f;
    s.rt = preview_.rt / 255.0f;
    if (hover_.el == El::Binding || hover_.el == El::Art) s.hover = hover_.idx;
    else if (focus_visible_) s.hover = focus_;
    s.target = cap_target_;
    s.pulse = 0.5f + 0.5f * std::sin(static_cast<float>(t_) * 2.0f * kPi * 1.3f);
    s.flash = last_flash_;
    s.flash_amount = flash_amount(last_flash_);
    art_.draw(rt_.Get(), lay_.art_area, s, f_art_.Get());

    std::wstring hint = L"Click any part of the controller to rebind it";
    D2D1_COLOR_F hc = col(kText3);
    if (cap_target_ >= 0) {
        hint = std::wstring(tab_ == Tab::Gamepad ? L"Press the controller input for " : L"Press the key for ") +
               widen(to_string(static_cast<SwitchInput>(cap_target_)));
        hc = col(kAmber);
    }
    text(hint, lay_.art_hint, f_small_.Get(), hc, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void SettingsWindow::draw_footer()
{
    fill_rr({0, lay_.h - 74.0f, lay_.w, lay_.h}, 0, col(kPanel));
    brush_->SetColor(col(kPanelBorder));
    rt_->DrawLine({0, lay_.h - 73.5f}, {lay_.w, lay_.h - 73.5f}, brush_.Get(), 1.0f);
    button(lay_.reset, L"Reset to defaults", {El::Reset, 0}, BtnStyle::Secondary);
    button(lay_.save, L"Save", {El::Save, 0}, BtnStyle::Primary);
    button(lay_.close, L"Close", {El::Close, 0}, BtnStyle::Secondary);

    const D2D1_RECT_F s = lay_.status;
    const double age = t_ - toast_t_;
    if (!toast_.empty() && age < 3.0) {
        const float a = static_cast<float>(std::min(1.0, (3.0 - age) / 0.4));
        text(toast_, {s.left + 2, s.top, s.right, s.bottom}, f_body_.Get(), col(kText, a));
    } else if (dirty()) {
        brush_->SetColor(col(kAmber));
        rt_->FillEllipse(D2D1::Ellipse({s.left + 6, (s.top + s.bottom) * 0.5f}, 4, 4), brush_.Get());
        text(L"Unsaved changes", {s.left + 18, s.top, s.right, s.bottom}, f_body_.Get(), col(kText2));
    }
}

void SettingsWindow::draw_popup()
{
    if (drop_ == Drop::None) return;
    const D2D1_RECT_F p = popup_rect();
    shadow(p, 10, 0.35f);
    fill_rr(p, 10, col(0x1E1F23));
    stroke_rr(p, 10, col(kControlBorder), 1.0f);
    highlight(p, 10, 0.05f);
    const auto items = drop_items(drop_);
    const int sel = drop_selected(drop_);
    for (size_t i = 0; i < items.size(); ++i) {
        const float y = p.top + 4 + kItemH * static_cast<float>(i);
        const D2D1_RECT_F r = {p.left + 4, y, p.right - 4, y + kItemH};
        const bool hov = hover_ == Hit{El::DropItem, static_cast<int>(i)};
        if (hov) fill_rr(r, 6, col(kControlHover));
        const bool is_sel = static_cast<int>(i) == sel;
        if (is_sel) fill_rr({r.left + 2, r.top + 9, r.left + 5, r.bottom - 9}, 1.5f, col(kAmber));
        text(items[i], {r.left + 14, r.top, r.right - 10, r.bottom}, is_sel ? f_body_sb_.Get() : f_body_.Get(),
             col(is_sel ? kText : 0xC8CCD1));
    }
}

void SettingsWindow::draw_dialog()
{
    if (dlg_.kind == DlgKind::None) return;
    brush_->SetColor(col(0x000000, 0.55f));
    rt_->FillRectangle(D2D1::RectF(0, 0, lay_.w, lay_.h), brush_.Get());
    const D2D1_RECT_F d = dialog_rect();
    shadow(d, 14, 0.5f);
    fill_rr(d, 14, col(kPanel));
    stroke_rr(d, 14, col(0xFFFFFF, 0.08f), 1.0f);
    highlight(d, 14, 0.04f);
    // Amber accent bar.
    fill_rr({d.left + 24, d.top + 20, d.left + 52, d.top + 23}, 1.5f, col(dlg_.danger ? kDanger : kAmber));
    text(dlg_.title, {d.left + 24, d.top + 28, d.right - 24, d.top + 58}, f_dlg_title_.Get(), col(kText));
    if (dlg_.kind == DlgKind::TextEntry) {
        text(dlg_.message, {d.left + 24, d.top + 58, d.right - 24, d.top + 84}, f_small_.Get(), col(kText2));
        const D2D1_RECT_F f = dialog_field_rect();
        fill_rr({f.left - 3, f.top - 3, f.right + 3, f.bottom + 3}, 11, col(kAmber, 0.16f));
        fill_rr(f, 8, col(kControl));
        stroke_rr(f, 8, col(kAmber), 1.0f);
        const D2D1_RECT_F tr = {f.left + 12, f.top, f.right - 12, f.bottom};
        if (dlg_.text.empty()) text(L"Profile name", tr, f_body_.Get(), col(kText3));
        else text(dlg_.text, tr, f_body_.Get(), col(kText));
        // Blinking caret.
        if (std::fmod(t_, 1.0) < 0.55) {
            const float cx = tr.left + (dlg_.text.empty() ? 0.0f : text_width(dlg_.text, f_body_.Get())) + 1;
            brush_->SetColor(col(kAmber));
            rt_->DrawLine({cx, f.top + 10}, {cx, f.bottom - 10}, brush_.Get(), 1.5f);
        }
        if (!dlg_.error.empty())
            text(dlg_.error, {f.left + 2, f.bottom + 4, f.right, f.bottom + 22}, f_small_.Get(), col(kDanger));
    } else {
        text(dlg_.message, {d.left + 24, d.top + 66, d.right - 24, d.bottom - 70}, f_wrap_.Get(), col(kText2),
             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    button(dialog_cancel_rect(), L"Cancel", {El::DlgCancel, 0}, BtnStyle::Secondary);
    button(dialog_ok_rect(), dlg_.ok_text.c_str(), {El::DlgOk, 0}, BtnStyle::Primary);
}

void SettingsWindow::render()
{
    if (IsIconic(hwnd_) || !create_device()) return;
    compute_layout();
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(col(kBg));
    draw_header();
    draw_toolbar();
    draw_bindings();
    draw_art_card();
    draw_footer();
    draw_popup();
    draw_dialog();
    const HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        HK_LOGI("controller settings: render target lost, recreating");
        release_device();
    } else if (FAILED(hr)) {
        HK_LOGE("controller settings: EndDraw failed: %s", hresult_string(hr).c_str());
        release_device();
    }
}

// ---- window plumbing -----------------------------------------------------------

constexpr wchar_t kClassName[] = L"HikariControllerSettings";
constexpr UINT_PTR kTimerId = 1;

LRESULT CALLBACK SettingsWindow::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* self = static_cast<SettingsWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->handle(msg, wp, lp);
}

LRESULT SettingsWindow::handle(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        render();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == kTimerId) {
            tick();
            if (!IsIconic(hwnd_)) InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_SIZE:
        if (rt_ && wp != SIZE_MINIMIZED) {
            rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        if (rt_) rt_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        RECT r = {0, 0, MulDiv(static_cast<int>(kMinW), static_cast<int>(dpi_), 96),
                  MulDiv(static_cast<int>(kMinH), static_cast<int>(dpi_), 96)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
        mmi->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        return 0;
    }
    case WM_MOUSEMOVE: {
        const float x = dip(GET_X_LPARAM(lp)), y = dip(GET_Y_LPARAM(lp));
        if (!tracking_mouse_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            tracking_mouse_ = true;
        }
        if (dragging_slider_) set_deadzone_from_x(x);
        hover_ = hit_test(x, y);
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_mouse_ = false;
        if (!dragging_slider_) hover_ = {};
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd_);
        SetCapture(hwnd_);
        const float x = dip(GET_X_LPARAM(lp)), y = dip(GET_Y_LPARAM(lp));
        pressed_ = hit_test(x, y);
        if (pressed_.el == El::Slider) {
            cancel_capture();
            dragging_slider_ = true;
            set_deadzone_from_x(x);
        } else if (pressed_.el == El::Backdrop && drop_ != Drop::None) {
            drop_ = Drop::None;  // click-away closes the popup immediately
            pressed_ = {};
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        const float x = dip(GET_X_LPARAM(lp)), y = dip(GET_Y_LPARAM(lp));
        const Hit h = hit_test(x, y);
        const Hit down = pressed_;
        pressed_ = {};
        if (dragging_slider_) {
            dragging_slider_ = false;
            return 0;
        }
        if (h == down && clickable(h)) {
            if (h.el == El::Binding || h.el == El::Art) focus_visible_ = false;
            activate(h);
        }
        hover_ = hit_test(x, y);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            const LPCWSTR cur = hover_.el == El::DlgField ? IDC_IBEAM : clickable(hover_) ? IDC_HAND : IDC_ARROW;
            SetCursor(LoadCursorW(nullptr, cur));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        bool handled = false;
        on_key(wp, lp, msg == WM_SYSKEYDOWN, &handled);
        if (handled) return 0;
        break;
    }
    case WM_CHAR: on_char(static_cast<wchar_t>(wp)); return 0;
    case WM_SYSCHAR:
        // Alt is a valid binding while capturing; swallow the menu beep.
        if (cap_target_ >= 0) return 0;
        break;
    case WM_CLOSE: request_close(); return 0;
    default: break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void apply_dark_title_bar(HWND hwnd) { ui::style_window(hwnd); }

bool SettingsWindow::run(HWND parent)
{
    if (!create_factories()) {
        HK_LOGE("controller settings: Direct2D/DirectWrite unavailable");
        return false;
    }
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    if (!GetClassInfoExW(inst, kClassName, &wc)) {
        wc = WNDCLASSEXW{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &SettingsWindow::wndproc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hIconSm = wc.hIcon;
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) {
            HK_LOGE("controller settings: RegisterClassEx failed (%lu)", GetLastError());
            return false;
        }
    }

    // Open on the parent's monitor (or the cursor's), per-monitor DPI aware
    // regardless of the process default.
    HMONITOR mon = nullptr;
    if (parent) {
        mon = MonitorFromWindow(parent, MONITOR_DEFAULTTOPRIMARY);
    } else {
        POINT pt{};
        GetCursorPos(&pt);
        mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    const DPI_AWARENESS_CONTEXT prev = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CreateWindowExW(0, kClassName, L"Hikari — Controller Settings", WS_OVERLAPPEDWINDOW, mi.rcWork.left + 40,
                    mi.rcWork.top + 40, 200, 200, parent, nullptr, inst, this);
    if (prev) SetThreadDpiAwarenessContext(prev);
    if (!hwnd_) {
        HK_LOGE("controller settings: CreateWindow failed (%lu)", GetLastError());
        return false;
    }
    apply_dark_title_bar(hwnd_);

    dpi_ = GetDpiForWindow(hwnd_);
    const int initial_w = deps_.initial_width ? std::max(static_cast<int>(kMinW), deps_.initial_width) : static_cast<int>(kDesignW);
    const int initial_h = deps_.initial_height ? std::max(static_cast<int>(kMinH), deps_.initial_height) : static_cast<int>(kDesignH);
    RECT r = {0, 0, MulDiv(initial_w, static_cast<int>(dpi_), 96),
              MulDiv(initial_h, static_cast<int>(dpi_), 96)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
    const int ww = std::min<int>(r.right - r.left, mi.rcWork.right - mi.rcWork.left);
    const int wh = std::min<int>(r.bottom - r.top, mi.rcWork.bottom - mi.rcWork.top);
    RECT around = mi.rcWork;
    if (parent && IsWindowVisible(parent) && !IsIconic(parent)) GetWindowRect(parent, &around);
    int x = (around.left + around.right - ww) / 2, y = (around.top + around.bottom - wh) / 2;
    x = std::clamp<int>(x, mi.rcWork.left, std::max<int>(mi.rcWork.left, mi.rcWork.right - ww));
    y = std::clamp<int>(y, mi.rcWork.top, std::max<int>(mi.rcWork.top, mi.rcWork.bottom - wh));
    SetWindowPos(hwnd_, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    dpi_ = GetDpiForWindow(hwnd_);

    raw_ = deps_.poll_pad ? deps_.poll_pad() : RawPadState{};
    refresh_devices();
    refresh_profiles();
    layout_pref_ = std::max(0, current_layout());
    compute_layout();
    tick();

    if (parent) EnableWindow(parent, FALSE);
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetTimer(hwnd_, kTimerId, 8, nullptr);  // ~120 Hz live preview (timer-resolution bound)
    HK_LOGI("controller settings: opened (profile '%s')", work_.profile.c_str());

    // Our own modal loop: the caller gets control back only once we're done.
    MSG msg{};
    bool quit = false;
    while (!done_) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0) {
            quit = true;
            break;
        }
        if (got == -1) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    KillTimer(hwnd_, kTimerId);
    if (parent) {
        // Re-enable before destroying so activation returns to the parent.
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
    }
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    release_device();
    if (quit) PostQuitMessage(static_cast<int>(msg.wParam));
    HK_LOGI("controller settings: closed (%s)", saved_any_ ? "saved" : "no changes saved");
    return saved_any_;
}

}  // namespace

bool show_controller_settings(HWND parent, Config& cfg, InputMapping& mapping, const ControllerSettingsDeps& deps)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool saved = false;
    {
        SettingsWindow w(cfg, mapping, deps);
        saved = w.run(parent);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return saved;
}

}  // namespace hikari
