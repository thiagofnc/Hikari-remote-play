#include "hikari/ui_kit.h"

#include <cmath>
#include <cstdio>

namespace hikari::ui {

namespace {
constexpr int kLogoResource = 101;  // RCDATA: assets/hikari_logo_256.png (app/hikari.rc)
constexpr float kPi = 3.14159265f;

D2D1_COLOR_F lerp(D2D1_COLOR_F a, D2D1_COLOR_F b, float t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
}  // namespace

D2D1_COLOR_F rgb(uint32_t hex, float a)
{
    return D2D1::ColorF(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, a);
}

std::wstring wide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

int Interaction::hit(float x, float y) const
{
    for (auto it = hits.rbegin(); it != hits.rend(); ++it)
        if (it->second.contains(x, y)) return it->first;
    return -1;
}

bool Painter::init()
{
    if (d2d_) return true;
    D2D1_FACTORY_OPTIONS opts{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                 reinterpret_cast<void**>(d2d_.GetAddressOf()))))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    load_logo();
    return true;
}

void Painter::load_logo()
{
    if (!wic_) return;
    ComPtr<IWICBitmapDecoder> dec;
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(kLogoResource), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
    if (res) {
        HGLOBAL h = LoadResource(mod, res);
        const DWORD size = SizeofResource(mod, res);
        void* data = LockResource(h);
        ComPtr<IWICStream> stream;
        if (data && SUCCEEDED(wic_->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(data), size)))
            wic_->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &dec);
    }
    if (!dec) {
        // Development builds without resources: look next to the exe / in the repo.
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring dir(path);
        dir = dir.substr(0, dir.find_last_of(L"\\/"));
        for (const wchar_t* rel : {L"\\hikari_logo_256.png", L"\\..\\..\\assets\\hikari_logo_256.png",
                                   L"\\..\\assets\\hikari_logo_256.png"}) {
            if (SUCCEEDED(wic_->CreateDecoderFromFilename((dir + rel).c_str(), nullptr, GENERIC_READ,
                                                         WICDecodeMetadataCacheOnLoad, &dec)))
                break;
        }
    }
    if (!dec) return;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return;
    if (FAILED(wic_->CreateFormatConverter(&logo_src_))) return;
    logo_src_->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                          WICBitmapPaletteTypeMedianCut);
}

void Painter::begin(ID2D1RenderTarget* rt, float scale, double time_s)
{
    rt_ = rt;
    scale_ = scale;
    time_ = time_s;
    if (rt != bound_rt_) {  // brushes and bitmaps belong to one render target
        brush_.Reset();
        logo_.Reset();
        bound_rt_ = rt;
    }
    if (!brush_) rt->CreateSolidColorBrush(rgb(theme::kText), &brush_);
    if (!logo_ && logo_src_) rt->CreateBitmapFromWicBitmap(logo_src_.Get(), nullptr, &logo_);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
}

ID2D1SolidColorBrush* Painter::brush(D2D1_COLOR_F c)
{
    brush_->SetColor(c);
    return brush_.Get();
}

IDWriteTextFormat* Painter::format(float size, Weight w, Align h, Align v, bool wrap)
{
    const uint64_t key = (uint64_t(size * 4) << 24) | (uint64_t(w) << 8) | (uint64_t(h) << 4) | (uint64_t(v) << 2) | wrap;
    auto& f = formats_[key];
    if (!f) {
        dwrite_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, static_cast<DWRITE_FONT_WEIGHT>(w),
                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
        if (!f)
            dwrite_->CreateTextFormat(L"Segoe UI", nullptr, static_cast<DWRITE_FONT_WEIGHT>(w), DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
        f->SetTextAlignment(h == Align::Leading ? DWRITE_TEXT_ALIGNMENT_LEADING
                            : h == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                 : DWRITE_TEXT_ALIGNMENT_TRAILING);
        f->SetParagraphAlignment(v == Align::Leading ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR
                                 : v == Align::Center ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
                                                      : DWRITE_PARAGRAPH_ALIGNMENT_FAR);
        f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (!wrap) {
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> ellipsis;
            dwrite_->CreateEllipsisTrimmingSign(f.Get(), &ellipsis);
            f->SetTrimming(&trim, ellipsis.Get());
        }
    }
    return f.Get();
}

void Painter::clear(uint32_t color) { rt_->Clear(rgb(color)); }

void Painter::background(const Rect& r)
{
    fill(r, rgb(theme::kBg));
}

void Painter::fill(const Rect& r, D2D1_COLOR_F c, float radius)
{
    if (radius > 0)
        rt_->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), brush(c));
    else
        rt_->FillRectangle(r.d2d(), brush(c));
}

void Painter::stroke(const Rect& r, D2D1_COLOR_F c, float width, float radius)
{
    const Rect in = r.inset(width / 2);
    if (radius > 0)
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(in.d2d(), radius, radius), brush(c), width);
    else
        rt_->DrawRectangle(in.d2d(), brush(c), width);
}

void Painter::line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width)
{
    rt_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush(c), width);
}

void Painter::circle(float cx, float cy, float rad, D2D1_COLOR_F c, bool filled, float width)
{
    const auto e = D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad);
    if (filled)
        rt_->FillEllipse(e, brush(c));
    else
        rt_->DrawEllipse(e, brush(c), width);
}

void Painter::text(const std::string& s, const Rect& r, float size, D2D1_COLOR_F c, Weight w, Align h, Align v, bool wrap)
{
    const std::wstring ws = wide(s);
    rt_->DrawTextW(ws.c_str(), static_cast<UINT32>(ws.size()), format(size, w, h, v, wrap), r.d2d(), brush(c),
                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Painter::text_width(const std::string& s, float size, Weight w)
{
    const std::wstring ws = wide(s);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(ws.c_str(), static_cast<UINT32>(ws.size()),
                                         format(size, w, Align::Leading, Align::Center, false), 10000, 1000, &layout)))
        return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

bool Painter::button(Interaction& ui, int id, const Rect& r, const std::string& label, ButtonStyle style, bool enabled)
{
    if (enabled) ui.hits.push_back({id, r});
    const bool hover = enabled && ui.hovered(id, r);
    const bool down = hover && ui.mouse_down && ui.pressed_id == id;
    const bool focus = enabled && ui.focus_id == id;
    const float rad = s(6);
    D2D1_COLOR_F bg, fg, border{0, 0, 0, 0};
    switch (style) {
    case ButtonStyle::Primary:
        bg = rgb(hover ? theme::kAccentHover : theme::kAccent);
        fg = rgb(theme::kOnAccent);
        break;
    case ButtonStyle::Danger:
        bg = rgb(theme::kBad, hover ? 0.28f : 0.18f);
        fg = rgb(0xFFB4B4);
        break;
    case ButtonStyle::Ghost:
        bg = rgb(0xFFFFFF, hover ? 0.06f : 0.0f);
        fg = rgb(hover ? theme::kText : theme::kText2);
        break;
    default:
        bg = rgb(hover ? theme::kSurface2 : theme::kSurface);
        fg = rgb(theme::kText);
        border = rgb(theme::kBorder);
        break;
    }
    if (!enabled) {
        bg.a *= 0.4f;
        fg.a *= 0.4f;
    }
    Rect rr = down ? r.inset(s(1)) : r;
    fill(rr, bg, rad);
    if (border.a > 0) stroke(rr, border, s(1), rad);
    if (focus) stroke(rr.inset(-s(3)), rgb(theme::kAccent, 0.9f), s(2), rad + s(3));
    text(label, rr, s(14), fg, Weight::SemiBold,
         Align::Center, Align::Center);
    return enabled && ui.clicked_id == id;
}

float Painter::pill(float x, float y, const std::string& label, uint32_t dot, bool right_align)
{
    const float size = s(12.5f);
    const float w = text_width(label, size, Weight::Medium) + s(36);
    const float h = s(26);
    const float l = right_align ? x - w : x;
    Rect r{l, y, l + w, y + h};
    fill(r, rgb(theme::kBg2), s(5));
    circle(r.l + s(13), r.cy(), s(4), rgb(dot));
    text(label, Rect{r.l + s(22), r.t, r.r - s(8), r.b}, size, rgb(theme::kText), Weight::Medium);
    return w;
}

void Painter::spinner(float cx, float cy, float radius, D2D1_COLOR_F c)
{
    const float a0 = static_cast<float>(std::fmod(time_ * 5.0, 2 * kPi));
    for (int i = 0; i < 12; ++i) {
        const float a = a0 + i * (2 * kPi / 12);
        const float alpha = (i + 1) / 12.0f;
        D2D1_COLOR_F cc = c;
        cc.a *= alpha;
        circle(cx + std::cos(a) * radius, cy + std::sin(a) * radius, radius * 0.16f, cc);
    }
}

void Painter::progress(const Rect& r, float frac, D2D1_COLOR_F c)
{
    fill(r, rgb(theme::kSurface2), r.h() / 2);
    if (frac <= 0) return;
    Rect f = r;
    f.r = r.l + r.w() * std::min(1.0f, frac);
    fill(f, c, r.h() / 2);
}

bool Painter::toggle(Interaction& ui, int id, const Rect& r, bool on)
{
    ui.hits.push_back({id, r});
    const bool hover = ui.hovered(id, r);
    fill(r, on ? rgb(theme::kAccent) : rgb(hover ? 0x4A505C : 0x3A3F49), r.h() / 2);
    const float knob = r.h() / 2 - s(3);
    circle(on ? r.r - r.h() / 2 : r.l + r.h() / 2, r.cy(), knob, on ? rgb(theme::kOnAccent) : rgb(theme::kText));
    if (ui.focus_id == id) stroke(r.inset(-s(3)), rgb(theme::kAccent, 0.9f), s(2), r.h() / 2 + s(3));
    return ui.clicked_id == id;
}

void Painter::logo(const Rect& r, float opacity)
{
    if (!logo_) {  // fallback mark: amber four-point star
        circle(r.cx(), r.cy(), std::min(r.w(), r.h()) * 0.18f, rgb(theme::kAccent, opacity));
        return;
    }
    rt_->DrawBitmap(logo_.Get(), r.d2d(), opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void Painter::console(const Rect& r, uint32_t status, bool screen_on)
{
    // Proportions loosely follow a docked Switch 2: console on top, dock below.
    const float w = r.w();
    const float unit = w / 100.0f;
    const float top = r.t + unit * 6;


    // Joy-Con 2 (left/right) and console body.
    const Rect body{r.l + unit * 12, top, r.r - unit * 12, top + unit * 42};
    const Rect jl{r.l + unit * 2, top + unit * 0.5f, r.l + unit * 13, top + unit * 41.5f};
    const Rect jr{r.r - unit * 13, top + unit * 0.5f, r.r - unit * 2, top + unit * 41.5f};
    fill(jl, rgb(theme::kNeonBlue), unit * 5);
    fill(Rect{jl.l + unit * 6, jl.t, jl.r, jl.b}, rgb(theme::kNeonBlue));
    fill(jr, rgb(theme::kNeonRed), unit * 5);
    fill(Rect{jr.l, jr.t, jr.r - unit * 6, jr.b}, rgb(theme::kNeonRed));
    fill(body, rgb(0x16181D));
    // Screen.
    const Rect screen = body.inset(unit * 2.2f);
    fill(screen, screen_on ? rgb(0x232831) : rgb(0x0C0D10), unit * 1.2f);
    if (screen_on) logo(Rect{screen.cx() - unit * 9, screen.cy() - unit * 9, screen.cx() + unit * 9, screen.cy() + unit * 9}, 0.9f);
    // Sticks and buttons.
    circle(jl.cx() + unit * 0.5f, jl.t + unit * 11, unit * 3.2f, rgb(0x1A1C21));
    circle(jr.cx() - unit * 0.5f, jr.t + unit * 26, unit * 3.2f, rgb(0x1A1C21));
    for (int i = 0; i < 4; ++i) {
        const float a = i * kPi / 2;
        circle(jr.cx() - unit * 0.5f + std::cos(a) * unit * 3.2f, jr.t + unit * 11 + std::sin(a) * unit * 3.2f,
               unit * 1.3f, rgb(0x1A1C21));
        circle(jl.cx() + unit * 0.5f + std::cos(a) * unit * 3.2f, jl.t + unit * 26 + std::sin(a) * unit * 3.2f,
               unit * 1.3f, rgb(0x1A1C21));
    }
    // Dock.
    const Rect dock{r.l + unit * 8, top + unit * 24, r.r - unit * 8, top + unit * 58};
    fill(dock, rgb(0x24272E), unit * 3);
    fill(Rect{dock.l, dock.t, dock.r, dock.t + unit * 4}, rgb(0x2C3038));
    stroke(dock, rgb(0x3A3F49), unit * 0.4f, unit * 3);
    // Status LED on the dock.
    circle(dock.l + unit * 6, dock.b - unit * 6, unit * 1.3f, rgb(status));
    circle(dock.l + unit * 6, dock.b - unit * 6, unit * 2.6f, rgb(status, 0.25f));
    (void)lerp;
}

}  // namespace hikari::ui
