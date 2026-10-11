// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#include "optical_surface.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

using Microsoft::WRL::ComPtr;
namespace {
LRESULT CALLBACK background(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if(message==WM_PAINT) {
        PAINTSTRUCT paint{};const auto dc=BeginPaint(window,&paint);RECT rect{};GetClientRect(window,&rect);
        const auto light=CreateSolidBrush(RGB(230,240,255)),dark=CreateSolidBrush(RGB(40,90,135));
        for(int y=0;y<rect.bottom;y+=24)for(int x=0;x<rect.right;x+=24){RECT square{x,y,x+24,y+24};FillRect(dc,&square,((x/24+y/24)&1)?light:dark);}
        DeleteObject(light);DeleteObject(dark);EndPaint(window,&paint);return 0;
    }
    if(message==WM_CLOSE)return 0;
    return DefWindowProcW(window,message,wparam,lparam);
}
LRESULT CALLBACK foreground(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_PAINT){PAINTSTRUCT paint{};BeginPaint(window,&paint);EndPaint(window,&paint);return 0;}
    if(message==WM_CLOSE)return 0;
    return DefWindowProcW(window,message,wparam,lparam);
}
void checked(HRESULT result,const char* operation) {
    if(FAILED(result)){std::cerr<<operation<<" HRESULT=0x"<<std::hex<<static_cast<unsigned long>(result)<<'\n';throw std::runtime_error(operation);}
}
}
int wmain(int argc,wchar_t** argv) {
    const bool hidden=argc>1&&std::wstring(argv[1])==L"--hidden";
    const bool observe=argc>1&&std::wstring(argv[1])==L"--observe";
    HWND back=nullptr,front=nullptr;
    try {
        const auto instance=GetModuleHandleW(nullptr);
        WNDCLASSW bg{};bg.lpfnWndProc=background;bg.hInstance=instance;bg.lpszClassName=L"ALARM.Optical.PrivateGrid";
        WNDCLASSW fg{};fg.lpfnWndProc=foreground;fg.hInstance=instance;fg.lpszClassName=L"ALARM.Optical.PrivateLens";
        if(!RegisterClassW(&bg)||!RegisterClassW(&fg))throw std::runtime_error("Register optical probe windows");
        back=CreateWindowExW(WS_EX_NOACTIVATE,bg.lpszClassName,L"ALARM — private generated grid / native optical probe",WS_OVERLAPPEDWINDOW,120,110,680,510,nullptr,nullptr,instance,nullptr);
        POINT point{80,70};ClientToScreen(back,&point);
        front=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP|WS_EX_NOACTIVATE,fg.lpszClassName,L"ALARM optical surface",WS_POPUP,point.x,point.y,480,320,nullptr,nullptr,instance,nullptr);
        if(!back||!front)throw std::runtime_error("Create optical probe windows");
        ComPtr<ID3D11Device> gpu;
        auto result=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,gpu.GetAddressOf(),nullptr,nullptr);
        if(FAILED(result))result=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,gpu.GetAddressOf(),nullptr,nullptr);
        checked(result,"D3D11CreateDevice");
        ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory2> factory;ComPtr<IDXGISwapChain1> swapchain;
        checked(gpu.As(&dxgi),"IDXGIDevice");checked(dxgi->GetAdapter(adapter.GetAddressOf()),"DXGI adapter");checked(adapter->GetParent(__uuidof(IDXGIFactory2),reinterpret_cast<void**>(factory.GetAddressOf())),"DXGI factory");
        DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=480;desc.Height=320;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
        desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.Scaling=DXGI_SCALING_STRETCH;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
        checked(factory->CreateSwapChainForComposition(gpu.Get(),&desc,nullptr,swapchain.GetAddressOf()),"Composition swapchain");
        ComPtr<ID2D1Factory> drawing;ComPtr<ID2D1RenderTarget> canvas;ComPtr<IDXGISurface> surface;ComPtr<ID2D1SolidColorBrush> rim;
        checked(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,drawing.GetAddressOf()),"D2D factory");
        checked(swapchain->GetBuffer(0,__uuidof(IDXGISurface),reinterpret_cast<void**>(surface.GetAddressOf())),"DXGI surface");
        const auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
        checked(drawing->CreateDxgiSurfaceRenderTarget(surface.Get(),&properties,canvas.GetAddressOf()),"D2D surface");
        checked(canvas->CreateSolidColorBrush(D2D1::ColorF(1,1,1,.55f),rim.GetAddressOf()),"D2D brush");
        canvas->BeginDraw();canvas->Clear(D2D1::ColorF(0,0,0,0));
        canvas->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(240,160),140,140),rim.Get(),1.3f);
        checked(canvas->EndDraw(),"D2D transparent UI frame");checked(swapchain->Present(1,0),"Present transparent UI");
        {
            alarmapp::ui::OpticalSurface optical;std::wstring error;
            if(!optical.attach(front,swapchain.Get(),480,320,error)){std::wcout<<optical.diagnostics()<<L"\nFAIL "<<error<<L'\n';throw std::runtime_error("OpticalSurface attach");}
            const std::vector<alarmapp::ui::OpticalLens> lenses{{100,20,380,300,140,18}};
            if(!optical.set_lenses(lenses,error))throw std::runtime_error("OpticalSurface lenses");
            const auto count=optical.zones();if(count!=36||!optical.set_lenses(lenses,error)||optical.zones()!=count)throw std::runtime_error("Optical mesh bound/deduplication");
            auto invalid=lenses;invalid[0].strength=std::numeric_limits<float>::quiet_NaN();
            if(optical.set_lenses(invalid,error)||optical.zones()!=count)throw std::runtime_error("Optical invalid input preservation");
            std::wcout<<optical.diagnostics()<<L"\nPASS composition graph/attach + affine mesh + swapchain overlay + rounded clip: "<<count<<L" GPU zones; visual output requires observation\n";
            if(!hidden) {ShowWindow(back,SW_SHOWNOACTIVATE);ShowWindow(front,SW_SHOWNOACTIVATE);UpdateWindow(back);}
            const auto due=GetTickCount64()+(hidden?100:(observe?60000:5000));
            while(GetTickCount64()<due) {
                MsgWaitForMultipleObjectsEx(0,nullptr,static_cast<DWORD>(std::min<ULONGLONG>(50,due-GetTickCount64())),QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
            }
            optical.release();if(optical.attached()||optical.zones())throw std::runtime_error("Optical release");
            std::wcout<<L"PASS optical release; no desktop readback/capture, sound or system setting change\n";
        }
        DestroyWindow(front);front=nullptr;DestroyWindow(back);back=nullptr;
        UnregisterClassW(fg.lpszClassName,instance);UnregisterClassW(bg.lpszClassName,instance);return 0;
    } catch(const std::exception& error) {
        std::cerr<<"FAIL optical probe: "<<error.what()<<'\n';if(front)DestroyWindow(front);if(back)DestroyWindow(back);return 1;
    }
}
