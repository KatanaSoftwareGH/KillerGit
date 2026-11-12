// Win32 + Direct3D 11 host for the Dear ImGui UI. Renders only when something changes
// (input, window events, background results, or an active operation), so it idles at ~0% CPU.
#include "app.h"
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windows.h>
#include <objbase.h>
#include <windowsx.h>
#include <wincodec.h>
#include <vector>

int runMcp();
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

HWND g_hwnd = nullptr;
void platformWake() { if (g_hwnd) PostMessageW(g_hwnd, WM_NULL, 0, 0); }
static ID3D11Device *dev = nullptr;
static ID3D11DeviceContext *ctx = nullptr;
static IDXGISwapChain *swap = nullptr;
static ID3D11RenderTargetView *rtv = nullptr;
static UINT resizeW = 0, resizeH = 0;
static float dpiScale = 1.f;
static bool fontsDirty = false;
static int framesToRender = 3;

static void createRT() {
  ID3D11Texture2D *bb; swap->GetBuffer(0, IID_PPV_ARGS(&bb));
  dev->CreateRenderTargetView(bb, nullptr, &rtv); bb->Release();
}
static void cleanupRT() { if (rtv) { rtv->Release(); rtv = nullptr; } }

static bool createDevice(HWND h) {
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = h; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
  D3D_FEATURE_LEVEL got;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &swap, &dev, &got, &ctx);
  if (hr == DXGI_ERROR_UNSUPPORTED)  // no GPU (VMs, RDP): fall back to WARP software rendering
    hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &swap, &dev, &got, &ctx);
  if (FAILED(hr)) return false;
  createRT();
  return true;
}

// Decodes the embedded PNG (resource 103) with WIC and uploads it as an RGBA texture for ImGui::Image.
static ImTextureID loadIconTexture() {
  const void *png; size_t pngSize;
  if (!embeddedResource(103, png, pngSize)) return 0;
  IWICImagingFactory *wic = nullptr; IWICStream *stream = nullptr; IWICBitmapDecoder *dec = nullptr;
  IWICBitmapFrameDecode *frame = nullptr; IWICFormatConverter *conv = nullptr;
  ImTextureID id = 0;
  if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
      SUCCEEDED(wic->CreateStream(&stream)) &&
      SUCCEEDED(stream->InitializeFromMemory((BYTE *)png, (DWORD)pngSize)) &&
      SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) &&
      SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
      SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
    UINT w, h; conv->GetSize(&w, &h);
    std::vector<BYTE> px((size_t)w * h * 4);
    if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()))) {
      D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
      td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA sd{ px.data(), w * 4, 0 };
      ID3D11Texture2D *tex = nullptr; ID3D11ShaderResourceView *srv = nullptr;
      if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, &tex)) && SUCCEEDED(dev->CreateShaderResourceView(tex, nullptr, &srv))) id = (ImTextureID)srv;
      if (tex) tex->Release();
    }
  }
  if (conv) conv->Release(); if (frame) frame->Release(); if (dec) dec->Release(); if (stream) stream->Release(); if (wic) wic->Release();
  return id;
}

static void styleTitleBar(HWND h, bool dark) {
  BOOL d = dark; DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &d, sizeof d);
  COLORREF c = dark ? RGB(0x1f, 0x1e, 0x1d) : RGB(0xf0, 0xee, 0xe6);
  DwmSetWindowAttribute(h, 35 /*DWMWA_CAPTION_COLOR*/, &c, sizeof c);
}

static LRESULT WINAPI WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
  if (ImGui_ImplWin32_WndProcHandler(h, msg, w, l)) { framesToRender = 3; return true; }
  switch (msg) {
    case WM_SIZE:
      if (w == SIZE_MINIMIZED) return 0;
      resizeW = LOWORD(l); resizeH = HIWORD(l); framesToRender = 3;
      return 0;
    case WM_DPICHANGED: {
      dpiScale = HIWORD(w) / 96.f; fontsDirty = true;
      RECT *r = (RECT *)l;
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_GETMINMAXINFO: { auto *mm = (MINMAXINFO *)l; mm->ptMinTrackSize = { (LONG)(900 * dpiScale), (LONG)(560 * dpiScale) }; return 0; }
    case WM_SETTINGCHANGE:
      if (l && lstrcmpW((LPCWSTR)l, L"ImmersiveColorSet") == 0) { app.saveSettings(json::object()); styleTitleBar(h, app.dark); framesToRender = 3; }
      break;
    case WM_ACTIVATE:
      if (LOWORD(w) != WA_INACTIVE && !app.repo.empty() && !app.busy && app.tab == Tab::Changes) app.refreshRepo();
      framesToRender = 3;
      break;
    case WM_CLOSE: {
      RECT r; GetWindowRect(h, &r);
      WINDOWPLACEMENT wp{ sizeof wp }; GetWindowPlacement(h, &wp);
      vault::saveSettings({ { "winX", wp.rcNormalPosition.left }, { "winY", wp.rcNormalPosition.top },
                            { "width", wp.rcNormalPosition.right - wp.rcNormalPosition.left }, { "height", wp.rcNormalPosition.bottom - wp.rcNormalPosition.top },
                            { "maximized", wp.showCmd == SW_SHOWMAXIMIZED } });
      break;
    }
    case WM_SYSCOMMAND:
      if ((w & 0xfff0) == SC_KEYMENU) return 0;
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int show) {
  if (wcsstr(cmd, L"--mcp")) return runMcp();

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  // One window per user: focus the running instance instead of starting a second.
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"KillerGit.SingleInstance");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND other = FindWindowW(L"KillerGitWindow", nullptr)) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
    return 0;
  }

  WNDCLASSEXW wc{ sizeof wc, CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"KillerGitWindow", nullptr };
  RegisterClassExW(&wc);
  json s = vault::settings();
  POINT pt{ s.value("winX", (int)CW_USEDEFAULT), s.value("winY", (int)CW_USEDEFAULT) };
  // Saved position on a monitor that is no longer connected: let Windows place the window.
  if (pt.x != CW_USEDEFAULT && !MonitorFromPoint(POINT{ pt.x + 40, pt.y + 20 }, MONITOR_DEFAULTTONULL)) pt = { CW_USEDEFAULT, CW_USEDEFAULT };
  HMONITOR mon = MonitorFromPoint(pt.x == CW_USEDEFAULT ? POINT{ 0, 0 } : pt, MONITOR_DEFAULTTOPRIMARY);
  UINT dx = 96, dy = 96; GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
  dpiScale = dx / 96.f;
  int W = s.value("width", (int)(1320 * dpiScale)), H = s.value("height", (int)(840 * dpiScale));
  g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"KillerGit", WS_OVERLAPPEDWINDOW, pt.x, pt.y, W, H, nullptr, nullptr, inst, nullptr);
  dpiScale = GetDpiForWindow(g_hwnd) / 96.f;

  if (!createDevice(g_hwnd)) { MessageBoxW(nullptr, L"Could not initialise Direct3D 11.", L"KillerGit", MB_ICONERROR); return 1; }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  ImGui_ImplWin32_Init(g_hwnd);
  ImGui_ImplDX11_Init(dev, ctx);

  buildFonts(dpiScale);
  g_appIcon = loadIconTexture();
  app.boot();
  styleTitleBar(g_hwnd, app.dark);
  ShowWindow(g_hwnd, s.value("maximized", false) ? SW_SHOWMAXIMIZED : show);
  UpdateWindow(g_hwnd);

  bool running = true;
  while (running) {
    // Sleep until a message arrives unless there is something to animate.
    bool animate = app.busy || app.reposLoading || app.repoLoading || app.diffLoading || app.historyLoading || !app.toasts.empty();
    if (framesToRender <= 0 && !g_dirty) MsgWaitForMultipleObjects(0, nullptr, FALSE, animate ? 33 : INFINITE, QS_ALLINPUT);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg); DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) running = false;
      framesToRender = std::max(framesToRender, 2);
    }
    if (!running) break;
    if (g_dirty.exchange(false)) framesToRender = std::max(framesToRender, 2);
    drainUiQueue();
    if (IsIconic(g_hwnd)) { framesToRender = 0; continue; }
    if (resizeW) { cleanupRT(); swap->ResizeBuffers(0, resizeW, resizeH, DXGI_FORMAT_UNKNOWN, 0); resizeW = resizeH = 0; createRT(); }
    if (fontsDirty) { fontsDirty = false; buildFonts(dpiScale); app.saveSettings(json::object()); ImGui_ImplDX11_InvalidateDeviceObjects(); }

    static bool wasDark = app.dark;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    app.frame();
    ImGui::Render();
    if (wasDark != app.dark) { wasDark = app.dark; styleTitleBar(g_hwnd, app.dark); }
    const float clear[4] = { C.bg.x, C.bg.y, C.bg.z, 1 };
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->ClearRenderTargetView(rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swap->Present(1, 0);
    if (framesToRender > 0) framesToRender--;
    if (animate) framesToRender = std::max(framesToRender, 1);
  }

  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  cleanupRT(); if (swap) swap->Release(); if (ctx) ctx->Release(); if (dev) dev->Release();
  DestroyWindow(g_hwnd);
  ReleaseMutex(mutex);
  return 0;
}
