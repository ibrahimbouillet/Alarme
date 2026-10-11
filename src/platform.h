// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include "engine.h"

namespace alarmapp {
class WakeTimer {
public:
    WakeTimer();
    ~WakeTimer();
    WakeTimer(const WakeTimer&) = delete;
    WakeTimer& operator=(const WakeTimer&) = delete;
    HANDLE handle() const { return handle_; }
    bool arm(TimePoint deadline, bool request_wake, std::wstring& status);
    void cancel();
private:
    HANDLE handle_ = nullptr;
};
std::wstring system_error(DWORD code);
std::wstring wake_policy();
bool wake_policy_allows();
std::filesystem::path config_directory();
std::wstring executable_path();
bool startup_probe(std::wstring& result);
bool tray_probe(std::wstring& result);
void append_log(const std::filesystem::path& directory, const std::wstring& message);
std::string utf8(const std::wstring& text);
}
