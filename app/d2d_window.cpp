#include "d2d_window.h"

#include <windowsx.h>

#include "hikari/clock.h"
#include "hikari/system.h"

namespace hikari {

namespace {
constexpr UINT_PTR kTimer = 1;
}

D2DWindow::~D2DWindow()
{
    if (hwnd_) DestroyWindow(hwnd_);
    if (font_) DeleteObject(font_);
    if (edit_brush_) DeleteObject(edit_brush_);
}

bool D2DWindow::create(const wchar_t* title, int w_dip, int h_dip, HWND owner, bool resizable)
{
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hIconSm = wc.hIcon;
        wc.lpszClassName = L"HikariWindow";
        RegisterClassExW(&wc);
        registered = true;
    }
    owner_ = owner;
    start_ = now_us();
    painter_.init();
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf());
    edit_brush_ = CreateSolidBrush(RGB(0x28, 0x2A, 0x2F));

    const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    scale_ = dpi / 96.0f;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    if (resizable) style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    RECT rc{0, 0, MulDiv(w_dip, dpi, 96), MulDiv(h_dip, dpi, 96)};
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpi);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    hwnd_ = CreateWindowExW(0, L"HikariWindow", title, style, wa.left + (wa.right - wa.left - w) / 2,
                            wa.top + (wa.bottom - wa.top - h) / 2, w, h, owner, nullptr, inst, this);
    if (!hwnd_) return false;
    const BOOL dark = TRUE;
    using SetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    if (auto f = reinterpret_cast<SetAttr>(reinterpret_cast<void*>(
            GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmSetWindowAttribute"))))
        f(hwnd_, 20, &dark, sizeof(dark));
    make_font();
    SetTimer(hwnd_, kTimer, 33, nullptr);
    return true;
}

void D2DWindow::make_font()
{
    if (font_) DeleteObject(font_);
    font_ = CreateFontW(-static_cast<int>(15 * scale_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (HWND e : edits_) SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
}

void D2DWindow::run_modal()
{
    if (owner_) EnableWindow(owner_, FALSE);
    ShowWindow(hwnd_, SW_SHOW);
    MSG m;
    while (IsWindow(hwnd_) && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd_, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (owner_) {
        EnableWindow(owner_, TRUE);
        SetForegroundWindow(owner_);
    }
}

HWND D2DWindow::add_edit(const std::string& text, bool password, bool upper)
{
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
    if (password) style |= ES_PASSWORD;
    if (upper) style |= ES_UPPERCASE;
    HWND e = CreateWindowExW(0, L"EDIT", widen(text).c_str(), style, 0, 0, 10, 10, hwnd_, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    const int m = static_cast<int>(10 * scale_);
    SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(m, m));
    edits_.push_back(e);
    return e;
}

void D2DWindow::place(HWND edit, const ui::Rect& r)
{
    // The visible field (rounded, drawn by D2D) is r; the native edit sits inside it.
    const int pad_y = static_cast<int>((r.h() - 20 * scale_) / 2);
    RECT cur;
    GetWindowRect(edit, &cur);
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&cur), 2);
    const int x = static_cast<int>(r.l + 4 * scale_), y = static_cast<int>(r.t) + pad_y;
    const int w = static_cast<int>(r.w() - 8 * scale_), h = static_cast<int>(r.h()) - 2 * pad_y;
    if (cur.left != x || cur.top != y || cur.right - cur.left != w || cur.bottom - cur.top != h)
        SetWindowPos(edit, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

std::string D2DWindow::text_of(HWND edit) const
{
    wchar_t buf[512];
    GetWindowTextW(edit, buf, 512);
    return utf8(buf);
}

void D2DWindow::set_text(HWND edit, const std::string& s) { SetWindowTextW(edit, widen(s).c_str()); }

LRESULT CALLBACK D2DWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        reinterpret_cast<D2DWindow*>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<D2DWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->handle(msg, wp, lp);
}

LRESULT D2DWindow::on_message(UINT, WPARAM, LPARAM, bool& handled)
{
    handled = false;
    return 0;
}

void D2DWindow::paint()
{
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (!rt_) {
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                                                        96, 96);
        factory_->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)),
                                         &rt_);
        if (!rt_) return;
    }
    rt_->BeginDraw();
    ui_.begin_frame();
    painter_.begin(rt_.Get(), scale_, (now_us() - start_) / 1e6);
    draw(painter_, ui::Rect{0, 0, float(rc.right), float(rc.bottom)});
    painter_.end();
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) rt_.Reset();
    const int click = ui_.take_click();
    if (click >= 0) on_action(click);
}

LRESULT D2DWindow::handle(UINT msg, WPARAM wp, LPARAM lp)
{
    bool handled = false;
    const LRESULT r = on_message(msg, wp, lp, handled);
    if (handled) return r;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        paint();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (rt_) rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        invalidate();
        return 0;
    case WM_DPICHANGED: {
        scale_ = HIWORD(wp) / 96.0f;
        make_font();
        auto* rr = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, rr->left, rr->top, rr->right - rr->left, rr->bottom - rr->top, SWP_NOZORDER);
        return 0;
    }
    case WM_TIMER:
        on_timer();
        if (animate_) invalidate();
        return 0;
    case WM_MOUSEMOVE: {
        ui_.mouse_x = static_cast<float>(GET_X_LPARAM(lp));
        ui_.mouse_y = static_cast<float>(GET_Y_LPARAM(lp));
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tme);
        invalidate();
        return 0;
    }
    case WM_MOUSELEAVE:
        ui_.mouse_x = ui_.mouse_y = -1;
        invalidate();
        return 0;
    case WM_LBUTTONDOWN:
        ui_.mouse_down = true;
        ui_.pressed_id = ui_.hit(static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
        SetCapture(hwnd_);
        SetFocus(hwnd_);
        invalidate();
        return 0;
    case WM_LBUTTONUP: {
        const int id = ui_.hit(static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
        if (id >= 0 && id == ui_.pressed_id) ui_.clicked_id = id;
        ui_.mouse_down = false;
        ui_.pressed_id = -1;
        ReleaseCapture();
        invalidate();
        return 0;
    }
    case WM_KEYDOWN:
        on_key(static_cast<UINT>(wp));
        return 0;
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, RGB(0xEC, 0xED, 0xEF));
        SetBkColor(dc, RGB(0x28, 0x2A, 0x2F));
        return reinterpret_cast<LRESULT>(edit_brush_);
    }
    case WM_CLOSE:
        if (on_close()) DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd_, kTimer);
        rt_.Reset();
        return 0;
    case WM_NCDESTROY: {
        HWND h = hwnd_;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        hwnd_ = nullptr;
        return DefWindowProcW(h, msg, wp, lp);
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace hikari
