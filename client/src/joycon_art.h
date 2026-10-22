#pragma once
// Vector drawing of a Joy-Con pair for the Controller Settings window.
//
// Everything is built from Direct2D path geometries in a fixed design space
// and scaled into the target rect at draw time, so it stays crisp at any DPI
// and hit-testing uses the exact shapes that were drawn.

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstdint>

#include "input_mapping.h"

namespace hikari::joycon {

struct ArtState {
    uint32_t active = 0;                   // bit i = SwitchInput i currently pressed (live preview)
    float lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1, +Y up
    float lt = 0, rt = 0;                  // 0..1 trigger travel
    int hover = -1;                        // SwitchInput under the mouse / focused row
    int target = -1;                       // SwitchInput being rebound (pulsing glow)
    float pulse = 0;                       // 0..1 animation phase
    int flash = -1;                        // SwitchInput that was just (re)bound
    float flash_amount = 0;                // 0..1, fades out
};

class JoyConArt {
public:
    bool create(ID2D1Factory* factory);
    bool create_device(ID2D1RenderTarget* rt);
    void release_device();

    // Draws into `area` (DIPs, current render-target transform must be
    // identity) preserving aspect ratio; remembers the fit for hit_test().
    void draw(ID2D1RenderTarget* rt, const D2D1_RECT_F& area, const ArtState& s, IDWriteTextFormat* label);
    // SwitchInput index at a DIP position from the last draw(), -1 if none.
    int hit_test(float x, float y) const;

private:
    struct Fit {
        float scale = 1, ox = 0, oy = 0;
    };
    void draw_shadowed_body(ID2D1RenderTarget* rt, int side, const D2D1_MATRIX_3X2_F& base);
    void draw_shoulder(ID2D1RenderTarget* rt, ID2D1Geometry* g, int input, float fill, const wchar_t* text,
                       D2D1_POINT_2F label_at, const ArtState& s, IDWriteTextFormat* label);
    void draw_round_button(ID2D1RenderTarget* rt, D2D1_POINT_2F c, float r, int input, const wchar_t* text,
                           float arrow_angle, const ArtState& s, IDWriteTextFormat* label);
    void draw_stick(ID2D1RenderTarget* rt, D2D1_POINT_2F c, float x, float y, int click, int dir_base, const ArtState& s,
                    const D2D1_MATRIX_3X2_F& base);
    void draw_glow(ID2D1RenderTarget* rt, D2D1_POINT_2F c, float radius, float opacity);
    void draw_triangle(ID2D1RenderTarget* rt, D2D1_POINT_2F c, float size, float angle_deg,
                       const D2D1_MATRIX_3X2_F& base, const D2D1_COLOR_F& color);
    float emphasis(int input, const ArtState& s, bool* is_target) const;

    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> body_[2], shoulder_[2], trigger_[2], triangle_, house_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> body_brush_[2], gloss_;
    Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> glow_;
    Fit fit_;
};

}  // namespace hikari::joycon
