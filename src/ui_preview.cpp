// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

// Artifact-only preview of ALARM's unified renderer. No HWND, desktop capture,
// audio, or input automation is needed: controls are part of GlassState.
#include <windows.h>
#include <objbase.h>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include "glass.h"
#include "ui_ids.h"

namespace {
constexpr int width = 580;
constexpr int height = 720;

struct MemoryBitmap {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    void* pixels = nullptr;
    BITMAPINFO info{};

    explicit MemoryBitmap(UINT dpi) {
        const int pixel_width = MulDiv(width, dpi, 96);
        const int pixel_height = MulDiv(height, dpi, 96);
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = pixel_width;
        info.bmiHeader.biHeight = -pixel_height; // top-down rows
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        info.bmiHeader.biSizeImage = pixel_width * pixel_height * 4;
        dc = CreateCompatibleDC(nullptr);
        if (!dc) throw std::runtime_error("Cannot create memory device context");
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap) {
            DeleteDC(dc);
            dc = nullptr;
            throw std::runtime_error("Cannot create memory bitmap");
        }
        previous = SelectObject(dc, bitmap);
        if (!previous || previous == HGDI_ERROR) {
            DeleteObject(bitmap);
            DeleteDC(dc);
            bitmap = nullptr;
            dc = nullptr;
            throw std::runtime_error("Cannot select memory bitmap");
        }
        std::memset(pixels, 0, info.bmiHeader.biSizeImage);
    }
    ~MemoryBitmap() {
        if (dc && previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
    MemoryBitmap(const MemoryBitmap&) = delete;
    MemoryBitmap& operator=(const MemoryBitmap&) = delete;

    void save(const std::filesystem::path& path) const {
        BITMAPFILEHEADER header{};
        header.bfType = 0x4D42;
        header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        header.bfSize = header.bfOffBits + info.bmiHeader.biSizeImage;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Cannot open preview artifact");
        output.write(reinterpret_cast<const char*>(&header), sizeof(header));
        output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
        output.write(static_cast<const char*>(pixels), info.bmiHeader.biSizeImage);
        output.close();
        if (!output) throw std::runtime_error("Cannot finish writing preview artifact");
    }
};

alarmapp::ui::GlassState scene(const std::wstring& name) {
    using namespace alarmapp::ui;
    GlassState state;
    state.width = width;
    state.height = height;
    state.next = L"Choose a time, then enable your alarm.";
    state.sound = L"Built-in ALARM tone";
    state.enabled = name == L"armed" || name == L"ringing" || name == L"silent";
    state.ringing = name == L"ringing";
    state.silent = name == L"silent";
    state.editing = name == L"editing";
    state.settings = name == L"settings" || name == L"diagnostics" || name == L"devices";
    state.diagnostics = name == L"diagnostics";
    if (state.enabled) state.next = L"Next alarm: 06:00\nIn 08:25:00";
    if (state.silent) state.next = L"Next transition in 00:10:00";
    auto add = [&](int id, const wchar_t* text, float x, float y, float w, float h) {
        GlassControl c;
        c.id = id; c.text = text; c.left = x; c.top = y; c.right = x + w; c.bottom = y + h;
        state.controls.push_back(std::move(c));
    };
    // Current main.cpp layout, in 96-DPI logical coordinates.
    add(CaptionMinimize, L"Minimize", width - 126, 5, 36, 32);
    add(CaptionMaximize, L"Maximize", width - 84, 5, 36, 32);
    add(CaptionClose, L"Close", width - 42, 5, 36, 32);
    if (!state.settings) {
        add(Settings, L"Settings", width - 154, 68, 126, 36);
        add(state.editing ? TimeInput : Time, L"06:00", 54, 248, width - 108, 103);
        if (state.editing) {
            state.controls.back().focused = true;
            state.controls.back().selection_end = 5;
        }
        add(state.editing ? TimeDone : Save, state.editing ? L"Save time" : state.enabled ? L"Disable alarm" : L"Enable alarm", 26, 478, width - 52, 60);
        add(state.ringing || state.silent ? Stop : TestNow, state.ringing || state.silent ? L"STOP" : L"Listen to sound", 26, 552, width - 52, 40);
    } else {
        add(Back, L"Back", width - 154, 68, 126, 36);
        if (state.diagnostics) {
            add(Test10, L"In 10 s", 48, 262, (width - 108) / 2, 44);
            add(Test30, L"In 30 s", width / 2 + 6, 262, (width - 108) / 2, 44);
            add(TestLocked, L"Locked test (10 s)", 48, 318, width - 96, 34);
            add(Stop, L"STOP", 26, 587, width - 52, 46);
            state.controls.back().enabled = false;
        } else {
            add(Device, L"Default Windows device", 48, 235, width - 96, 44);
            add(VolumeDown, L"−", width - 207, 290, 38, 36);
            add(SessionVolume, L"50", width - 159, 290, 59, 36);
            add(VolumeUp, L"+", width - 90, 290, 38, 36);
            add(Browse, L"Choose a WAV", 48, 346, 148, 38);
            add(BuiltIn, L"Built-in sound", width - 186, 346, 138, 38);
            state.controls.back().enabled = false;
            add(Override, L"Windows at 100% during the alarm", 48, 430, width - 96, 28);
            add(Startup, L"Start with Windows near the clock", 48, 496, width - 96, 28);
            add(SettingsApply, L"Save", 26, 580, width - 52, 52);
            add(Diagnostics, L"Diagnostics", 26, 651, 166, 34);
            add(Quit, L"Quit", width - 138, 651, 112, 34);
            if (name == L"devices") {
                state.devices = {L"Default Windows device", L"Speakers", L"Headphones"};
                state.selected_device = 0;
                add(DeviceList, L"", 48, 278, width - 96, 102);
            }
        }
    }
    return state;
}

void render(const std::filesystem::path& output, const std::wstring& name, UINT dpi) {
    MemoryBitmap image(dpi);
    alarmapp::ui::Glass glass;
    const auto state = scene(name);
    const RECT client{0, 0, image.info.bmiHeader.biWidth, -image.info.bmiHeader.biHeight};
    glass.paint(image.dc, client, state, dpi);
    GdiFlush();
    image.save(output);
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 4) {
        std::wcerr << L"Usage: alarm_ui_preview <output.bmp> [off|armed|ringing|silent|settings|diagnostics|editing|devices] [96|144|192]\n";
        return 2;
    }
    const std::wstring name = argc >= 3 ? argv[2] : L"off";
    if (name != L"off" && name != L"armed" && name != L"ringing" && name != L"silent" && name != L"settings" && name != L"diagnostics" && name != L"editing" && name != L"devices") return 2;
    wchar_t* end = nullptr;
    const auto dpi = argc >= 4 ? std::wcstoul(argv[3], &end, 10) : 96;
    if ((argc >= 4 && (!argv[3][0] || !end || *end)) || (dpi != 96 && dpi != 144 && dpi != 192)) return 2;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        std::cerr << "Cannot initialize artifact renderer COM apartment\n";
        return 1;
    }
    int result = 0;
    try {
        render(std::filesystem::path(argv[1]), name, static_cast<UINT>(dpi));
        std::wcout << L"PASS artifact-only render: " << name << L", " << dpi << L" DPI, " << MulDiv(width, dpi, 96) << L" x " << MulDiv(height, dpi, 96) << L" BMP written to " << argv[1]
                   << L"\nNo window created/displayed, desktop captured, input injected, or sound played.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    CoUninitialize();
    return result;
}
