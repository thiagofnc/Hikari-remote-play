#include "joycon_art.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace hikari::joycon {

using Microsoft::WRL::ComPtr;

namespace {

// Design space (arbitrary units): two Joy-Cons 150 x 390 with a 26 unit gap,
// shoulder buttons rising above y = 0.
constexpr float kW = 150.0f, kH = 390.0f, kGap = 26.0f;
constexpr float kTotalW = kW * 2 + kGap;
constexpr float kR = 64.0f;  // outer corner radius
constexpr float kPi = 3.14159265f;
constexpr D2D1_RECT_F kCanvas = {-34.0f, -58.0f, kTotalW + 34.0f, kH + 22.0f};

constexpr uint32_t kAmber = 0xFBBB04;
constexpr uint32_t kButton = 0x24282E;
constexpr uint32_t kLabel = 0xD6DAE0;
constexpr uint32_t kInk = 0x1E2127;

constexpr int in(SwitchInput s) { return static_cast<int>(s); }

// Element positions; the right Joy-Con mirrors the left around the centre line.
constexpr float mx(float x) { return kTotalW - x; }
constexpr D2D1_POINT_2F kMinus = {118, 30}, kPlus = {mx(118), 30};
constexpr D2D1_POINT_2F kLStick = {78, 104}, kFace = {mx(78), 104};
constexpr D2D1_POINT_2F kDpad = {78, 216}, kRStick = {mx(78), 216};
constexpr D2D1_POINT_2F kCapture = {106, 304}, kHome = {mx(106), 304};
constexpr float kFaceR = 15.0f, kFaceD = 31.0f;
constexpr float kWellR = 36.0f, kCapR = 27.0f, kTravel = 9.0f, kChevronR = 47.0f;

D2D1_COLOR_F col(uint32_t rgb, float a = 1.0f) { return D2D1::ColorF(rgb, a); }

// Builds mirrored paths: x is flipped and arc sweeps reverse on the right side.
struct PathBuilder {
    ID2D1GeometrySink* sink;
    bool mirror;
    D2D1_POINT_2F p(float x, float y) const { return {mirror ? mx(x) : x, y}; }
    D2D1_POINT_2F polar(float r, float deg) const
    {
        const float a = deg * kPi / 180.0f;
        return p(kR + r * std::cos(a), kR + r * std::sin(a));
    }
    void begin(D2D1_POINT_2F at) { sink->BeginFigure(at, D2D1_FIGURE_BEGIN_FILLED); }
    void line(D2D1_POINT_2F to) { sink->AddLine(to); }
    void arc(D2D1_POINT_2F to, float r, bool clockwise)
    {
        const bool cw = clockwise != mirror;
        sink->AddArc(D2D1::ArcSegment(to, D2D1::SizeF(r, r), 0.0f,
                                      cw ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL));
    }
    void end() { sink->EndFigure(D2D1_FIGURE_END_CLOSED); }
};

template <typename F>
ComPtr<ID2D1PathGeometry> make_path(ID2D1Factory* f, F&& build)
{
    ComPtr<ID2D1PathGeometry> g;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(&g)) || FAILED(g->Open(&sink))) return nullptr;
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    build(sink.Get());
    if (FAILED(sink->Close())) return nullptr;
    return g;
}

D2D1::Matrix3x2F mat(const D2D1_MATRIX_3X2_F& m) { return D2D1::Matrix3x2F(m._11, m._12, m._21, m._22, m._31, m._32); }

float dist(D2D1_POINT_2F a, D2D1_POINT_2F b) { return std::hypot(a.x - b.x, a.y - b.y); }

D2D1_POINT_2F offset(D2D1_POINT_2F c, float dx, float dy) { return {c.x + dx, c.y + dy}; }

}  // namespace

bool JoyConArt::create(ID2D1Factory* factory)
{
    factory_ = factory;
    for (int side = 0; side < 2; ++side) {
        const bool m = side == 1;
        // Body: rounded rail on the inner edge, big curved corners outside.
        body_[side] = make_path(factory, [&](ID2D1GeometrySink* s) {
            PathBuilder b{s, m};
            const float r = 9.0f;
            b.begin(b.p(kR, 0));
            b.line(b.p(kW - r, 0));
            b.arc(b.p(kW, r), r, true);
            b.line(b.p(kW, kH - r));
            b.arc(b.p(kW - r, kH), r, true);
            b.line(b.p(kR, kH));
            b.arc(b.p(0, kH - kR), kR, true);
            b.line(b.p(0, kR));
            b.arc(b.p(kR, 0), kR, true);
            b.end();
        });
        // L / R: a band hugging the top outer corner.
        shoulder_[side] = make_path(factory, [&](ID2D1GeometrySink* s) {
            PathBuilder b{s, m};
            b.begin(b.polar(68, 200));
            b.arc(b.p(kR, -4), 68, true);
            b.line(b.p(120, -4));
            b.arc(b.p(120, -18), 7, false);
            b.line(b.p(kR, -18));
            b.arc(b.polar(82, 200), 82, false);
            b.end();
        });
        // ZL / ZR: the larger trigger behind it.
        trigger_[side] = make_path(factory, [&](ID2D1GeometrySink* s) {
            PathBuilder b{s, m};
            b.begin(b.polar(88, 222));
            b.arc(b.p(kR, -24), 88, true);
            b.line(b.p(108, -24));
            b.arc(b.p(108, -42), 9, false);
            b.line(b.p(kR, -42));
            b.arc(b.polar(106, 222), 106, false);
            b.end();
        });
    }
    // Unit triangle pointing up (rotated for d-pad arrows and stick chevrons).
    triangle_ = make_path(factory, [](ID2D1GeometrySink* s) {
        s->BeginFigure({0.0f, -0.62f}, D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine({0.58f, 0.38f});
        s->AddLine({-0.58f, 0.38f});
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
    });
    house_ = make_path(factory, [](ID2D1GeometrySink* s) {
        s->BeginFigure({0.0f, -0.6f}, D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine({0.6f, -0.05f});
        s->AddLine({0.4f, -0.05f});
        s->AddLine({0.4f, 0.5f});
        s->AddLine({-0.4f, 0.5f});
        s->AddLine({-0.4f, -0.05f});
        s->AddLine({-0.6f, -0.05f});
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
    });
    for (int i = 0; i < 2; ++i)
        if (!body_[i] || !shoulder_[i] || !trigger_[i]) return false;
    return triangle_ && house_;
}

bool JoyConArt::create_device(ID2D1RenderTarget* rt)
{
    release_device();
    if (FAILED(rt->CreateSolidColorBrush(col(0xFFFFFF), &brush_))) return false;

    auto linear = [&](uint32_t a, uint32_t b, float alpha_a, float alpha_b, ComPtr<ID2D1LinearGradientBrush>& out) {
        const D2D1_GRADIENT_STOP stops[] = {{0.0f, col(a, alpha_a)}, {1.0f, col(b, alpha_b)}};
        ComPtr<ID2D1GradientStopCollection> coll;
        if (FAILED(rt->CreateGradientStopCollection(stops, 2, &coll))) return false;
        return SUCCEEDED(rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties({0, 0}, {kW, kH}), coll.Get(), &out));
    };
    // Neon blue / neon red, lit from the top left.
    if (!linear(0x35D8F2, 0x0090B6, 1, 1, body_brush_[0])) return false;
    if (!linear(0xFF7B6F, 0xDC4136, 1, 1, body_brush_[1])) return false;
    if (!linear(0xFFFFFF, 0xFFFFFF, 0.28f, 0.0f, gloss_)) return false;

    const D2D1_GRADIENT_STOP gs[] = {{0.0f, col(kAmber, 0.85f)}, {0.45f, col(kAmber, 0.35f)}, {1.0f, col(kAmber, 0.0f)}};
    ComPtr<ID2D1GradientStopCollection> gcoll;
    if (FAILED(rt->CreateGradientStopCollection(gs, 3, &gcoll))) return false;
    return SUCCEEDED(rt->CreateRadialGradientBrush(
        D2D1::RadialGradientBrushProperties({0, 0}, {0, 0}, 10, 10), gcoll.Get(), &glow_));
}

void JoyConArt::release_device()
{
    brush_.Reset();
    body_brush_[0].Reset();
    body_brush_[1].Reset();
    gloss_.Reset();
    glow_.Reset();
}

float JoyConArt::emphasis(int input, const ArtState& s, bool* is_target) const
{
    *is_target = input == s.target;
    if (*is_target) return 0.55f + 0.45f * s.pulse;
    float e = 0.0f;
    if (input == s.hover) e = 0.75f;
    if (input == s.flash) e = std::max(e, s.flash_amount);
    return e;
}

void JoyConArt::draw_glow(ID2D1RenderTarget* rt, D2D1_POINT_2F c, float radius, float opacity)
{
    glow_->SetCenter(c);
    glow_->SetRadiusX(radius);
    glow_->SetRadiusY(radius);
    glow_->SetOpacity(std::clamp(opacity, 0.0f, 1.0f));
    rt->FillEllipse(D2D1::Ellipse(c, radius, radius), glow_.Get());
}

void JoyConArt::draw_triangle(ID2D1RenderTarget* rt, D2D1_POINT_2F c, float size, float angle_deg,
                              const D2D1_MATRIX_3X2_F& base, const D2D1_COLOR_F& color)
{
    rt->SetTransform(D2D1::Matrix3x2F::Scale(size, size) * D2D1::Matrix3x2F::Rotation(angle_deg) *
                     D2D1::Matrix3x2F::Translation(c.x, c.y) * mat(base));
    brush_->SetColor(color);
    rt->FillGeometry(triangle_.Get(), brush_.Get());
    rt->SetTransform(base);
}

void JoyConArt::draw_shadowed_body(ID2D1RenderTarget* rt, int side, const D2D1_MATRIX_3X2_F& base)
{
    ID2D1PathGeometry* g = body_[side].Get();
    // Soft drop shadow: a few widening translucent strokes under an offset copy.
    rt->SetTransform(D2D1::Matrix3x2F::Translation(0, 12) * mat(base));
    const float widths[] = {26, 16, 8};
    for (float w : widths) {
        brush_->SetColor(col(0x000000, 0.07f));
        rt->DrawGeometry(g, brush_.Get(), w);
    }
    brush_->SetColor(col(0x000000, 0.30f));
    rt->FillGeometry(g, brush_.Get());
    rt->SetTransform(base);

    const float x0 = side == 0 ? 0.0f : kW + kGap;
    body_brush_[side]->SetStartPoint({x0, 0});
    body_brush_[side]->SetEndPoint({x0 + kW, kH});
    rt->FillGeometry(g, body_brush_[side].Get());

    gloss_->SetStartPoint({x0 + (side == 0 ? 0.0f : kW), 0});
    gloss_->SetEndPoint({x0 + kW * 0.5f, kH * 0.45f});
    rt->FillGeometry(g, gloss_.Get());

    brush_->SetColor(col(0x000000, 0.35f));
    rt->DrawGeometry(g, brush_.Get(), 1.2f);

    // Dark slider rail along the inner edge.
    const float rx = side == 0 ? kW - 8.0f : kW + kGap + 2.0f;
    brush_->SetColor(col(0x000000, 0.30f));
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rx, 14, rx + 6, kH - 14), 3, 3), brush_.Get());
}

void JoyConArt::draw_shoulder(ID2D1RenderTarget* rt, ID2D1Geometry* g, int input, float fill, const wchar_t* text,
                              D2D1_POINT_2F label_at, const ArtState& s, IDWriteTextFormat* label)
{
    const bool pressed = (s.active >> input) & 1u;
    bool target = false;
    const float em = emphasis(input, s, &target);

    if (pressed || target) {
        brush_->SetColor(col(kAmber, pressed ? 0.30f : 0.20f + 0.25f * s.pulse));
        rt->DrawGeometry(g, brush_.Get(), 10.0f);
    }
    brush_->SetColor(col(0x3A3F47));
    rt->FillGeometry(g, brush_.Get());

    D2D1_RECT_F b{};
    g->GetBounds(nullptr, &b);
    fill = pressed ? 1.0f : std::clamp(fill, 0.0f, 1.0f);
    if (fill > 0.0f) {
        // Analog travel fills from the outer tip towards the centre.
        const bool left = b.left < kTotalW * 0.5f;
        const float w = (b.right - b.left) * fill;
        const D2D1_RECT_F clip = left ? D2D1::RectF(b.left, b.top, b.left + w, b.bottom)
                                      : D2D1::RectF(b.right - w, b.top, b.right, b.bottom);
        rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        brush_->SetColor(col(kAmber, pressed ? 1.0f : 0.85f));
        rt->FillGeometry(g, brush_.Get());
        rt->PopAxisAlignedClip();
    }
    brush_->SetColor(col(0x000000, 0.45f));
    rt->DrawGeometry(g, brush_.Get(), 1.0f);
    if (em > 0.0f) {
        brush_->SetColor(col(kAmber, em));
        rt->DrawGeometry(g, brush_.Get(), 2.2f);
    }

    label->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    label->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    brush_->SetColor(fill > 0.5f ? col(kInk) : col(0xC4C9D0));
    rt->DrawText(text, static_cast<UINT32>(wcslen(text)), label,
                 D2D1::RectF(label_at.x - 20, label_at.y - 10, label_at.x + 20, label_at.y + 10), brush_.Get());
}

void JoyConArt::draw_round_button(ID2D1RenderTarget* rt, D2D1_POINT_2F c, float r, int input, const wchar_t* text,
                                  float arrow_angle, const ArtState& s, IDWriteTextFormat* label)
{
    const bool pressed = (s.active >> input) & 1u;
    bool target = false;
    const float em = emphasis(input, s, &target);
    const D2D1_MATRIX_3X2_F base = [&] {
        D2D1_MATRIX_3X2_F m;
        rt->GetTransform(&m);
        return m;
    }();

    if (target) draw_glow(rt, c, r * 2.6f, 0.30f + 0.45f * s.pulse);
    if (pressed) draw_glow(rt, c, r * 2.4f, 0.85f);

    brush_->SetColor(col(0x000000, 0.40f));
    rt->FillEllipse(D2D1::Ellipse(offset(c, 0, 1.6f), r, r), brush_.Get());
    brush_->SetColor(pressed ? col(kAmber) : col(kButton));
    rt->FillEllipse(D2D1::Ellipse(c, r, r), brush_.Get());
    if (!pressed) {
        brush_->SetColor(col(0xFFFFFF, 0.07f));
        rt->DrawEllipse(D2D1::Ellipse(c, r - 0.8f, r - 0.8f), brush_.Get(), 1.2f);
    }

    const D2D1_COLOR_F ink = pressed ? col(kInk) : col(kLabel);
    if (arrow_angle >= 0.0f) {
        draw_triangle(rt, c, r * 0.62f, arrow_angle, base, ink);
    } else if (text) {
        label->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        label->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        brush_->SetColor(ink);
        rt->DrawText(text, static_cast<UINT32>(wcslen(text)), label, D2D1::RectF(c.x - r, c.y - r, c.x + r, c.y + r),
                     brush_.Get());
    }
    if (em > 0.0f) {
        brush_->SetColor(col(kAmber, em));
        rt->DrawEllipse(D2D1::Ellipse(c, r + 3.2f, r + 3.2f), brush_.Get(), 2.0f);
    }
}

void JoyConArt::draw_stick(ID2D1RenderTarget* rt, D2D1_POINT_2F c, float x, float y, int click, int dir_base,
                           const ArtState& s, const D2D1_MATRIX_3X2_F& base)
{
    // Well.
    brush_->SetColor(col(0x101215));
    rt->FillEllipse(D2D1::Ellipse(c, kWellR, kWellR), brush_.Get());
    brush_->SetColor(col(0x000000, 0.45f));
    rt->DrawEllipse(D2D1::Ellipse(c, kWellR, kWellR), brush_.Get(), 1.5f);

    // Direction chevrons (up, down, left, right — SwitchInput order).
    static const float kAngles[4] = {0, 180, 270, 90};
    static const float kDx[4] = {0, 0, -1, 1}, kDy[4] = {-1, 1, 0, 0};
    for (int d = 0; d < 4; ++d) {
        const int input = dir_base + d;
        const bool on = (s.active >> input) & 1u;
        bool target = false;
        const float em = emphasis(input, s, &target);
        const D2D1_POINT_2F p = offset(c, kDx[d] * kChevronR, kDy[d] * kChevronR);
        if (target) draw_glow(rt, p, 15.0f, 0.35f + 0.5f * s.pulse);
        else if (on) draw_glow(rt, p, 12.0f, 0.6f);
        D2D1_COLOR_F color = col(0x000000, 0.30f);
        if (on) color = col(kAmber);
        else if (em > 0.0f) color = col(kAmber, std::max(0.6f, em));
        draw_triangle(rt, p, 9.0f, kAngles[d], base, color);
    }

    // Cap, displaced by the live stick position.
    const bool pressed = (s.active >> click) & 1u;
    bool target = false;
    const float em = emphasis(click, s, &target);
    const D2D1_POINT_2F cap = offset(c, std::clamp(x, -1.0f, 1.0f) * kTravel, -std::clamp(y, -1.0f, 1.0f) * kTravel);
    if (target) draw_glow(rt, cap, kCapR * 2.0f, 0.30f + 0.45f * s.pulse);
    if (pressed) draw_glow(rt, cap, kCapR * 1.9f, 0.8f);
    brush_->SetColor(col(0x000000, 0.5f));
    rt->FillEllipse(D2D1::Ellipse(offset(cap, 0, 2.5f), kCapR, kCapR), brush_.Get());
    brush_->SetColor(pressed ? col(kAmber) : col(0x2D3239));
    rt->FillEllipse(D2D1::Ellipse(cap, kCapR, kCapR), brush_.Get());
    brush_->SetColor(col(0xFFFFFF, pressed ? 0.25f : 0.08f));
    rt->DrawEllipse(D2D1::Ellipse(cap, kCapR - 1.2f, kCapR - 1.2f), brush_.Get(), 1.4f);
    brush_->SetColor(pressed ? col(0xC08F00) : col(0x22262B));
    rt->DrawEllipse(D2D1::Ellipse(cap, kCapR * 0.6f, kCapR * 0.6f), brush_.Get(), 2.5f);
    if (em > 0.0f) {
        brush_->SetColor(col(kAmber, em));
        rt->DrawEllipse(D2D1::Ellipse(cap, kCapR + 3.0f, kCapR + 3.0f), brush_.Get(), 2.0f);
    }
}

void JoyConArt::draw(ID2D1RenderTarget* rt, const D2D1_RECT_F& area, const ArtState& s, IDWriteTextFormat* label)
{
    if (!brush_ && !create_device(rt)) return;
    const float cw = kCanvas.right - kCanvas.left, ch = kCanvas.bottom - kCanvas.top;
    const float aw = area.right - area.left, ah = area.bottom - area.top;
    if (aw <= 1 || ah <= 1) return;
    fit_.scale = std::min(aw / cw, ah / ch);
    fit_.ox = area.left + (aw - cw * fit_.scale) * 0.5f - kCanvas.left * fit_.scale;
    fit_.oy = area.top + (ah - ch * fit_.scale) * 0.5f - kCanvas.top * fit_.scale;

    D2D1_MATRIX_3X2_F old;
    rt->GetTransform(&old);
    const D2D1_MATRIX_3X2_F base = D2D1::Matrix3x2F::Scale(fit_.scale, fit_.scale) *
                                   D2D1::Matrix3x2F::Translation(fit_.ox, fit_.oy) * mat(old);
    rt->SetTransform(base);

    // Shoulders sit behind the bodies.
    draw_shoulder(rt, trigger_[0].Get(), in(SwitchInput::ZL), s.lt, L"ZL", {86, -33}, s, label);
    draw_shoulder(rt, trigger_[1].Get(), in(SwitchInput::ZR), s.rt, L"ZR", {mx(86), -33}, s, label);
    draw_shoulder(rt, shoulder_[0].Get(), in(SwitchInput::L), 0, L"L", {96, -11}, s, label);
    draw_shoulder(rt, shoulder_[1].Get(), in(SwitchInput::R), 0, L"R", {mx(96), -11}, s, label);

    draw_shadowed_body(rt, 0, base);
    draw_shadowed_body(rt, 1, base);

    // Minus / Plus.
    auto small_button = [&](D2D1_POINT_2F c, int input, bool plus) {
        const bool pressed = (s.active >> input) & 1u;
        bool target = false;
        const float em = emphasis(input, s, &target);
        if (target) draw_glow(rt, c, 22, 0.3f + 0.45f * s.pulse);
        if (pressed) draw_glow(rt, c, 20, 0.85f);
        brush_->SetColor(pressed ? col(kAmber) : col(kButton));
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.x - 9, c.y - 2.6f, c.x + 9, c.y + 2.6f), 1.5f, 1.5f),
                                 brush_.Get());
        if (plus)
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(c.x - 2.6f, c.y - 9, c.x + 2.6f, c.y + 9), 1.5f, 1.5f), brush_.Get());
        if (em > 0.0f) {
            brush_->SetColor(col(kAmber, em));
            rt->DrawEllipse(D2D1::Ellipse(c, 14, 14), brush_.Get(), 2.0f);
        }
    };
    small_button(kMinus, in(SwitchInput::Minus), false);
    small_button(kPlus, in(SwitchInput::Plus), true);

    // Face buttons: X top, B bottom, Y left, A right.
    draw_round_button(rt, offset(kFace, 0, -kFaceD), kFaceR, in(SwitchInput::X), L"X", -1, s, label);
    draw_round_button(rt, offset(kFace, 0, kFaceD), kFaceR, in(SwitchInput::B), L"B", -1, s, label);
    draw_round_button(rt, offset(kFace, -kFaceD, 0), kFaceR, in(SwitchInput::Y), L"Y", -1, s, label);
    draw_round_button(rt, offset(kFace, kFaceD, 0), kFaceR, in(SwitchInput::A), L"A", -1, s, label);
    // D-pad buttons with arrows.
    draw_round_button(rt, offset(kDpad, 0, -kFaceD), kFaceR, in(SwitchInput::DpadUp), nullptr, 0, s, label);
    draw_round_button(rt, offset(kDpad, 0, kFaceD), kFaceR, in(SwitchInput::DpadDown), nullptr, 180, s, label);
    draw_round_button(rt, offset(kDpad, -kFaceD, 0), kFaceR, in(SwitchInput::DpadLeft), nullptr, 270, s, label);
    draw_round_button(rt, offset(kDpad, kFaceD, 0), kFaceR, in(SwitchInput::DpadRight), nullptr, 90, s, label);

    draw_stick(rt, kLStick, s.lx, s.ly, in(SwitchInput::LStickClick), in(SwitchInput::LStickUp), s, base);
    draw_stick(rt, kRStick, s.rx, s.ry, in(SwitchInput::RStickClick), in(SwitchInput::RStickUp), s, base);

    // Capture: rounded square with a recessed circle.
    {
        const int input = in(SwitchInput::Capture);
        const bool pressed = (s.active >> input) & 1u;
        bool target = false;
        const float em = emphasis(input, s, &target);
        if (target) draw_glow(rt, kCapture, 26, 0.3f + 0.45f * s.pulse);
        if (pressed) draw_glow(rt, kCapture, 24, 0.85f);
        const D2D1_RECT_F r = D2D1::RectF(kCapture.x - 10, kCapture.y - 10, kCapture.x + 10, kCapture.y + 10);
        brush_->SetColor(col(0x000000, 0.4f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, r.top + 1.5f, r.right, r.bottom + 1.5f), 4, 4),
                                 brush_.Get());
        brush_->SetColor(pressed ? col(kAmber) : col(kButton));
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, 4, 4), brush_.Get());
        brush_->SetColor(pressed ? col(0xC08F00) : col(0x3B4048));
        rt->FillEllipse(D2D1::Ellipse(kCapture, 5, 5), brush_.Get());
        if (em > 0.0f) {
            brush_->SetColor(col(kAmber, em));
            rt->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(r.left - 3.5f, r.top - 3.5f, r.right + 3.5f, r.bottom + 3.5f), 6, 6),
                brush_.Get(), 2.0f);
        }
    }
    // Home: ringed circle with a house glyph.
    {
        const int input = in(SwitchInput::Home);
        const bool pressed = (s.active >> input) & 1u;
        bool target = false;
        const float em = emphasis(input, s, &target);
        if (target) draw_glow(rt, kHome, 30, 0.3f + 0.45f * s.pulse);
        if (pressed) draw_glow(rt, kHome, 28, 0.85f);
        brush_->SetColor(col(0x000000, 0.4f));
        rt->FillEllipse(D2D1::Ellipse(offset(kHome, 0, 1.6f), 13, 13), brush_.Get());
        brush_->SetColor(pressed ? col(kAmber) : col(kButton));
        rt->FillEllipse(D2D1::Ellipse(kHome, 13, 13), brush_.Get());
        brush_->SetColor(pressed ? col(0xC08F00) : col(0x5A6069));
        rt->DrawEllipse(D2D1::Ellipse(kHome, 10.5f, 10.5f), brush_.Get(), 1.6f);
        rt->SetTransform(D2D1::Matrix3x2F::Scale(11, 11) * D2D1::Matrix3x2F::Translation(kHome.x, kHome.y) *
                         mat(base));
        brush_->SetColor(pressed ? col(kInk) : col(kLabel));
        rt->FillGeometry(house_.Get(), brush_.Get());
        rt->SetTransform(base);
        if (em > 0.0f) {
            brush_->SetColor(col(kAmber, em));
            rt->DrawEllipse(D2D1::Ellipse(kHome, 16.5f, 16.5f), brush_.Get(), 2.0f);
        }
    }

    rt->SetTransform(old);
}

int JoyConArt::hit_test(float px, float py) const
{
    if (fit_.scale <= 0) return -1;
    const D2D1_POINT_2F p = {(px - fit_.ox) / fit_.scale, (py - fit_.oy) / fit_.scale};

    struct Circle {
        D2D1_POINT_2F c;
        float r;
        SwitchInput s;
    };
    const Circle circles[] = {
        {offset(kFace, 0, -kFaceD), kFaceR + 3, SwitchInput::X},
        {offset(kFace, 0, kFaceD), kFaceR + 3, SwitchInput::B},
        {offset(kFace, -kFaceD, 0), kFaceR + 3, SwitchInput::Y},
        {offset(kFace, kFaceD, 0), kFaceR + 3, SwitchInput::A},
        {offset(kDpad, 0, -kFaceD), kFaceR + 3, SwitchInput::DpadUp},
        {offset(kDpad, 0, kFaceD), kFaceR + 3, SwitchInput::DpadDown},
        {offset(kDpad, -kFaceD, 0), kFaceR + 3, SwitchInput::DpadLeft},
        {offset(kDpad, kFaceD, 0), kFaceR + 3, SwitchInput::DpadRight},
        {kMinus, 14, SwitchInput::Minus},
        {kPlus, 14, SwitchInput::Plus},
        {kCapture, 15, SwitchInput::Capture},
        {kHome, 16, SwitchInput::Home},
    };
    for (const auto& c : circles)
        if (dist(p, c.c) <= c.r) return in(c.s);

    // Sticks: the cap clicks the stick, the ring around it picks a direction.
    const struct {
        D2D1_POINT_2F c;
        SwitchInput click, up;
    } sticks[] = {{kLStick, SwitchInput::LStickClick, SwitchInput::LStickUp},
                  {kRStick, SwitchInput::RStickClick, SwitchInput::RStickUp}};
    for (const auto& st : sticks) {
        const float d = dist(p, st.c);
        if (d > kChevronR + 9) continue;
        if (d <= kCapR * 0.75f) return in(st.click);
        const float dx = p.x - st.c.x, dy = p.y - st.c.y;
        int dir;  // up, down, left, right
        if (std::fabs(dx) > std::fabs(dy)) dir = dx > 0 ? 3 : 2;
        else dir = dy < 0 ? 0 : 1;
        return in(st.up) + dir;
    }

    const struct {
        ID2D1PathGeometry* g;
        SwitchInput s;
    } shapes[] = {{shoulder_[0].Get(), SwitchInput::L},
                  {shoulder_[1].Get(), SwitchInput::R},
                  {trigger_[0].Get(), SwitchInput::ZL},
                  {trigger_[1].Get(), SwitchInput::ZR}};
    for (const auto& sh : shapes) {
        BOOL inside = FALSE;
        if (sh.g && SUCCEEDED(sh.g->FillContainsPoint(p, nullptr, 2.0f, &inside)) && inside) return in(sh.s);
    }
    return -1;
}

}  // namespace hikari::joycon
