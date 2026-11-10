// ==WindhawkMod==
// @id              desktop-countdown-overlay
// @name            Desktop Countdown Overlay
// @description     Countdown to March 28, 2027 00:00 IST - live seconds, flip-calendar style
// @version         1.4.0
// @author          custom
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -ldxgi -ld2d1 -ldwrite -ld3d11 -ldcomp -lgdi32 -lshcore
// ==/WindhawkMod==

// ==WindhawkModSettings==
/*
- x: 1200
  $name: X Position
  $description: "Horizontal pixel position on screen. 0 = left edge. Increase to move right. Tip: right side of a 1920px monitor is around 1400-1600."
- y: 120
  $name: Y Position
  $description: "Vertical pixel position on screen. 0 = top edge. Increase to move down."
- daysSize: 78
  $name: Days number size
  $description: "Font size (pt) for the large day count number. Default is 78."
- hmsSize: 28
  $name: Hours/Min/Sec size
  $description: "Font size (pt) for the HH : MM : SS row below the day count. Default is 28."
- bgOpacity: 28
  $name: Background opacity (0-100)
  $description: "Opacity of the rounded frosted container. 0 = no background, 100 = fully opaque black. Default is 28."
- textOpacity: 93
  $name: Text opacity (0-100)
  $description: "Opacity of the number text. 100 = fully visible. Default is 93."
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_2.h>
#include <dwrite_1.h>
#include <dcomp.h>
#include <shellscalingapi.h>
#include <stdint.h>
#include <stdio.h>

using Microsoft::WRL::ComPtr;

// March 28 2027 00:00:00 IST = March 27 2027 18:30:00 UTC = unix 1806172200
static constexpr int64_t TARGET_UNIX = 1806172200LL;

static HWND g_wnd = nullptr;
static ComPtr<ID3D11Device>         g_d3d;
static ComPtr<IDXGIDevice>          g_dxgi;
static ComPtr<IDXGIFactory2>        g_factory;
static ComPtr<ID2D1Factory1>        g_d2df;
static ComPtr<ID2D1Device>          g_d2dd;
static ComPtr<IDWriteFactory1>      g_dwrite;
static ComPtr<IDXGISwapChain1>      g_swap;
static ComPtr<ID2D1DeviceContext>   g_dc;
static ComPtr<IDCompositionDevice>  g_dcomp;
static ComPtr<IDCompositionTarget>  g_dctarget;
static ComPtr<IDCompositionVisual>  g_dcvisual;

#define WC_OVR  L"CDO_Overlay_" WH_MOD_ID
#define WC_MSG  L"CDO_Message_" WH_MOD_ID

static HWND g_msg   = nullptr;
static bool g_wc_ovr = false, g_wc_msg = false;

static HMODULE CurrentModule() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)CurrentModule, &m);
    return m;
}

// ---------------------------------------------------------------------------
// Clamp an int setting to [lo, hi]; use def when value is 0 (not set)
// ---------------------------------------------------------------------------
static int IntSetting(const WCHAR* key, int def, int lo, int hi) {
    int v = Wh_GetIntSetting(key);
    if (v == 0) v = def;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

// ---------------------------------------------------------------------------
// Apply IDWriteTypography tabular-figures to a full layout
// ---------------------------------------------------------------------------
static void ApplyTabFigs(IDWriteTextLayout* layout, UINT32 len) {
    ComPtr<IDWriteTypography> typo;
    if (FAILED(g_dwrite->CreateTypography(&typo))) return;
    DWRITE_FONT_FEATURE feat = {DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1};
    typo->AddFontFeature(feat);
    DWRITE_TEXT_RANGE all = {0, len};
    layout->SetTypography(typo.Get(), all);
}

// ---------------------------------------------------------------------------
// Make a Comic Sans / fallback text format
// ---------------------------------------------------------------------------
static ComPtr<IDWriteTextFormat> MakeFmt(float sz) {
    ComPtr<IDWriteTextFormat> fmt;
    HRESULT hr = g_dwrite->CreateTextFormat(
        L"Comic Sans MS", nullptr,
        DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, sz, L"", &fmt);
    if (FAILED(hr))
        g_dwrite->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_LIGHT, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, sz, L"", &fmt);
    return fmt;
}

// ---------------------------------------------------------------------------
// Draw soft white radial glow ellipse
// ---------------------------------------------------------------------------
static void DrawGlow(ID2D1GradientStopCollection* stops,
                     float cx, float cy, float rx, float ry) {
    if (!stops || rx < 2.f || ry < 2.f) return;
    D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES rp;
    rp.center               = {cx, cy};
    rp.gradientOriginOffset = {0.f, 0.f};
    rp.radiusX = rx;
    rp.radiusY = ry;
    ComPtr<ID2D1RadialGradientBrush> glow;
    g_dc->CreateRadialGradientBrush(rp, stops, &glow);
    if (glow) g_dc->FillEllipse({{cx, cy}, rx, ry}, glow.Get());
}

// ---------------------------------------------------------------------------
// Measure a text layout
// ---------------------------------------------------------------------------
static void Measure(IDWriteTextLayout* layout, float& outW, float& outH) {
    DWRITE_TEXT_METRICS m = {};
    layout->GetMetrics(&m);
    outW = m.widthIncludingTrailingWhitespace;
    outH = m.height;
}

// ---------------------------------------------------------------------------
// Draw a small centered label at (cx, y)
// ---------------------------------------------------------------------------
static void DrawCenteredLabel(IDWriteTextFormat* fmt,
                              ID2D1Brush* brush,
                              const WCHAR* text, UINT32 tlen,
                              float cx, float y, float maxW, float maxH) {
    ComPtr<IDWriteTextLayout> lbl;
    if (FAILED(g_dwrite->CreateTextLayout(text, tlen, fmt, maxW, maxH, &lbl))) return;
    float lw, lh; Measure(lbl.Get(), lw, lh);
    g_dc->DrawTextLayout({cx - lw * 0.5f, y}, lbl.Get(), brush,
                         D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
static void Render() {
    if (!g_dc || !g_swap || !g_wnd) return;

    // ---- Time --------------------------------------------------------------
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    int64_t now  = (int64_t)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
    int64_t diff = TARGET_UNIX - now;
    if (diff < 0) diff = 0;
    int64_t nDays = diff / 86400;
    int64_t nHrs  = (diff % 86400) / 3600;
    int64_t nMins = (diff % 3600) / 60;
    int64_t nSecs = diff % 60;

    // ---- DPI ---------------------------------------------------------------
    RECT rc; GetClientRect(g_wnd, &rc);
    float W = (float)(rc.right - rc.left);
    float H = (float)(rc.bottom - rc.top);
    HMONITOR mon = MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST);
    UINT dpix = 96, dpiy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpix, &dpiy);
    float sc = dpix / 96.f;

    // ---- Settings ----------------------------------------------------------
    float ox = (float)IntSetting(L"x", 1200, 0, 9000) * sc;
    float oy = (float)IntSetting(L"y",  120,  0, 9000) * sc;

    float daysFs = (float)IntSetting(L"daysSize", 78, 20, 200) * sc;
    float hmsFs  = (float)IntSetting(L"hmsSize",  28, 10, 120) * sc;
    float smlFs  = 11.f * sc;

    float bgAlpha  = IntSetting(L"bgOpacity",  28, 0, 100) / 100.f;
    float txtAlpha = IntSetting(L"textOpacity", 93, 10, 100) / 100.f;

    // ---- Formats -----------------------------------------------------------
    auto bigFmt = MakeFmt(daysFs);
    auto medFmt = MakeFmt(hmsFs);
    auto smlFmt = MakeFmt(smlFs);
    if (!bigFmt || !medFmt || !smlFmt) return;

    // ---- Brushes -----------------------------------------------------------
    ComPtr<ID2D1SolidColorBrush> mainBrush, colonBrush, labelBrush,
                                  bgFillBrush, bgStrokeBrush;
    g_dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, txtAlpha),  &mainBrush);
    g_dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.22f),     &colonBrush);
    g_dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.44f),     &labelBrush);
    g_dc->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, bgAlpha),   &bgFillBrush);
    g_dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.14f),     &bgStrokeBrush);
    if (!mainBrush || !colonBrush || !labelBrush || !bgFillBrush || !bgStrokeBrush) return;

    // ---- Glow gradient stops -----------------------------------------------
    D2D1_GRADIENT_STOP gs[3] = {
        {0.00f, D2D1::ColorF(1.f, 1.f, 1.f, 0.26f)},
        {0.55f, D2D1::ColorF(1.f, 1.f, 1.f, 0.09f)},
        {1.00f, D2D1::ColorF(1.f, 1.f, 1.f, 0.00f)}
    };
    ComPtr<ID2D1GradientStopCollection> glowStops;
    g_dc->CreateGradientStopCollection(gs, 3,
        D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &glowStops);

    // =========================================================================
    // Build text layouts
    // =========================================================================

    // (A) Days number
    WCHAR daysBuf[32];
    int dl = _snwprintf(daysBuf, 32, L"%lld", nDays);
    if (dl < 0) return;
    UINT32 uDl = (UINT32)dl;

    ComPtr<IDWriteTextLayout> daysLayout;
    if (FAILED(g_dwrite->CreateTextLayout(daysBuf, uDl, bigFmt.Get(), W, H, &daysLayout))) return;
    ApplyTabFigs(daysLayout.Get(), uDl);
    float daysW, daysH; Measure(daysLayout.Get(), daysW, daysH);

    // (B) HMS layout: "HH : MM : SS"
    WCHAR hmsBuf[64];
    int hl = _snwprintf(hmsBuf, 64, L"%02lld : %02lld : %02lld", nHrs, nMins, nSecs);
    if (hl < 0) return;
    UINT32 uHl = (UINT32)hl;

    ComPtr<IDWriteTextLayout> hmsLayout;
    if (FAILED(g_dwrite->CreateTextLayout(hmsBuf, uHl, medFmt.Get(), W, H, &hmsLayout))) return;
    ApplyTabFigs(hmsLayout.Get(), uHl);

    // Tight letter spacing for HMS
    ComPtr<IDWriteTextLayout1> hmsLayout1;
    hmsLayout.As(&hmsLayout1);
    if (hmsLayout1) {
        DWRITE_TEXT_RANGE all = {0, uHl};
        hmsLayout1->SetCharacterSpacing(0.f, 0.5f * sc, 0.f, all);
    }

    // Dim colons
    for (UINT32 i = 0; i < uHl; i++) {
        if (hmsBuf[i] == L':') {
            DWRITE_TEXT_RANGE r = {i, 1};
            hmsLayout->SetDrawingEffect(colonBrush.Get(), r);
        }
    }

    float hmsW, hmsH; Measure(hmsLayout.Get(), hmsW, hmsH);

    // =========================================================================
    // Container geometry — center everything on max(daysW, hmsW)
    // =========================================================================
    float padX    = 28.f * sc;
    float padTop  = 22.f * sc;
    float padBot  = 18.f * sc;

    // Gap between rows
    float gDaysLbl  = 1.f * sc;   // days number -> "days" label
    float gLblHms   = 10.f * sc;  // "days" label -> HMS row
    float gHmsLbl   = 2.f * sc;   // HMS row -> "hours/min/sec" labels

    float contentW  = (daysW > hmsW) ? daysW : hmsW;
    float contentH  = daysH + smlFs + gDaysLbl + gLblHms + hmsH + smlFs + gHmsLbl;
    float ctnW      = contentW + padX * 2.f;
    float ctnH      = contentH + padTop + padBot;

    // Container top-left
    float cx0 = ox;
    float cy0 = oy;

    // Content area left
    float ctX = cx0 + padX;

    // Centered X for each element
    float daysX   = ctX + (contentW - daysW)  * 0.5f;   // days number
    float daysCX  = cx0 + ctnW * 0.5f;                   // center of container (for labels)
    float hmsX    = ctX + (contentW - hmsW)   * 0.5f;   // HMS row

    // Y positions from top
    float daysY    = cy0 + padTop;
    float daysLblY = daysY   + daysH   + gDaysLbl;
    float hmsY     = daysLblY + smlFs   + gLblHms;
    float hmsLblY  = hmsY    + hmsH    + gHmsLbl;

    // =========================================================================
    // Find HMS digit-group bounding boxes for glow + label centering
    // =========================================================================
    struct DigRange { UINT32 start, length; };
    DigRange hmsGrp[4]; int hmsGN = 0;
    for (UINT32 i = 0; i < uHl; ) {
        if (hmsBuf[i] >= L'0' && hmsBuf[i] <= L'9') {
            UINT32 s = i;
            while (i < uHl && hmsBuf[i] >= L'0' && hmsBuf[i] <= L'9') i++;
            if (hmsGN < 4) { hmsGrp[hmsGN].start = s; hmsGrp[hmsGN].length = i - s; hmsGN++; }
        } else { i++; }
    }

    // HitTestTextRange with layout origin at (hmsX, hmsY)
    struct GroupBox { float cx, cy, hw, hh; bool valid; };
    GroupBox gbox[4] = {};
    for (int gi = 0; gi < hmsGN && gi < 4; gi++) {
        UINT32 needed = 0;
        hmsLayout->HitTestTextRange(hmsGrp[gi].start, hmsGrp[gi].length,
                                    hmsX, hmsY, nullptr, 0, &needed);
        if (needed == 0) needed = 1;
        DWRITE_HIT_TEST_METRICS hm[4] = {}; UINT32 actual = 0;
        UINT32 cap = (needed < 4u) ? needed : 4u;
        hmsLayout->HitTestTextRange(hmsGrp[gi].start, hmsGrp[gi].length,
                                    hmsX, hmsY, hm, cap, &actual);
        if (actual > 0)
            gbox[gi] = {hm[0].left + hm[0].width*0.5f,
                         hm[0].top  + hm[0].height*0.5f,
                         hm[0].width*0.5f, hm[0].height*0.5f, true};
    }

    // =========================================================================
    // DRAW
    // =========================================================================
    g_dc->BeginDraw();
    g_dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    // -- Rounded frosted container background
    if (bgAlpha > 0.001f) {
        D2D1_ROUNDED_RECT rr;
        rr.rect    = {cx0, cy0, cx0 + ctnW, cy0 + ctnH};
        rr.radiusX = 16.f * sc;
        rr.radiusY = 16.f * sc;
        g_dc->FillRoundedRectangle(rr, bgFillBrush.Get());
        g_dc->DrawRoundedRectangle(rr, bgStrokeBrush.Get(), 1.2f * sc);
    }

    // -- Subtle divider line between days block and HMS block
    {
        float divY = hmsY - gLblHms * 0.5f - smlFs * 0.5f;
        ComPtr<ID2D1SolidColorBrush> divBrush;
        g_dc->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.10f), &divBrush);
        if (divBrush)
            g_dc->DrawLine({cx0 + padX, divY}, {cx0 + ctnW - padX, divY},
                           divBrush.Get(), 1.f * sc);
    }

    // -- Glow behind days number
    DrawGlow(glowStops.Get(),
             daysX + daysW * 0.5f, daysY + daysH * 0.5f,
             daysW * 0.60f, daysH * 0.52f);

    // -- Glow behind each HMS digit group
    for (int gi = 0; gi < hmsGN && gi < 4; gi++)
        if (gbox[gi].valid)
            DrawGlow(glowStops.Get(),
                     gbox[gi].cx, gbox[gi].cy,
                     gbox[gi].hw * 1.5f, gbox[gi].hh * 1.4f);

    // -- Days number (big, centered)
    g_dc->DrawTextLayout({daysX, daysY}, daysLayout.Get(), mainBrush.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);

    // -- "days" label centered under the days number
    DrawCenteredLabel(smlFmt.Get(), labelBrush.Get(),
                      L"days", 4,
                      daysX + daysW * 0.5f, daysLblY, W, H);

    // -- HMS row (centered)
    g_dc->DrawTextLayout({hmsX, hmsY}, hmsLayout.Get(), mainBrush.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);

    // -- HMS labels: "hours" "min" "sec" centered under each digit group
    const WCHAR* hmsLbls[]   = {L"hours", L"min", L"sec"};
    const UINT32 hmsLblLens[] = {5, 3, 3};
    for (int gi = 0; gi < 3 && gi < hmsGN; gi++) {
        if (!gbox[gi].valid) continue;
        DrawCenteredLabel(smlFmt.Get(), labelBrush.Get(),
                          hmsLbls[gi], hmsLblLens[gi],
                          gbox[gi].cx, hmsLblY, W, H);
    }

    g_dc->EndDraw();
    g_swap->Present(1, 0);
}

// ---------------------------------------------------------------------------

static HWND GetWorkerW() {
    HWND prog = FindWindowW(L"Progman", nullptr);
    if (!prog) return nullptr;
    DWORD pid = 0;
    GetWindowThreadProcessId(prog, &pid);
    if (pid != GetCurrentProcessId()) return nullptr;
    SendMessageW(prog, 0x052C, 0xD, 0);
    SendMessageW(prog, 0x052C, 0xD, 1);
    HWND worker = nullptr;
    EnumWindows([](HWND w, LPARAM p) -> BOOL {
        if (FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr)) {
            HWND ww = FindWindowExW(nullptr, w, L"WorkerW", nullptr);
            if (ww) { *(HWND*)p = ww; return FALSE; }
        }
        return TRUE;
    }, (LPARAM)&worker);
    if (!worker) worker = FindWindowExW(prog, nullptr, L"WorkerW", nullptr);
    if (!worker) worker = prog;
    return worker;
}

static bool InitDX(UINT W2, UINT H2) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            flags, nullptr, 0, D3D11_SDK_VERSION, &g_d3d, nullptr, nullptr)))
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                flags, nullptr, 0, D3D11_SDK_VERSION, &g_d3d, nullptr, nullptr)))
            return false;

    if (FAILED(g_d3d.As(&g_dxgi))) return false;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&g_factory)))) return false;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&g_d2df)))) return false;
    if (FAILED(g_d2df->CreateDevice(g_dxgi.Get(), &g_d2dd))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory1),
            reinterpret_cast<IUnknown**>(g_dwrite.GetAddressOf())))) return false;

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = W2; sd.Height = H2;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(g_factory->CreateSwapChainForComposition(g_dxgi.Get(), &sd, nullptr, &g_swap))) return false;

    if (FAILED(g_d2dd->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_dc))) return false;

    ComPtr<IDXGISurface2> surf;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&surf)))) return false;
    ComPtr<ID2D1Bitmap1> bmp;
    D2D1_BITMAP_PROPERTIES1 bp = {};
    bp.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bp.bitmapOptions         = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    if (FAILED(g_dc->CreateBitmapFromDxgiSurface(surf.Get(), bp, &bmp))) return false;
    g_dc->SetTarget(bmp.Get());

    if (FAILED(DCompositionCreateDevice(g_dxgi.Get(), IID_PPV_ARGS(&g_dcomp)))) return false;
    if (FAILED(g_dcomp->CreateTargetForHwnd(g_wnd, TRUE, &g_dctarget))) return false;
    if (FAILED(g_dcomp->CreateVisual(&g_dcvisual))) return false;
    if (FAILED(g_dcvisual->SetContent(g_swap.Get()))) return false;
    if (FAILED(g_dctarget->SetRoot(g_dcvisual.Get()))) return false;
    if (FAILED(g_dcomp->Commit())) return false;
    return true;
}

static LRESULT CALLBACK OvrProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == 1) { Render(); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_DESTROY)    { g_wnd = nullptr; return 0; }
    if (msg == WM_APP)        { KillTimer(w, 1); DestroyWindow(w); return 0; }
    return DefWindowProcW(w, msg, wp, lp);
}

static LRESULT CALLBACK MsgProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_POWERBROADCAST &&
        (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND)) {
        if (g_wnd) PostMessageW(g_wnd, WM_TIMER, 1, 0);
        return TRUE;
    }
    if (msg == WM_DESTROY) { g_msg = nullptr; return 0; }
    if (msg == WM_APP)     { DestroyWindow(w); return 0; }
    return DefWindowProcW(w, msg, wp, lp);
}

static void CreateOverlay() {
    if (g_wnd) return;
    HWND worker = GetWorkerW();
    if (!worker) return;

    HMODULE mod = CurrentModule();
    if (!g_wc_ovr) {
        WNDCLASSW wc = {}; wc.lpfnWndProc = OvrProc;
        wc.hInstance = mod; wc.lpszClassName = WC_OVR;
        RegisterClassW(&wc); g_wc_ovr = true;
    }

    RECT rc; GetWindowRect(worker, &rc);
    UINT W2 = rc.right - rc.left, H2 = rc.bottom - rc.top;

    g_wnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        WC_OVR, nullptr, WS_CHILD | WS_VISIBLE,
        0, 0, W2, H2, worker, nullptr, mod, nullptr);
    if (!g_wnd) return;

    if (InitDX(W2, H2)) {
        Render();
        SetTimer(g_wnd, 1, 1000, nullptr);
    }
}

static void CreateMsg() {
    if (g_msg) return;
    HMODULE mod = CurrentModule();
    if (!g_wc_msg) {
        WNDCLASSW wc = {}; wc.lpfnWndProc = MsgProc;
        wc.hInstance = mod; wc.lpszClassName = WC_MSG;
        RegisterClassW(&wc); g_wc_msg = true;
    }
    g_msg = CreateWindowExW(0, WC_MSG, nullptr, 0, 0,0,0,0,
                             nullptr, nullptr, mod, nullptr);
}

using CWE_t = decltype(&CreateWindowExW);
static CWE_t CWE_orig = nullptr;

static bool IsFolderView(HWND w) {
    WCHAR cls[64], txt[64];
    if (!GetClassNameW(w, cls, 64) || _wcsicmp(cls, L"SysListView32")) return false;
    if (!GetWindowTextW(w, txt, 64)  || _wcsicmp(txt, L"FolderView"))   return false;
    HWND p = GetAncestor(w, GA_PARENT);
    if (!p || !GetClassNameW(p, cls, 64) || _wcsicmp(cls, L"SHELLDLL_DefView")) return false;
    return true;
}

static HWND WINAPI CWE_hook(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style,
        int x, int y, int W2, int H2, HWND parent, HMENU menu, HINSTANCE inst, PVOID param) {
    HWND hw = CWE_orig(ex, cls, name, style, x, y, W2, H2, parent, menu, inst, param);
    if (hw && IsFolderView(hw)) {
        static UINT_PTR tid = 0;
        tid = SetTimer(nullptr, tid, 1000, [](HWND, UINT, UINT_PTR id, DWORD) {
            KillTimer(nullptr, id);
            CreateOverlay(); CreateMsg();
        });
    }
    return hw;
}

using RFWTP_t = void(WINAPI*)(void*);
static bool RunOnThread(HWND w, RFWTP_t fn, void* p) {
    static const UINT MSG = RegisterWindowMessageW(L"WH_RunOnThread_" WH_MOD_ID);
    struct Param { RFWTP_t fn; void* p; };
    DWORD tid = GetWindowThreadProcessId(w, nullptr);
    if (!tid) return false;
    if (tid == GetCurrentThreadId()) { fn(p); return true; }
    HHOOK h = SetWindowsHookExW(WH_CALLWNDPROC,
        [](int code, WPARAM wp, LPARAM lp) -> LRESULT {
            if (code == HC_ACTION) {
                auto* cw = (CWPSTRUCT*)lp;
                if (cw->message == MSG)
                    ((Param*)cw->lParam)->fn(((Param*)cw->lParam)->p);
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        }, nullptr, tid);
    if (!h) return false;
    Param param{fn, p};
    SendMessageW(w, MSG, 0, (LPARAM)&param);
    UnhookWindowsHookEx(h);
    return true;
}

BOOL Wh_ModInit() {
    Wh_Log(L"init");
    Wh_SetFunctionHook((void*)CreateWindowExW, (void*)CWE_hook, (void**)&CWE_orig);
    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L"after-init");
    HWND worker = GetWorkerW();
    if (worker) RunOnThread(worker, [](void*) { CreateOverlay(); CreateMsg(); }, nullptr);
}

void Wh_ModUninit() {
    if (g_wnd) SendMessageW(g_wnd, WM_APP, 0, 0);
    if (g_msg) SendMessageW(g_msg, WM_APP, 0, 0);
    if (g_wc_ovr) { UnregisterClassW(WC_OVR, CurrentModule()); g_wc_ovr = false; }
    if (g_wc_msg) { UnregisterClassW(WC_MSG, CurrentModule()); g_wc_msg = false; }
    g_dcvisual.Reset(); g_dctarget.Reset(); g_dcomp.Reset();
    g_dc.Reset(); g_swap.Reset(); g_dwrite.Reset();
    g_d2dd.Reset(); g_d2df.Reset(); g_factory.Reset();
    g_dxgi.Reset(); g_d3d.Reset();
}

void Wh_ModSettingsChanged() {
    if (g_wnd) PostMessageW(g_wnd, WM_TIMER, 1, 0);
}
