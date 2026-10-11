// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <memory>
#include <string>

namespace alarmapp::ui {
// UI-thread-only. attach never captures or opens UI. request_picker is an
// explicit user action; Windows owns selection/consent and its capture border.
// No CPU readback, persistence, networking or automatic source selection.
class DesktopCapture {
public:
    static constexpr UINT frame_message=WM_APP+0x4A1;
    DesktopCapture();
    ~DesktopCapture();
    DesktopCapture(const DesktopCapture&)=delete;
    DesktopCapture& operator=(const DesktopCapture&)=delete;
    bool attach(HWND window,ID3D11Device* device,std::wstring& error);
    bool request_picker(std::wstring& error);
    // Call on frame_message; acknowledges a coalesced worker notification.
    // Success may contain no new frame. No synchronous picker/GPU wait.
    bool poll(std::wstring& error);
    // Borrowed GPU copy, valid until the next poll/release. BGRA8 SDR pixels.
    ID3D11Texture2D* texture() const noexcept;
    SIZE size() const noexcept;
    std::uint64_t frames() const noexcept;
    bool active() const noexcept;
    bool pending() const noexcept;
    bool attached() const noexcept;
    std::wstring status() const;
    std::wstring error() const;
    std::wstring diagnostics() const;
    static bool supported() noexcept;
    // Call before HWND/device destruction, hiding, session lock, suspend,
    // display-off and demo idle. A subsequent selection requires a new picker.
    // Stops pool/session, drops all frame pixels, restores this HWND's affinity.
    void release() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
