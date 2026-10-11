// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#pragma once
#include <windows.h>
#include <filesystem>
#include <memory>
#include <string>

namespace alarmapp {
// Native Windows notifications for the unpackaged desktop application.
// Initialize, show, clear and destroy on the same (UI) thread. Toast callbacks
// can arrive asynchronously; status_message carries no pointer or payload.
class Notifications {
public:
    static constexpr UINT status_message = WM_APP + 0x2A0;
    Notifications();
    ~Notifications();
    Notifications(const Notifications&) = delete;
    Notifications& operator=(const Notifications&) = delete;

    // Registers only this user's notification identity, Start Menu shortcut
    // and ibrahim-alarm: URI.
    // The executable must remain at this path. Protocol activation starts it
    // with the fixed argument --open; no untrusted URI is forwarded to it.
    bool initialize(HWND owner, const std::filesystem::path& executable);
    // True means Windows accepted the notification, not that a banner was
    // visibly rendered. Settings, Do Not Disturb and lock-screen policy apply.
    // Alarm toasts stay expanded until dismissed; their audio is silent because
    // the native alarm engine is responsible for the selected audio output.
    bool show(const std::wstring& title, const std::wstring& body, bool alarm = false);
    bool clear();
    bool available() const;
    std::wstring error() const;
    static const wchar_t* app_id();
    static const wchar_t* protocol_uri();
private:
    friend class NotificationsTestAccess;
    // Isolated shortcut regression seam: no notification platform, registry,
    // real Start Menu location, or banner is accessed by this helper.
    static HRESULT register_shortcut_for_testing(const std::filesystem::path& executable,
                                                const std::filesystem::path& directory);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
