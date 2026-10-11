// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#pragma once
#include <windows.h>
#include <dxgi.h>
#include <memory>
#include <string>
#include <vector>

namespace alarmapp::ui {
// Coordinates, corner radius and displacement are physical client pixels.
struct OpticalLens {
    float left=0, top=0, right=0, bottom=0;
    float radius=24;
    float strength=12; // Maximum inward sample displacement, 0..24 pixels.
    bool operator==(const OpticalLens&) const = default;
};

// Experimental: graph attachment succeeds on the tested system, but a private
// Sky observation returned black HostBackdrop pixels. This prototype is not a
// visually validated desktop-refraction renderer or a production fallback.
// UI-thread-only. The HWND must have no other active topmost composition target.
// HostBackdrop is sampled only inside the Windows compositor; no capture or
// readback of desktop pixels is performed. Unsupported systems report failure.
class OpticalSurface {
public:
    OpticalSurface();
    ~OpticalSurface();
    OpticalSurface(const OpticalSurface&)=delete;
    OpticalSurface& operator=(const OpticalSurface&)=delete;
    bool attach(HWND window,IDXGISwapChain* ui_surface,UINT width,UINT height,std::wstring& error);
    bool resize(UINT width,UINT height,std::wstring& error);
    bool set_lenses(const std::vector<OpticalLens>& lenses,std::wstring& error);
    void release() noexcept; // Releases every visual/brush/target; queue stays idle until destruction.
    bool attached() const noexcept;
    unsigned zones() const noexcept;
    std::wstring error() const;
    std::wstring diagnostics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
