// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

// Isolated renderer checks: private hidden windows and memory DIBs only.
// No ALARM engine, audio, tray, notifications, desktop capture or input.
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "glass.h"
#include "ui_ids.h"

namespace {
using namespace alarmapp::ui;
int assertions = 0;
void require(bool condition, const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}
void require_render(Glass& glass, HWND window, const GlassState& state, UINT dpi, const char* message) {
    const bool rendered = glass.render(window, state, dpi);
    if (!rendered) std::wcerr << L"Renderer error: " << glass.error() << L'\n';
    require(rendered, message);
}

struct WindowClass {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    static constexpr wchar_t name[] = L"ALARM_Isolated_Renderer_Test";
    WindowClass() {
        WNDCLASSW value{};
        value.hInstance = instance;
        value.lpfnWndProc = DefWindowProcW;
        value.lpszClassName = name;
        require(RegisterClassW(&value) != 0, "Private window class registers");
    }
    ~WindowClass() { UnregisterClassW(name, instance); }
};
struct HiddenWindow {
    HWND handle = nullptr;
    HiddenWindow(const WindowClass& window_class, int width, int height) {
        handle = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
            WindowClass::name, L"Private renderer test", WS_POPUP,
            0, 0, width, height, nullptr, nullptr, window_class.instance, nullptr);
        require(handle != nullptr, "Private hidden window creates");
        require(!IsWindowVisible(handle), "Test window is never visible");
    }
    ~HiddenWindow() { if (handle) DestroyWindow(handle); }
    void resize(int width, int height) {
        require(SetWindowPos(handle, nullptr, 0, 0, width, height,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW) != FALSE,
            "Private hidden window resizes");
        require(!IsWindowVisible(handle), "Resize never shows the test window");
    }
    HWND destroy() {
        const HWND old = handle;
        require(DestroyWindow(handle) != FALSE, "Private hidden window destroys");
        handle = nullptr;
        return old;
    }
    HiddenWindow(const HiddenWindow&) = delete;
    HiddenWindow& operator=(const HiddenWindow&) = delete;
};
struct Bitmap {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;
    void* data = nullptr;
    int width, height;
    Bitmap(int logical_width, int logical_height, UINT dpi)
        : width(MulDiv(logical_width, dpi, 96)), height(MulDiv(logical_height, dpi, 96)) {
        dc = CreateCompatibleDC(nullptr);
        if (!dc) throw std::runtime_error("Cannot create private memory DC");
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &data, nullptr, 0);
        if (!bitmap) { DeleteDC(dc); throw std::runtime_error("Cannot create private memory bitmap"); }
        old = SelectObject(dc, bitmap);
        if (!old || old == HGDI_ERROR) {
            DeleteObject(bitmap); DeleteDC(dc);
            throw std::runtime_error("Cannot select private memory bitmap");
        }
        std::memset(data, 0, bytes());
    }
    ~Bitmap() { SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); }
    std::size_t bytes() const { return static_cast<std::size_t>(width) * height * 4; }
    std::vector<std::uint32_t> pixels() const {
        GdiFlush();
        const auto first = static_cast<const std::uint32_t*>(data);
        return {first, first + static_cast<std::size_t>(width) * height};
    }
    void paint(Glass& glass, const GlassState& state, UINT dpi) {
        const RECT region{0, 0, width, height};
        glass.paint(dc, region, state, dpi);
        GdiFlush();
    }
    Bitmap(const Bitmap&) = delete;
    Bitmap& operator=(const Bitmap&) = delete;
};

GlassState off_state(float width = 580, float height = 720) {
    GlassState state;
    state.width = width; state.height = height;
    state.next = L"Choose a time, then enable your alarm.";
    state.sound = L"Built-in ALARM tone";
    auto add = [&](int id, const wchar_t* text, float x, float y, float w, float h) {
        GlassControl control;
        control.id = id; control.text = text;
        control.left = x; control.top = y; control.right = x + w; control.bottom = y + h;
        state.controls.push_back(std::move(control));
    };
    add(CaptionMinimize, L"Minimize", width - 126, 5, 36, 32);
    add(CaptionMaximize, L"Maximize", width - 84, 5, 36, 32);
    add(CaptionClose, L"Close", width - 42, 5, 36, 32);
    add(Settings, L"Settings", width - 154, 68, 126, 36);
    add(Time, L"06:00", 54, 248, width - 108, 103);
    add(Save, L"Enable alarm", 26, 478, width - 52, 60);
    add(TestNow, L"Listen to sound", 26, 552, width - 52, 40);
    return state;
}
GlassControl& find_control(GlassState& state, int id) {
    const auto found = std::find_if(state.controls.begin(), state.controls.end(),
        [id](const auto& control) { return control.id == id; });
    if (found == state.controls.end()) throw std::runtime_error("Missing fixture control");
    return *found;
}
bool differs_in_region(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b,
                       int width, const RECT& region) {
    for (LONG y = region.top; y < region.bottom; ++y)
        for (LONG x = region.left; x < region.right; ++x)
            if (a[static_cast<std::size_t>(y) * width + x] != b[static_cast<std::size_t>(y) * width + x]) return true;
    return false;
}

void artifact_tests() {
    Glass glass;
    const auto state = off_state();
    Bitmap image96(580, 720, 96);
    image96.paint(glass, state, 96);
    const auto reference96 = image96.pixels();
    require(std::any_of(reference96.begin(), reference96.end(), [](auto pixel) { return pixel != 0; }), "Artifact contains rendered pixels");
    auto background_only = state;
    background_only.controls.clear();
    image96.paint(glass, background_only, 96);
    const auto without_controls = image96.pixels();
    require(differs_in_region(reference96, without_controls, image96.width, RECT{54, 248, 526, 351}), "Unified frame includes clock text");
    require(differs_in_region(reference96, without_controls, image96.width, RECT{26, 478, 554, 538}), "Unified frame includes primary button");

    image96.paint(glass, state, 96); // warm all stable resources
    const DWORD gdi_before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD user_before = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    for (int frame = 0; frame < 24; ++frame) {
        image96.paint(glass, state, 96);
        require(image96.pixels() == reference96, "Identical idle state renders identical pixels");
    }
    require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= gdi_before + 2, "Stable artifact frames do not grow GDI objects");
    require(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) <= user_before + 2, "Stable artifact frames do not grow USER objects");
    require(glass.frames() == 0 && glass.generations() == 0, "Artifact rendering never creates a DComp presentation");

    auto focused = state;
    find_control(focused, Save).focused = true;
    image96.paint(glass, focused, 96);
    require(differs_in_region(reference96, image96.pixels(), image96.width, RECT{26, 478, 554, 538}), "Keyboard focus appears inside the primary button");
    auto editing = state;
    editing.editing = true;
    auto& time = find_control(editing, Time);
    time.id = TimeInput; time.focused = true; time.selection_end = 5;
    image96.paint(glass, editing, 96);
    const auto selection = image96.pixels();
    time.selection_end = 0;
    image96.paint(glass, editing, 96);
    require(differs_in_region(selection, image96.pixels(), image96.width, RECT{54, 248, 526, 351}), "Edit selection and caret produce distinct pixels");

    // Changing the DPI on an existing artifact target must match a fresh target.
    Bitmap image144(580, 720, 144);
    image144.paint(glass, state, 144);
    Glass fresh144;
    Bitmap expected144(580, 720, 144);
    expected144.paint(fresh144, state, 144);
    require(image144.pixels() == expected144.pixels(), "96 to 144 DPI switch matches a fresh 144 DPI artifact");
    const auto reference144 = expected144.pixels();
    for (int frame = 0; frame < 8; ++frame) {
        image144.paint(glass, state, 144);
        require(image144.pixels() == reference144, "Repeated 144 DPI idle frames remain identical");
    }
    glass.release_visuals();
    image96.paint(glass, state, 96);
    require(image96.pixels() == reference96, "Artifact release/recreate preserves the complete scene");
    std::cout << "PASS unified artifacts: control/focus/selection pixels, deterministic frames, 96/144 DPI, release/recreate\n";
}

void composition_tests(const WindowClass& window_class) {
    HiddenWindow window(window_class, 580, 720);
    Glass glass;
    auto state = off_state();
    require_render(glass, window.handle, state, 96, "Hidden DComp 96 DPI render succeeds");
    const auto initial_generation = glass.generations();
    require(initial_generation == 1 && glass.frames() == 1, "First DComp frame creates one generation");
    require(glass.error().empty(), "Successful DComp frame has no error");
    const auto initial_lighting = glass.lighting_draws();
    require(initial_lighting > 0, "First GPU frame draws real PointSpecular lighting");
    state.pointer_x = 510; state.pointer_y = 570;
    require_render(glass, window.handle, state, 96, "GPU frame with opposite mouse light succeeds");
    require(glass.lighting_draws() > initial_lighting, "Moving mouse light executes subsequent PointSpecular draws");
    require(glass.generations() == initial_generation, "Mouse lighting changes reuse the same graphics generation");
    const DWORD gdi_before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD user_before = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    for (int frame = 0; frame < 24; ++frame)
        require_render(glass, window.handle, state, 96, "Repeated hidden 96 DPI composition succeeds");
    require(glass.generations() == initial_generation, "Stable DComp frames reuse the device/swapchain generation");
    require(glass.frames() == 26, "DComp frame counter records every presentation");
    require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= gdi_before + 4, "Stable DComp frames do not grow GDI objects");
    require(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) <= user_before + 4, "Stable DComp frames do not grow USER objects");

    window.resize(870, 1080);
    for (int frame = 0; frame < 12; ++frame)
        require_render(glass, window.handle, state, 144, "Hidden DComp 144 DPI render after resize succeeds");
    require(glass.generations() == initial_generation, "Resize reuses the graphics device generation");
    window.resize(730, 840);
    state = off_state(730, 840);
    require_render(glass, window.handle, state, 96, "Second arbitrary resize renders successfully");
    require(!IsWindowVisible(window.handle), "All composition tests remain hidden");

    glass.release_visuals();
    glass.release_visuals();
    require_render(glass, window.handle, state, 96, "Idempotent release permits DComp recreation");
    require(glass.generations() == initial_generation + 1, "Release creates exactly one subsequent generation");
    auto frames_before_failure = glass.frames();
    require(!glass.render(nullptr, state, 96), "Null HWND is rejected");
    require(!glass.error().empty(), "Invalid HWND reports a renderer error");
    require(glass.frames() == frames_before_failure, "Failed render never counts as a presentation");
    require_render(glass, window.handle, state, 96, "Renderer recovers after invalid HWND");
    require(glass.error().empty(), "Recovery clears the renderer failure");

    // Switching between the production composition target and artifact target
    // must release incompatible target resources and permit either path again.
    Bitmap artifact(730, 840, 96);
    artifact.paint(glass, state, 96);
    require_render(glass, window.handle, state, 96, "GPU rendering recovers after an artifact target");
    frames_before_failure = glass.frames();
    const HWND stale = window.destroy();
    require(!glass.render(stale, state, 96), "Destroyed HWND is rejected");
    require(glass.frames() == frames_before_failure, "Destroyed HWND failure preserves frame count");
    HiddenWindow replacement(window_class, 580, 720);
    state = off_state();
    require_render(glass, replacement.handle, state, 96, "A new hidden HWND recovers after destruction");
    require(!IsWindowVisible(replacement.handle), "Recovery never shows its window");
    std::cout << "PASS hidden DComp: repeated 96/144 DPI, stable generations/GUI resources, resize, release, failure/recovery\n";
}

std::vector<std::uint32_t> own_gpu_frame(Glass& glass,HWND window,const GlassState& state,UINT dpi,bool refraction,UINT& width,UINT& height,bool black_white=false) {
    std::vector<std::uint32_t> pixels;
    const bool rendered=glass.render_own_test_frame(window,state,dpi,refraction,pixels,width,height,black_white);
    if(!rendered)std::wcerr<<L"Own GPU probe error: "<<glass.error()<<L'\n';
    require(rendered,"Own generated grid GPU frame succeeds");
    require(width==static_cast<UINT>(MulDiv(580,dpi,96))&&height==static_cast<UINT>(MulDiv(720,dpi,96)),"Own GPU readback has physical DPI dimensions");
    require(pixels.size()==static_cast<std::size_t>(width)*height,"Own GPU readback has exactly one complete private frame");
    return pixels;
}
RECT scaled_region(LONG left,LONG top,LONG right,LONG bottom,UINT dpi) {
    return {MulDiv(left,dpi,96),MulDiv(top,dpi,96),MulDiv(right,dpi,96),MulDiv(bottom,dpi,96)};
}
struct PixelChanges { std::size_t inside=0,outside=0; };
PixelChanges changed_pixels(const std::vector<std::uint32_t>& a,const std::vector<std::uint32_t>& b,
                           UINT width,UINT height,const RECT& allowed) {
    require(a.size()==b.size()&&a.size()==static_cast<std::size_t>(width)*height,
            "Pixel comparison uses two complete private frames of equal dimensions");
    PixelChanges changes;
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
        const auto index=static_cast<std::size_t>(y)*width+x;
        if(a[index]==b[index])continue;
        if(static_cast<LONG>(x)>=allowed.left&&static_cast<LONG>(x)<allowed.right&&
           static_cast<LONG>(y)>=allowed.top&&static_cast<LONG>(y)<allowed.bottom)++changes.inside;
        else ++changes.outside;
    }
    return changes;
}
unsigned settle_gesture(Glass& glass,const char* message) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    unsigned ticks=0;
    bool more=glass.tick(false);
    while(more&&std::chrono::steady_clock::now()<deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        more=glass.tick(false);++ticks;
    }
    require(!more,message);
    require(!glass.tick(false)&&!glass.tick(false),"A settled gesture requests no idle animation frames");
    return ticks;
}
void save_own_bmp(const std::filesystem::path& path,const std::vector<std::uint32_t>& pixels,UINT width,UINT height) {
    BITMAPFILEHEADER header{};BITMAPINFOHEADER info{};
    header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(info);
    header.bfSize=header.bfOffBits+static_cast<DWORD>(pixels.size()*sizeof(std::uint32_t));
    info.biSize=sizeof(info);info.biWidth=static_cast<LONG>(width);info.biHeight=-static_cast<LONG>(height);
    info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;info.biSizeImage=static_cast<DWORD>(pixels.size()*sizeof(std::uint32_t));
    std::ofstream output(path,std::ios::binary);
    output.write(reinterpret_cast<const char*>(&header),sizeof(header));
    output.write(reinterpret_cast<const char*>(&info),sizeof(info));
    output.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()*sizeof(std::uint32_t)));
    output.close();require(static_cast<bool>(output),"Own generated GPU probe BMP writes completely");
}
void refraction_tests(const WindowClass& window_class,const std::filesystem::path& artifact_dir) {
    HiddenWindow window(window_class,580,720);
    Glass glass;
    auto state=off_state();
    UINT width=0,height=0;
    const auto disabled=own_gpu_frame(glass,window.handle,state,96,false,width,height);
    const auto generation=glass.generations();
    require(glass.refraction_draws()==0,"Disabled probe executes no displacement draws");
    const auto enabled=own_gpu_frame(glass,window.handle,state,96,true,width,height);
    const auto refraction_count=glass.refraction_draws();
    require(refraction_count>0,"Enabled probe executes the real displacement effect");
    require(glass.generations()==generation,"Enabling refraction reuses the same GPU generation");
    require(differs_in_region(disabled,enabled,width,scaled_region(48,480,532,492,96)),"Own source grid bends inside the primary button bevel");
    require(differs_in_region(disabled,enabled,width,scaled_region(448,70,530,82,96)),"Own source grid bends inside the settings button bevel");
    require(!differs_in_region(disabled,enabled,width,scaled_region(12,620,568,690,96)),"Refraction leaves the external footer region unchanged");
    require(!differs_in_region(disabled,enabled,width,scaled_region(66,500,100,518,96)),"Neutral primary interior preserves source pixels and alpha exactly");
    const auto repeat=own_gpu_frame(glass,window.handle,state,96,true,width,height);
    require(repeat==enabled,"Repeated refraction state is pixel deterministic");
    require(glass.refraction_draws()>refraction_count,"Repeated probe performs subsequent displacement draws");
    require(glass.generations()==generation,"Repeated probe keeps the original graphics generation");
    if(!artifact_dir.empty()) {
        save_own_bmp(artifact_dir/"renderer-refraction-disabled-96.bmp",disabled,width,height);
        save_own_bmp(artifact_dir/"renderer-refraction-enabled-96.bmp",enabled,width,height);
    }
    window.resize(870,1080);
    const auto disabled144=own_gpu_frame(glass,window.handle,state,144,false,width,height);
    const auto enabled144=own_gpu_frame(glass,window.handle,state,144,true,width,height);
    require(differs_in_region(disabled144,enabled144,width,scaled_region(48,480,532,492,144)),"144 DPI primary bevel bends the generated source grid");
    require(!differs_in_region(disabled144,enabled144,width,scaled_region(12,620,568,690,144)),"144 DPI refraction keeps external pixels untouched");
    require(glass.generations()==generation,"DPI resize reuses the displacement GPU generation");
    glass.release_visuals();
    const auto recreated=own_gpu_frame(glass,window.handle,state,144,true,width,height);
    require(recreated==enabled144,"Release/recreate preserves the refracted GPU pixels");
    require(glass.generations()==generation+1,"Probe recreation adds exactly one generation");
    require(!IsWindowVisible(window.handle),"All real GPU pixel probes keep their own window hidden");
    std::cout<<"PASS own GPU refraction: bevel pixels, neutral alpha/interior, external clip, deterministic frames, 96/144 DPI, release/recreate\n";
}
void boundary_and_edit_tests(const WindowClass& window_class,const std::filesystem::path& artifact_dir) {
    HiddenWindow window(window_class,580,720);
    Glass glass;GlassState state;
    state.controls={{Save,48,480,532,536,L"",true},{Settings,448,64,532,111,L"",true}};
    UINT width=0,height=0;
    const auto before=own_gpu_frame(glass,window.handle,state,96,false,width,height,true);
    const auto after=own_gpu_frame(glass,window.handle,state,96,true,width,height,true);
    std::size_t changed=0;
    bool equal_alpha=true;
    for(std::size_t index=0;index<before.size();++index) {
        changed+=before[index]!=after[index];
        equal_alpha=equal_alpha&&(before[index]>>24)==(after[index]>>24);
    }
    require(changed>32,"Refraction changes a substantial number of private black/white boundary pixels");
    require(equal_alpha,"Black/white optical deviation preserves the source alpha exactly");
    require(!differs_in_region(before,after,width,RECT{100,500,480,516}),"Black/white neutral interior remains exact");
    require(!differs_in_region(before,after,width,RECT{0,620,580,720}),"Black/white refraction leaves the outside footer exact");
    for(const UINT y:{70u,74u,78u,82u,86u,90u,94u,98u}) {
        UINT boundary_before=0,boundary_after=0;
        for(UINT x=515;x<530;++x) {
            if(!boundary_before&&(before[static_cast<std::size_t>(y)*width+x]&255)>80)boundary_before=x;
            if(!boundary_after&&(after[static_cast<std::size_t>(y)*width+x]&255)>80)boundary_after=x;
        }
        require(boundary_before==522,"Private source boundary is straight at the fixture location");
        require(boundary_after>=boundary_before+2&&boundary_after<=boundary_before+6,"Settings lens bends the black/white boundary by two to six physical pixels");
    }
    const auto repeat=own_gpu_frame(glass,window.handle,state,96,true,width,height,true);
    require(repeat==after,"Black/white refracted pixels remain deterministic");
    if(!artifact_dir.empty()) {
        save_own_bmp(artifact_dir/"renderer-bw-undeviated-96.bmp",before,width,height);
        save_own_bmp(artifact_dir/"renderer-bw-refracted-96.bmp",after,width,height);
    }

    state.controls={{Settings,468,550,510,595,L"A",true},{TestNow,533,550,575,595,L"A",true}};
    const auto labels=own_gpu_frame(glass,window.handle,state,96,false,width,height,true);
    for(auto& control:state.controls)control.text.clear();
    const auto plates=own_gpu_frame(glass,window.handle,state,96,false,width,height,true);
    unsigned bright_on_black=0,dark_on_white=0;
    for(UINT y=565;y<582;++y) {
        for(UINT x=482;x<497;++x) {
            const auto index=static_cast<std::size_t>(y)*width+x;
            bright_on_black+=labels[index]!=plates[index]&&(labels[index]&255)>180;
        }
        for(UINT x=547;x<562;++x) {
            const auto index=static_cast<std::size_t>(y)*width+x;
            dark_on_white+=labels[index]!=plates[index]&&(labels[index]&255)<50;
        }
    }
    require(bright_on_black>8,"Actual label glyphs become bright over the private black background");
    require(dark_on_white>8,"Actual label glyphs become dark over the private white background");

    state.keyboard_focus=false; // Mouse focus still needs native edit selection/caret.
    state.controls={{SessionVolume,304,289,366,328,L"50",true,false,false,true,0,2}};
    const auto selected=own_gpu_frame(glass,window.handle,state,96,false,width,height);
    state.controls.front().selection_start=state.controls.front().selection_end=2;
    const auto caret=own_gpu_frame(glass,window.handle,state,96,false,width,height);
    unsigned edit_changed=0,edit_outside=0;
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
        const auto index=static_cast<std::size_t>(y)*width+x;
        if(selected[index]!=caret[index]) {
            if(x>=304&&x<366&&y>=289&&y<328)++edit_changed;
            else ++edit_outside;
        }
    }
    require(edit_changed>32,"Session volume selection and end caret alter real glyph-layout pixels");
    require(edit_outside==0,"Volume selection/caret never alters pixels outside its field");
    require(!IsWindowVisible(window.handle),"Boundary, contrast and edit tests never show their window");
    std::cout<<"PASS private B/W lens: "<<changed<<" changed pixels, measured curved seam, exact alpha/neutral/external pixels; adaptive text and volume selection/caret\n";
}

void gesture_and_focus_tests(const WindowClass& window_class,const std::filesystem::path& artifact_dir) {
    HiddenWindow window(window_class,580,720);
    Glass glass;
    auto state=off_state();
    state.pointer_x=290;state.pointer_y=506;state.keyboard_focus=false;
    UINT width=0,height=0;
    const auto plain=own_gpu_frame(glass,window.handle,state,96,false,width,height);
    const auto baseline=own_gpu_frame(glass,window.handle,state,96,true,width,height);
    const auto generation=glass.generations();
    const auto lighting=glass.lighting_draws(),refraction=glass.refraction_draws();
    const RECT elastic_region{-14,438,594,578}; // Save bounds inflated by 40 DIPs.
    find_control(state,Save).focused=true;
    const auto mouse_focus=own_gpu_frame(glass,window.handle,state,96,true,width,height);
    require(mouse_focus==baseline,"Mouse focus on the primary button paints no keyboard outline");
    state.keyboard_focus=true;
    const auto keyboard_focus=own_gpu_frame(glass,window.handle,state,96,true,width,height);
    const auto focus_changes=changed_pixels(mouse_focus,keyboard_focus,width,height,RECT{26,478,554,538});
    require(focus_changes.inside>32&&focus_changes.outside==0,
            "Keyboard navigation adds a real focus outline strictly inside its button");
    find_control(state,Save).focused=false;state.keyboard_focus=false;

    glass.begin_drag(Save,270,506);
    glass.drag(365,530);
    const auto held_ticks=settle_gesture(glass,"A held elastic gesture settles within two seconds");
    require(held_ticks>0,"Moving a held control requires actual spring integration");
    const auto held_plain=own_gpu_frame(glass,window.handle,state,96,false,width,height);
    const auto held=own_gpu_frame(glass,window.handle,state,96,true,width,height);
    const auto deformation=changed_pixels(baseline,held,width,height,elastic_region);
    require(deformation.inside>256,"Held drag changes substantial real optical surface pixels");
    require(deformation.outside==0,"Held drag does not alter pixels outside its bounded elastic region");
    require(differs_in_region(plain,held_plain,width,RECT{48,480,532,536}),
            "Held deformation changes actual reflected lighting even without refraction");
    std::size_t optical_delta=0;
    for(UINT y=438;y<578;++y)for(UINT x=0;x<width;++x) {
        const auto index=static_cast<std::size_t>(y)*width+x;
        bool changed=false;
        for(unsigned shift:{0u,8u,16u}) {
            const int original=static_cast<int>((baseline[index]>>shift)&255)-static_cast<int>((plain[index]>>shift)&255);
            const int pulled=static_cast<int>((held[index]>>shift)&255)-static_cast<int>((held_plain[index]>>shift)&255);
            changed=changed||original!=pulled;
        }
        optical_delta+=changed;
    }
    require(optical_delta>64,"Held drag changes the real displacement-effect contribution, beyond reflected lighting");
    require(glass.lighting_draws()>lighting&&glass.refraction_draws()>refraction,
            "Elastic frames execute real specular and displacement GPU effects");
    require(glass.generations()==generation,"Holding and deforming reuse the cached graphics generation");
    require(own_gpu_frame(glass,window.handle,state,96,true,width,height)==held,
            "A settled held gesture renders identical pixels without continued animation");

    glass.drag(1000000,-1000000);
    settle_gesture(glass,"Extreme held movement remains bounded and settles promptly");
    const auto extreme=own_gpu_frame(glass,window.handle,state,96,true,width,height);
    const auto extreme_changes=changed_pixels(baseline,extreme,width,height,elastic_region);
    require(extreme_changes.inside>256&&extreme_changes.outside==0,
            "Extreme drag clamps the rendered deformation to the same bounded region");
    glass.end_drag();
    const auto return_ticks=settle_gesture(glass,"Released elastic surface returns within two seconds");
    require(return_ticks>0,"Release performs a spring return instead of leaving a held transform");
    const auto returned=own_gpu_frame(glass,window.handle,state,96,true,width,height);
    require(returned==baseline,"Spring return restores the original GPU pixels exactly");
    require(glass.generations()==generation,"Spring return preserves the cached graphics generation");

    // Keep geometry settled while moving only the light/pointer. This is a
    // shader response, independent of changes to the gesture coordinates.
    glass.begin_drag(Save,290,506);
    settle_gesture(glass,"A stationary held press settles without idle animation");
    state.pointer_x=145;state.pointer_y=504;
    const auto lit_left=own_gpu_frame(glass,window.handle,state,96,true,width,height);
    state.pointer_x=435;
    const auto lit_right=own_gpu_frame(glass,window.handle,state,96,true,width,height);
    require(differs_in_region(lit_left,lit_right,width,RECT{72,509,508,530}),
            "Pressed lower reflection responds to pointer movement at unchanged held geometry");
    require(!differs_in_region(lit_left,lit_right,width,RECT{0,620,580,720}),
            "Pressed pointer light leaves the external footer pixels exact");
    state.pointer_x=290;state.pointer_y=506;
    glass.drag(350,520);settle_gesture(glass,"Final held deformation settles before graphics release");
    glass.release_visuals(); // Release while still held must forget the gesture.
    require(!glass.tick(false),"Graphics release clears the held gesture and all spring animation");
    const auto recreated=own_gpu_frame(glass,window.handle,state,96,true,width,height);
    require(recreated==baseline,"Release during a gesture recreates the undeformed GPU surface");
    require(glass.generations()==generation+1,"Gesture release adds exactly one new GPU generation on recreation");
    glass.drag(1000000,-1000000);
    require(!glass.tick(false)&&own_gpu_frame(glass,window.handle,state,96,true,width,height)==baseline,
            "A stale drag after graphics release cannot deform the recreated surface");
    require(!IsWindowVisible(window.handle),"Gesture and focus probes keep the complete private window hidden");
    if(!artifact_dir.empty()) {
        save_own_bmp(artifact_dir/"renderer-gesture-rest-96.bmp",baseline,width,height);
        save_own_bmp(artifact_dir/"renderer-gesture-held-96.bmp",held,width,height);
        save_own_bmp(artifact_dir/"renderer-gesture-returned-96.bmp",returned,width,height);
    }
    std::cout<<"PASS own GPU gesture: "<<deformation.inside<<" changed pixels / "<<optical_delta
             <<" displacement deltas; bounded hold/return, mouse/keyboard focus, pointer light, stable generation, release clears gesture\n";
}
}

int main(int argc,char** argv) {
    if(argc>2){std::cerr<<"Usage: renderer_tests [own_artifact_directory]\n";return 2;}
    const std::filesystem::path artifact_dir=argc==2?std::filesystem::path(argv[1]):std::filesystem::path{};
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) { std::cerr << "FAIL COM initialization\n"; return 1; }
    int result = 0;
    try {
        WindowClass window_class;
        artifact_tests();
        composition_tests(window_class);
        refraction_tests(window_class,artifact_dir);
        boundary_and_edit_tests(window_class,artifact_dir);
        gesture_and_focus_tests(window_class,artifact_dir);
        std::cout << "All " << assertions << " renderer assertions passed.\n"
                  << "Only private hidden windows and memory DIBs were used; no desktop capture/input/audio.\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << assertions << " assertions: " << error.what() << '\n';
        result = 1;
    }
    CoUninitialize();
    return result;
}
