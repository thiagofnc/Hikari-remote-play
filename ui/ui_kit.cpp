#include "hikari/ui_kit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hikari::ui {

namespace {
constexpr int kLogoResource = 101;  // RCDATA: assets/hikari_logo_256.png (app/hikari.rc)
constexpr float kPi = 3.14159265f;

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

void style_window(HWND hwnd)
{
    // dwmapi is loaded dynamically so the UI library adds no link dependency.
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    using SetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    if (auto set = reinterpret_cast<SetAttr>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")))) {
        const BOOL dark = TRUE;
        set(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        // Windows 11 only; older systems ignore these.
        const int round = 2 /* DWMWCP_ROUND */;
        set(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
        const COLORREF border = RGB(0x2B, 0x2D, 0x33), caption = RGB(0x0E, 0x0F, 0x11), caption_text = RGB(0xC9, 0xCC, 0xD1);
        set(hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
        set(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
        set(hwnd, 36 /* DWMWA_TEXT_COLOR */, &caption_text, sizeof(caption_text));
    }
    FreeLibrary(dwm);
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
    // Windows 11 ships Segoe UI Variable and Cascadia Mono; Windows 10 keeps the fallbacks.
    ComPtr<IDWriteFontCollection> fonts;
    if (SUCCEEDED(dwrite_->GetSystemFontCollection(&fonts))) {
        const auto has = [&](const wchar_t* name) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            return SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists;
        };
        if (has(L"Segoe UI Variable Text")) families_[0] = L"Segoe UI Variable Text";
        if (has(L"Segoe UI Variable Display")) families_[1] = L"Segoe UI Variable Display";
        if (has(L"Cascadia Mono")) families_[2] = L"Cascadia Mono";
    }
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

IDWriteTextFormat* Painter::format(float size, Weight w, Align h, Align v, bool wrap, int family)
{
    // Titles of 20 DIP and up use the Display optical size.
    if (family == 0 && size >= 20 * scale_) family = 1;
    const uint64_t key = (uint64_t(size * 4) << 24) | (uint64_t(family) << 20) | (uint64_t(w) << 8) | (uint64_t(h) << 4) |
                         (uint64_t(v) << 2) | wrap;
    auto& f = formats_[key];
    if (!f) {
        dwrite_->CreateTextFormat(families_[family].c_str(), nullptr, static_cast<DWRITE_FONT_WEIGHT>(w),
                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
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

void Painter::fill_gradient(const Rect& r, D2D1_COLOR_F top, D2D1_COLOR_F bottom, float radius)
{
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, top}, {1.0f, bottom}};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> gradient;
    if (FAILED(rt_->CreateGradientStopCollection(stops, 2, &collection)) ||
        FAILED(rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.l, r.t), D2D1::Point2F(r.l, r.b)),
                                              collection.Get(), &gradient))) {
        fill(r, bottom, radius);
        return;
    }
    if (radius > 0)
        rt_->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), gradient.Get());
    else
        rt_->FillRectangle(r.d2d(), gradient.Get());
}

void Painter::shadow(const Rect& r, float radius, float strength)
{
    // Stacked translucent layers approximate a soft, downward-offset blur.
    constexpr int kLayers = 6;
    for (int i = 1; i <= kLayers; ++i) {
        const float g = s(1.6f * float(i));
        const float dy = s(2.0f + 0.7f * float(i));
        fill(Rect{r.l - g, r.t - g + dy, r.r + g, r.b + g + dy}, rgb(0x000000, strength * (1.0f - float(i) / (kLayers + 1)) / 3.0f),
             radius + g);
    }
}

void Painter::card(const Rect& r, float radius)
{
    if (radius <= 0) radius = s(12);
    shadow(r, radius, 0.32f);
    fill(r, rgb(theme::kBg2), radius);
    stroke(r, rgb(0xFFFFFF, 0.065f), s(1), radius);
    // A faint top highlight gives the edge a lit, machined feel.
    line(r.l + radius, r.t + s(1.5f), r.r - radius, r.t + s(1.5f), rgb(0xFFFFFF, 0.035f), s(1));
}

void Painter::footer_bar(const Rect& r)
{
    fill(r, rgb(theme::kBg2));
    line(r.l, r.t + s(0.5f), r.r, r.t + s(0.5f), rgb(theme::kBorder), s(1));
}

void Painter::divider(float x0, float x1, float y) { line(x0, y, x1, y, rgb(0xFFFFFF, 0.06f), s(1)); }

void Painter::field(const Rect& r, bool focused)
{
    const float rad = s(8);
    if (focused) fill(r.inset(-s(3)), rgb(theme::kAccent, 0.16f), rad + s(3));
    fill(r, rgb(theme::kSurface), rad);
    stroke(r, focused ? rgb(theme::kAccent) : rgb(theme::kBorderStrong), s(1), rad);
}

void Painter::icon_tile(const Rect& r)
{
    const float rad = s(10);
    fill_gradient(r, rgb(0x25272C), rgb(0x1C1D21), rad);
    stroke(r, rgb(0xFFFFFF, 0.08f), s(1), rad);
}

void Painter::chevron_right(float cx, float cy, D2D1_COLOR_F c)
{
    line(cx - s(2.5f), cy - s(5), cx + s(2.5f), cy, c, s(1.6f));
    line(cx + s(2.5f), cy, cx - s(2.5f), cy + s(5), c, s(1.6f));
}

void Painter::status_icon(float cx, float cy, bool ok, bool pending)
{
    const float rad = s(10);
    if (pending) {
        circle(cx, cy, rad - s(0.5f), rgb(theme::kText3, 0.7f), false, s(1.2f));
        circle(cx, cy, s(1.6f), rgb(theme::kText3));
        return;
    }
    const uint32_t c = ok ? theme::kGood : theme::kWarn;
    circle(cx, cy, rad, rgb(c, 0.14f));
    circle(cx, cy, rad - s(0.5f), rgb(c, 0.38f), false, s(1));
    if (ok) {
        line(cx - s(4.2f), cy + s(0.2f), cx - s(1.2f), cy + s(3.2f), rgb(c), s(1.7f));
        line(cx - s(1.2f), cy + s(3.2f), cx + s(4.4f), cy - s(3.0f), rgb(c), s(1.7f));
    } else {
        line(cx, cy - s(4.5f), cx, cy + s(0.8f), rgb(c), s(1.8f));
        circle(cx, cy + s(3.6f), s(1.1f), rgb(c));
    }
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

void Painter::mono(const std::string& s8, const Rect& r, float size, D2D1_COLOR_F c, Align h)
{
    const std::wstring ws = wide(s8);
    rt_->DrawTextW(ws.c_str(), static_cast<UINT32>(ws.size()), format(size, Weight::Medium, h, Align::Center, false, 2), r.d2d(),
                   brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
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
    // Flat button colours, kept from the original graphite palette.
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
        bg = rgb(hover ? 0x303339 : 0x282A2F);
        fg = rgb(theme::kText);
        border = rgb(0x41444B);
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
    if (!label.empty()) text(label, rr, s(14), fg, Weight::SemiBold, Align::Center, Align::Center);
    return enabled && ui.clicked_id == id;
}

float Painter::pill(float x, float y, const std::string& label, uint32_t dot, bool right_align)
{
    const float size = s(12);
    const float w = text_width(label, size, Weight::Medium) + s(30);
    const float h = s(24);
    const float l = right_align ? x - w : x;
    Rect r{l, y, l + w, y + h};
    fill(r, rgb(dot, 0.11f), h / 2);
    stroke(r, rgb(dot, 0.30f), s(1), h / 2);
    circle(r.l + s(12), r.cy(), s(3), rgb(dot));
    text(label, Rect{r.l + s(21), r.t, r.r - s(6), r.b}, size, rgb(theme::kText), Weight::Medium);
    return w;
}

float Painter::keycap(float x, float cy, const std::string& label)
{
    const float size = s(11.5f);
    const float w = std::max(s(20), text_width(label, size, Weight::SemiBold) + s(12));
    const Rect k{x, cy - s(10), x + w, cy + s(9)};
    fill(Rect{k.l, k.t + s(2), k.r, k.b + s(2)}, rgb(0x050506), s(5));
    fill_gradient(k, rgb(0x34373E), rgb(0x2A2C32), s(5));
    stroke(k, rgb(0xFFFFFF, 0.09f), s(1), s(5));
    text(label, k, size, rgb(theme::kText), Weight::SemiBold, Align::Center, Align::Center);
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
    fill(r, on ? rgb(theme::kAccent) : rgb(hover ? 0x40434B : 0x33363D), r.h() / 2);
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

void Painter::console(const Rect& r, uint32_t status, ConsoleModel model)
{
    // Front elevation, in a 100 x 64 coordinate space. Dock edges stay
    // inside the controller rails so sticks and buttons remain visible.
    const float u = std::min(r.w()/100.0f, r.h()/64.0f);
    const float ox = r.cx()-50*u, oy = r.cy()-32*u;
    const auto box = [&](float l,float t,float right,float bottom) {
        return Rect{ox+l*u,oy+t*u,ox+right*u,oy+bottom*u};
    };
    const auto disk = [&](float x,float y,float radius,uint32_t color) {
        circle(ox+x*u,oy+y*u,radius*u,rgb(color));
    };
    // Move the complete console as one layer; the dock stays fixed below.
    D2D1_MATRIX_3X2_F original_transform;
    rt_->GetTransform(&original_transform);
    rt_->SetTransform(D2D1::Matrix3x2F::Translation(0,8*u)*original_transform);
    const bool second = model == ConsoleModel::Switch2;
    const uint32_t ink=0x15171B, cap=0x353A3F;
    const uint32_t left=second?0x444A50:0x00B8D2, right=second?0x444A50:0xFF5B54;
    const Rect jl=box(0,2,13,46), jr=box(87,2,100,46), tablet=box(13,2,87,46);
    fill(jl,rgb(left),u*7);
    fill(box(7,2,13,46),rgb(left));
    fill(jr,rgb(right),u*7);
    fill(box(87,2,93,46),rgb(right));
    fill(tablet,rgb(0x101216),u*0.8f);
    stroke(tablet,rgb(0x3A3E44),u*0.45f,u*0.8f);
    const Rect display=box(15,4,85,44);
    fill(display,rgb(0x090B0D),u*0.5f);

    const auto stick=[&](float x,float y,uint32_t ring) {
        disk(x,y,3.55f,ink);
        circle(ox+x*u,oy+y*u,3.25f*u,rgb(ring),false,u*0.5f);
        disk(x,y,2.65f,cap);
        circle(ox+x*u,oy+y*u,2.65f*u,rgb(0x50565C),false,u*0.25f);
    };
    const auto button=[&](float x,float y) {
        disk(x,y,1.45f,ink); disk(x,y,1.1f,cap);
    };
    stick(6.8f,13.0f,second?0x64BEDA:0x40484F);
    stick(93.2f,28.5f,second?0xF27965:0x40484F);
    // Right face diamond and left directional diamond are centred on each grip.
    for(const auto& point : {std::pair<float,float>{0,-3.1f},{3.1f,0},{0,3.1f},{-3.1f,0}}) {
        button(93.2f+point.first,14.5f+point.second);
        button(6.8f+point.first,27.0f+point.second);
    }
    // Minus, plus, capture, Home, and Switch 2's lower C button.
    fill(box(8.0f,6.2f,10.8f,7.0f),rgb(ink),u*0.2f);
    fill(box(89.1f,6.2f,92.0f,7.0f),rgb(ink),u*0.2f);
    fill(box(90.15f,5.15f,90.95f,8.05f),rgb(ink),u*0.2f);
    fill(box(8.0f,36.5f,10.8f,39.3f),rgb(ink),u*0.5f);
    disk(90.5f,37.8f,1.55f,ink);
    if(second) fill(box(89.1f,42.0f,91.9f,44.8f),rgb(ink),u*0.5f);

    rt_->SetTransform(original_transform);
    const Rect dock=box(14,23.5f,86,63);
    fill(dock,rgb(0x24272E),u*2);
    stroke(dock,rgb(0x42474F),u*0.4f,u*2);
    line(ox+17*u,oy+24.5f*u,ox+83*u,oy+24.5f*u,rgb(0x353A42),u*0.7f);
    // Flat indicator, without a decorative halo.
    disk(18,58.5f,0.9f,status);
}

}  // namespace hikari::ui
