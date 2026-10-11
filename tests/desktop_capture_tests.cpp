// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

// Non-capturing lifecycle checks. Never opens the picker or selects a source.
#include "desktop_capture.h"
#include <d3d11_4.h>
#include <wrl/client.h>
#include <iostream>
#include <thread>

namespace {
unsigned checks=0;
void require(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam){return DefWindowProcW(window,message,wparam,lparam);}
}
int main() {
    HWND window=nullptr,child=nullptr;
    try {
        const auto instance=GetModuleHandleW(nullptr);WNDCLASSW type{};type.lpfnWndProc=procedure;type.hInstance=instance;type.lpszClassName=L"ALARM.DesktopCapture.NonCapturingTests";
        require(RegisterClassW(&type)!=0,"register hidden test window");
        window=CreateWindowExW(0,type.lpszClassName,L"Private non-capturing test",WS_OVERLAPPEDWINDOW,0,0,100,100,nullptr,nullptr,instance,nullptr);
        child=CreateWindowExW(0,L"STATIC",L"",WS_CHILD,0,0,20,20,window,nullptr,instance,nullptr);
        require(window&&child,"create only private hidden windows");
        Microsoft::WRL::ComPtr<ID3D11Device> device;Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;Microsoft::WRL::ComPtr<ID3D11Multithread> protection;
        auto result=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,device.GetAddressOf(),nullptr,context.GetAddressOf());
        if(FAILED(result))result=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,device.GetAddressOf(),nullptr,context.GetAddressOf());
        require(SUCCEEDED(result),"create private D3D device");require(SUCCEEDED(context.As(&protection)),"multithread interface");
        const auto previous_protection=protection->GetMultithreadProtected();DWORD previous_affinity=0;
        require(GetWindowDisplayAffinity(window,&previous_affinity)!=FALSE,"read initial affinity");
        {
            alarmapp::ui::DesktopCapture capture;std::wstring error;
            require(!capture.active()&&!capture.pending()&&!capture.attached()&&!capture.texture()&&!capture.frames(),"default has no captured frames");
            require(capture.poll(error),"idle poll succeeds without capture");
            require(!capture.request_picker(error),"picker refused before attach");
            require(!capture.attach(nullptr,device.Get(),error),"invalid HWND refused");
            require(!capture.attach(window,nullptr,error),"missing GPU refused");
            require(!capture.attach(child,device.Get(),error),"child HWND refused");
            require(!capture.attach(GetDesktopWindow(),device.Get(),error),"foreign desktop HWND refused");
            require(alarmapp::ui::DesktopCapture::supported(),"public capture and exclusion supported on this machine");
            for(unsigned repeat=0;repeat<3;++repeat) {
                require(capture.attach(window,device.Get(),error),"attach changes only private HWND/device");
                DWORD applied=0;require(GetWindowDisplayAffinity(window,&applied)&&applied==WDA_EXCLUDEFROMCAPTURE,"own demo excluded before picker");
                require(protection->GetMultithreadProtected()!=FALSE,"shared D3D context protected");
                require(capture.attached()&&!capture.active()&&!capture.pending()&&!capture.texture()&&!capture.frames(),"attach never starts capture");
                require(!capture.request_picker(error),"hidden test HWND never opens picker");
                require(capture.poll(error),"attached idle poll never opens picker");
                bool wrong_thread=false;std::thread other([&]{std::wstring issue;wrong_thread=!capture.poll(issue)&&!issue.empty();});other.join();
                require(wrong_thread,"other thread refused without mutating HWND");
                require(capture.attached(),"wrong-thread poll preserves attachment");
                capture.release();require(!capture.active()&&!capture.pending()&&!capture.attached()&&!capture.texture()&&!capture.frames(),"release drops all capture state");
                require(GetWindowDisplayAffinity(window,&applied)&&applied==previous_affinity,"restore original own HWND affinity");
                require(protection->GetMultithreadProtected()==previous_protection,"restore private context protection");
                capture.release();require(capture.poll(error),"release is repeatable");
            }
            require(capture.attach(window,device.Get(),error),"attach before destructor");
        }
        DWORD restored=0;require(GetWindowDisplayAffinity(window,&restored)&&restored==previous_affinity,"destructor restores affinity");
        require(protection->GetMultithreadProtected()==previous_protection,"destructor restores D3D protection");
        require(SetWindowDisplayAffinity(window,WDA_MONITOR)!=FALSE,"set non-default own HWND affinity");
        protection->SetMultithreadProtected(TRUE);
        require(alarmapp::ui::DesktopCapture::supported(),"query after previous COM apartment teardown");
        {
            alarmapp::ui::DesktopCapture capture;std::wstring error;
            require(capture.attach(window,device.Get(),error),"new instance after previous teardown");
            require(!capture.active()&&!capture.pending()&&!capture.texture(),"new instance still captures nothing");
        }
        require(GetWindowDisplayAffinity(window,&restored)&&restored==WDA_MONITOR,"restore non-default own HWND snapshot");
        require(protection->GetMultithreadProtected()!=FALSE,"preserve preexisting D3D multithread protection");
        require(SetWindowDisplayAffinity(window,previous_affinity)!=FALSE,"restore original hidden-window affinity after test");
        protection->SetMultithreadProtected(previous_protection);
        require(DestroyWindow(child)!=FALSE,"destroy child");child=nullptr;
        require(DestroyWindow(window)!=FALSE,"destroy hidden window");window=nullptr;
        require(UnregisterClassW(type.lpszClassName,instance)!=FALSE,"unregister window class");
        std::cout<<"PASS "<<checks<<" non-capturing checks; picker/source/frames never opened or acquired\n";return 0;
    }catch(const std::exception& issue){std::cerr<<"FAIL "<<issue.what()<<" after "<<checks<<" checks\n";if(child)DestroyWindow(child);if(window)DestroyWindow(window);return 1;}
}
