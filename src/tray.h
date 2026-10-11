// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <string>

namespace alarmapp {

enum class TrayAction { None, Open, ToggleEnabled, Stop, Quit };

struct TrayStatus {
    bool enabled = false;
    bool can_stop = false;
    std::wstring state = L"Disabled";
    int hour = 6;
    int minute = 0;
};

// Owned by the HWND's UI thread. The application dispatches returned actions;
// this class never activates an alarm, exits the process or changes preferences.
class Tray {
public:
    static constexpr UINT callback_message = WM_APP + 20;

    Tray();
    // Isolated native tests or side-by-side binaries must use another identity.
    explicit Tray(const GUID& identity);
    ~Tray();
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    bool create(HWND owner, HINSTANCE instance, std::wstring& error, UINT icon_resource = 101);
    bool update(const TrayStatus& status, std::wstring& error);
    [[nodiscard]] TrayAction handle_callback(WPARAM wparam, LPARAM lparam, std::wstring& error);
    [[nodiscard]] UINT taskbar_created_message() const noexcept { return taskbar_message_; }
    // Call only when taskbar_created_message() is received (Explorer restarted).
    bool recreate(std::wstring& error);
    void remove() noexcept;
    [[nodiscard]] bool available() const noexcept { return added_; }

private:
    NOTIFYICONDATAW identity() const noexcept;
    bool add_icon(std::wstring& error);
    TrayAction show_menu(std::wstring& error);

    HWND owner_ = nullptr;
    HICON icon_ = nullptr;
    UINT taskbar_message_ = 0;
    bool added_ = false;
    GUID guid_{};
    TrayStatus status_;
    std::wstring desired_tip_ = L"ALARM — Disabled · 06:00";
    std::wstring applied_tip_;
};

// Optional startup integration; these functions never run automatically.
// The fixed HKCU Run value contains a quoted absolute executable path followed
// by the application's fixed --background flag (no user-supplied arguments).
// Readback confirms registration, not that Windows will allow/run it at login.
bool query_current_user_startup(
    const std::filesystem::path& executable, bool& enabled, std::wstring& error);
bool set_current_user_startup(
    const std::filesystem::path& executable, bool enabled, std::wstring& error);

} // namespace alarmapp
