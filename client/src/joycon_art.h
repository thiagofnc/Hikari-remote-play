#pragma once
// Original supplied artwork with aligned interactive control regions.
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdint>
#include "input_mapping.h"

namespace hikari::joycon {
struct ArtState {
    uint32_t active = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;
    float lt = 0, rt = 0;
    int hover = -1, target = -1;
    float pulse = 0;
    int flash = -1;
    float flash_amount = 0;
};
class JoyConArt {
public:
    bool create(ID2D1Factory* factory);
    bool create_device(ID2D1RenderTarget* rt);
    void release_device();
    void draw(ID2D1RenderTarget* rt, const D2D1_RECT_F& area, const ArtState& state, IDWriteTextFormat* label);
    int hit_test(float x, float y) const;
private:
    Microsoft::WRL::ComPtr<IWICFormatConverter> source_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> shoulder_[2], trigger_[2];
    float scale_ = 1, ox_ = 0, oy_ = 0;
};
}
