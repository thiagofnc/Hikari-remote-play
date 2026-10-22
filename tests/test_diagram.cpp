// Render the embedded reference artwork without a GPU or a visible window,
// then check reference-aligned input selection at common DPI scales.
#include "joycon_art.h"
#include "test_framework.h"
#include <array>

using namespace hikari;
using Microsoft::WRL::ComPtr;

TEST(controller_reference_renders_and_selects_all_inputs)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CHECK(SUCCEEDED(com));
    if (FAILED(com)) return;
    {
        ComPtr<ID2D1Factory> factory;
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICBitmap> surface;
        ComPtr<ID2D1RenderTarget> target;
        ComPtr<IDWriteFactory> dwrite;
        ComPtr<IDWriteTextFormat> label;
        CHECK(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())));
        CHECK(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))));
        if (!factory || !wic) { CoUninitialize(); return; }
        CHECK(SUCCEEDED(wic->CreateBitmap(760,1190,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,&surface)));
        CHECK(SUCCEEDED(factory->CreateWicBitmapRenderTarget(surface.Get(),D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE),&target)));
        CHECK(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()))));
        if (!target || !dwrite) { CoUninitialize(); return; }
        dwrite->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,12,L"en-us",&label);
        joycon::JoyConArt art;
        CHECK(art.create(factory.Get()));
        CHECK(art.create_device(target.Get()));
        struct Sample { float x,y; SwitchInput input; };
        // Coordinates measured from the supplied front and top views.
        const Sample samples[] = {
            {301,113,SwitchInput::A},{270,144,SwitchInput::B},{270,82,SwitchInput::X},{239,113,SwitchInput::Y},
            {74,202,SwitchInput::DpadUp},{74,264,SwitchInput::DpadDown},{43,233,SwitchInput::DpadLeft},{105,233,SwitchInput::DpadRight},
            {115,48,SwitchInput::Minus},{227,48,SwitchInput::Plus},{96,313,SwitchInput::Capture},{247,307,SwitchInput::Home},
            {74,115,SwitchInput::LStickClick},{270,229,SwitchInput::RStickClick},
            {74,73,SwitchInput::LStickUp},{74,157,SwitchInput::LStickDown},{32,115,SwitchInput::LStickLeft},{116,115,SwitchInput::LStickRight},
            {270,187,SwitchInput::RStickUp},{270,271,SwitchInput::RStickDown},{228,229,SwitchInput::RStickLeft},{312,229,SwitchInput::RStickRight},
            {65,-72,SwitchInput::L},{279,-72,SwitchInput::R},{65,-117,SwitchInput::ZL},{279,-117,SwitchInput::ZR},
        };
        for (float scale : {1.0f,1.5f,2.0f}) {
            target->BeginDraw();
            target->Clear(D2D1::ColorF(0,0));
            art.draw(target.Get(),{0,0,380*scale,595*scale},{},label.Get());
            CHECK(SUCCEEDED(target->EndDraw()));
            for (const auto& point:samples)
                CHECK_EQ(art.hit_test((point.x+18)*scale,(point.y+150)*scale),static_cast<int>(point.input));
            CHECK_EQ(art.hit_test(190*scale,350*scale),-1); // transparent gap
            CHECK_EQ(art.hit_test(-10,-10),-1);
        }
    }
    CoUninitialize();
}
int main() { return hktest::run_all(nullptr); }
