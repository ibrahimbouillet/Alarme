// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#include "optical_surface.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d2d1effects.h>
#include <dwmapi.h>
#include <dispatcherqueue.h>
#include <roapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <algorithm>
#include <cmath>
#include <sstream>

// The installed MinGW headers omit these SDK interop declarations. Their ABI
// matches SDK 10.0.26100 windows.ui.composition.interop.h and
// windows.graphics.effects.interop.h; no SDK DLL or Win2D package is required.
struct OpticalDesktopInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateDesktopWindowTarget(HWND,BOOL,void**)=0;
    virtual HRESULT STDMETHODCALLTYPE EnsureOnThread(DWORD)=0;
};
__CRT_UUID_DECL(OpticalDesktopInterop,0x29e691fa,0x4567,0x4dca,0xb3,0x19,0xd0,0xf2,0x07,0xeb,0x68,0x07)
struct OpticalCompositorInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateCompositionSurfaceForHandle(HANDLE,void**)=0;
    virtual HRESULT STDMETHODCALLTYPE CreateCompositionSurfaceForSwapChain(IUnknown*,void**)=0;
    virtual HRESULT STDMETHODCALLTYPE CreateGraphicsDevice(IUnknown*,void**)=0;
};
__CRT_UUID_DECL(OpticalCompositorInterop,0x25297d5c,0x3ad4,0x4c9c,0xb5,0xcf,0xe3,0x6a,0x38,0x51,0x23,0x30)
struct OpticalEffectInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetEffectId(GUID*)=0;
    virtual HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(LPCWSTR,UINT*,UINT*)=0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT*)=0;
    virtual HRESULT STDMETHODCALLTYPE GetProperty(UINT,void**)=0;
    virtual HRESULT STDMETHODCALLTYPE GetSource(UINT,void**)=0;
    virtual HRESULT STDMETHODCALLTYPE GetSourceCount(UINT*)=0;
};
__CRT_UUID_DECL(OpticalEffectInterop,0x2fc57384,0xa068,0x44d7,0xa3,0x31,0x30,0x98,0x2f,0xcf,0x71,0x77)

namespace alarmapp::ui {
namespace {
namespace C=winrt::Windows::UI::Composition;
namespace F=winrt::Windows::Foundation;
namespace E=winrt::Windows::Graphics::Effects;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3x2;

struct AffineEffect : winrt::implements<AffineEffect,E::IGraphicsEffect,E::IGraphicsEffectSource,OpticalEffectInterop> {
    bool transform=true;
    winrt::hstring name{L"LensTransform"};
    E::IGraphicsEffectSource source{nullptr};
    float3x2 matrix=winrt::Windows::Foundation::Numerics::float3x2::identity();
    winrt::hstring Name() const {return name;}
    void Name(winrt::hstring const& value) {name=value;}
    HRESULT STDMETHODCALLTYPE GetEffectId(GUID* id) noexcept override {
        if(!id)return E_POINTER;
        *id=transform?CLSID_D2D12DAffineTransform:CLSID_D2D1Saturation;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(LPCWSTR property,UINT* index,UINT* mapping) noexcept override {
        if(!property||!index||!mapping)return E_POINTER;
        if(std::wstring_view(property)!=L"TransformMatrix")return E_INVALIDARG;
        *index=D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX;*mapping=1;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT* count) noexcept override {if(!count)return E_POINTER;*count=transform?4:1;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetProperty(UINT index,void** value) noexcept override {
        if(!value)return E_POINTER;
        *value=nullptr;
        try {
            F::IInspectable boxed{nullptr};
            if(!transform) {if(index)return E_INVALIDARG;boxed=F::PropertyValue::CreateSingle(1.f);}
            else if(index==0)boxed=F::PropertyValue::CreateUInt32(D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_LINEAR);
            else if(index==1)boxed=F::PropertyValue::CreateUInt32(D2D1_BORDER_MODE_HARD);
            else if(index==2) {
                const float data[]{matrix.m11,matrix.m12,matrix.m21,matrix.m22,matrix.m31,matrix.m32};
                boxed=F::PropertyValue::CreateSingleArray(data);
            } else if(index==3)boxed=F::PropertyValue::CreateSingle(0.f);
            else return E_INVALIDARG;
            auto property=boxed.as<F::IPropertyValue>();*value=winrt::detach_abi(property);return S_OK;
        } catch(...) {return winrt::to_hresult();}
    }
    HRESULT STDMETHODCALLTYPE GetSource(UINT index,void** value) noexcept override {
        if(!value)return E_POINTER;
        *value=nullptr;if(index||!source)return E_INVALIDARG;
        auto copy=source;*value=winrt::detach_abi(copy);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) noexcept override {if(!count)return E_POINTER;*count=1;return S_OK;}
};

float distance(const OpticalLens& lens,float x,float y) {
    const float radius=std::clamp(lens.radius,0.f,std::min(lens.right-lens.left,lens.bottom-lens.top)*.5f);
    const float qx=std::abs(x-(lens.left+lens.right)*.5f)-(lens.right-lens.left)*.5f+radius;
    const float qy=std::abs(y-(lens.top+lens.bottom)*.5f)-(lens.bottom-lens.top)*.5f+radius;
    return std::hypot(std::max(qx,0.f),std::max(qy,0.f))+std::min(std::max(qx,qy),0.f)-radius;
}
float2 sample(const OpticalLens& lens,float x,float y) {
    const float depth=std::max(0.f,-distance(lens,x,y));
    const float band=std::max(18.f,std::min(72.f,lens.radius+24.f));
    const float weight=std::max(0.f,1.f-depth/band);
    const float dx=(distance(lens,x+.25f,y)-distance(lens,x-.25f,y))*2;
    const float dy=(distance(lens,x,y+.25f)-distance(lens,x,y-.25f))*2;
    const float length=std::hypot(dx,dy);
    if(length<.0001f)return {x,y};
    const float offset=lens.strength*weight*weight;
    return {x-dx/length*offset,y-dy/length*offset};
}
float3x2 affine(const OpticalLens& lens,float x,float y) {
    const auto center=sample(lens,x,y),xp=sample(lens,x+.5f,y),xm=sample(lens,x-.5f,y),yp=sample(lens,x,y+.5f),ym=sample(lens,x,y-.5f);
    const float a=xp.x-xm.x,b=yp.x-ym.x,c=xp.y-xm.y,d=yp.y-ym.y;
    const float determinant=a*d-b*c;
    if(!std::isfinite(determinant)||std::abs(determinant)<.1f)return {1,0,0,1,x-center.x,y-center.y};
    const float ia=d/determinant,ib=-b/determinant,ic=-c/determinant,id=a/determinant;
    return {ia,ic,ib,id,x-ia*center.x-ib*center.y,y-ic*center.x-id*center.y};
}
bool valid_lens(const OpticalLens& lens,UINT width,UINT height) {
    for(float value:{lens.left,lens.top,lens.right,lens.bottom,lens.radius,lens.strength})if(!std::isfinite(value))return false;
    return lens.left>=0&&lens.top>=0&&lens.right<=width&&lens.bottom<=height&&lens.right>lens.left&&lens.bottom>lens.top&&
        lens.radius>=0&&lens.radius<=std::min(lens.right-lens.left,lens.bottom-lens.top)*.5f&&lens.strength>=0&&lens.strength<=24;
}
}

struct OpticalSurface::Impl {
    DWORD thread=0;HWND window=nullptr;UINT width=0,height=0;unsigned zone_count=0;
    bool ro_initialized=false,host_enabled=false;
    winrt::Windows::System::DispatcherQueueController queue{nullptr};
    C::Compositor compositor{nullptr};
    C::Desktop::DesktopWindowTarget target{nullptr};
    C::ContainerVisual root{nullptr},lens_root{nullptr};
    C::SpriteVisual ui{nullptr},base{nullptr};
    C::CompositionBackdropBrush backdrop{nullptr};
    C::CompositionVisualSurface projection{nullptr};
    C::CompositionSurfaceBrush sample_source{nullptr};
    C::CompositionEffectFactory effect_factory{nullptr};
    std::vector<OpticalLens> lenses;
    std::wstring failure,steps;
    ~Impl() {
        release();
        if(queue) {try {queue.ShutdownQueueAsync();}catch(...){}queue=nullptr;}
        if(ro_initialized)RoUninitialize();
    }
    void record(const wchar_t* operation,HRESULT result) {
        wchar_t code[16]{};swprintf_s(code,L"%08lX",static_cast<unsigned long>(result));
        if(!steps.empty())steps+=L"\n";
        steps+=std::wstring(operation)+L"=0x"+code;
    }
    void checked(const wchar_t* operation,HRESULT result) {record(operation,result);winrt::check_hresult(result);}
    bool same_thread(std::wstring& error) {
        if(thread&&thread!=GetCurrentThreadId()){error=L"OpticalSurface must remain on its HWND thread.";return false;}return true;
    }
    void release() noexcept {
        try {if(target)target.Root(nullptr);} catch(...) {}
        ui=nullptr;base=nullptr;lens_root=nullptr;root=nullptr;effect_factory=nullptr;sample_source=nullptr;projection=nullptr;backdrop=nullptr;target=nullptr;
        try {if(compositor)compositor.Close();} catch(...) {}
        compositor=nullptr;
        if(host_enabled&&window&&IsWindow(window)){const BOOL disabled=FALSE;DwmSetWindowAttribute(window,DWMWA_USE_HOSTBACKDROPBRUSH,&disabled,sizeof(disabled));}
        host_enabled=false;window=nullptr;width=height=0;zone_count=0;lenses.clear();
    }
    C::CompositionEffectBrush brush(const float3x2& matrix) {
        auto result=effect_factory.CreateBrush();
        result.SetSourceParameter(L"Backdrop",sample_source);
        result.Properties().InsertMatrix3x2(L"LensTransform.TransformMatrix",matrix);
        return result;
    }
    C::CompositionGeometricClip rounded(float left,float top,float w,float h,float radius) {
        auto geometry=compositor.CreateRoundedRectangleGeometry();geometry.Offset({left,top});geometry.Size({w,h});geometry.CornerRadius({radius,radius});
        return compositor.CreateGeometricClip(geometry);
    }
    void update_size(UINT w,UINT h) {
        width=w;height=h;root.Size({static_cast<float>(w),static_cast<float>(h)});ui.Size(root.Size());base.Size(root.Size());
        projection.SourceSize(root.Size());
        root.Clip(rounded(0,0,static_cast<float>(w),static_cast<float>(h),16.f*GetDpiForWindow(window)/96.f));
    }
    void rebuild(const std::vector<OpticalLens>& input) {
        auto replacement=compositor.CreateContainerVisual();replacement.Size(root.Size());unsigned count=0;
        // A small retained mesh approximates a varying sample displacement.
        // Each zone has a real affine HostBackdrop sample, not a painted rim.
        // Maximum 8 lenses * 6 * 6 = 288 zones; no CPU frame loop is required.
        for(const auto& lens:input) {
            auto group=compositor.CreateContainerVisual();group.Size(root.Size());
            group.Clip(rounded(lens.left,lens.top,lens.right-lens.left,lens.bottom-lens.top,lens.radius));
            constexpr unsigned columns=6,rows=6;
            for(unsigned y=0;y<rows;++y)for(unsigned x=0;x<columns;++x) {
                const float left=lens.left+(lens.right-lens.left)*x/columns,right=lens.left+(lens.right-lens.left)*(x+1)/columns;
                const float top=lens.top+(lens.bottom-lens.top)*y/rows,bottom=lens.top+(lens.bottom-lens.top)*(y+1)/rows;
                const float cx=(left+right)*.5f,cy=(top+bottom)*.5f;
                auto zone=compositor.CreateSpriteVisual();zone.Size(root.Size());zone.Brush(brush(affine(lens,cx,cy)));
                auto clip=compositor.CreateInsetClip(left,top,width-right,height-bottom);zone.Clip(clip);
                group.Children().InsertAtTop(zone);++count;
            }
            replacement.Children().InsertAtTop(group);
        }
        if(lens_root)root.Children().Remove(lens_root);
        root.Children().InsertBelow(replacement,ui);lens_root=replacement;zone_count=count;lenses=input;
    }
    bool exception(std::wstring& error,const wchar_t* operation) {
        try {throw;}catch(const winrt::hresult_error& issue){record(operation,issue.code());failure=std::wstring(operation)+L": "+std::wstring(issue.message());}
        catch(const std::exception&){failure=std::wstring(operation)+L": memory or C++ error.";}
        catch(...){failure=std::wstring(operation)+L": unknown error.";}
        error=failure;return false;
    }
};

OpticalSurface::OpticalSurface():impl_(std::make_unique<Impl>()) {}
OpticalSurface::~OpticalSurface()=default;
bool OpticalSurface::attach(HWND window,IDXGISwapChain* ui_surface,UINT width,UINT height,std::wstring& error) {
    auto& p=*impl_;error.clear();if(!p.same_thread(error))return false;p.release();p.failure.clear();p.steps.clear();
    if(!IsWindow(window)||!ui_surface||!width||!height||width>8192||height>8192){error=p.failure=L"Invalid OpticalSurface parameters.";return false;}
    p.thread=GetCurrentThreadId();p.window=window;
    try {
        HIGHCONTRASTW contrast{sizeof(contrast),0,nullptr};
        if(SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0)&&(contrast.dwFlags&HCF_HIGHCONTRASTON))winrt::throw_hresult(E_ACCESSDENIED);
        if(!p.ro_initialized){p.checked(L"RoInitialize",RoInitialize(RO_INIT_SINGLETHREADED));p.ro_initialized=true;}
        if(!winrt::Windows::UI::ViewManagement::UISettings().AdvancedEffectsEnabled())winrt::throw_hresult(E_ACCESSDENIED);
        if(!winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) {
            DispatcherQueueOptions options{sizeof(options),DQTYPE_THREAD_CURRENT,DQTAT_COM_NONE};
            p.checked(L"DispatcherQueue",CreateDispatcherQueueController(options,reinterpret_cast<PDISPATCHERQUEUECONTROLLER*>(winrt::put_abi(p.queue))));
        }
        const BOOL enabled=TRUE;p.checked(L"DWMWA_USE_HOSTBACKDROPBRUSH",DwmSetWindowAttribute(window,DWMWA_USE_HOSTBACKDROPBRUSH,&enabled,sizeof(enabled)));p.host_enabled=true;
        p.compositor=C::Compositor();p.record(L"Compositor",S_OK);
        p.backdrop=p.compositor.CreateHostBackdropBrush();p.record(L"CreateHostBackdropBrush",S_OK);
        auto passthrough=winrt::make_self<AffineEffect>();passthrough->transform=false;passthrough->source=C::CompositionEffectSourceParameter(L"Backdrop");
        auto passthrough_factory=p.compositor.CreateEffectFactory(passthrough.as<E::IGraphicsEffect>());
        auto passthrough_brush=passthrough_factory.CreateBrush();passthrough_brush.SetSourceParameter(L"Backdrop",p.backdrop);p.record(L"HostBackdrop->identity Saturation",S_OK);
        p.root=p.compositor.CreateContainerVisual();p.base=p.compositor.CreateSpriteVisual();p.base.Size({static_cast<float>(width),static_cast<float>(height)});p.base.Brush(passthrough_brush);p.root.Children().InsertAtBottom(p.base);
        p.projection=p.compositor.CreateVisualSurface();p.projection.SourceVisual(p.base);p.projection.SourceSize(p.base.Size());p.record(L"CompositionVisualSurface.SourceVisual(HostBackdrop)",S_OK);
        p.sample_source=p.compositor.CreateSurfaceBrush(p.projection);p.sample_source.Stretch(C::CompositionStretch::None);
        auto effect=winrt::make_self<AffineEffect>();effect->source=C::CompositionEffectSourceParameter(L"Backdrop");
        const std::vector<winrt::hstring> animatable{L"LensTransform.TransformMatrix"};
        p.effect_factory=p.compositor.CreateEffectFactory(effect.as<E::IGraphicsEffect>(),animatable);
        p.record(L"CreateEffectFactory(AffineTransform2D)",S_OK);
        p.checked(L"EffectFactory.ExtendedError",p.effect_factory.ExtendedError());
        C::ICompositionSurface surface{nullptr};
        p.checked(L"CreateCompositionSurfaceForSwapChain",p.compositor.as<OpticalCompositorInterop>()->CreateCompositionSurfaceForSwapChain(ui_surface,winrt::put_abi(surface)));
        p.ui=p.compositor.CreateSpriteVisual();auto ui_brush=p.compositor.CreateSurfaceBrush(surface);ui_brush.Stretch(C::CompositionStretch::Fill);p.ui.Brush(ui_brush);p.root.Children().InsertAtTop(p.ui);
        p.checked(L"CreateDesktopWindowTarget",p.compositor.as<OpticalDesktopInterop>()->CreateDesktopWindowTarget(window,TRUE,winrt::put_abi(p.target)));
        p.update_size(width,height);p.target.Root(p.root);p.record(L"DesktopWindowTarget.Root(rounded)",S_OK);
        return true;
    } catch(...) {const bool result=p.exception(error,L"OpticalSurface.attach");p.release();return result;}
}
bool OpticalSurface::resize(UINT width,UINT height,std::wstring& error) {
    auto& p=*impl_;error.clear();if(!p.same_thread(error))return false;
    if(!p.target||!width||!height||width>8192||height>8192){error=L"Invalid OpticalSurface resize.";return false;}
    try {const auto saved=p.lenses;p.update_size(width,height);std::vector<OpticalLens> clipped;for(auto lens:saved){lens.right=std::min(lens.right,static_cast<float>(width));lens.bottom=std::min(lens.bottom,static_cast<float>(height));lens.radius=std::min(lens.radius,std::min(lens.right-lens.left,lens.bottom-lens.top)*.5f);if(valid_lens(lens,width,height))clipped.push_back(lens);}p.rebuild(clipped);return true;}
    catch(...){return p.exception(error,L"OpticalSurface.resize");}
}
bool OpticalSurface::set_lenses(const std::vector<OpticalLens>& lenses,std::wstring& error) {
    auto& p=*impl_;error.clear();if(!p.same_thread(error))return false;
    if(!p.target||lenses.size()>8||std::any_of(lenses.begin(),lenses.end(),[&](const auto& lens){return !valid_lens(lens,p.width,p.height);})){error=L"Invalid lenses: maximum 8, finite rectangles within the window, strength 0..24 pixels.";return false;}
    if(lenses==p.lenses)return true;
    try {p.rebuild(lenses);return true;}catch(...){return p.exception(error,L"OpticalSurface.set_lenses");}
}
void OpticalSurface::release() noexcept {impl_->release();}
bool OpticalSurface::attached() const noexcept {return static_cast<bool>(impl_->target);}
unsigned OpticalSurface::zones() const noexcept {return impl_->zone_count;}
std::wstring OpticalSurface::error() const {return impl_->failure;}
std::wstring OpticalSurface::diagnostics() const {return impl_->steps;}
}
