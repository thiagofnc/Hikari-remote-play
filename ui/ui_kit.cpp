#include "hikari/ui_kit.h"

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

void Painter::console(const Rect& r, uint32_t status, bool screen_on, ConsoleModel model)
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
    fill(display,rgb(screen_on?0x252930:0x090B0D),u*0.5f);
    // The taller dock deliberately covers the lower part of the display-centered mark.
    if(screen_on) logo(box(37,11,63,37));

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

    const Rect dock=box(14,23.5f,86,63);
    fill(dock,rgb(0x24272E),u*2);
    stroke(dock,rgb(0x42474F),u*0.4f,u*2);
    line(ox+17*u,oy+24.5f*u,ox+83*u,oy+24.5f*u,rgb(0x353A42),u*0.7f);
    // Flat indicator, without a decorative halo.
    disk(18,58.5f,0.9f,status);
}

}  // namespace hikari::ui
