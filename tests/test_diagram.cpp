// Render the embedded reference artwork without a GPU or a visible window,
// then check reference-aligned input selection at common DPI scales.
#include "joycon_art.h"
#include "test_framework.h"
#include <array>
#include <algorithm>
#include <cstdlib>
#include <span>

using namespace hikari;
using Microsoft::WRL::ComPtr;

void verify_diagram(joycon::Model model)
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
        CHECK(art.create(factory.Get(), model));
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
        const Sample switch2_samples[] = {
            {312.7f,112.8f,SwitchInput::A},{282.4f,144,SwitchInput::B},{283.7f,83.2f,SwitchInput::X},{252.1f,112.8f,SwitchInput::Y},
            {69.5f,198.1f,SwitchInput::DpadUp},{69.5f,260,SwitchInput::DpadDown},{37.7f,229.2f,SwitchInput::DpadLeft},{101.5f,229.2f,SwitchInput::DpadRight},
            {108.4f,54.2f,SwitchInput::Minus},{245.1f,52.9f,SwitchInput::Plus},{96.3f,307,SwitchInput::Capture},{257.9f,290.3f,SwitchInput::Home},
            {70.1f,124,SwitchInput::LStickClick},{282.1f,218.6f,SwitchInput::RStickClick},
            {70.1f,82,SwitchInput::LStickUp},{70.1f,166,SwitchInput::LStickDown},{28.1f,124,SwitchInput::LStickLeft},{112.1f,124,SwitchInput::LStickRight},
            {282.1f,176.6f,SwitchInput::RStickUp},{282.1f,260.6f,SwitchInput::RStickDown},{240.1f,218.6f,SwitchInput::RStickLeft},{324.1f,218.6f,SwitchInput::RStickRight},
            {65,-70,SwitchInput::L},{279,-70,SwitchInput::R},{67,-112,SwitchInput::ZL},{277,-112,SwitchInput::ZR},
        };
        const auto points=model==joycon::Model::Switch2?std::span<const Sample>(switch2_samples):std::span<const Sample>(samples);
        for (float scale : {1.0f,1.5f,2.0f}) {
            target->BeginDraw();
            target->Clear(D2D1::ColorF(0,0));
            art.draw(target.Get(),{0,0,380*scale,595*scale},{},label.Get());
            CHECK(SUCCEEDED(target->EndDraw()));
            for (const auto& point:points)
                CHECK_EQ(art.hit_test((point.x+18)*scale,(point.y+150)*scale),static_cast<int>(point.input));
            CHECK_EQ(art.hit_test(190*scale,350*scale),-1); // transparent gap
            CHECK_EQ(art.hit_test(-10,-10),-1);
            if (model==joycon::Model::Switch2)
                CHECK_EQ(art.hit_test((258.2f+18)*scale,(332.4f+150)*scale),-1); // C is not a bridge input.
            ComPtr<IWICBitmapLock> pixels;
            WICRect bounds{0,0,760,1190};
            CHECK(SUCCEEDED(surface->Lock(&bounds,WICBitmapLockRead,&pixels)));
            if (pixels) {
                UINT stride=0,size=0; BYTE* data=nullptr;
                pixels->GetStride(&stride); pixels->GetDataPointer(&size,&data);
                const auto alpha=[&](float x,float y) { return data[static_cast<UINT>((y+150)*scale)*stride+static_cast<UINT>((x+18)*scale)*4+3]; };
                CHECK(alpha(74,115)>200); // Visible original front artwork.
                CHECK(alpha(65,-70)>200); // Supplied shoulder view decoded and drawn.
                CHECK_EQ(alpha(172,200),0); // Actual transparent gap between images.
                if (model==joycon::Model::Switch2) {
                    int max_alpha_difference=0;
                    const UINT mirror_width=static_cast<UINT>(380*scale);
                    for (UINT y=0;y<static_cast<UINT>(150*scale);++y)
                        for (UINT x=0;x<mirror_width/2;++x) {
                            const int left=data[y*stride+x*4+3],right=data[y*stride+(mirror_width-1-x)*4+3];
                            max_alpha_difference=std::max(max_alpha_difference,std::abs(left-right));
                        }
                    CHECK(max_alpha_difference<=2); // Exact shared silhouette at every scale.
                }
            }
        }
    }
    CoUninitialize();
}
TEST(controller_reference_renders_and_selects_all_inputs) { verify_diagram(joycon::Model::Switch); }
TEST(switch2_reference_renders_and_selects_all_inputs) { verify_diagram(joycon::Model::Switch2); }
int main() { return hktest::run_all(nullptr); }
