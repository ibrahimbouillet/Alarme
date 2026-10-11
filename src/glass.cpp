// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#include "glass.h"
#include "ui_ids.h"
#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <new>
#include <cstring>

namespace alarmapp::ui {
namespace {
template<class T> void release(T*& p) {if(p){p->Release();p=nullptr;}}
D2D1_COLOR_F rgba(unsigned c,float a=1) {return D2D1::ColorF(((c>>16)&255)/255.f,((c>>8)&255)/255.f,(c&255)/255.f,a);}
D2D1::Matrix3x2F matrix(const D2D1_MATRIX_3X2_F& m){return {m._11,m._12,m._21,m._22,m._31,m._32};}
}
struct Glass::Impl {
    ID2D1Factory* factory=nullptr;
    IDWriteFactory* write=nullptr;
    ID2D1RenderTarget* target=nullptr;
    ID2D1DeviceContext* context=nullptr; // borrowed from target
    ID2D1Device* d2d_device=nullptr;
    ID2D1SolidColorBrush* brush=nullptr;
    ID2D1Bitmap1* own_canvas=nullptr;
    ID2D1Effect* own_luminance=nullptr;
    ID2D1Effect* own_contrast=nullptr;
    ID2D1ImageBrush* adaptive_brush=nullptr;
    ID2D1PathGeometry* waves[2]{};
    float wave_width=0,wave_height=0;
    UINT canvas_dpi=0;
    ID3D11Device* gpu=nullptr;
    IDXGISwapChain1* chain=nullptr;
    IDCompositionDevice* composition=nullptr;
    IDCompositionTarget* destination=nullptr;
    IDCompositionVisual* visual=nullptr;
    HWND window=nullptr;
    UINT pixel_width=0,pixel_height=0;
    std::map<int,IDWriteTextFormat*> formats;
    struct Hover {float value=0,target=0,velocity=0,pressure=0,pressure_target=0,pressure_velocity=0;
        float pull_x=0,pull_y=0,pull_target_x=0,pull_target_y=0,pull_velocity_x=0,pull_velocity_y=0;};
    std::map<int,Hover> hovers;
    struct Material {ID2D1Bitmap1* height=nullptr;ID2D1Effect* specular=nullptr;UINT width=0,height_pixels=0,dpi=0;};
    std::map<int,Material> materials;
    struct Lens {
        ID2D1Bitmap1* normals=nullptr;
        ID2D1Effect* positioned=nullptr;
        ID2D1Effect* normal_transform=nullptr;
        ID2D1Effect* displacement=nullptr;
        ID2D1RoundedRectangleGeometry* mask=nullptr;
        UINT width=0,height=0,dpi=0;
        float logical_width=0,logical_height=0,radius=0;
    };
    std::map<int,Lens> lenses;
    float light_x=290,light_y=330;
    int drag_id=0;
    float drag_origin_x=0,drag_origin_y=0;
    bool keyboard_focus=true;
    bool animations=true,contrast=false,dc_mode=false;
    ULONGLONG previous=0,transition_start=0,device_reveal_start=0;
    unsigned long long frame_count=0;
    unsigned long long lighting_count=0;
    unsigned long long refraction_count=0;
    bool refract_frame=false;
    unsigned generation_count=0;
    std::wstring failure;
    ~Impl() {
        discard_gpu();
        for(auto& [key,f]:formats){(void)key;release(f);} release(write);release(factory);
    }
    void discard_target() {
        for(auto& [id,m]:materials){(void)id;release(m.specular);release(m.height);}materials.clear();
        for(auto& [id,l]:lenses){(void)id;discard_lens(l);}lenses.clear();
        release(adaptive_brush);release(own_contrast);release(own_luminance);release(own_canvas);canvas_dpi=0;
        for(auto*& wave:waves)release(wave);
        wave_width=wave_height=0;
        context=nullptr;release(brush);release(target);
    }
    static void discard_lens(Lens& lens) {
        release(lens.displacement);release(lens.normal_transform);release(lens.positioned);release(lens.normals);release(lens.mask);
        lens={};
    }
    void discard_gpu() {
        discard_target();
        if(destination) destination->SetRoot(nullptr);
        if(composition) composition->Commit();
        release(visual);release(destination);release(composition);release(chain);release(d2d_device);release(gpu);
        pixel_width=pixel_height=0;window=nullptr;dc_mode=false;
        previous=0;transition_start=0;device_reveal_start=0;
        drag_id=0;drag_origin_x=drag_origin_y=0;
        for(auto& [id,h]:hovers){(void)id;h={};}
    }
    bool checked(HRESULT hr,const wchar_t* operation) {
        if(SUCCEEDED(hr)) return true;
        wchar_t hex[16]{};swprintf_s(hex,L"%08lX",static_cast<unsigned long>(hr));
        failure=std::wstring(operation)+L" (0x"+hex+L")";return false;
    }
    bool shared_resources() {
        if(!factory) {
            ID2D1Factory1* modern=nullptr;
            if(!checked(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,&modern),L"Direct2D"))return false;
            factory=modern;
        }
        if(!write&&!checked(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&write)),L"DirectWrite")) return false;
        return true;
    }
    bool target_brush(UINT dpi) {
        target->SetDpi(static_cast<float>(dpi),static_cast<float>(dpi));
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        return checked(target->CreateSolidColorBrush(rgba(0xffffff),&brush),L"Direct2D brush");
    }
    bool gpu_resources(HWND hwnd,UINT dpi) {
        RECT rect{};
        if(!IsWindow(hwnd)||!GetClientRect(hwnd,&rect)){failure=L"Invalid graphics window";discard_gpu();return false;}
        const auto w=static_cast<UINT>(std::max<LONG>(1,rect.right));
        const auto h=static_cast<UINT>(std::max<LONG>(1,rect.bottom));
        if(!shared_resources()) return false;
        if(dc_mode||window!=hwnd) discard_gpu();
        if(!gpu) {
            auto hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&gpu,nullptr,nullptr);
            if(FAILED(hr)) hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&gpu,nullptr,nullptr);
            if(!checked(hr,L"Graphics device")) return false;
            IDXGIDevice* dxgi=nullptr;IDXGIAdapter* adapter=nullptr;IDXGIFactory2* dxgi_factory=nullptr;
            hr=gpu->QueryInterface(__uuidof(IDXGIDevice),reinterpret_cast<void**>(&dxgi));
            if(SUCCEEDED(hr)) hr=dxgi->GetAdapter(&adapter);
            if(SUCCEEDED(hr)) hr=adapter->GetParent(__uuidof(IDXGIFactory2),reinterpret_cast<void**>(&dxgi_factory));
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width=w;desc.Height=h;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;
            desc.Scaling=DXGI_SCALING_STRETCH;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
            if(SUCCEEDED(hr)) hr=dxgi_factory->CreateSwapChainForComposition(gpu,&desc,nullptr,&chain);
            if(SUCCEEDED(hr)) hr=DCompositionCreateDevice(dxgi,__uuidof(IDCompositionDevice),reinterpret_cast<void**>(&composition));
            // One visual above native input HWNDs. No child GDI surface is composited.
            if(SUCCEEDED(hr)) hr=composition->CreateTargetForHwnd(hwnd,TRUE,&destination);
            if(SUCCEEDED(hr)) hr=composition->CreateVisual(&visual);
            if(SUCCEEDED(hr)) hr=visual->SetContent(chain);
            if(SUCCEEDED(hr)) hr=destination->SetRoot(visual);
            if(SUCCEEDED(hr)) hr=composition->Commit();
            if(SUCCEEDED(hr)) hr=static_cast<ID2D1Factory1*>(factory)->CreateDevice(dxgi,&d2d_device);
            release(dxgi_factory);release(adapter);release(dxgi);
            if(!checked(hr,L"Window composition")){discard_gpu();return false;}
            window=hwnd;pixel_width=w;pixel_height=h;++generation_count;
        }
        if(pixel_width!=w||pixel_height!=h) {
            discard_target();
            if(!checked(chain->ResizeBuffers(2,w,h,DXGI_FORMAT_B8G8R8A8_UNORM,0),L"Resize")){discard_gpu();return false;}
            pixel_width=w;pixel_height=h;
        }
        if(!target) {
            IDXGISurface* surface=nullptr;
            auto hr=chain->GetBuffer(0,__uuidof(IDXGISurface),reinterpret_cast<void**>(&surface));
            ID2D1Bitmap1* bitmap=nullptr;
            if(SUCCEEDED(hr)) hr=d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&context);
            const auto properties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),static_cast<float>(dpi),static_cast<float>(dpi));
            if(SUCCEEDED(hr)) hr=context->CreateBitmapFromDxgiSurface(surface,&properties,&bitmap);
            if(SUCCEEDED(hr)){context->SetTarget(bitmap);target=context;}
            else {release(context);}
            release(bitmap);
            release(surface);
            if(!checked(hr,L"Direct2D surface")||!target_brush(dpi)){discard_gpu();return false;}
        }
        target->SetDpi(static_cast<float>(dpi),static_cast<float>(dpi));
        return true;
    }
    bool canvas_resources(UINT dpi) {
        if(!context)return true;
        if(dpi<48||dpi>768||pixel_width>8192||pixel_height>8192||
           static_cast<std::uint64_t>(pixel_width)*pixel_height>32ull*1024*1024)
            return checked(E_INVALIDARG,L"Internal canvas dimensions");
        if(own_canvas&&canvas_dpi==dpi)return true;
        // Effects retain their source. Drop the old graph before its bitmap.
        for(auto& [id,l]:lenses){(void)id;discard_lens(l);}lenses.clear();
        release(adaptive_brush);release(own_contrast);release(own_luminance);release(own_canvas);
        const auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(dpi),static_cast<float>(dpi));
        if(!checked(context->CreateBitmap(D2D1::SizeU(pixel_width,pixel_height),nullptr,0,props,&own_canvas),
                    L"Internal GPU canvas"))return false;
        auto hr=context->CreateEffect(CLSID_D2D1ColorMatrix,&own_luminance);
        if(SUCCEEDED(hr))hr=context->CreateEffect(CLSID_D2D1DiscreteTransfer,&own_contrast);
        if(SUCCEEDED(hr)) {
            own_luminance->SetInput(0,own_canvas);
            // ColorMatrix unpremultiplies first: translucent white still has
            // luminance 1, independently of the app surface's alpha.
            const auto matrix=D2D1::Matrix5x4F(.2126f,.2126f,.2126f,0,
                .7152f,.7152f,.7152f,0,.0722f,.0722f,.0722f,0,
                0,0,0,0,0,0,0,1);
            hr=own_luminance->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX,matrix);
            own_contrast->SetInputEffect(0,own_luminance);
        }
        const float inverse[2]={1.f,0.f},opaque[2]={1.f,1.f};
        if(SUCCEEDED(hr))hr=own_contrast->SetValue(D2D1_DISCRETETRANSFER_PROP_RED_TABLE,inverse);
        if(SUCCEEDED(hr))hr=own_contrast->SetValue(D2D1_DISCRETETRANSFER_PROP_GREEN_TABLE,inverse);
        if(SUCCEEDED(hr))hr=own_contrast->SetValue(D2D1_DISCRETETRANSFER_PROP_BLUE_TABLE,inverse);
        if(SUCCEEDED(hr))hr=own_contrast->SetValue(D2D1_DISCRETETRANSFER_PROP_ALPHA_TABLE,opaque);
        ID2D1Image* contrast_image=nullptr;
        if(SUCCEEDED(hr))own_contrast->GetOutput(&contrast_image);
        if(SUCCEEDED(hr))hr=context->CreateImageBrush(contrast_image,
            D2D1::ImageBrushProperties(D2D1::RectF(0,0,pixel_width*96.f/dpi,pixel_height*96.f/dpi),
                D2D1_EXTEND_MODE_CLAMP,D2D1_EXTEND_MODE_CLAMP,D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR),
            D2D1::BrushProperties(),&adaptive_brush);
        release(contrast_image);
        if(!checked(hr,L"Internal canvas contrast"))return false;
        canvas_dpi=dpi;return true;
    }
    HRESULT build_lens(const GlassControl& control,float radius,Lens& lens) {
        const float width=control.right-control.left,height=control.bottom-control.top;
        if(!std::isfinite(width)||!std::isfinite(height)||width<=0||height<=0||width>4096||height>2048)
            return E_INVALIDARG;
        const auto w=static_cast<UINT>(std::ceil(width*canvas_dpi/96.f));
        const auto h=static_cast<UINT>(std::ceil(height*canvas_dpi/96.f));
        if(!w||!h||w>8192||h>4096||static_cast<std::uint64_t>(w)*h>1024ull*1024)
            return E_INVALIDARG;
        if(lens.normals&&lens.width==w&&lens.height==h&&lens.dpi==canvas_dpi&&
           lens.logical_width==width&&lens.logical_height==height&&lens.radius==radius)return S_OK;
        discard_lens(lens);
        try {
            // This is a synthetic normal map of our own rounded control, not a
            // screenshot. Float channels keep an exactly neutral .5 at rest.
            std::vector<D2D1_VECTOR_4F> pixels(static_cast<std::size_t>(w)*h);
            const float bevel=std::max(3.f,std::min(15.f,height*.23f));
            const float half_x=width*.5f,half_y=height*.5f;
            for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x) {
                const float px=(x+.5f)*96.f/canvas_dpi-half_x;
                const float py=(y+.5f)*96.f/canvas_dpi-half_y;
                const float qx=std::abs(px)-(half_x-radius),qy=std::abs(py)-(half_y-radius);
                const float a=std::max(qx,0.f),b=std::max(qy,0.f),length=std::hypot(a,b);
                const float distance=length+std::min(std::max(qx,qy),0.f)-radius;
                float nx=0,ny=0;
                if(length>0){nx=a/length;ny=b/length;}
                else if(qx>qy)nx=1;else ny=1;
                nx*=px<0?-1.f:1.f;ny*=py<0?-1.f:1.f;
                const float depth=std::clamp(-distance/bevel,0.f,1.f);
                const float offset=distance<0&&depth<1?5.2f*std::sin(depth*3.14159265359f):0;
                pixels[static_cast<std::size_t>(y)*w+x]=D2D1::Vector4F(
                    std::clamp(.5f-nx*offset/12.f,0.f,1.f),
                    std::clamp(.5f-ny*offset/12.f,0.f,1.f),.5f,1.f);
            }
            const auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_R32G32B32A32_FLOAT,D2D1_ALPHA_MODE_PREMULTIPLIED),
                static_cast<float>(canvas_dpi),static_cast<float>(canvas_dpi));
            auto hr=context->CreateBitmap(D2D1::SizeU(w,h),pixels.data(),w*sizeof(D2D1_VECTOR_4F),props,&lens.normals);
            if(SUCCEEDED(hr))hr=context->CreateEffect(CLSID_D2D12DAffineTransform,&lens.positioned);
            if(SUCCEEDED(hr))hr=context->CreateEffect(CLSID_D2D1ColorMatrix,&lens.normal_transform);
            if(SUCCEEDED(hr))hr=context->CreateEffect(CLSID_D2D1DisplacementMap,&lens.displacement);
            if(SUCCEEDED(hr))hr=factory->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(D2D1::RectF(0,0,width,height),radius,radius),&lens.mask);
            if(SUCCEEDED(hr)) {
                lens.positioned->SetInput(0,lens.normals);
                hr=lens.positioned->SetValue(D2D1_2DAFFINETRANSFORM_PROP_INTERPOLATION_MODE,D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_LINEAR);
                lens.displacement->SetInput(0,own_canvas);
                lens.normal_transform->SetInputEffect(0,lens.positioned);
                lens.displacement->SetInputEffect(1,lens.normal_transform);
            }
            // A UNORM8 intermediate would quantize neutral .5 to 128/255 and
            // introduce a small displacement, blurring an otherwise flat center.
            if(SUCCEEDED(hr))hr=lens.positioned->SetValue(D2D1_PROPERTY_PRECISION,D2D1_BUFFER_PRECISION_16BPC_FLOAT);
            if(SUCCEEDED(hr))hr=lens.normal_transform->SetValue(D2D1_PROPERTY_PRECISION,D2D1_BUFFER_PRECISION_16BPC_FLOAT);
            if(SUCCEEDED(hr))hr=lens.normal_transform->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT,TRUE);
            if(SUCCEEDED(hr))hr=lens.displacement->SetValue(D2D1_PROPERTY_PRECISION,D2D1_BUFFER_PRECISION_16BPC_FLOAT);
            if(SUCCEEDED(hr))hr=lens.displacement->SetValue(D2D1_DISPLACEMENTMAP_PROP_X_CHANNEL_SELECT,D2D1_CHANNEL_SELECTOR_R);
            if(SUCCEEDED(hr))hr=lens.displacement->SetValue(D2D1_DISPLACEMENTMAP_PROP_Y_CHANNEL_SELECT,D2D1_CHANNEL_SELECTOR_G);
            if(FAILED(hr)){discard_lens(lens);return hr;}
            lens.width=w;lens.height=h;lens.dpi=canvas_dpi;
            lens.logical_width=width;lens.logical_height=height;lens.radius=radius;
            return S_OK;
        } catch(const std::bad_alloc&) {discard_lens(lens);return E_OUTOFMEMORY;}
    }
    void refraction(const GlassControl& control,D2D1_RECT_F rectangle,float radius,float level,float pressure,
                    const D2D1_MATRIX_3X2_F& gelatin) {
        if(!refract_frame||!context||!own_canvas||contrast||!control.enabled)return;
        if(lenses.size()>=64&&lenses.find(control.id)==lenses.end()) {
            checked(E_INVALIDARG,L"Optical surface count");return;
        }
        auto& lens=lenses[control.id];
        if(!checked(build_lens(control,radius,lens),L"Glass normal map"))return;
        const float sx=(rectangle.right-rectangle.left)/lens.logical_width;
        const float sy=(rectangle.bottom-rectangle.top)/lens.logical_height;
        const auto transform=D2D1::Matrix3x2F::Scale(sx,sy)*D2D1::Matrix3x2F::Translation(rectangle.left,rectangle.top)*gelatin;
        auto hr=lens.positioned->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX,transform);
        const float determinant=gelatin._11*gelatin._22-gelatin._12*gelatin._21;
        if(!std::isfinite(determinant)||determinant<.5f){checked(E_INVALIDARG,L"Elastic glass geometry");return;}
        const float xx=gelatin._22/determinant,xy=-gelatin._21/determinant;
        const float yx=-gelatin._12/determinant,yy=gelatin._11/determinant;
        const auto normals=D2D1::Matrix5x4F(xx,xy,0,0,yx,yy,0,0,0,0,1,0,0,0,0,1,
            .5f*(1-xx-yx),.5f*(1-xy-yy),0,0);
        if(SUCCEEDED(hr))hr=lens.normal_transform->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX,normals);
        if(SUCCEEDED(hr))hr=lens.displacement->SetValue(D2D1_DISPLACEMENTMAP_PROP_SCALE,12.f*(1+.10f*level+.18f*pressure));
        if(!checked(hr,L"Optical glass deformation"))return;
        // COPY also applies when PopLayer resolves a geometry mask. SOURCE_OVER
        // here would add a second copy of the translucent internal background.
        const auto old_blend=context->GetPrimitiveBlend();
        D2D1_MATRIX_3X2_F world{};target->GetTransform(&world);
        auto inverse=matrix(gelatin);inverse.Invert();
        target->SetTransform(inverse*world);
        const auto point=[&](float x,float y){return D2D1::Point2F(x*gelatin._11+y*gelatin._21+gelatin._31,x*gelatin._12+y*gelatin._22+gelatin._32);};
        const D2D1_POINT_2F corners[]={point(rectangle.left,rectangle.top),point(rectangle.right,rectangle.top),point(rectangle.left,rectangle.bottom),point(rectangle.right,rectangle.bottom)};
        auto bounds=D2D1::RectF(corners[0].x,corners[0].y,corners[0].x,corners[0].y);
        for(const auto p:corners){bounds.left=std::min(bounds.left,p.x);bounds.top=std::min(bounds.top,p.y);bounds.right=std::max(bounds.right,p.x);bounds.bottom=std::max(bounds.bottom,p.y);}
        context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
        context->PushLayer(D2D1::LayerParameters1(bounds,lens.mask,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,transform),nullptr);
        const auto origin=D2D1::Point2F(bounds.left,bounds.top);
        context->DrawImage(lens.displacement,&origin,&bounds,D2D1_INTERPOLATION_MODE_LINEAR,D2D1_COMPOSITE_MODE_SOURCE_COPY);
        context->PopLayer();context->SetPrimitiveBlend(old_blend);
        target->SetTransform(world);
        ++refraction_count;
    }
    void preferences() {
        BOOL animation=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animation,0);
        HIGHCONTRASTW high{sizeof(high),0,nullptr};
        contrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(high),&high,0)&&(high.dwFlags&HCF_HIGHCONTRASTON);
        animations=animation&&!contrast;
    }
    void color(unsigned c,float a=1) {brush->SetColor(rgba(c,a));}
    IDWriteTextFormat* format(float size,bool bold,bool center) {
        const int key=static_cast<int>(size*10)+(bold?10000:0);
        auto& f=formats[key];
        if(!f) {
            if(!checked(write->CreateTextFormat(L"Segoe UI Variable Display",nullptr,bold?DWRITE_FONT_WEIGHT_SEMI_BOLD:DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"en-US",&f),L"Text format"))return nullptr;
            if(!checked(f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP),L"Word wrapping"))return nullptr;
        }
        if(!checked(f->SetTextAlignment(center?DWRITE_TEXT_ALIGNMENT_CENTER:DWRITE_TEXT_ALIGNMENT_LEADING),L"Text alignment"))return nullptr;
        if(!checked(f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER),L"Paragraph alignment"))return nullptr;
        return f;
    }
    void text(const std::wstring& value,D2D1_RECT_F r,float size,unsigned c=0xffffff,bool bold=false,bool center=false,bool adaptive=false) {
        auto* f=format(size,bold,center);if(!f)return;
        color(contrast?0xffffff:c);
        ID2D1Brush* ink=brush;
        if(adaptive&&!contrast&&context&&adaptive_brush){adaptive_brush->SetOpacity(.94f);ink=adaptive_brush;}
        target->DrawText(value.c_str(),static_cast<UINT32>(value.size()),f,r,ink,D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void volume_edit(const GlassControl& c,D2D1_RECT_F r,bool caret) {
        if(!c.focused||c.text.size()>32)return;
        const auto start=std::min<UINT32>(c.selection_start,static_cast<UINT32>(c.text.size()));
        const auto end=std::min<UINT32>(c.selection_end,static_cast<UINT32>(c.text.size()));
        IDWriteTextLayout* layout=nullptr;auto* f=format(16,false,true);if(!f)return;
        if(!checked(write->CreateTextLayout(c.text.data(),static_cast<UINT32>(c.text.size()),f,
                    r.right-r.left,r.bottom-r.top,&layout),L"Volume editing"))return;
        if(!caret&&end>start) {
            DWRITE_HIT_TEST_METRICS hits[32]{};UINT32 count=0;
            const auto hr=layout->HitTestTextRange(start,end-start,r.left,r.top,hits,32,&count);
            if(checked(hr,L"Volume selection")) {
                color(0x97c9f0,.35f);
                for(UINT32 i=0;i<count;++i)target->FillRoundedRectangle(D2D1::RoundedRect(
                    D2D1::RectF(hits[i].left,hits[i].top,hits[i].left+hits[i].width,hits[i].top+hits[i].height),3,3),brush);
            }
        } else if(caret&&end==start) {
            float x=0,y=0;DWRITE_HIT_TEST_METRICS hit{};
            if(checked(layout->HitTestTextPosition(start,FALSE,&x,&y,&hit),L"Volume caret")) {
                ID2D1Brush* ink=brush;color(0xdcf5ff,.95f);
                if(context&&adaptive_brush&&!contrast){adaptive_brush->SetOpacity(.95f);ink=adaptive_brush;}
                target->DrawLine(D2D1::Point2F(r.left+x,r.top+y),D2D1::Point2F(r.left+x,r.top+y+hit.height),ink,1.5f);
            }
        }
        release(layout);
    }
    void gradient(D2D1_RECT_F r,unsigned first,unsigned second,float alpha,float radius=0) {
        if(contrast){color(0x000000);target->FillRoundedRectangle(D2D1::RoundedRect(r,radius,radius),brush);color(0xffffff);target->DrawRoundedRectangle(D2D1::RoundedRect(r,radius,radius),brush,1);return;}
        D2D1_GRADIENT_STOP stops[]={{0,rgba(first,alpha)},{1,rgba(second,alpha)}};
        ID2D1GradientStopCollection* collection=nullptr;ID2D1LinearGradientBrush* g=nullptr;
        if(SUCCEEDED(target->CreateGradientStopCollection(stops,2,&collection))) {
            if(SUCCEEDED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left,r.top),D2D1::Point2F(r.right,r.bottom)),collection,&g))) target->FillRoundedRectangle(D2D1::RoundedRect(r,radius,radius),g);
        }
        release(g);release(collection);
    }
    void glow(float x,float y,float rx,float ry,unsigned c,float a) {
        if(contrast) return;
        D2D1_GRADIENT_STOP stops[]={{0,rgba(c,a)},{1,rgba(c,0)}};
        ID2D1GradientStopCollection* collection=nullptr;ID2D1RadialGradientBrush* g=nullptr;
        if(SUCCEEDED(target->CreateGradientStopCollection(stops,2,&collection))) {
            if(SUCCEEDED(target->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(x,y),D2D1::Point2F(0,0),rx,ry),collection,&g))) target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x,y),rx,ry),g);
        }
        release(g);release(collection);
    }
    void plate(D2D1_RECT_F r,float radius,float alpha=.12f,bool active=false) {
        const float cx=(r.left+r.right)*.5f,cy=(r.top+r.bottom)*.5f;
        const float dx=light_x-cx,dy=light_y-cy,length=std::sqrt(dx*dx+dy*dy+10000);
        auto shadow=r;const float sx=-dx/length*5,sy=-dy/length*5;
        shadow.left+=sx;shadow.right+=sx;shadow.top+=sy;shadow.bottom+=sy;
        color(0x040811,.09f);target->FillRoundedRectangle(D2D1::RoundedRect(shadow,radius,radius),brush);
        gradient(r,0xdcecff,0x586c88,alpha,radius);
        color(active?0xc6e9ff:0xffffff,active?.5f:.23f);target->DrawRoundedRectangle(D2D1::RoundedRect(r,radius,radius),brush,1);
        auto inner=r;inner.left+=1.5f;inner.top+=1.5f;inner.right-=1.5f;inner.bottom-=1.5f;
        color(0xffffff,.055f);target->DrawRoundedRectangle(D2D1::RoundedRect(inner,radius-1,radius-1),brush,1);
    }
    void chrome_rim(D2D1_RECT_F r,float radius,float level) {
        if(contrast)return;
        const float cx=(r.left+r.right)*.5f,cy=(r.top+r.bottom)*.5f;
        const float dx=light_x-cx,dy=light_y-cy,length=std::sqrt(dx*dx+dy*dy+100);
        const float extent=std::max(r.right-r.left,r.bottom-r.top)*.6f;
        // The far rim remains present opposite the light, with the paired dark
        // line characteristic of a clear lens; neither is a filled white patch.
        D2D1_GRADIENT_STOP stops[]={{0,rgba(0xffefd7,.53f+.06f*level)},
            {.46f,rgba(0xe2e9ed,.08f)},{.68f,rgba(0x111821,.40f)},{1,rgba(0xd5dbe4,.48f)}};
        ID2D1GradientStopCollection* collection=nullptr;ID2D1LinearGradientBrush* rim=nullptr;
        auto hr=target->CreateGradientStopCollection(stops,4,&collection);
        if(SUCCEEDED(hr))hr=target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(cx+dx/length*extent,cy+dy/length*extent),
            D2D1::Point2F(cx-dx/length*extent,cy-dy/length*extent)),collection,&rim);
        if(checked(hr,L"Glass edge")) {
            target->DrawRoundedRectangle(D2D1::RoundedRect(r,radius,radius),rim,.95f);
            auto inner=r;inner.left+=1;inner.top+=1;inner.right-=1;inner.bottom-=1;
            color(0x07111c,.20f);target->DrawRoundedRectangle(D2D1::RoundedRect(inner,std::max(0.f,radius-1),std::max(0.f,radius-1)),brush,.65f);
        }
        release(rim);release(collection);
        if(context&&adaptive_brush) {
            adaptive_brush->SetOpacity(.17f+.04f*level);
            target->DrawRoundedRectangle(D2D1::RoundedRect(r,radius,radius),adaptive_brush,.7f);
        }
    }
    void organic_background(const GlassState& state) {
        if(contrast)return;
        if(!waves[0]||wave_width!=state.width||wave_height!=state.height) {
            for(auto*& wave:waves)release(wave);
            for(int i=0;i<2;++i) {
                ID2D1GeometrySink* sink=nullptr;auto hr=factory->CreatePathGeometry(&waves[i]);
                if(SUCCEEDED(hr))hr=waves[i]->Open(&sink);
                if(SUCCEEDED(hr)) {
                    const float shift=i*73.f;
                    sink->BeginFigure(D2D1::Point2F(-40,state.height*.74f+shift),D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(state.width*.20f,state.height*.55f+shift),
                        D2D1::Point2F(state.width*.63f,state.height*.84f+shift),
                        D2D1::Point2F(state.width+40,state.height*.63f+shift)));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);hr=sink->Close();
                }
                release(sink);if(!checked(hr,L"Internal background curve"))return;
            }
            wave_width=state.width;wave_height=state.height;
        }
        for(int i=0;i<2;++i) {
            color(i?0x91b9cb:0xe9c9a5,.017f);target->DrawGeometry(waves[i],brush,26);
            color(i?0xb1cbd8:0xf0d4b2,.14f);target->DrawGeometry(waves[i],brush,1.5f);
        }
    }
    void shade(const GlassControl& c,D2D1_RECT_F r,const D2D1_MATRIX_3X2_F& gelatin) {
        if(!context||contrast||!c.enabled)return;
        auto& material=materials[c.id];
        float dpi_x=96,dpi_y=96;target->GetDpi(&dpi_x,&dpi_y);
        const float width=c.right-c.left,height=c.bottom-c.top;
        const auto w=static_cast<UINT>(std::clamp(std::ceil(width*dpi_x/96),2.f,8192.f));
        const auto h=static_cast<UINT>(std::clamp(std::ceil(height*dpi_y/96),2.f,2048.f));
        const auto actual_dpi=static_cast<UINT>(dpi_x);
        if(!material.height||material.width!=w||material.height_pixels!=h||material.dpi!=actual_dpi) {
            release(material.specular);release(material.height);
            std::vector<unsigned> pixels(static_cast<std::size_t>(w)*h);
            const float radius=std::min(height/2,24.f)*dpi_x/96;
            const float bevel=std::max(3.f,std::min(18.f,height*.24f))*dpi_x/96;
            for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x) {
                const float qx=std::abs(x+.5f-w*.5f)-(w*.5f-radius);
                const float qy=std::abs(y+.5f-h*.5f)-(h*.5f-radius);
                const float a=std::max(qx,0.f),b=std::max(qy,0.f);
                const float distance=std::sqrt(a*a+b*b)+std::min(std::max(qx,qy),0.f)-radius;
                const float edge=1-std::clamp(-distance/bevel,0.f,1.f);
                const auto z=static_cast<unsigned>(std::lround(std::sqrt(std::max(0.f,1-edge*edge))*255));
                pixels[static_cast<std::size_t>(y)*w+x]=z*0x01010101u;
            }
            const auto properties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),dpi_x,dpi_y);
            auto hr=context->CreateBitmap(D2D1::SizeU(w,h),pixels.data(),w*4,properties,&material.height);
            if(SUCCEEDED(hr))hr=context->CreateEffect(CLSID_D2D1PointSpecular,&material.specular);
            if(!checked(hr,L"Glass relief and lighting")){release(material.height);release(material.specular);return;}
            material.specular->SetInput(0,material.height);
            hr=material.specular->SetValue(D2D1_POINTSPECULAR_PROP_SURFACE_SCALE,18.f);
            if(SUCCEEDED(hr))hr=material.specular->SetValue(D2D1_POINTSPECULAR_PROP_SPECULAR_EXPONENT,42.f);
            if(SUCCEEDED(hr))hr=material.specular->SetValue(D2D1_POINTSPECULAR_PROP_COLOR,D2D1::Vector3F(.98f,.96f,.92f));
            if(!checked(hr,L"Lighting properties"))return;
            material.width=w;material.height_pixels=h;material.dpi=actual_dpi;
        }
        const float sx=(r.right-r.left)/width,sy=(r.bottom-r.top)/height;
        auto inverse=matrix(gelatin);inverse.Invert();
        const auto local_light=inverse.TransformPoint(D2D1::Point2F(light_x,light_y));
        const auto& motion=hovers[c.id];
        const float relief=18.f*(1+.12f*std::clamp(motion.pressure,0.f,1.15f)+
            .07f*std::min(1.f,std::hypot(motion.pull_x,motion.pull_y)/14.f));
        const auto light=D2D1::Vector3F((local_light.x-r.left)/sx,(local_light.y-r.top)/sy,110.f);
        const float dx=light_x-(r.left+r.right)*.5f,dy=light_y-(r.top+r.bottom)*.5f;
        const float attenuation=1/(1+(dx*dx+dy*dy)/360000.f);
        auto hr=material.specular->SetValue(D2D1_POINTSPECULAR_PROP_LIGHT_POSITION,light);
        if(SUCCEEDED(hr))hr=material.specular->SetValue(D2D1_POINTSPECULAR_PROP_SURFACE_SCALE,relief);
        if(SUCCEEDED(hr))hr=material.specular->SetValue(D2D1_POINTSPECULAR_PROP_SPECULAR_CONSTANT,.13f*attenuation);
        if(!checked(hr,L"Lighting position"))return;
        D2D1_MATRIX_3X2_F old{};target->GetTransform(&old);
        target->SetTransform(D2D1::Matrix3x2F::Scale(sx,sy)*D2D1::Matrix3x2F::Translation(r.left,r.top)*old);
        context->DrawImage(material.specular,D2D1::Point2F(0,0),D2D1_INTERPOLATION_MODE_LINEAR,D2D1_COMPOSITE_MODE_PLUS);
        ++lighting_count;
        target->SetTransform(old);
    }
    void emblem(float x,float y,float size) {
        gradient(D2D1::RectF(x,y,x+size,y+size),0x83ddf8,0xa19de9,.82f,size*.26f);
        color(0xffffff,.65f);target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x,y,x+size,y+size),size*.26f,size*.26f),brush,1);
        color(0xffffff);auto center=D2D1::Point2F(x+size*.5f,y+size*.53f);
        target->DrawEllipse(D2D1::Ellipse(center,size*.235f,size*.235f),brush,size*.042f);
        target->DrawLine(center,D2D1::Point2F(x+size*.41f,y+size*.4f),brush,size*.044f);
        target->DrawLine(center,D2D1::Point2F(x+size*.65f,y+size*.46f),brush,size*.044f);
        target->DrawLine(D2D1::Point2F(x+size*.3f,y+size*.31f),D2D1::Point2F(x+size*.4f,y+size*.25f),brush,size*.048f);
        target->DrawLine(D2D1::Point2F(x+size*.6f,y+size*.25f),D2D1::Point2F(x+size*.7f,y+size*.31f),brush,size*.048f);
    }
    void background(const GlassState& s) {
        // Premultiplied alpha is presented directly to DWM, never through a GDI DC.
        gradient(D2D1::RectF(0,0,s.width,s.height),0x182736,0x24253c,contrast?1.f:.36f,12);
        glow(s.width*.87f,100,430,390,0x8fbdde,.08f);
        glow(25,s.height-20,420,340,0xb8a4d6,.07f);
        organic_background(s);
        color(0xffffff,s.active?.22f:.12f);
        target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(.5f,.5f,s.width-.5f,s.height-.5f),11,11),brush,1);
        text(L"ALARM",D2D1::RectF(19,7,150,35),11,0xcad5e2,true);
        emblem(28,65,43);
        text(L"ALARM",D2D1::RectF(87,64,s.width-163,91),23,0xffffff,true);
        text(L"Every day, at your time.",D2D1::RectF(88,92,s.width-157,111),11,0xc5d1e2);
    }
    void content(const GlassState& s) {
        if(!s.settings) {
            text(L"Your next alarm",D2D1::RectF(30,143,s.width-30,174),21,0xffffff,true);
            const float pulse=s.ringing&&animations?.02f*(1+std::sin(GetTickCount64()/420.f)):0;
            plate(D2D1::RectF(26,195,s.width-26,452),32,.052f+pulse,s.enabled||s.ringing);
            text(s.preview?L"SOUND PREVIEW":L"DAILY ALARM",D2D1::RectF(54,218,s.width-54,242),11,0xc3d2e4,true,true);
            text(s.editing?L"Enter a time · Press Enter to save":s.audio_starting?L"Connecting to audio output…":s.preview?L"Stops automatically after 8 seconds":s.ringing?L"It is time to wake up":L"Select the time to edit it",D2D1::RectF(38,352,s.width-38,383),13,0xd2dce9,false,true);
            const auto status=s.audio_starting?L"Preparing audio":s.audio_error?L"Audio unavailable":s.preview?L"Sound preview":s.ringing?L"Alarm ringing":s.silent?L"Silent · next ring pending":s.enabled?L"Alarm enabled":L"Alarm disabled";
            auto pill=D2D1::RectF(s.width/2-116,401,s.width/2+116,434);
            gradient(pill,s.ringing?0xda7997:s.enabled?0x63b9aa:0xa9bbd5,0x8192af,.13f,17);
            color(s.audio_starting?0xc6d3e5:s.audio_error?0xffbeb9:s.ringing?0xffb1c8:s.enabled?0xa7f3dd:0xc6d3e5);
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(pill.left+21,417.5f),3,3),brush);
            text(status,D2D1::RectF(pill.left+32,pill.top,pill.right-8,pill.bottom),12,0xf1f8fc,true);
            text(s.audio_starting?L"Preparing audio playback…":s.audio_error?s.detail:s.ringing?L"STOP immediately silences the alarm.":s.next,D2D1::RectF(38,614,s.width-38,663),13,0xd2dce8,false,true);
            text(L"Close the window. ALARM keeps running in the background.",D2D1::RectF(20,s.height-38,s.width-20,s.height-14),11,0xbdcbdc,false,true);
        } else {
            text(s.diagnostics?L"Diagnostics":L"Sound and preferences",D2D1::RectF(28,143,s.width-28,174),22,0xffffff,true);
            if(s.diagnostics) {
                plate(D2D1::RectF(26,195,s.width-26,478),28,.10f);
                text(L"Tests run only when you select them.",D2D1::RectF(48,213,s.width-48,245),14,0xffffff,true);
                text(L"The locked test plays for 3 s, locks Windows,\nthen rings 10 s later and stops after 20 s.",D2D1::RectF(48,360,s.width-48,417),13,0xd3deeb);
                text(L"The 30 s test uses the normal alarm cycle:\n5 min ringing / 10 min silent, until STOP.",D2D1::RectF(48,422,s.width-48,464),12,0xd3deeb);
                text(s.detail,D2D1::RectF(32,496,s.width-32,565),12,0xd3deeb);
            } else {
                plate(D2D1::RectF(26,195,s.width-26,469),28,.055f);
                text(L"AUDIO OUTPUT",D2D1::RectF(48,210,s.width-48,232),10,0xc2d2e4,true);
                text(L"Alarm volume",D2D1::RectF(48,292,295,322),14,0xffffff,true);
                text(s.sound,D2D1::RectF(48,391,s.width-48,419),12,0xd5dfeb);
                plate(D2D1::RectF(26,484,s.width-26,558),24,.09f);
                text(L"Previous volume restored at STOP and during silent periods.",D2D1::RectF(48,529,s.width-48,548),11,0xc5d2e0);
                text(s.detail,D2D1::RectF(30,690,s.width-30,713),10,0xc5d2e0);
            }
        }
    }
    void control(const GlassControl& c,float pointer_x=-1000,float pointer_y=-1000) {
        auto r=D2D1::RectF(c.left,c.top,c.right,c.bottom);const auto id=c.id;
        const float level=std::clamp(hovers[id].value,0.f,1.15f);
        const float pressure=std::clamp(hovers[id].pressure,0.f,1.15f);
        const auto center=D2D1::Point2F((r.left+r.right)/2,(r.top+r.bottom)/2);
        if(id==CaptionMinimize||id==CaptionMaximize||id==CaptionClose) {
            if(level>.001f||c.pressed) gradient(r,id==CaptionClose?0xe07589:0xc4d9ed,0x829aba,.12f+level*.16f,12);
            color(0xffffff,c.enabled?.9f:.4f);
            if(id==CaptionMinimize) target->DrawLine(D2D1::Point2F(center.x-5,center.y+2),D2D1::Point2F(center.x+5,center.y+2),brush,1);
            else if(id==CaptionMaximize) target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(center.x-4.5f,center.y-4.5f,center.x+4.5f,center.y+4.5f),1,1),brush,1);
            else {target->DrawLine(D2D1::Point2F(center.x-4,center.y-4),D2D1::Point2F(center.x+4,center.y+4),brush,1);target->DrawLine(D2D1::Point2F(center.x+4,center.y-4),D2D1::Point2F(center.x-4,center.y+4),brush,1);}return;
        }
        if(id==Time||id==TimeInput) {
            if(level>.001f||c.focused||id==TimeInput) plate(r,22,.035f+level*.06f,c.focused);
            if(id==TimeInput&&c.focused&&c.selection_end>c.selection_start) {
                const float glyph=48,origin=center.x-c.text.size()*glyph/2;
                color(0x97c9f0,.24f);target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(origin+c.selection_start*glyph,r.top+12,origin+c.selection_end*glyph,r.bottom-12),7,7),brush);
            }
            text(c.text,r,84,c.enabled?0xffffff:0xabb7c7,true,true);
            if(id==TimeInput&&c.focused&&c.selection_start==c.selection_end) {
                const float x=center.x-c.text.size()*48/2+c.selection_start*48;color(0xdcf5ff,.9f);target->DrawLine(D2D1::Point2F(x,r.top+19),D2D1::Point2F(x,r.bottom-19),brush,2);
            }return;
        }
        if(id==Override) {
            text(L"Windows at 100% during the alarm",r,12,0xe1eaf3,true);
            return;
        }
        if(id==Startup) {
            const auto track=D2D1::RectF(r.right-45,center.y-12,r.right,center.y+12);
            gradient(track,c.checked?0x96d6e9:0x71849c,c.checked?0x80c6c8:0x64758c,c.enabled?.53f:.22f,12);
            color(0xffffff,c.enabled?.96f:.42f);target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.checked?track.right-12:track.left+12,center.y),9,9),brush);
            text(c.text,D2D1::RectF(r.left,r.top,r.right-56,r.bottom),12,c.enabled?0xe1eaf3:0x9aaabf);
            if(c.focused&&keyboard_focus){color(0xd1efff);target->DrawRoundedRectangle(D2D1::RoundedRect(track,12,12),brush,1.5f);}return;
        }
        if(id==DeviceList) return;
        const bool primary=id==Save||id==SettingsApply||id==TimeDone;
        D2D1_MATRIX_3X2_F world{};target->GetTransform(&world);
        auto gelatin=D2D1::Matrix3x2F::Identity();
        if(animations&&id!=Device&&id!=SessionVolume){
            const float half_width=(r.right-r.left)*.5f*(1+.005f*level+.012f*pressure);
            const float half_height=(r.bottom-r.top)*.5f*(1+.015f*level-.07f*pressure);
            const float y=center.y-level*1.2f+pressure*1.2f;
            r=D2D1::RectF(center.x-half_width,y-half_height,center.x+half_width,y+half_height);
            const float px=std::clamp(hovers[id].pull_x/14.f,-1.f,1.f);
            const float py=std::clamp(hovers[id].pull_y/14.f,-1.f,1.f);
            const float stretch_x=1+.075f*std::abs(px)-.03f*std::abs(py);
            const float stretch_y=1+.14f*std::abs(py)-.045f*std::abs(px);
            const auto shape=D2D1::Matrix3x2F(stretch_x,.045f*py,.12f*px,stretch_y,0,0);
            gelatin=D2D1::Matrix3x2F::Translation(-center.x,-center.y)*shape*
                D2D1::Matrix3x2F::Translation(center.x+hovers[id].pull_x*.42f,center.y+hovers[id].pull_y*.42f);
            target->SetTransform(gelatin*world);
        }
        const float optical_radius=std::min(primary?22.f:id==VolumeDown||id==VolumeUp?16.f:18.f,(c.bottom-c.top)/2);
        refraction(c,r,optical_radius,level,pressure,gelatin);
        if(primary) {
            gradient(r,0xc0e6fc,0x9fc4ec,c.enabled?.065f:.035f,22);
            color(0xf3fbff,.22f+level*.08f);target->DrawRoundedRectangle(D2D1::RoundedRect(r,22,22),brush,1);
        } else if(id==Stop) {
            gradient(r,0xc17a94,0x965f8c,.13f+level*.055f,18);color(0xffd6e3,.28f);target->DrawRoundedRectangle(D2D1::RoundedRect(r,18,18),brush,1);
        } else plate(r,id==VolumeDown||id==VolumeUp?16:18,c.enabled?.027f+level*.025f:.018f,c.focused&&keyboard_focus);
        if(!contrast&&c.enabled) {
            // Curved specular light, clipped to the pill, never a rectangular stripe.
            ID2D1RoundedRectangleGeometry* mask=nullptr;
            const float radius=std::min(22.f,(r.bottom-r.top)/2);
            if(SUCCEEDED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r,radius,radius),&mask))) {
                target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),mask),nullptr);
                glow(center.x,r.top-14,(r.right-r.left)*.48f,25,0xffffff,primary?.04f:.025f);
                auto inverse=D2D1::Matrix3x2F(gelatin);inverse.Invert();
                const auto local_pointer=inverse.TransformPoint(D2D1::Point2F(pointer_x,pointer_y));
                if(level>.001f||pressure>.001f)glow(std::clamp(local_pointer.x,r.left+8,r.right-8),
                    std::clamp(local_pointer.y,r.top+3,r.bottom-3),80,30,0xc8ecff,.04f*level+.22f*pressure);
                if(pressure>.001f)glow(std::clamp(local_pointer.x,r.left+8,r.right-8),
                    std::min(r.bottom-3,local_pointer.y+12),(r.right-r.left)*.36f,21,0xc6deeb,.12f*pressure);
                shade(c,r,gelatin);
                target->PopLayer();release(mask);
            }
            if(level>.001f) glow(std::clamp(pointer_x,r.left,r.right),r.bottom+3,74,23,0x82c5ff,.09f*level);
        }
        chrome_rim(r,optical_radius,level);
        if(c.focused&&keyboard_focus){auto inner=r;inner.left+=3;inner.right-=3;inner.top+=3;inner.bottom-=3;color(0xe7f7ff,.85f);target->DrawRoundedRectangle(D2D1::RoundedRect(inner,16,16),brush,1);}
        // Keep glyphs legible instead of shearing them with the optical surface.
        target->SetTransform(world);
        auto label=r;
        const float label_x=animations?hovers[id].pull_x*.14f:0,label_y=animations?hovers[id].pull_y*.14f:0;
        label.left+=label_x;label.right+=label_x;label.top+=label_y;label.bottom+=label_y;
        if(id==Device){label.left+=15;label.right-=34;color(0xd7e9f9);target->DrawLine(D2D1::Point2F(r.right-23,center.y-2),D2D1::Point2F(r.right-19,center.y+2),brush,1.4f);target->DrawLine(D2D1::Point2F(r.right-19,center.y+2),D2D1::Point2F(r.right-15,center.y-2),brush,1.4f);}
        if(id==SessionVolume)volume_edit(c,label,false);
        text(c.text,label,primary?16:id==VolumeDown||id==VolumeUp?21:id==SessionVolume?16:13,c.enabled?0xf0f6fc:0x96a8bc,primary,id!=Device,c.enabled);
        if(id==SessionVolume)volume_edit(c,label,true);
    }
    void own_test_grid(const GlassState& state,bool black_white) {
        // All pixels originate here. Constant alpha makes an accidental second
        // SOURCE_OVER copy measurable independently from the lens distortion.
        color(0x294766,.63f);target->FillRectangle(D2D1::RectF(0,0,state.width,state.height),brush);
        const auto old_blend=context->GetPrimitiveBlend();context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
        if(black_white) {
            const float boundary=std::clamp(state.width-58.f,1.f,state.width-1.f);
            color(0x000000,.63f);target->FillRectangle(D2D1::RectF(0,0,boundary,state.height),brush);
            color(0xffffff,.63f);target->FillRectangle(D2D1::RectF(boundary,0,state.width,state.height),brush);
            context->SetPrimitiveBlend(old_blend);return;
        }
        for(float x=0;x<state.width;x+=12) {
            color((static_cast<int>(x)/12)%2?0xdcab55:0x92dff2,.63f);
            target->FillRectangle(D2D1::RectF(x,0,x+2,state.height),brush);
        }
        for(float y=0;y<state.height;y+=12) {
            color((static_cast<int>(y)/12)%2?0xb799e9:0xe8f2e9,.63f);
            target->FillRectangle(D2D1::RectF(0,y,state.width,y+1),brush);
        }
        context->SetPrimitiveBlend(old_blend);
    }
    void draw(const GlassState& state,bool test_grid=false,bool use_refraction=true,bool black_white=false) {
        light_x=state.pointer_x;light_y=state.pointer_y;
        keyboard_focus=state.keyboard_focus;
        ID2D1Image* output=nullptr;
        if(context&&own_canvas){context->GetTarget(&output);context->SetTarget(own_canvas);context->Clear(D2D1::ColorF(0,0));}
        if(test_grid&&context)own_test_grid(state,black_white);else background(state);
        const float progress=animations&&transition_start?std::min(1.f,(GetTickCount64()-transition_start)/220.f):1.f;
        const float eased=1-std::pow(1-progress,3.f);
        ID2D1Layer* layer=nullptr;
        if(progress<1&&SUCCEEDED(target->CreateLayer(&layer))) target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),.55f+.45f*eased),layer);
        target->SetTransform(D2D1::Matrix3x2F::Translation(0,8*(1-eased)));
        content(state);
        if(output) {
            target->SetTransform(D2D1::Matrix3x2F::Identity());
            if(layer){target->PopLayer();release(layer);}
            context->SetTarget(output);release(output);
            context->DrawImage(own_canvas,D2D1::Point2F(0,0),D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,D2D1_COMPOSITE_MODE_SOURCE_COPY);
            if(progress<1&&SUCCEEDED(target->CreateLayer(&layer)))target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),.55f+.45f*eased),layer);
            target->SetTransform(D2D1::Matrix3x2F::Translation(0,8*(1-eased)));
        }
        // The 220 ms page transition composites opacity once; do not refract a
        // transitional layer against an already faded snapshot.
        refract_frame=use_refraction&&context&&own_canvas&&progress>=1;
        for(const auto& c:state.controls) if(c.id!=DeviceList&&c.id<CaptionMinimize) control(c,state.pointer_x,state.pointer_y);
        const auto popup=std::find_if(state.controls.begin(),state.controls.end(),[](const auto& c){return c.id==DeviceList;});
        if(popup!=state.controls.end()) {
            const auto final=D2D1::RectF(popup->left,popup->top,popup->right,popup->bottom);
            auto r=final;
            const float reveal=animations&&device_reveal_start?std::min(1.f,(GetTickCount64()-device_reveal_start)/220.f):1.f;
            const float expansion=1-std::pow(1-reveal,3.f);
            const auto origin=std::find_if(state.controls.begin(),state.controls.end(),[](const auto& c){return c.id==Device;});
            if(origin!=state.controls.end()&&reveal<1) {
                const auto mix=[&](float a,float b){return a+(b-a)*expansion;};
                r=D2D1::RectF(mix(origin->left,final.left),mix(origin->top,final.top),
                    mix(origin->right,final.right),mix(origin->bottom,final.bottom));
            }
            gradient(r,0x314b62,0x293f56,.93f,18);chrome_rim(r,18,0);
            target->PushAxisAlignedClip(r,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            ID2D1Layer* rows=nullptr;
            const float row_opacity=std::clamp((reveal-.34f)/.66f,0.f,1.f);
            if(row_opacity<1&&checked(target->CreateLayer(&rows),L"Show audio menu"))target->PushLayer(D2D1::LayerParameters(
                r,nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),row_opacity),rows);
            if(row_opacity>0)for(std::size_t i=static_cast<std::size_t>(std::max(0,state.device_top));i<state.devices.size();++i){const float y=final.top+(static_cast<int>(i)-state.device_top)*34;if(y>=final.bottom)break;auto row=D2D1::RectF(final.left+6,y,final.right-6,y+34);if(static_cast<int>(i)==state.selected_device){color(0xabddff,.18f);target->FillRoundedRectangle(D2D1::RoundedRect(row,12,12),brush);}row.left+=10;text(state.devices[i],row,12,0xf4faff);}
            if(rows){target->PopLayer();release(rows);}
            target->PopAxisAlignedClip();
        }
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        if(layer){target->PopLayer();release(layer);}
        for(const auto& c:state.controls) if(c.id>=CaptionMinimize) control(c);
        refract_frame=false;
    }
    bool read_own_backbuffer(std::vector<std::uint32_t>& pixels,UINT& width,UINT& height) {
        pixels.clear();width=height=0;
        ID3D11Texture2D* source=nullptr;ID3D11Texture2D* staging=nullptr;
        ID3D11DeviceContext* gpu_context=nullptr;
        auto hr=chain->GetBuffer(0,__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&source));
        D3D11_TEXTURE2D_DESC desc{};
        if(SUCCEEDED(hr)) {
            source->GetDesc(&desc);
            if(desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM||desc.SampleDesc.Count!=1||
               static_cast<std::uint64_t>(desc.Width)*desc.Height>32ull*1024*1024)hr=E_INVALIDARG;
        }
        if(SUCCEEDED(hr)) {
            desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            hr=gpu->CreateTexture2D(&desc,nullptr,&staging);
        }
        if(SUCCEEDED(hr)) {
            gpu->GetImmediateContext(&gpu_context);gpu_context->CopyResource(staging,source);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            hr=gpu_context->Map(staging,0,D3D11_MAP_READ,0,&mapped);
            if(SUCCEEDED(hr)) {
                try {
                    pixels.resize(static_cast<std::size_t>(desc.Width)*desc.Height);
                    for(UINT y=0;y<desc.Height;++y)std::memcpy(pixels.data()+static_cast<std::size_t>(y)*desc.Width,
                        static_cast<const unsigned char*>(mapped.pData)+static_cast<std::size_t>(y)*mapped.RowPitch,
                        static_cast<std::size_t>(desc.Width)*4);
                    width=desc.Width;height=desc.Height;
                } catch(const std::bad_alloc&) {hr=E_OUTOFMEMORY;pixels.clear();}
                gpu_context->Unmap(staging,0);
            }
        }
        release(gpu_context);release(staging);release(source);
        return checked(hr,L"Diagnostic read of the internal GPU canvas");
    }
};

Glass::Glass():impl_(std::make_unique<Impl>()){impl_->preferences();}
Glass::~Glass()=default;
void Glass::configure(HWND window) {
    const BOOL dark=TRUE;DwmSetWindowAttribute(window,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
    const DWM_WINDOW_CORNER_PREFERENCE corners=DWMWCP_ROUND;DwmSetWindowAttribute(window,DWMWA_WINDOW_CORNER_PREFERENCE,&corners,sizeof(corners));
    const DWM_SYSTEMBACKDROP_TYPE backdrop=impl_->contrast?DWMSBT_NONE:DWMSBT_TRANSIENTWINDOW;
    DwmSetWindowAttribute(window,DWMWA_SYSTEMBACKDROP_TYPE,&backdrop,sizeof(backdrop));
    const MARGINS margins{-1,-1,-1,-1};DwmExtendFrameIntoClientArea(window,&margins);
}
void Glass::release_visuals(){impl_->discard_gpu();}
bool Glass::render(HWND window,const GlassState& state,UINT dpi) {
    if(!impl_->gpu_resources(window,dpi)) return false;
    if(!impl_->canvas_resources(dpi)){impl_->discard_gpu();return false;}
    impl_->failure.clear();
    impl_->target->BeginDraw();impl_->target->SetTransform(D2D1::Matrix3x2F::Identity());impl_->target->Clear(D2D1::ColorF(0,0));impl_->draw(state);
    auto hr=impl_->target->EndDraw();
    if(!impl_->failure.empty()){impl_->discard_gpu();return false;}
    if(SUCCEEDED(hr)) hr=impl_->chain->Present(1,0);
    if(!impl_->checked(hr,L"Graphics presentation")){impl_->discard_gpu();return false;}
    ++impl_->frame_count;impl_->failure.clear();return true;
}
bool Glass::render_own_test_frame(HWND window,const GlassState& state,UINT dpi,bool refraction,
                                std::vector<std::uint32_t>& pixels,UINT& width,UINT& height,bool black_white) {
    pixels.clear();width=height=0;
    if(!impl_->gpu_resources(window,dpi))return false;
    if(!impl_->canvas_resources(dpi)){impl_->discard_gpu();return false;}
    impl_->failure.clear();
    impl_->target->BeginDraw();impl_->target->SetTransform(D2D1::Matrix3x2F::Identity());
    impl_->target->Clear(D2D1::ColorF(0,0));impl_->draw(state,true,refraction,black_white);
    auto hr=impl_->target->EndDraw();
    if(!impl_->failure.empty()){impl_->discard_gpu();return false;}
    if(!impl_->checked(hr,L"Diagnostic glass rendering")||!impl_->read_own_backbuffer(pixels,width,height)) {
        impl_->discard_gpu();return false;
    }
    hr=impl_->chain->Present(1,0);
    if(!impl_->checked(hr,L"Diagnostic presentation")){impl_->discard_gpu();return false;}
    ++impl_->frame_count;impl_->failure.clear();return true;
}
void Glass::paint(HDC dc,const RECT& region,const GlassState& state,UINT dpi) {
    // Artifact renderer only. Production uses render(), never this GDI bridge.
    if(!impl_->shared_resources()) return;
    if(!impl_->dc_mode){impl_->discard_gpu();impl_->dc_mode=true;}
    ID2D1DCRenderTarget* dc_target=nullptr;
    if(!impl_->target) {
        const auto props=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
        if(FAILED(impl_->factory->CreateDCRenderTarget(&props,&dc_target))) return;
        impl_->target=dc_target;if(!impl_->target_brush(dpi)) return;
    } else if(FAILED(impl_->target->QueryInterface(__uuidof(ID2D1DCRenderTarget),reinterpret_cast<void**>(&dc_target)))) return;
    else dc_target->Release(); // target retains ownership
    if(FAILED(dc_target->BindDC(dc,&region))) return;
    impl_->target->SetDpi(static_cast<float>(dpi),static_cast<float>(dpi));
    impl_->target->BeginDraw();impl_->target->SetTransform(D2D1::Matrix3x2F::Identity());impl_->target->Clear(rgba(0x18212d));impl_->draw(state);impl_->target->EndDraw();
}
void Glass::hover(int id,bool hovered){auto& h=impl_->hovers[id];h.target=hovered?1.f:0.f;if(!impl_->animations) h.value=h.target;}
void Glass::press(int id,bool pressed){auto& h=impl_->hovers[id];h.pressure_target=pressed?1.f:0.f;if(!impl_->animations)h.pressure=h.pressure_target;}
void Glass::begin_drag(int id,float x,float y) {
    end_drag();
    if(!std::isfinite(x)||!std::isfinite(y)||id<=0)return;
    impl_->drag_id=id;impl_->drag_origin_x=x;impl_->drag_origin_y=y;
    auto& motion=impl_->hovers[id];motion.pressure_target=1.f;
    if(!impl_->animations)motion.pressure=1.f; // Static press feedback remains.
}
void Glass::drag(float x,float y) {
    if(!impl_->drag_id||!impl_->animations||!std::isfinite(x)||!std::isfinite(y))return;
    auto& h=impl_->hovers[impl_->drag_id];
    h.pull_target_x=14.f*std::tanh((x-impl_->drag_origin_x)/42.f);
    h.pull_target_y=14.f*std::tanh((y-impl_->drag_origin_y)/42.f);
}
void Glass::end_drag() {
    if(impl_->drag_id) {
        auto& h=impl_->hovers[impl_->drag_id];
        h.pull_target_x=h.pull_target_y=0;h.pressure_target=0;
        if(!impl_->animations)h.pull_x=h.pull_y=h.pull_velocity_x=h.pull_velocity_y=h.pressure=h.pressure_velocity=0;
    }
    impl_->drag_id=0;
}
bool Glass::tick(bool ringing) {
    const auto now=GetTickCount64();const float dt=impl_->previous?std::min(.032f,(now-impl_->previous)/1000.f):.016f;impl_->previous=now;
    bool more=ringing&&impl_->animations;
    auto spring=[&](float& value,float goal,float& velocity,float omega){
        if(!impl_->animations){value=goal;velocity=0;return;}
        for(int i=0;i<2;++i){const float step=dt*.5f;velocity+=(omega*omega*(goal-value)-2*.72f*omega*velocity)*step;value+=velocity*step;}
        if(std::abs(value-goal)<.001f&&std::abs(velocity)<.02f){value=goal;velocity=0;}else more=true;
    };
    for(auto& [id,h]:impl_->hovers){(void)id;spring(h.value,h.target,h.velocity,23);spring(h.pressure,h.pressure_target,h.pressure_velocity,32);
        spring(h.pull_x,h.pull_target_x,h.pull_velocity_x,18);spring(h.pull_y,h.pull_target_y,h.pull_velocity_y,18);}
    if(impl_->transition_start&&now-impl_->transition_start<220) more=true;
    else impl_->transition_start=0;
    if(impl_->device_reveal_start&&now-impl_->device_reveal_start<220)more=true;
    else impl_->device_reveal_start=0;
    if(!more)impl_->previous=0;
    return more;
}
bool Glass::animation_enabled()const{return impl_->animations;}
void Glass::settings_changed(){impl_->preferences();if(!impl_->animations)end_drag();}
void Glass::transition(){impl_->transition_start=impl_->animations?GetTickCount64():0;}
void Glass::reveal_devices(){impl_->device_reveal_start=impl_->animations?GetTickCount64():0;}
std::wstring Glass::error()const{return impl_->failure;}
unsigned long long Glass::frames()const{return impl_->frame_count;}
unsigned Glass::generations()const{return impl_->generation_count;}
unsigned long long Glass::lighting_draws()const{return impl_->lighting_count;}
unsigned long long Glass::refraction_draws()const{return impl_->refraction_count;}
}
