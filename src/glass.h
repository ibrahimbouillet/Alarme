// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alarmapp::ui {
struct GlassControl {
    int id=0;
    float left=0,top=0,right=0,bottom=0;
    std::wstring text;
    bool enabled=true,checked=false,pressed=false,focused=false;
    unsigned selection_start=0,selection_end=0;
};
struct GlassState {
    bool enabled=false, ringing=false, silent=false, settings=false, diagnostics=false, audio_error=false, audio_starting=false, preview=false;
    std::wstring next, detail, sound;
    float width=580, height=720, pointer_x=290, pointer_y=330;
    bool editing=false, active=true,keyboard_focus=true;
    std::vector<GlassControl> controls;
    std::vector<std::wstring> devices;
    int selected_device=0,device_top=0;
};

class Glass {
public:
    Glass();
    ~Glass();
    Glass(const Glass&)=delete;
    Glass& operator=(const Glass&)=delete;
    void configure(HWND window);
    void release_visuals();
    bool render(HWND window,const GlassState& state,UINT dpi);
    void paint(HDC dc,const RECT& region,const GlassState& state,UINT dpi);
    void hover(int id,bool hovered);
    void press(int id,bool pressed);
    // Logical window DIPs. The owner decides when a hold becomes a drag and
    // owns mouse capture/activation. Movement and release are bounded springs.
    void begin_drag(int id,float x,float y);
    void drag(float x,float y);
    void end_drag();
    bool tick(bool ringing);
    bool animation_enabled() const;
    void settings_changed();
    void transition();
    void reveal_devices();
    std::wstring error() const;
    unsigned long long frames() const;
    unsigned generations() const;
    unsigned long long lighting_draws() const;
    unsigned long long refraction_draws() const;
    // Diagnostic only: generate an internal grid and read this app's GPU
    // backbuffer before Present. No desktop, DWM or external image is sampled.
    // Pixels are BGRA8 premultiplied alpha, in row-major order.
    bool render_own_test_frame(HWND window,const GlassState& state,UINT dpi,
                              bool refraction,std::vector<std::uint32_t>& pixels,
                              UINT& width,UINT& height,bool black_white=false);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
