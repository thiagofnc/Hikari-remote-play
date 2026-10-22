#include "joycon_art.h"
#include "hikari/log.h"
#include "hikari/system.h"
#include <algorithm>
#include <cmath>
#include <cwchar>

namespace hikari::joycon {
using Microsoft::WRL::ComPtr;
namespace {
constexpr float kWidth = 344;
constexpr D2D1_RECT_F kCanvas{-18, -150, 362, 445};
constexpr D2D1_POINT_2F kLeftStick{74, 115}, kRightStick{270, 229};
constexpr uint32_t kAmber = 0xFBBB04;
constexpr int in(SwitchInput s) { return static_cast<int>(s); }
float distance(D2D1_POINT_2F a, D2D1_POINT_2F b) { return std::hypot(a.x-b.x, a.y-b.y); }
D2D1_COLOR_F color(uint32_t value, float alpha=1) { return D2D1::ColorF(value, alpha); }
struct Control { D2D1_POINT_2F center; float radius; SwitchInput input; const wchar_t* label; };
constexpr Control kControls[] = {
    {{270,82},15,SwitchInput::X,L"X"}, {{270,144},15,SwitchInput::B,L"B"},
    {{239,113},15,SwitchInput::Y,L"Y"}, {{301,113},15,SwitchInput::A,L"A"},
    {{74,202},15,SwitchInput::DpadUp,nullptr}, {{74,264},15,SwitchInput::DpadDown,nullptr},
    {{43,233},15,SwitchInput::DpadLeft,nullptr}, {{105,233},15,SwitchInput::DpadRight,nullptr},
    {{115,48},12,SwitchInput::Minus,nullptr}, {{227,48},13,SwitchInput::Plus,nullptr},
    {{96,313},13,SwitchInput::Capture,nullptr}, {{247,307},16,SwitchInput::Home,nullptr},
};
float emphasis(int input, const ArtState& s)
{
    if (s.target == input) return 0.65f + 0.35f*s.pulse;
    if (s.hover == input) return 0.8f;
    if (s.flash == input) return s.flash_amount;
    return 0;
}
}

bool JoyConArt::create(ID2D1Factory* factory)
{
    // Decode the unmodified transparent image once; select views when drawing.
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(102), RT_RCDATA);
    if (!resource) return false;
    const HGLOBAL loaded = LoadResource(module, resource);
    auto* data = static_cast<BYTE*>(LockResource(loaded));
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (!data || FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(data, SizeofResource(module, resource))) ||
        FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&source_)) ||
        FAILED(source_->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                   nullptr, 0, WICBitmapPaletteTypeMedianCut))) return false;
    for (int side=0; side<2; ++side) {
        const auto point = [side](float x,float y) { return D2D1::Point2F(side ? kWidth-x : x,y); };
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(factory->CreatePathGeometry(&shoulder_[side])) || FAILED(shoulder_[side]->Open(&sink))) return false;
        sink->BeginFigure(point(5,-84), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(point(116,-84)); sink->AddLine(point(116,-70));
        sink->AddBezier({point(116,-63),point(111,-60),point(103,-60)});
        sink->AddLine(point(17,-60));
        sink->AddBezier({point(8,-60),point(5,-64),point(5,-70)});
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close())) return false;
        sink.Reset();
        if (FAILED(factory->CreatePathGeometry(&trigger_[side])) || FAILED(trigger_[side]->Open(&sink))) return false;
        sink->BeginFigure(point(16,-97),D2D1_FIGURE_BEGIN_FILLED);
        sink->AddBezier({point(29,-124),point(38,-138),point(62,-138)});
        sink->AddLine(point(84,-138));
        sink->AddBezier({point(105,-138),point(111,-119),point(114,-97)});
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close())) return false;
    }
    return true;
}
bool JoyConArt::create_device(ID2D1RenderTarget* rt)
{
    release_device();
    if (!source_) return false;
    const HRESULT hr = rt->CreateBitmapFromWicBitmap(source_.Get(), nullptr, &bitmap_);
    if (FAILED(hr)) {
        HK_LOGE("Controller artwork bitmap: %s", hresult_string(hr).c_str());
        return false;
    }
    return SUCCEEDED(rt->CreateSolidColorBrush(color(kAmber), &brush_));
}
void JoyConArt::release_device() { bitmap_.Reset(); brush_.Reset(); }

void JoyConArt::draw(ID2D1RenderTarget* rt,const D2D1_RECT_F& area,const ArtState& s,IDWriteTextFormat* label)
{
    if (!bitmap_ && !create_device(rt)) return;
    const float width=area.right-area.left,height=area.bottom-area.top;
    if (width<=1 || height<=1) return;
    scale_=std::min(width/(kCanvas.right-kCanvas.left),height/(kCanvas.bottom-kCanvas.top));
    ox_=area.left+(width-(kCanvas.right-kCanvas.left)*scale_)/2-kCanvas.left*scale_;
    oy_=area.top+(height-(kCanvas.bottom-kCanvas.top)*scale_)/2-kCanvas.top*scale_;
    D2D1_MATRIX_3X2_F previous;
    rt->GetTransform(&previous);
    const D2D1::Matrix3x2F old(previous._11,previous._12,previous._21,previous._22,previous._31,previous._32);
    rt->SetTransform(D2D1::Matrix3x2F::Scale(scale_,scale_)*D2D1::Matrix3x2F::Translation(ox_,oy_)*old);
    const auto view=[&](D2D1_RECT_F source,D2D1_RECT_F target) {
        rt->DrawBitmap(bitmap_.Get(),target,1,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,source);
    };
    view({28,193,182,620},{0,0,154,427});
    view({1018,193,1172,620},{190,0,344,427});
    view({28,18,180,138},{0,-140,152,-20});
    view({1020,18,1172,138},{192,-140,344,-20});
    for (const auto& c:kControls) {
        const int input=in(c.input);
        const bool pressed=(s.active>>input)&1u;
        const float em=emphasis(input,s);
        if (!pressed && em<=0) continue;
        if (pressed) { brush_->SetColor(color(kAmber,0.65f)); rt->FillEllipse(D2D1::Ellipse(c.center,c.radius,c.radius),brush_.Get()); }
        brush_->SetColor(color(kAmber,pressed?1:em));
        rt->DrawEllipse(D2D1::Ellipse(c.center,c.radius+3,c.radius+3),brush_.Get(),2);
        if (pressed && c.label) {
            label->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            label->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            brush_->SetColor(color(0x17191D));
            rt->DrawText(c.label,static_cast<UINT32>(wcslen(c.label)),label,
                {c.center.x-c.radius,c.center.y-c.radius,c.center.x+c.radius,c.center.y+c.radius},brush_.Get());
        }
    }
    for (int side=0;side<2;++side) {
        const int inputs[]={in(side?SwitchInput::R:SwitchInput::L),in(side?SwitchInput::ZR:SwitchInput::ZL)};
        ID2D1PathGeometry* shapes[]={shoulder_[side].Get(),trigger_[side].Get()};
        for (int i=0;i<2;++i) {
            const bool pressed=(s.active>>inputs[i])&1u;
            const float em=emphasis(inputs[i],s),travel=i?(side?s.rt:s.lt):0;
            if (pressed || travel>0) { brush_->SetColor(color(kAmber,pressed?0.55f:travel*0.5f)); rt->FillGeometry(shapes[i],brush_.Get()); }
            if (pressed || em>0) { brush_->SetColor(color(kAmber,pressed?1:em)); rt->DrawGeometry(shapes[i],brush_.Get(),2); }
        }
    }
    const D2D1_POINT_2F centers[]={kLeftStick,kRightStick};
    const int clicks[]={in(SwitchInput::LStickClick),in(SwitchInput::RStickClick)};
    const int directions[]={in(SwitchInput::LStickUp),in(SwitchInput::RStickUp)};
    for (int side=0;side<2;++side) {
        const auto c=centers[side];
        const float x=std::clamp(side?s.rx:s.lx,-1.0f,1.0f),y=std::clamp(side?s.ry:s.ly,-1.0f,1.0f);
        const bool pressed=(s.active>>clicks[side])&1u;
        const float em=emphasis(clicks[side],s);
        if (std::fabs(x)>0.01f || std::fabs(y)>0.01f || pressed) {
            brush_->SetColor(color(0x41474A)); rt->FillEllipse(D2D1::Ellipse(c,31,31),brush_.Get());
            const auto cap=D2D1::Point2F(c.x+x*8,c.y-y*8);
            brush_->SetColor(color(pressed?kAmber:0x41474A)); rt->FillEllipse(D2D1::Ellipse(cap,22,22),brush_.Get());
            brush_->SetColor(color(0x14171A)); rt->DrawEllipse(D2D1::Ellipse(cap,22,22),brush_.Get(),1);
        }
        if (pressed || em>0) { brush_->SetColor(color(kAmber,pressed?1:em)); rt->DrawEllipse(D2D1::Ellipse(c,34,34),brush_.Get(),2); }
        const D2D1_POINT_2F delta[]={{0,-42},{0,42},{-42,0},{42,0}};
        for (int d=0;d<4;++d) {
            const int input=directions[side]+d;
            const bool on=(s.active>>input)&1u;
            const float e=emphasis(input,s);
            if (!on && e<=0) continue;
            brush_->SetColor(color(kAmber,on?1:e));
            rt->FillEllipse(D2D1::Ellipse({c.x+delta[d].x,c.y+delta[d].y},4,4),brush_.Get());
        }
    }
    rt->SetTransform(previous);
}
int JoyConArt::hit_test(float x,float y) const
{
    if (scale_<=0) return -1;
    const D2D1_POINT_2F p{(x-ox_)/scale_,(y-oy_)/scale_};
    for (const auto& c:kControls) if (distance(p,c.center)<=c.radius+3) return in(c.input);
    const D2D1_POINT_2F centers[]={kLeftStick,kRightStick};
    const SwitchInput clicks[]={SwitchInput::LStickClick,SwitchInput::RStickClick};
    const SwitchInput dirs[]={SwitchInput::LStickUp,SwitchInput::RStickUp};
    for (int side=0;side<2;++side) {
        const float d=distance(p,centers[side]);
        if (d>49) continue;
        if (d<=22) return in(clicks[side]);
        const float dx=p.x-centers[side].x,dy=p.y-centers[side].y;
        return in(dirs[side])+(std::fabs(dx)>std::fabs(dy)?(dx>0?3:2):(dy<0?0:1));
    }
    for (int side=0;side<2;++side) {
        BOOL inside=FALSE;
        if (SUCCEEDED(shoulder_[side]->FillContainsPoint(p,nullptr,0.25f,&inside)) && inside) return in(side?SwitchInput::R:SwitchInput::L);
        if (SUCCEEDED(trigger_[side]->FillContainsPoint(p,nullptr,0.25f,&inside)) && inside) return in(side?SwitchInput::ZR:SwitchInput::ZL);
    }
    return -1;
}
}
