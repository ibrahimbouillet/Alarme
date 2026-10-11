// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#include "desktop_capture.h"
#include <d3d11_4.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <roapi.h>
#include <shobjidl.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>

namespace alarmapp::ui {
namespace {
namespace C=winrt::Windows::Graphics::Capture;
namespace F=winrt::Windows::Foundation;
namespace D=winrt::Windows::Graphics::DirectX::Direct3D11;
using SizeInt32=winrt::Windows::Graphics::SizeInt32;
constexpr auto pixel_format=winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized;
constexpr unsigned buffer_count=2;
// Uncached factories are deliberate: capability queries may temporarily open
// and close a COM apartment. A cached WinRT factory can outlive that apartment
// and become invalid when this same native thread is subsequently initialized.
bool capture_supported() {
    const auto capture=winrt::get_activation_factory<C::IGraphicsCaptureSessionStatics>(L"Windows.Graphics.Capture.GraphicsCaptureSession");
    const auto metadata=winrt::get_activation_factory<winrt::Windows::Foundation::Metadata::IApiInformationStatics>(L"Windows.Foundation.Metadata.ApiInformation");
    return capture.IsSupported()&&metadata.IsApiContractPresent(L"Windows.Foundation.UniversalApiContract",10);
}
bool valid_size(SizeInt32 size) noexcept {
    return size.Width>0&&size.Height>0&&size.Width<=8192&&size.Height<=8192&&
        static_cast<std::uint64_t>(size.Width)*size.Height<=33554432;
}
// Read-only boundary check. Never draw or capture the secure desktop.
// If access is denied or the input desktop is not Default, no frame is exposed.
bool ordinary_desktop() noexcept {
    const auto desktop=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
    if(!desktop)return false;
    wchar_t name[64]{};DWORD needed=0;
    const bool normal=GetUserObjectInformationW(desktop,UOI_NAME,name,sizeof(name),&needed)&&
        needed<=sizeof(name)&&_wcsicmp(name,L"Default")==0;
    CloseDesktop(desktop);return normal;
}
struct Notice {
    std::atomic<HWND> window{nullptr};
    std::atomic<bool> live{true},queued{false},closed{false};
    void post() noexcept {
        const auto hwnd=window.load(std::memory_order_acquire);
        if(!live.load(std::memory_order_acquire)||!hwnd||queued.exchange(true,std::memory_order_acq_rel))return;
        if(!PostMessageW(hwnd,DesktopCapture::frame_message,0,0))queued.store(false,std::memory_order_release);
    }
    void disable() noexcept {live.store(false,std::memory_order_release);window.store(nullptr,std::memory_order_release);}
};
}

struct DesktopCapture::Impl {
    DWORD thread=0;HWND window=nullptr;DWORD previous_affinity=WDA_NONE;
    bool ro_initialized=false,affinity_changed=false,multithread_changed=false,previous_multithread=false;
    winrt::com_ptr<ID3D11Device> gpu;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<ID3D11Multithread> multithread;
    winrt::com_ptr<ID3D11Texture2D> output;
    D::IDirect3DDevice device{nullptr};
    F::IAsyncOperation<C::GraphicsCaptureItem> selection{nullptr};
    C::GraphicsCapturePicker picker{nullptr};
    C::GraphicsCaptureItem item{nullptr};
    C::Direct3D11CaptureFramePool pool{nullptr};
    C::GraphicsCaptureSession session{nullptr};
    winrt::event_token frame_token{},closed_token{};
    bool frame_subscribed=false,closed_subscribed=false;
    SizeInt32 pool_size{};SIZE output_size{};
    std::uint64_t frame_count=0;
    std::shared_ptr<Notice> notice;
    std::wstring state=L"No source selected.",failure,steps;
    ~Impl(){release();if(ro_initialized)RoUninitialize();}
    void record(const wchar_t* operation,HRESULT result) {
        wchar_t code[16]{};swprintf_s(code,L"%08lX",static_cast<unsigned long>(result));
        if(steps.size()>8192) {
            const auto boundary=steps.find(L'\n',steps.size()-8192);
            if(boundary==std::wstring::npos)steps.clear();else steps.erase(0,boundary+1);
        }
        if(!steps.empty())steps+=L"\n";
        steps+=std::wstring(operation)+L"=0x"+code;
    }
    void checked(const wchar_t* operation,HRESULT result){record(operation,result);winrt::check_hresult(result);}
    void checked_win32(const wchar_t* operation,BOOL result) {
        checked(operation,result?S_OK:HRESULT_FROM_WIN32(GetLastError()?GetLastError():ERROR_GEN_FAILURE));
    }
    bool same_thread(std::wstring& error) {
        if(thread&&thread!=GetCurrentThreadId()){error=L"DesktopCapture must remain on its HWND thread.";return false;}return true;
    }
    void stop_stream() noexcept {
        if(notice)notice->disable();
        if(selection){try{selection.Cancel();}catch(...){}selection=nullptr;}picker=nullptr;
        if(pool&&frame_subscribed){try{pool.FrameArrived(frame_token);}catch(...){}}frame_subscribed=false;
        if(item&&closed_subscribed){try{item.Closed(closed_token);}catch(...){}}closed_subscribed=false;
        // Close session before pool. Callbacks only own Notice, never this Impl.
        if(session){try{session.Close();}catch(...){}session=nullptr;}
        if(pool){try{pool.Close();}catch(...){}pool=nullptr;}
        item=nullptr;notice.reset();output=nullptr;output_size={};pool_size={};frame_count=0;
    }
    void release() noexcept {
        stop_stream();
        if(device){try{device.Close();}catch(...){}device=nullptr;}
        context=nullptr;
        if(multithread_changed&&multithread)multithread->SetMultithreadProtected(previous_multithread);
        multithread=nullptr;gpu=nullptr;multithread_changed=false;
        if(affinity_changed&&window&&IsWindow(window)&&!SetWindowDisplayAffinity(window,previous_affinity)) {
            const auto restore_error=GetLastError();
            try{failure=L"Capture stopped; window display affinity could not be restored.";record(L"RestoreWindowDisplayAffinity",HRESULT_FROM_WIN32(restore_error?restore_error:ERROR_GEN_FAILURE));}catch(...){}
        }
        affinity_changed=false;window=nullptr;
        try{state=L"Capture stopped; select a source again.";}catch(...){}
    }
    bool exception(std::wstring& error,const wchar_t* operation) {
        try{throw;}catch(const winrt::hresult_error& issue){record(operation,issue.code());failure=std::wstring(operation)+L" : "+std::wstring(issue.message());}
        catch(const std::exception&){failure=std::wstring(operation)+L": memory or C++ error.";}
        catch(...){failure=std::wstring(operation)+L": unknown error.";}
        state=L"Capture unavailable.";error=failure;stop_stream();return false;
    }
    void begin(C::GraphicsCaptureItem selected) {
        const auto size=selected.Size();if(!valid_size(size))winrt::throw_hresult(E_INVALIDARG);
        item=std::move(selected);pool_size=size;
        const auto pool_factory=winrt::get_activation_factory<C::IDirect3D11CaptureFramePoolStatics2>(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool");
        pool=pool_factory.CreateFreeThreaded(device,pixel_format,buffer_count,size);record(L"CreateFreeThreaded(2 BGRA GPU buffers)",S_OK);
        session=pool.CreateCaptureSession(item);
        if(const auto cursor=session.try_as<C::IGraphicsCaptureSession2>())cursor.IsCursorCaptureEnabled(false);
        // Retain the system consent border. Do not request borderless access.
        if(const auto interval=session.try_as<C::IGraphicsCaptureSession5>())interval.MinUpdateInterval(std::chrono::milliseconds(33));
        const auto signal=notice;
        closed_token=item.Closed([signal](const auto&,const auto&) noexcept {signal->closed.store(true,std::memory_order_release);signal->post();});closed_subscribed=true;
        frame_token=pool.FrameArrived([signal](const auto&,const auto&) noexcept {signal->post();});frame_subscribed=true;
        session.StartCapture();record(L"StartCapture(user-selected source)",S_OK);
        state=L"Capture active; pixels remain in GPU memory only.";
    }
};

DesktopCapture::DesktopCapture():impl_(std::make_unique<Impl>()){}
DesktopCapture::~DesktopCapture()=default;
bool DesktopCapture::supported() noexcept {
    const auto initialized=RoInitialize(RO_INIT_SINGLETHREADED);
    if(FAILED(initialized)&&initialized!=RPC_E_CHANGED_MODE)return false;
    bool result=false;try{result=capture_supported();}catch(...){}
    if(SUCCEEDED(initialized))RoUninitialize();
    return result;
}
bool DesktopCapture::attach(HWND window,ID3D11Device* device,std::wstring& error) {
    auto& p=*impl_;error.clear();if(!p.same_thread(error))return false;p.release();p.failure.clear();p.steps.clear();
    DWORD owner=0;const auto owner_thread=GetWindowThreadProcessId(window,&owner);
    if(!IsWindow(window)||!device||owner!=GetCurrentProcessId()||owner_thread!=GetCurrentThreadId()||(GetWindowLongPtrW(window,GWL_STYLE)&WS_CHILD)||
        (device->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED)) {
        error=p.failure=L"DesktopCapture requires the current thread's top-level HWND and its GPU device.";return false;
    }
    p.thread=GetCurrentThreadId();p.window=window;
    try {
        if(!p.ro_initialized){p.checked(L"RoInitialize",RoInitialize(RO_INIT_SINGLETHREADED));p.ro_initialized=true;}
        if(!capture_supported())winrt::throw_hresult(E_NOTIMPL);
        BOOL composition=FALSE;p.checked(L"DwmIsCompositionEnabled",DwmIsCompositionEnabled(&composition));if(!composition)winrt::throw_hresult(E_NOTIMPL);
        p.checked_win32(L"GetWindowDisplayAffinity",GetWindowDisplayAffinity(window,&p.previous_affinity));
        p.checked_win32(L"WDA_EXCLUDEFROMCAPTURE",SetWindowDisplayAffinity(window,WDA_EXCLUDEFROMCAPTURE));p.affinity_changed=true;
        DWORD applied=0;p.checked_win32(L"VerifyWindowDisplayAffinity",GetWindowDisplayAffinity(window,&applied));if(applied!=WDA_EXCLUDEFROMCAPTURE)winrt::throw_hresult(E_ACCESSDENIED);
        p.gpu.copy_from(device);p.gpu->GetImmediateContext(p.context.put());
        p.record(L"ImmediateContext",S_OK);
        p.multithread=p.context.as<ID3D11Multithread>();p.previous_multithread=p.multithread->GetMultithreadProtected();
        p.record(L"ID3D11Multithread",S_OK);
        if(!p.previous_multithread){p.multithread->SetMultithreadProtected(TRUE);p.multithread_changed=true;}
        const auto dxgi=p.gpu.as<IDXGIDevice>();winrt::Windows::Foundation::IInspectable inspectable{nullptr};
        p.record(L"IDXGIDevice",S_OK);
        p.checked(L"CreateDirect3D11DeviceFromDXGIDevice",CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(),reinterpret_cast<IInspectable**>(winrt::put_abi(inspectable))));
        p.device=inspectable.as<D::IDirect3DDevice>();p.state=L"Ready; choose a source in the Windows picker.";return true;
    }catch(...){const bool result=p.exception(error,L"DesktopCapture.attach");p.release();return result;}
}
bool DesktopCapture::request_picker(std::wstring& error) {
    auto& p=*impl_;error.clear();if(!p.same_thread(error))return false;
    if(!p.window||!p.device||!IsWindowVisible(p.window)||IsIconic(p.window)||!ordinary_desktop()) {
        error=L"Capture refused: demo hidden, desktop locked, or device unavailable.";return false;
    }
    if(p.selection){error=L"The Windows picker is already open.";return false;}
    p.stop_stream();p.failure.clear();
    try {
        DWORD applied=0;p.checked_win32(L"Verify exclusion before picker",GetWindowDisplayAffinity(p.window,&applied));if(applied!=WDA_EXCLUDEFROMCAPTURE)winrt::throw_hresult(E_ACCESSDENIED);
        p.notice=std::make_shared<Notice>();p.notice->window.store(p.window,std::memory_order_release);
        p.picker=winrt::get_activation_factory<F::IActivationFactory>(L"Windows.Graphics.Capture.GraphicsCapturePicker").ActivateInstance<C::GraphicsCapturePicker>();
        p.checked(L"Picker.Initialize(HWND)",p.picker.as<IInitializeWithWindow>()->Initialize(p.window));
        p.selection=p.picker.PickSingleItemAsync();const auto signal=p.notice;
        p.selection.Completed([signal](const auto&,F::AsyncStatus) noexcept {signal->post();});
        p.state=L"Waiting for Windows selection; no active source.";return true;
    }catch(...){return p.exception(error,L"DesktopCapture.request_picker");}
}
bool DesktopCapture::poll(std::wstring& error) {
    auto& p=*impl_;error.clear();if(!p.same_thread(error))return false;
    if(p.notice)p.notice->queued.store(false,std::memory_order_release);
    if(!p.selection&&!p.session)return true;
    if(!p.window||!IsWindow(p.window)||!IsWindowVisible(p.window)||IsIconic(p.window)||!ordinary_desktop()) {
        p.stop_stream();p.state=L"Capture stopped: demo hidden or desktop unavailable.";return true;
    }
    try {
        DWORD applied=0;
        if(!GetWindowDisplayAffinity(p.window,&applied)||applied!=WDA_EXCLUDEFROMCAPTURE)winrt::throw_hresult(E_ACCESSDENIED);
        if(p.selection) {
            const auto state=p.selection.Status();if(state==F::AsyncStatus::Started)return true;
            auto operation=std::move(p.selection);p.selection=nullptr;p.picker=nullptr;
            if(state==F::AsyncStatus::Canceled){p.stop_stream();p.state=L"Selection cancelled; no active source.";return true;}
            if(state==F::AsyncStatus::Error)winrt::throw_hresult(operation.ErrorCode());
            auto selected=operation.GetResults();
            if(!selected){p.stop_stream();p.state=L"Selection cancelled; no active source.";return true;}
            p.begin(std::move(selected));
        }
        if(p.notice&&p.notice->closed.load(std::memory_order_acquire)){p.stop_stream();p.state=L"Source closed; select a source again.";return true;}
        if(!p.pool)return true;
        // Drain at most the bounded pool capacity. Keep only the newest frame.
        C::Direct3D11CaptureFrame latest{nullptr};
        for(unsigned i=0;i<buffer_count;++i){auto frame=p.pool.TryGetNextFrame();if(!frame)break;if(latest)latest.Close();latest=std::move(frame);}
        if(!latest)return true;
        const auto size=latest.ContentSize();
        if(!valid_size(size)){latest.Close();winrt::throw_hresult(E_INVALIDARG);}
        const auto surface=latest.Surface();winrt::com_ptr<ID3D11Texture2D> source;
        winrt::check_hresult(surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>()->GetInterface(__uuidof(ID3D11Texture2D),source.put_void()));
        D3D11_TEXTURE2D_DESC description{};source->GetDesc(&description);
        if(size.Width>static_cast<int>(description.Width)||size.Height>static_cast<int>(description.Height)) {
            latest.Close();p.pool.Recreate(p.device,pixel_format,buffer_count,size);p.pool_size=size;return true;
        }
        if(!p.output||p.output_size.cx!=size.Width||p.output_size.cy!=size.Height) {
            D3D11_TEXTURE2D_DESC copy{};copy.Width=size.Width;copy.Height=size.Height;copy.MipLevels=copy.ArraySize=1;
            copy.Format=description.Format;copy.SampleDesc.Count=1;copy.Usage=D3D11_USAGE_DEFAULT;copy.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            winrt::com_ptr<ID3D11Texture2D> replacement;winrt::check_hresult(p.gpu->CreateTexture2D(&copy,nullptr,replacement.put()));p.output=std::move(replacement);p.output_size={size.Width,size.Height};
        }
        const D3D11_BOX box{0,0,0,static_cast<UINT>(size.Width),static_cast<UINT>(size.Height),1};
        p.context->CopySubresourceRegion(p.output.get(),0,0,0,0,source.get(),0,&box);
        latest.Close();++p.frame_count;
        if(size.Width!=p.pool_size.Width||size.Height!=p.pool_size.Height){p.pool.Recreate(p.device,pixel_format,buffer_count,size);p.pool_size=size;}
        const auto removed=p.gpu->GetDeviceRemovedReason();if(FAILED(removed))winrt::throw_hresult(removed);
        return true;
    }catch(...){return p.exception(error,L"DesktopCapture.poll");}
}
ID3D11Texture2D* DesktopCapture::texture()const noexcept{return impl_->output.get();}
SIZE DesktopCapture::size()const noexcept{return impl_->output_size;}
std::uint64_t DesktopCapture::frames()const noexcept{return impl_->frame_count;}
bool DesktopCapture::active()const noexcept{return static_cast<bool>(impl_->session);}
bool DesktopCapture::pending()const noexcept{return static_cast<bool>(impl_->selection);}
bool DesktopCapture::attached()const noexcept{return impl_->window&&static_cast<bool>(impl_->device);}
std::wstring DesktopCapture::status()const{return impl_->state;}
std::wstring DesktopCapture::error()const{return impl_->failure;}
std::wstring DesktopCapture::diagnostics()const{return impl_->steps;}
void DesktopCapture::release()noexcept{impl_->release();}
}
