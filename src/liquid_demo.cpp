// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

// Separate, opt-in liquid playground. Only its own transparent window receives
// input. Grid mode uses generated pixels; --desktop requires a manual Windows
// source selection before a memory-only GPU texture is sampled.
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <wtsapi32.h>
#include <shellapi.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <vector>
#include "liquid_physics.h"
#include "desktop_capture.h"

namespace {
using Microsoft::WRL::ComPtr;
using namespace alarmapp::liquid;
constexpr UINT_PTR frame_timer=1;
constexpr UINT_PTR source_idle_timer=2;
constexpr auto source_idle_timeout=std::chrono::seconds(15);
constexpr int close_id=900;
constexpr int picker_id=901;
constexpr wchar_t window_class[]=L"ALARM_Separate_Liquid_Demo";
using Monotonic=std::chrono::steady_clock;

const char shader[]=R"hlsl(
cbuffer Parameters : register(b0) {
    float4 viewportMouse;
    float4 options;
    float4 sourceMapping;
    float4 lenses[6];
};
Texture2D sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);
float4 vertex(uint id:SV_VertexID):SV_POSITION {
    float2 uv=float2((id<<1)&2,id&2);
    return float4(uv*float2(2,-2)+float2(-1,1),0,1);
}
float4 over(float4 front,float4 back) {return front+back*(1-front.a);}
float3 grid(float2 p) {
    p/=52*options.z;
    float2 cell=floor(p),fraction=frac(p);
    float gridLine=1-smoothstep(.012,.035,min(min(fraction.x,1-fraction.x),min(fraction.y,1-fraction.y)));
    float checker=fmod(abs(cell.x+cell.y),2);
    float3 color=lerp(float3(.27,.60,.74),float3(.70,.49,.67),checker);
    return lerp(color,float3(.94,.98,1),gridLine*.8);
}
float4 pixel(float4 position:SV_POSITION):SV_TARGET {
    float2 p=position.xy;
    float4 result=0;
    if(options.y>.5) {
        // A generated reference grid is an explicit test mode. It is not an
        // opaque desktop substitute; the rest of the overlay remains clear.
        float2 cell=frac(p/(52*options.z));
        float gridLine=1-smoothstep(.005,.02,min(min(cell.x,1-cell.x),min(cell.y,1-cell.y)));
        result=float4(float3(.55,.80,.94)*gridLine*.10,gridLine*.10);
    }
    [unroll] for(int i=0;i<3;++i) {
        if(i>=options.x)break;
        float4 shape=lenses[i*2],properties=lenses[i*2+1];
        float2 center=shape.xy,axes=shape.zw;
        float angle=properties.x,opacity=properties.y,radiusZ=properties.z,puddle=properties.w;
        float cs=cos(angle),sn=sin(angle);
        float2 delta=p-center;
        float3 light=normalize(float3(viewportMouse.zw-center,240*options.z));
        float2 shadowCenter=center-light.xy*radiusZ*.65*lerp(1,.08,puddle);
        float2 shadowDelta=p-shadowCenter;
        float2 shadowLocal=float2(cs*shadowDelta.x+sn*shadowDelta.y,-sn*shadowDelta.x+cs*shadowDelta.y)/axes;
        float shadowDistance=length(shadowLocal);
        if(shadowDistance<2.3) {
            float alpha=exp(-pow(shadowDistance*1.4,2))*.15*opacity;
            result=over(float4(0,0,0,alpha),result);
        }
        float2 local=float2(cs*delta.x+sn*delta.y,-sn*delta.x+cs*delta.y);
        float2 unit=local/axes;
        float rho=dot(unit,unit);
        if(rho>=1)continue;
        float z=radiusZ*sqrt(max(0,1-rho));
        float3 surface=float3(local,z),radii=float3(axes,radiusZ);
        float3 normalLocal=normalize(surface/(radii*radii));
        float3 normal=float3(cs*normalLocal.x-sn*normalLocal.y,sn*normalLocal.x+cs*normalLocal.y,normalLocal.z);
        const float waterIOR=1.333;
        float3 transmitted=refract(float3(0,0,-1),normalLocal,1/waterIOR);
        float3 inside=surface+transmitted*max(.0001,min(axes.x,axes.y)*.0001);
        float3 exitPoint=surface,outgoing=0;
        // A thin pool frequently has total internal reflection at its upper
        // or lower meniscus. Follow the actual internal ray, rather than
        // sampling its first contact as though it had already left the water.
        [loop] for(int reflectionStep=0;reflectionStep<24;++reflectionStep) {
            float3 scaledDirection=transmitted/radii,scaledPoint=inside/radii;
            float a=dot(scaledDirection,scaledDirection),b=dot(scaledPoint,scaledDirection);
            float c=dot(scaledPoint,scaledPoint)-1;
            float distance=(-b+sqrt(max(0,b*b-a*c)))/a;
            exitPoint=inside+transmitted*max(0,distance);
            float3 rearNormal=normalize(exitPoint/(radii*radii));
            outgoing=refract(transmitted,-rearNormal,waterIOR);
            if(dot(outgoing,outgoing)>.5)break;
            transmitted=reflect(transmitted,rearNormal);
            inside=exitPoint+transmitted*max(.0001,min(axes.x,axes.y)*.0001);
        }
        float2 sampledLocal=exitPoint.xy;
        if(outgoing.z<-.0001) sampledLocal+=outgoing.xy*((-radiusZ-80*options.z-exitPoint.z)/outgoing.z);
        float2 sampled=center+float2(cs*sampledLocal.x-sn*sampledLocal.y,sn*sampledLocal.x+cs*sampledLocal.y);
        float f0=pow((1-waterIOR)/(1+waterIOR),2);
        float reflection=f0+(1-f0)*pow(1-saturate(normal.z),5);
        float3 halfVector=normalize(light+float3(0,0,1));
        float specular=pow(saturate(dot(normal,halfVector)),90)*1.3;
        float broad=pow(saturate(dot(normal,halfVector)),9)*.035;
        float rim=smoothstep(.75,.99,rho)*(.12+.48*saturate(dot(normal,light)));
        float edge=saturate((1-rho)*min(axes.x,axes.y)*.8);
        float alpha=saturate(.025+reflection*.18+rim*.16+specular*.74+broad)*opacity*edge;
        float3 tint=float3(.69,.89,.98)*(.04+reflection*.24)+float3(1,1,1)*(specular+broad+rim*.35);
        float4 water;
        if(options.w>.5) {
            float sourceAlpha=.97*opacity*edge;
            float2 sourcePoint=options.w>1.5?p:sampled;
            float3 chosen=sourceTexture.SampleLevel(sourceSampler,sourcePoint*sourceMapping.xy+sourceMapping.zw,0).rgb;
            water=float4(saturate(chosen*(1-reflection)+tint)*sourceAlpha,sourceAlpha);
        } else if(options.y>.5) {
            float gridAlpha=.78*opacity*edge;
            float3 lit=grid(sampled)*(1-reflection)+tint;
            water=float4(saturate(lit)*gridAlpha,gridAlpha);
        } else water=float4(saturate(tint)*alpha,alpha);
        result=over(water,result);
    }
    return result;
}
)hlsl";

void checked(HRESULT result,const char* operation) {
    if(FAILED(result)) {
        char hex[32]{};std::snprintf(hex,sizeof(hex),"0x%08lX",static_cast<unsigned long>(result));
        throw std::runtime_error(std::string(operation)+" "+hex);
    }
}
struct alignas(16) Parameters {float viewport_mouse[4]{};float options[4]{};float source_mapping[4]{};float lenses[6][4]{};};
static_assert(sizeof(Parameters)==144);

// Flatten an own generated GPU frame onto the same own black/white source,
// solely for the hidden optical proof. No external image is ever passed here.
void save_own_optical_bmp(const std::filesystem::path& path,const std::vector<unsigned char>& frame,UINT width,UINT height) {
    auto pixels=frame;
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
        const auto offset=(static_cast<std::size_t>(y)*width+x)*4;
        const unsigned background=x>=width/2?255:0,alpha=pixels[offset+3];
        for(unsigned channel=0;channel<3;++channel)pixels[offset+channel]=static_cast<unsigned char>(std::min(255u,pixels[offset+channel]+background*(255-alpha)/255));
        pixels[offset+3]=255;
    }
    BITMAPFILEHEADER file{};BITMAPINFOHEADER image{};
    file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(image);file.bfSize=file.bfOffBits+static_cast<DWORD>(pixels.size());
    image.biSize=sizeof(image);image.biWidth=static_cast<LONG>(width);image.biHeight=-static_cast<LONG>(height);
    image.biPlanes=1;image.biBitCount=32;image.biCompression=BI_RGB;image.biSizeImage=static_cast<DWORD>(pixels.size());
    std::ofstream output(path,std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file),sizeof(file));output.write(reinterpret_cast<const char*>(&image),sizeof(image));
    output.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()));
    output.close();if(!output)throw std::runtime_error("Cannot export own black/white optical proof");
}

class Renderer {
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> chain_;
    ComPtr<IDCompositionDevice> composition_;
    ComPtr<IDCompositionTarget> destination_;
    ComPtr<IDCompositionVisual> visual_;
    ComPtr<ID3D11VertexShader> vertex_;
    ComPtr<ID3D11PixelShader> pixel_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11RenderTargetView> view_;
    ComPtr<ID3D11ShaderResourceView> source_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ID3D11Texture2D* source_identity_=nullptr;
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> write_;
    ComPtr<ID2D1RenderTarget> text_target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> title_,instructions_;
    ComPtr<ID3DBlob> vertex_code_,pixel_code_;
    HWND window_=nullptr;
    UINT width_=0,height_=0;
    void compile() {
        if(vertex_code_&&pixel_code_)return;
        ComPtr<ID3DBlob> error;
        auto hr=D3DCompile(shader,std::strlen(shader),"ALARM own liquid shader",nullptr,nullptr,"vertex","vs_4_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS,0,&vertex_code_,&error);
        if(FAILED(hr))throw std::runtime_error(error?static_cast<const char*>(error->GetBufferPointer()):"Vertex shader compilation failed");
        error.Reset();
        hr=D3DCompile(shader,std::strlen(shader),"ALARM own liquid shader",nullptr,nullptr,"pixel","ps_4_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS,0,&pixel_code_,&error);
        if(FAILED(hr))throw std::runtime_error(error?static_cast<const char*>(error->GetBufferPointer()):"Pixel shader compilation failed");
    }
    void target_resources(UINT dpi) {
        if(!view_) {
            ComPtr<ID3D11Texture2D> texture;
            checked(chain_->GetBuffer(0,IID_PPV_ARGS(&texture)),"Swapchain texture");
            checked(device_->CreateRenderTargetView(texture.Get(),nullptr,&view_),"Render target view");
            ComPtr<IDXGISurface> surface;checked(texture.As(&surface),"DXGI surface");
            const auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),static_cast<float>(dpi),static_cast<float>(dpi));
            checked(factory_->CreateDxgiSurfaceRenderTarget(surface.Get(),&properties,&text_target_),"Direct2D HUD target");
            checked(text_target_->CreateSolidColorBrush(D2D1::ColorF(1,1,1),&brush_),"HUD brush");
        }
        text_target_->SetDpi(static_cast<float>(dpi),static_cast<float>(dpi));
    }
    void initialize(HWND window,UINT width,UINT height,UINT dpi) {
        if(!IsWindow(window))throw std::runtime_error("Invalid liquid window");
        compile();
        if(!factory_)checked(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory_.GetAddressOf()),"Direct2D factory");
        if(!write_)checked(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(write_.GetAddressOf())),"DirectWrite factory");
        if(!title_)checked(write_->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_SEMI_BOLD,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,15,L"en-US",&title_),"Title font");
        if(!instructions_)checked(write_->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,12,L"en-US",&instructions_),"Instructions font");
        if(window_!=window)release();
        if(!device_) {
            auto hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device_,nullptr,&context_);
            if(FAILED(hr))hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device_,nullptr,&context_);
            checked(hr,"Liquid graphics device");
            ComPtr<IDXGIDevice> dxgi;checked(device_.As(&dxgi),"DXGI device");
            ComPtr<IDXGIAdapter> adapter;checked(dxgi->GetAdapter(&adapter),"Graphics adapter");
            ComPtr<IDXGIFactory2> dxgi_factory;checked(adapter->GetParent(IID_PPV_ARGS(&dxgi_factory)),"DXGI factory");
            DXGI_SWAP_CHAIN_DESC1 description{};
            description.Width=width;description.Height=height;description.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count=1;description.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;description.BufferCount=2;
            description.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;description.Scaling=DXGI_SCALING_STRETCH;description.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
            checked(dxgi_factory->CreateSwapChainForComposition(device_.Get(),&description,nullptr,&chain_),"Transparent liquid swapchain");
            checked(DCompositionCreateDevice(dxgi.Get(),__uuidof(IDCompositionDevice),reinterpret_cast<void**>(composition_.GetAddressOf())),"Liquid composition device");
            checked(composition_->CreateTargetForHwnd(window,TRUE,&destination_),"Own liquid composition target");
            checked(composition_->CreateVisual(&visual_),"Liquid visual");
            checked(visual_->SetContent(chain_.Get()),"Liquid visual content");
            checked(destination_->SetRoot(visual_.Get()),"Liquid composition root");
            checked(composition_->Commit(),"Liquid composition commit");
            checked(device_->CreateVertexShader(vertex_code_->GetBufferPointer(),vertex_code_->GetBufferSize(),nullptr,&vertex_),"Liquid vertex shader");
            checked(device_->CreatePixelShader(pixel_code_->GetBufferPointer(),pixel_code_->GetBufferSize(),nullptr,&pixel_),"Liquid pixel shader");
            D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=sizeof(Parameters);buffer.Usage=D3D11_USAGE_DYNAMIC;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;buffer.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            checked(device_->CreateBuffer(&buffer,nullptr,&constants_),"Liquid constant buffer");
            D3D11_SAMPLER_DESC sampling{};
            sampling.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sampling.AddressU=sampling.AddressV=sampling.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
            sampling.MaxLOD=D3D11_FLOAT32_MAX;
            checked(device_->CreateSamplerState(&sampling,&sampler_),"Liquid source sampler");
            D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;
            raster.DepthClipEnable=TRUE;raster.ScissorEnable=TRUE;
            checked(device_->CreateRasterizerState(&raster,&rasterizer_),"Liquid bounded rasterizer");
            window_=window;width_=width;height_=height;
        }
        if(width_!=width||height_!=height) {
            context_->OMSetRenderTargets(0,nullptr,nullptr);
            brush_.Reset();text_target_.Reset();view_.Reset();
            checked(chain_->ResizeBuffers(2,width,height,DXGI_FORMAT_B8G8R8A8_UNORM,0),"Liquid resize");
            width_=width;height_=height;
        }
        target_resources(dpi);
    }
    void read_own_pixels(std::vector<unsigned char>& pixels) {
        // This reads only the texture generated by this renderer, never any
        // desktop/window/backdrop surface. It is used by --check while hidden.
        ComPtr<ID3D11Texture2D> texture;checked(chain_->GetBuffer(0,IID_PPV_ARGS(&texture)),"Own test texture");
        D3D11_TEXTURE2D_DESC description{};texture->GetDesc(&description);
        description.Usage=D3D11_USAGE_STAGING;description.BindFlags=0;description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;description.MiscFlags=0;
        ComPtr<ID3D11Texture2D> staging;checked(device_->CreateTexture2D(&description,nullptr,&staging),"Own test staging texture");
        context_->CopyResource(staging.Get(),texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};checked(context_->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Own test texture read");
        pixels.resize(static_cast<std::size_t>(width_)*height_*4);
        for(UINT y=0;y<height_;++y)std::memcpy(pixels.data()+static_cast<std::size_t>(y)*width_*4,static_cast<const unsigned char*>(mapped.pData)+static_cast<std::size_t>(y)*mapped.RowPitch,width_*4);
        context_->Unmap(staging.Get(),0);
    }
public:
    ~Renderer(){release();}
    ID3D11Device* device(HWND window,UINT dpi) {
        RECT client{};
        if(!GetClientRect(window,&client)||client.right<=0||client.bottom<=0)throw std::runtime_error("Invalid liquid client area");
        initialize(window,static_cast<UINT>(client.right),static_cast<UINT>(client.bottom),dpi);
        return device_.Get();
    }
    void unbind_source() {
        if(context_){ID3D11ShaderResourceView* empty=nullptr;context_->PSSetShaderResources(0,1,&empty);}
    }
    void clear_source() {
        unbind_source();
        source_.Reset();source_identity_=nullptr;
    }
    void release() {
        if(context_)context_->ClearState();
        clear_source();sampler_.Reset();rasterizer_.Reset();
        if(destination_)destination_->SetRoot(nullptr);
        if(composition_)composition_->Commit();
        brush_.Reset();text_target_.Reset();view_.Reset();constants_.Reset();pixel_.Reset();vertex_.Reset();
        visual_.Reset();destination_.Reset();composition_.Reset();chain_.Reset();context_.Reset();device_.Reset();
        window_=nullptr;width_=height_=0;
    }
    void draw(HWND window,const World& world,UINT dpi,bool grid_mode,std::vector<unsigned char>* own_pixels=nullptr,
              ID3D11Texture2D* chosen_source=nullptr,bool desktop_mode=false,const std::wstring& source_status={},bool full_shader=false,bool refract_source=true) {
        RECT client{};if(!GetClientRect(window,&client)||client.right<=0||client.bottom<=0)throw std::runtime_error("Invalid liquid client area");
        initialize(window,static_cast<UINT>(client.right),static_cast<UINT>(client.bottom),dpi);
        context_->ClearState();
        const float scale=dpi/96.f;
        Parameters parameters;
        parameters.viewport_mouse[0]=static_cast<float>(width_);parameters.viewport_mouse[1]=static_cast<float>(height_);
        parameters.viewport_mouse[2]=static_cast<float>(world.mouse().x)*scale;parameters.viewport_mouse[3]=static_cast<float>(world.mouse().y)*scale;
        parameters.options[1]=grid_mode?1.f:0.f;parameters.options[2]=scale;
        if(chosen_source) {
            if(source_identity_!=chosen_source) {
                clear_source();
                checked(device_->CreateShaderResourceView(chosen_source,nullptr,&source_),"Chosen source shader view");
                source_identity_=chosen_source;
            }
            parameters.options[3]=refract_source?1.f:2.f;
            parameters.source_mapping[0]=1.f/width_;parameters.source_mapping[1]=1.f/height_;
            D3D11_TEXTURE2D_DESC chosen{};chosen_source->GetDesc(&chosen);
            MONITORINFO monitor{sizeof(monitor),{}, {},0};RECT own{};
            if(GetWindowRect(window,&own)&&GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor)&&
               chosen.Width==static_cast<UINT>(monitor.rcMonitor.right-monitor.rcMonitor.left)&&
               chosen.Height==static_cast<UINT>(monitor.rcMonitor.bottom-monitor.rcMonitor.top)) {
                parameters.source_mapping[0]=1.f/chosen.Width;parameters.source_mapping[1]=1.f/chosen.Height;
                parameters.source_mapping[2]=(own.left-monitor.rcMonitor.left)/static_cast<float>(chosen.Width);
                parameters.source_mapping[3]=(own.top-monitor.rcMonitor.top)/static_cast<float>(chosen.Height);
            }
        } else clear_source();
        int count=0;
        RECT shader_bounds{static_cast<LONG>(width_),static_cast<LONG>(height_),0,0};
        for(const auto& drop:world.drops())if(drop.alive&&(!desktop_mode||chosen_source)) {
            auto& geometry=parameters.lenses[count*2];auto& properties=parameters.lenses[count*2+1];
            geometry[0]=static_cast<float>(drop.position.x)*scale;geometry[1]=static_cast<float>(drop.position.y)*scale;
            geometry[2]=static_cast<float>(drop.radius_x())*scale;geometry[3]=static_cast<float>(drop.radius_y())*scale;
            properties[0]=static_cast<float>(drop.angle);properties[1]=static_cast<float>(drop.opacity());properties[2]=static_cast<float>(drop.radius_z())*scale;
            properties[3]=drop.phase==Phase::Puddle?1.f:0.f;
            // The shadow shader returns zero outside 2.3 rotated ellipse radii.
            // A conservative .65*r offset covers every possible mouse light.
            const double x=drop.position.x*scale,y=drop.position.y*scale;
            const double extent_x=(2.3*drop.extent_x()+.65*drop.radius_z())*scale+2;
            const double extent_y=(2.3*drop.extent_y()+.65*drop.radius_z())*scale+2;
            shader_bounds.left=std::min(shader_bounds.left,static_cast<LONG>(std::floor(x-extent_x)));
            shader_bounds.top=std::min(shader_bounds.top,static_cast<LONG>(std::floor(y-extent_y)));
            shader_bounds.right=std::max(shader_bounds.right,static_cast<LONG>(std::ceil(x+extent_x)));
            shader_bounds.bottom=std::max(shader_bounds.bottom,static_cast<LONG>(std::ceil(y+extent_y)));
            ++count;
        }
        parameters.options[0]=static_cast<float>(count);
        D3D11_MAPPED_SUBRESOURCE mapped{};checked(context_->Map(constants_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Liquid parameters upload");
        std::memcpy(mapped.pData,&parameters,sizeof(parameters));context_->Unmap(constants_.Get(),0);
        const float clear[4]{};context_->ClearRenderTargetView(view_.Get(),clear);
        ID3D11RenderTargetView* target=view_.Get();context_->OMSetRenderTargets(1,&target,nullptr);
        D3D11_VIEWPORT viewport{0,0,static_cast<float>(width_),static_cast<float>(height_),0,1};context_->RSSetViewports(1,&viewport);
        if(grid_mode||full_shader)shader_bounds={0,0,static_cast<LONG>(width_),static_cast<LONG>(height_)};
        shader_bounds.left=std::clamp(shader_bounds.left,0L,static_cast<LONG>(width_));
        shader_bounds.top=std::clamp(shader_bounds.top,0L,static_cast<LONG>(height_));
        shader_bounds.right=std::clamp(shader_bounds.right,0L,static_cast<LONG>(width_));
        shader_bounds.bottom=std::clamp(shader_bounds.bottom,0L,static_cast<LONG>(height_));
        context_->RSSetState(rasterizer_.Get());context_->RSSetScissorRects(1,&shader_bounds);
        context_->IASetInputLayout(nullptr);context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex_.Get(),nullptr,0);context_->PSSetShader(pixel_.Get(),nullptr,0);
        ID3D11Buffer* buffer=constants_.Get();context_->PSSetConstantBuffers(0,1,&buffer);
        ID3D11ShaderResourceView* source=source_.Get();context_->PSSetShaderResources(0,1,&source);
        ID3D11SamplerState* sampler=sampler_.Get();context_->PSSetSamplers(0,1,&sampler);
        if(shader_bounds.left<shader_bounds.right&&shader_bounds.top<shader_bounds.bottom)context_->Draw(3,0);
        context_->OMSetRenderTargets(0,nullptr,nullptr);
        text_target_->BeginDraw();text_target_->SetTransform(D2D1::Matrix3x2F::Identity());
        const float logical_width=width_/scale;
        const auto panel=D2D1::RectF(18,18,std::min(logical_width-118.f,desktop_mode?950.f:770.f),desktop_mode?136.f:85.f);
        brush_->SetColor(D2D1::ColorF(.055f,.09f,.14f,.83f));text_target_->FillRoundedRectangle(D2D1::RoundedRect(panel,18,18),brush_.Get());
        brush_->SetColor(D2D1::ColorF(.91f,.97f,1,.95f));
        const std::wstring title=desktop_mode?L"ALARM · Liquid playground — SOURCE CHOSEN IN WINDOWS":grid_mode?L"ALARM · Liquid playground — OPTICAL GRID (TEST)":L"ALARM · Liquid playground — transparent drop";
        text_target_->DrawText(title.c_str(),static_cast<UINT32>(title.size()),title_.Get(),D2D1::RectF(34,28,panel.right-12,51),brush_.Get());
        const std::wstring help=desktop_mode&&!chosen_source?L"Choose a source in Windows to show the water and start dragging.":L"Hold · drag · release. Water spreads on the floor. Fades in 4 s. Esc closes.";
        text_target_->DrawText(help.c_str(),static_cast<UINT32>(help.size()),instructions_.Get(),D2D1::RectF(34,51,panel.right-12,80),brush_.Get());
        if(desktop_mode) {
            const auto picker=D2D1::RectF(34,88,190,122);
            brush_->SetColor(D2D1::ColorF(.22f,.32f,.43f,.9f));text_target_->FillRoundedRectangle(D2D1::RoundedRect(picker,12,12),brush_.Get());
            brush_->SetColor(D2D1::ColorF(1,1,1));
            instructions_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);instructions_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const std::wstring label=L"Choose screen";
            text_target_->DrawText(label.c_str(),static_cast<UINT32>(label.size()),instructions_.Get(),picker,brush_.Get());
            instructions_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);instructions_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            text_target_->DrawText(source_status.c_str(),static_cast<UINT32>(source_status.size()),instructions_.Get(),D2D1::RectF(208,92,panel.right-12,130),brush_.Get());
        }
        const auto close=D2D1::RectF(logical_width-104,20,logical_width-18,59);
        brush_->SetColor(D2D1::ColorF(.22f,.32f,.43f,.9f));text_target_->FillRoundedRectangle(D2D1::RoundedRect(close,14,14),brush_.Get());
        brush_->SetColor(D2D1::ColorF(1,1,1));
        instructions_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);instructions_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        constexpr wchar_t close_label[]=L"Close · Esc";
        text_target_->DrawText(close_label,static_cast<UINT32>(std::size(close_label)-1),instructions_.Get(),close,brush_.Get());
        instructions_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);instructions_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        checked(text_target_->EndDraw(),"Liquid HUD draw");
        if(own_pixels)read_own_pixels(*own_pixels);
        checked(chain_->Present(1,0),"Liquid presentation");
    }
    ComPtr<ID3D11Texture2D> own_test_source(HWND window,bool black_white=false) {
        auto gpu=device(window,96);
        constexpr UINT width=640,height=480;
        std::vector<std::uint32_t> pixels(width*height);
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
            pixels[y*width+x]=black_white?(x>=width/2?0xffffffffu:0xff000000u):0xff000000u|((x*255/width)<<16)|((y*255/height)<<8)|((x/32+y/32)%2?220u:30u);
        D3D11_TEXTURE2D_DESC description{};
        description.Width=width;description.Height=height;description.MipLevels=1;description.ArraySize=1;
        description.Format=DXGI_FORMAT_B8G8R8A8_UNORM;description.SampleDesc.Count=1;
        description.Usage=D3D11_USAGE_DEFAULT;description.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{pixels.data(),width*4,0};ComPtr<ID3D11Texture2D> result;
        checked(gpu->CreateTexture2D(&description,&data,&result),"Own source-texture shader test");
        return result;
    }
};

class Demo {
    HWND window_=nullptr,close_=nullptr,picker_=nullptr;
    Renderer renderer_;
    World world_;
    std::unique_ptr<alarmapp::ui::DesktopCapture> capture_;
    std::wstring source_status_=L"Click here, then choose this screen in Windows. Pixels stay in GPU memory only.";
    bool grid_=false,desktop_=false,hidden_check_=false,locked_=false,suspended_=false,display_off_=false,closing_=false,timer_running_=false;
    HPOWERNOTIFY power_notify_=nullptr,display_notify_=nullptr;
    bool session_registered_=false;
    int exit_code_=0;
    std::filesystem::path metrics_path_;
    std::uint64_t frames_=0,drag_starts_=0,drag_updates_=0,releases_=0,expirations_=0;
    std::uint64_t max_capture_frames_=0,capture_sessions_=0;
    Monotonic::time_point started_=Monotonic::now(),last_metrics_=Monotonic::now();
    Monotonic::time_point source_activity_=Monotonic::now();
    bool source_idle_running_=false;
    Monotonic::time_point previous_=Monotonic::now(),pointer_time_=Monotonic::now();
    void metrics(bool force=false) {
        if(metrics_path_.empty())return;
        const auto now=Monotonic::now();
        if(!force&&now-last_metrics_<std::chrono::seconds(1))return;
        last_metrics_=now;
        std::ofstream output(metrics_path_,std::ios::binary|std::ios::trunc);
        if(!output)return; // Optional diagnostics must not interrupt interaction.
        output<<std::fixed<<std::setprecision(4)<<"{\n  \"elapsed_seconds\": "<<std::chrono::duration<double>(now-started_).count()
              <<",\n  \"frames\": "<<frames_<<",\n  \"drag_starts\": "<<drag_starts_<<",\n  \"drag_updates\": "<<drag_updates_
              <<",\n  \"releases\": "<<releases_<<",\n  \"expirations\": "<<expirations_
              <<",\n  \"animation_timer\": "<<(timer_running_?"true":"false")<<",\n  \"visible\": "<<(visible()?"true":"false")
              <<",\n  \"capture_active\": "<<(capture_&&capture_->active()?"true":"false")
              <<",\n  \"capture_pending\": "<<(capture_&&capture_->pending()?"true":"false")
              <<",\n  \"capture_frames\": "<<(capture_?capture_->frames():0)
              <<",\n  \"capture_frames_peak\": "<<max_capture_frames_
              <<",\n  \"capture_sessions\": "<<capture_sessions_
              <<",\n  \"drops\": [";
        bool separator=false;
        for(const auto& drop:world_.drops())if(drop.alive) {
            if(separator)output<<',';
            separator=true;
            output<<"{\"phase\": \""<<(drop.phase==Phase::Preview?"preview":drop.phase==Phase::Dragging?"dragging":drop.phase==Phase::Puddle?"puddle":"falling")
                  <<"\", \"x\": "<<drop.position.x<<", \"y\": "<<drop.position.y<<", \"age\": "<<drop.age
                  <<", \"rx\": "<<drop.radius_x()<<", \"ry\": "<<drop.radius_y()<<", \"rz\": "<<drop.radius_z()<<", \"angle\": "<<drop.angle<<'}';
        }
        output<<"]\n}\n";
    }
    bool visible()const{return !hidden_check_&&!closing_&&!locked_&&!suspended_&&!display_off_&&IsWindowVisible(window_)&&!IsIconic(window_);}
    void invalidate(){if(visible())InvalidateRect(window_,nullptr,FALSE);}
    void timer() {
        const bool needed=visible()&&world_.animated();
        if(needed&&!timer_running_){previous_=Monotonic::now();if(!SetTimer(window_,frame_timer,16,nullptr))throw std::runtime_error("Cannot schedule liquid animation");timer_running_=true;}
        else if(!needed&&timer_running_){KillTimer(window_,frame_timer);timer_running_=false;}
    }
    void release_drag(){if(world_.dragging())++releases_;world_.release();if(GetCapture()==window_)ReleaseCapture();timer();invalidate();metrics(true);}
    void release_source() {
        KillTimer(window_,source_idle_timer);source_idle_running_=false;
        renderer_.clear_source();
        if(capture_)capture_->release();
        if(picker_)EnableWindow(picker_,TRUE);
        source_status_=L"Capture stopped. Choose a source in Windows again.";
    }
    void source_activity(bool reset=true) {
        const auto now=Monotonic::now();
        if(reset)source_activity_=now;
        const bool preview=visible()&&capture_&&capture_->active()&&!world_.animated()&&world_.count()!=0;
        if(preview&&!source_idle_running_) {
            const auto inactive=now-source_activity_;
            if(inactive>=source_idle_timeout) {
                release_source();source_status_=L"Capture stopped after 15 s without movement. Click to select again.";return;
            }
            const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(source_idle_timeout-inactive).count();
            if(!SetTimer(window_,source_idle_timer,static_cast<UINT>(std::max<std::int64_t>(1,remaining)),nullptr)) {
                release_source();source_status_=L"Capture stopped: idle deadline unavailable.";return;
            }
            source_idle_running_=true;
        } else if(!preview&&source_idle_running_) {KillTimer(window_,source_idle_timer);source_idle_running_=false;}
    }
    void pause(){KillTimer(window_,frame_timer);timer_running_=false;world_.clear();if(GetCapture()==window_)ReleaseCapture();release_source();renderer_.release();metrics(true);}
    void choose_source() {
        if(!desktop_||!visible())return;
        if(!capture_)capture_=std::make_unique<alarmapp::ui::DesktopCapture>();
        if(capture_->pending())return;
        release_drag();
        RECT client{};GetClientRect(window_,&client);const double scale=96.0/GetDpiForWindow(window_);
        world_.seed({client.right*scale*.5,client.bottom*scale*.38});timer();
        renderer_.clear_source();
        std::wstring error;
        if(!capture_->attached()&&!capture_->attach(window_,renderer_.device(window_,GetDpiForWindow(window_)),error))source_status_=error;
        else if(!capture_->request_picker(error))source_status_=error;
        else source_status_=L"Windows picker open: choose a source. Cancel leaves the source inactive.";
        EnableWindow(picker_,capture_->pending()?FALSE:TRUE);invalidate();metrics(true);
    }
    Vec2 point(LPARAM value)const {const double scale=96.0/GetDpiForWindow(window_);return {GET_X_LPARAM(value)*scale,GET_Y_LPARAM(value)*scale};}
    void layout() {
        RECT client{};GetClientRect(window_,&client);const auto dpi=GetDpiForWindow(window_);
        world_.bounds(std::max(128.0,client.right*96.0/dpi),std::max(128.0,client.bottom*96.0/dpi));
        SetWindowPos(close_,nullptr,client.right-MulDiv(104,dpi,96),MulDiv(20,dpi,96),MulDiv(86,dpi,96),MulDiv(39,dpi,96),SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
        if(picker_)SetWindowPos(picker_,nullptr,MulDiv(34,dpi,96),MulDiv(88,dpi,96),MulDiv(156,dpi,96),MulDiv(34,dpi,96),SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
        invalidate();
    }
    static LRESULT CALLBACK close_proc(HWND h,UINT msg,WPARAM wparam,LPARAM lparam,UINT_PTR,DWORD_PTR data) {
        auto app=reinterpret_cast<Demo*>(data);
        if(msg==WM_PAINT){PAINTSTRUCT painting{};BeginPaint(h,&painting);EndPaint(h,&painting);return 0;}
        if(msg==WM_ERASEBKGND)return 1;
        if(msg==WM_KEYDOWN&&wparam==VK_ESCAPE){DestroyWindow(app->window_);return 0;}
        if(msg==WM_MOUSEMOVE&&app->visible()) {
            POINT pointer{};
            if(GetCursorPos(&pointer)&&ScreenToClient(app->window_,&pointer))app->world_.pointer({pointer.x*96.0/GetDpiForWindow(app->window_),pointer.y*96.0/GetDpiForWindow(app->window_)});
            app->source_activity();app->invalidate();
        }
        if(msg==WM_SETFOCUS||msg==WM_KILLFOCUS)app->invalidate();
        if(msg==WM_NCDESTROY)RemoveWindowSubclass(h,close_proc,1);
        return DefSubclassProc(h,msg,wparam,lparam);
    }
    LRESULT message(UINT msg,WPARAM wparam,LPARAM lparam) {
        switch(msg) {
        case WM_ERASEBKGND:return 1;
        case WM_PAINT: {
            PAINTSTRUCT painting{};BeginPaint(window_,&painting);EndPaint(window_,&painting);
            if(visible()){renderer_.draw(window_,world_,GetDpiForWindow(window_),grid_,nullptr,capture_?capture_->texture():nullptr,desktop_,source_status_);++frames_;metrics();}
            return 0;
        }
        case WM_SIZE:if(wparam==SIZE_MINIMIZED)pause();else if(close_)layout();timer();return 0;
        case WM_SHOWWINDOW:if(!wparam)pause();else{previous_=Monotonic::now();timer();invalidate();}return 0;
        case WM_DPICHANGED: {
            const auto rect=reinterpret_cast<RECT*>(lparam);
            SetWindowPos(window_,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);layout();return 0;
        }
        case WM_LBUTTONDOWN:
            if(desktop_&&(!capture_||!capture_->texture()))return 0;
            if(visible()){world_.begin_drag(point(lparam));++drag_starts_;pointer_time_=Monotonic::now();SetCapture(window_);source_activity();timer();invalidate();metrics(true);}return 0;
        case WM_MOUSEMOVE:
            if(visible()) {
                const auto now=Monotonic::now();const auto elapsed=std::chrono::duration<double>(now-pointer_time_).count();
                if(world_.dragging())++drag_updates_;
                world_.drag_to(point(lparam),elapsed);pointer_time_=now;source_activity();invalidate();
            }return 0;
        case WM_LBUTTONUP:release_drag();return 0;
        case WM_CAPTURECHANGED:if(!closing_&&world_.dragging()&&reinterpret_cast<HWND>(lparam)!=window_)release_drag();return 0;
        case WM_CANCELMODE:release_drag();return 0;
        case WM_ACTIVATE:if(LOWORD(wparam)==WA_INACTIVE)release_drag();return 0;
        case WM_KEYDOWN:if(wparam==VK_ESCAPE)DestroyWindow(window_);else if(wparam==VK_F12)metrics(true);return 0;
        case WM_COMMAND:
            if(HIWORD(wparam)==BN_CLICKED){if(LOWORD(wparam)==close_id)DestroyWindow(window_);else if(LOWORD(wparam)==picker_id)choose_source();}return 0;
        case alarmapp::ui::DesktopCapture::frame_message:
            if(capture_&&capture_->attached()) {
                if(!visible()||world_.count()==0){release_source();return 0;}
                const bool was_active=capture_->active();renderer_.unbind_source();std::wstring error;
                if(!capture_->poll(error))source_status_=error;
                else source_status_=capture_->active()?L"Source active in GPU memory. Stops after 15 s without movement. Esc closes.":capture_->status();
                max_capture_frames_=std::max(max_capture_frames_,capture_->frames());
                if(!was_active&&capture_->active()){++capture_sessions_;source_activity();}
                if(!capture_->texture())renderer_.clear_source();
                if(!capture_->active()&&source_idle_running_){KillTimer(window_,source_idle_timer);source_idle_running_=false;}
                if(picker_)EnableWindow(picker_,capture_->pending()?FALSE:TRUE);
                invalidate();metrics();
            }return 0;
        case WM_TIMER:
            if(wparam==source_idle_timer) {
                KillTimer(window_,source_idle_timer);source_idle_running_=false;
                if(visible()&&capture_&&capture_->active()&&!world_.animated()) {
                    const auto inactive=Monotonic::now()-source_activity_;
                    if(inactive>=source_idle_timeout){release_source();source_status_=L"Capture stopped after 15 s without movement. Click to select again.";invalidate();metrics(true);}
                    else {
                        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(source_idle_timeout-inactive).count();
                        if(!SetTimer(window_,source_idle_timer,static_cast<UINT>(std::max<std::int64_t>(1,remaining)),nullptr))throw std::runtime_error("Cannot bound idle capture");
                        source_idle_running_=true;
                    }
                }
                return 0;
            }
            if(wparam==frame_timer&&visible()) {
                const auto now=Monotonic::now();const auto count_before=world_.count();const bool animated_before=world_.animated();
                world_.advance(std::chrono::duration<double>(now-previous_).count());previous_=now;
                expirations_+=count_before-world_.count();
                if(count_before&&world_.count()==0&&capture_&&capture_->attached())release_source();
                else if(animated_before&&!world_.animated())source_activity(false);
                POINT pointer{};if(GetCursorPos(&pointer)&&ScreenToClient(window_,&pointer))world_.pointer({pointer.x*96.0/GetDpiForWindow(window_),pointer.y*96.0/GetDpiForWindow(window_)});
                timer();invalidate();metrics(count_before!=world_.count());
            }return 0;
        case WM_WTSSESSION_CHANGE:
            if(wparam==WTS_SESSION_LOCK){locked_=true;pause();}
            else if(wparam==WTS_SESSION_UNLOCK){locked_=false;timer();invalidate();}
            return 0;
        case WM_POWERBROADCAST:
            if(wparam==PBT_APMSUSPEND){suspended_=true;pause();}
            else if(wparam==PBT_APMRESUMEAUTOMATIC||wparam==PBT_APMRESUMESUSPEND){suspended_=false;timer();invalidate();}
            else if(wparam==PBT_POWERSETTINGCHANGE) {
                const auto setting=reinterpret_cast<POWERBROADCAST_SETTING*>(lparam);
                if(setting&&IsEqualGUID(setting->PowerSetting,GUID_CONSOLE_DISPLAY_STATE)&&setting->DataLength>=sizeof(DWORD)) {
                    DWORD value=0;std::memcpy(&value,setting->Data,sizeof(value));display_off_=value==0;
                    if(display_off_)pause();else{timer();invalidate();}
                }
            }return TRUE;
        case WM_QUERYENDSESSION:return TRUE;
        case WM_ENDSESSION:if(wparam)DestroyWindow(window_);return 0;
        case WM_CLOSE:DestroyWindow(window_);return 0;
        case WM_DESTROY:closing_=true;pause();PostQuitMessage(exit_code_);return 0;
        }
        return DefWindowProcW(window_,msg,wparam,lparam);
    }
public:
    Demo(bool grid,bool desktop,bool hidden_check,std::filesystem::path metrics_path):grid_(grid),desktop_(desktop),hidden_check_(hidden_check),metrics_path_(std::move(metrics_path)){}
    ~Demo() {
        if(power_notify_)UnregisterSuspendResumeNotification(power_notify_);
        if(display_notify_)UnregisterPowerSettingNotification(display_notify_);
        if(session_registered_&&window_)WTSUnRegisterSessionNotification(window_);
        if(window_&&IsWindow(window_))DestroyWindow(window_);
    }
    static LRESULT CALLBACK procedure(HWND h,UINT msg,WPARAM wparam,LPARAM lparam) {
        auto app=reinterpret_cast<Demo*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(msg==WM_NCCREATE){app=static_cast<Demo*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);app->window_=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}
        if(!app)return DefWindowProcW(h,msg,wparam,lparam);
        try{return app->message(msg,wparam,lparam);}
        catch(const std::exception& error) {
            app->exit_code_=1;
            if(!app->hidden_check_){const std::string text=error.what();MessageBoxA(h,text.c_str(),"ALARM liquid demo",MB_OK|MB_ICONERROR);}
            DestroyWindow(h);return 0;
        }
    }
    bool create(HINSTANCE instance) {
        WNDCLASSW definition{};definition.hInstance=instance;definition.lpfnWndProc=procedure;definition.lpszClassName=window_class;definition.hCursor=LoadCursorW(nullptr,IDC_HAND);
        if(!RegisterClassW(&definition))return false;
        POINT mouse{};GetCursorPos(&mouse);MONITORINFO monitor{sizeof(monitor),{}, {},0};
        if(!GetMonitorInfoW(MonitorFromPoint(mouse,MONITOR_DEFAULTTOPRIMARY),&monitor))return false;
        const RECT area=hidden_check_?RECT{0,0,640,480}:monitor.rcWork;
        const DWORD extended_style=WS_EX_NOREDIRECTIONBITMAP|(hidden_check_?WS_EX_TOOLWINDOW:WS_EX_APPWINDOW);
        window_=CreateWindowExW(extended_style,window_class,L"ALARM · Liquid playground",WS_POPUP,area.left,area.top,area.right-area.left,area.bottom-area.top,nullptr,nullptr,instance,this);
        if(!window_)return false;
        close_=CreateWindowExW(0,L"BUTTON",L"Close · Esc",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,0,0,86,39,window_,reinterpret_cast<HMENU>(close_id),instance,nullptr);
        if(!close_)return false;
        SetWindowSubclass(close_,close_proc,1,reinterpret_cast<DWORD_PTR>(this));
        if(desktop_) {
            picker_=CreateWindowExW(0,L"BUTTON",L"Choose screen",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,0,0,156,34,window_,reinterpret_cast<HMENU>(picker_id),instance,nullptr);
            if(!picker_)return false;
            SetWindowSubclass(picker_,close_proc,1,reinterpret_cast<DWORD_PTR>(this));
        }
        layout();RECT client{};GetClientRect(window_,&client);const double scale=96.0/GetDpiForWindow(window_);
        world_.seed({client.right*scale*.5,client.bottom*scale*.38});world_.pointer({client.right*scale*.34,client.bottom*scale*.21});
        if(!hidden_check_) {
            power_notify_=RegisterSuspendResumeNotification(window_,DEVICE_NOTIFY_WINDOW_HANDLE);
            display_notify_=RegisterPowerSettingNotification(window_,&GUID_CONSOLE_DISPLAY_STATE,DEVICE_NOTIFY_WINDOW_HANDLE);
            session_registered_=WTSRegisterSessionNotification(window_,NOTIFY_FOR_THIS_SESSION)!=FALSE;
            if(!power_notify_||!display_notify_||!session_registered_)throw std::runtime_error("Cannot register safe demo pause events");
            ShowWindow(window_,SW_SHOWNORMAL);UpdateWindow(window_);
        }
        return true;
    }
    HWND window()const{return window_;}
    void check(const std::filesystem::path& report) {
        if(!hidden_check_||IsWindowVisible(window_))throw std::runtime_error("Graphics check must stay hidden");
        std::vector<unsigned char> left,right,repeat;
        world_.pointer({120,180});renderer_.draw(window_,world_,96,false,&left);
        renderer_.draw(window_,world_,96,false,&repeat);
        if(left!=repeat)throw std::runtime_error("Identical liquid state changed its own texture pixels");
        world_.pointer({520,180});renderer_.draw(window_,world_,96,false,&right);
        if(left==right)throw std::runtime_error("Mouse-directed light/shadow did not change own texture pixels");
        renderer_.draw(window_,world_,96,false,&repeat,nullptr,false,{},true);
        if(repeat!=right)throw std::runtime_error("Scissored liquid/shadow differs from the full-viewport shader");
        renderer_.draw(window_,world_,96,true,&repeat);
        if(repeat==right)throw std::runtime_error("Explicit refraction test grid did not change own pixels");
        {
            World empty(640,480);empty.pointer(world_.mouse());
            std::vector<unsigned char> waiting_with_drop,waiting_without_drop;
            renderer_.draw(window_,world_,96,false,&waiting_with_drop,nullptr,true,L"Choose a source in Windows.");
            renderer_.draw(window_,empty,96,false,&waiting_without_drop,nullptr,true,L"Choose a source in Windows.");
            if(waiting_with_drop!=waiting_without_drop)
                throw std::runtime_error("Desktop waiting state rendered a false lens/shadow without a source");
        }
        {
            auto source=renderer_.own_test_source(window_);
            std::vector<unsigned char> texture_frame,texture_repeat;
            renderer_.draw(window_,world_,96,false,&texture_frame,source.Get());
            renderer_.draw(window_,world_,96,false,&texture_repeat,source.Get());
            if(texture_frame!=texture_repeat||texture_frame==right||texture_frame==repeat)
                throw std::runtime_error("Own SRV texture refraction is not deterministic/distinct");
            renderer_.draw(window_,world_,96,false,&texture_repeat,source.Get(),false,{},true);
            if(texture_frame!=texture_repeat)throw std::runtime_error("Scissored SRV refraction differs from the full-viewport shader");
            renderer_.clear_source();
        }
        std::size_t droplet_boundary_changes=0,puddle_boundary_changes=0;
        {
            auto source=renderer_.own_test_source(window_,true);
            World proof(640,480);proof.seed({280,220});proof.pointer({500,180});
            auto boundary_proof=[&](const char* phase) {
                std::vector<unsigned char> undeviated,refracted;
                renderer_.draw(window_,proof,96,false,&undeviated,source.Get(),false,{},false,false);
                renderer_.draw(window_,proof,96,false,&refracted,source.Get());
                std::size_t changed=0;unsigned max_color_change=0;
                const auto& shape=proof.drops()[0];
                for(UINT y=0;y<480;++y)for(UINT x=0;x<640;++x) {
                    const auto offset=(static_cast<std::size_t>(y)*640+x)*4;
                    if(undeviated[offset+3]!=refracted[offset+3])throw std::runtime_error("Optical source deviation changed opacity");
                    bool different=false;
                    for(unsigned channel=0;channel<3;++channel) {
                        const unsigned delta=static_cast<unsigned>(std::abs(static_cast<int>(undeviated[offset+channel])-static_cast<int>(refracted[offset+channel])));
                        max_color_change=std::max(max_color_change,delta);different=different||delta!=0;
                    }
                    if(different) {
                        ++changed;
                        const auto dx=(x+.5-shape.position.x)/shape.radius_x(),dy=(y+.5-shape.position.y)/shape.radius_y();
                        if(dx*dx+dy*dy>1.001)throw std::runtime_error("Black/white refraction changed pixels outside the water");
                    }
                }
                save_own_optical_bmp(report.parent_path()/(std::string("liquid-")+phase+"-undeviated.bmp"),undeviated,640,480);
                save_own_optical_bmp(report.parent_path()/(std::string("liquid-")+phase+"-refracted.bmp"),refracted,640,480);
                if(changed<32||max_color_change<96)throw std::runtime_error(std::string(phase)+" did not visibly bend the own black/white boundary: pixels="+std::to_string(changed)+", max_delta="+std::to_string(max_color_change));
                return changed;
            };
            droplet_boundary_changes=boundary_proof("droplet");
            proof.begin_drag({280,220});proof.release();
            for(int step=0;step<240&&proof.drops()[0].phase!=Phase::Puddle;++step)proof.advance(fixed_step);
            if(proof.drops()[0].phase!=Phase::Puddle)throw std::runtime_error("Own fluid proof never reached the floor");
            for(int step=0;step<120;++step)proof.advance(fixed_step);
            if(proof.drops()[0].radius_x()/proof.drops()[0].radius_y()<40)throw std::runtime_error("Own fluid proof stayed spherical");
            puddle_boundary_changes=boundary_proof("puddle");renderer_.clear_source();
        }
        renderer_.release();renderer_.draw(window_,world_,96,false,&repeat);
        if(repeat!=right)throw std::runtime_error("Release/recreate changed the liquid reference frame");
        if(IsWindowVisible(window_))throw std::runtime_error("Hidden check displayed its window");
        std::ofstream output(report,std::ios::binary);
        output<<"PASS hidden liquid D3D/HLSL/D2D/DComp creation and presentation\nPASS deterministic own texture, mouse-light/shadow response, explicit grid mode, own generated SRV refraction, scissored/full-viewport pixel equivalence, release/recreate\nPASS own 50% black/white boundary bends inside droplet ("<<droplet_boundary_changes<<" changed pixels) and fluid puddle ("<<puddle_boundary_changes<<" changed pixels), opacity/external pixels unchanged\nOwn generated proof pairs: liquid-droplet/puddle-undeviated/refracted.bmp\nNo desktop pixels captured/read, no capture module/picker instantiated, no window shown, no input/sound/lock/settings action.\n";
        output.close();if(!output)throw std::runtime_error("Cannot write liquid check report");
    }
};
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int) {
    int count=0;LPWSTR* args=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!args)return 2;
    bool grid=false,desktop=false,transparent=false,check=false;std::filesystem::path report,metrics_path;
    for(int i=1;i<count;++i) {
        const std::wstring value=args[i];
        if(value==L"--grid")grid=true;
        else if(value==L"--desktop")desktop=true;
        else if(value==L"--transparent")transparent=true;
        else if(value==L"--check")check=true;
        else if(value==L"--report"&&i+1<count)report=args[++i];
        else if(value==L"--metrics"&&i+1<count)metrics_path=args[++i];
        else{LocalFree(args);return 2;}
    }
    LocalFree(args);
    const int visible_modes=static_cast<int>(grid)+static_cast<int>(desktop)+static_cast<int>(transparent);
    if(visible_modes>1||(check&&(report.empty()||visible_modes))||(!check&&!report.empty()))return 2;
    if(!check&&!grid&&!transparent)desktop=true;
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(initialized))return 1;
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES};InitCommonControlsEx(&common);
    int result=0;
    try {
        Demo demo(grid,desktop,check,std::move(metrics_path));
        if(!demo.create(instance))throw std::runtime_error("Cannot create separate liquid window");
        if(check)demo.check(report);
        else {
            MSG message{};BOOL read;
            while((read=GetMessageW(&message,nullptr,0,0))>0) {
                if(message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE){DestroyWindow(demo.window());continue;}
                if(message.message==WM_KEYDOWN&&message.wParam==VK_F12){SendMessageW(demo.window(),WM_KEYDOWN,VK_F12,0);continue;}
                if(!IsDialogMessageW(demo.window(),&message)){TranslateMessage(&message);DispatchMessageW(&message);}
            }
            result=read<0?1:static_cast<int>(message.wParam);
        }
    }catch(const std::exception& error) {
        result=1;
        if(check){std::ofstream output(report,std::ios::binary);output<<"FAIL "<<error.what()<<'\n';}
        else MessageBoxA(nullptr,error.what(),"ALARM liquid demo",MB_OK|MB_ICONERROR);
    }
    CoUninitialize();return result;
}
