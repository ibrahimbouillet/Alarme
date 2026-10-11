// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#include "tray.h"

#include <commctrl.h>
#include <algorithm>
#include <array>
#include <cwchar>
#include <iterator>
#include <sstream>
#include <vector>

namespace alarmapp {
namespace {

// Product identity stays fixed across launches so Explorer can retain the
// user's notification-area visibility choice. Never write IconStreams/etc.
constexpr GUID tray_guid = {0x1d664eee, 0x4337, 0x4354,
    {0x99, 0x79, 0x1a, 0xd3, 0x8c, 0x1e, 0x01, 0x57}};
constexpr GUID null_identity{};
constexpr UINT icon_id = 1;
constexpr UINT open_command = 40001, toggle_command = 40002,
    stop_command = 40003, quit_command = 40004;
constexpr wchar_t run_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t startup_name[] = L"ALARM.Ibrahim";

std::wstring windows_error(const wchar_t* action, LSTATUS code) {
    return std::wstring(action) + L" (Windows error " + std::to_wstring(code) + L").";
}

std::wstring tooltip(const TrayStatus& status) {
    auto state = status.state;
    std::replace_if(state.begin(), state.end(), [](wchar_t c) {
        return c == L'\r' || c == L'\n' || c == L'\t';
    }, L' ');
    // Leave room for ALARM, punctuation, HH:MM and a terminating null. Do not
    // cut a Windows UTF-16 surrogate pair at the boundary.
    if (state.size() > 104) state.resize(104);
    if (!state.empty() && state.back() >= 0xd800 && state.back() <= 0xdbff) state.pop_back();
    wchar_t time[8]{};
    std::swprintf(time, std::size(time), L"%02d:%02d", status.hour, status.minute);
    return L"ALARM — " + state + L" · " + time;
}

class RegistryKey {
public:
    ~RegistryKey() { if (value) RegCloseKey(value); }
    RegistryKey() = default;
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;
    HKEY value = nullptr;
};

enum class Registration { Missing, Present, Error };

Registration read_registration(HKEY key, std::wstring& command, std::wstring& error) {
    DWORD type = 0, bytes = 0;
    auto code = RegQueryValueExW(key, startup_name, nullptr, &type, nullptr, &bytes);
    if (code == ERROR_FILE_NOT_FOUND) return Registration::Missing;
    if (code != ERROR_SUCCESS) {
        error = windows_error(L"Cannot read startup configuration", code);
        return Registration::Error;
    }
    if (type != REG_SZ || bytes < sizeof(wchar_t) || bytes > 4096 || bytes % sizeof(wchar_t)) {
        error = L"The ALARM startup entry is malformed.";
        return Registration::Error;
    }
    std::vector<wchar_t> data(bytes / sizeof(wchar_t) + 1, L'\0');
    const DWORD expected_bytes = bytes;
    code = RegQueryValueExW(key, startup_name, nullptr, &type, reinterpret_cast<BYTE*>(data.data()), &bytes);
    if (code != ERROR_SUCCESS || type != REG_SZ || bytes != expected_bytes) {
        error = windows_error(L"Cannot read a stable startup configuration",
            code == ERROR_SUCCESS ? ERROR_INVALID_DATA : code);
        return Registration::Error;
    }
    const auto count = bytes / sizeof(wchar_t);
    if (data[count - 1] != L'\0' || std::find(data.begin(), data.begin() + count - 1, L'\0') != data.begin() + count - 1) {
        error = L"The ALARM startup entry contains an invalid string.";
        return Registration::Error;
    }
    command.assign(data.data(), count - 1);
    return Registration::Present;
}

bool startup_command(const std::filesystem::path& executable, std::wstring& command, std::wstring& error) {
    const auto path = executable.lexically_normal().wstring();
    if (!executable.is_absolute() || path.empty() || path.find_first_of(L"\"\r\n") != std::wstring::npos ||
        path.find(L'\0') != std::wstring::npos) {
        error = L"Startup requires a valid absolute path to ALARM.";
        return false;
    }
    command = L"\"" + path + L"\" --background";
    // Windows documents a 260-character command line limit for Run values.
    if (command.size() > 260) {
        error = L"The path is too long for Windows startup (260 characters).";
        return false;
    }
    return true;
}

} // namespace

Tray::Tray() : guid_(tray_guid) {}
Tray::Tray(const GUID& identity) : guid_(identity) {}
Tray::~Tray() { remove(); }

NOTIFYICONDATAW Tray::identity() const noexcept {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner_;
    data.uID = icon_id;
    data.uFlags = NIF_GUID;
    data.guidItem = guid_;
    return data;
}

bool Tray::create(HWND owner, HINSTANCE instance, std::wstring& error, UINT icon_resource) {
    error.clear();
    remove();
    if (!owner || !IsWindow(owner) || !instance || !icon_resource || IsEqualGUID(guid_, null_identity)) {
        error = L"Invalid ALARM icon window or resource.";
        return false;
    }
    taskbar_message_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (!taskbar_message_) {
        error = windows_error(L"Cannot register the system tray callback", GetLastError());
        return false;
    }
    const auto loaded = LoadIconMetric(instance, MAKEINTRESOURCEW(icon_resource), LIM_SMALL, &icon_);
    if (FAILED(loaded) || !icon_) {
        std::wostringstream report;
        report << L"Cannot load the ALARM logo (0x" << std::hex << static_cast<unsigned long>(loaded) << L").";
        error = report.str();
        remove();
        return false;
    }
    owner_ = owner;
    return add_icon(error);
}

bool Tray::add_icon(std::wstring& error) {
    error.clear();
    if (!owner_ || !IsWindow(owner_) || !icon_) {
        error = L"The ALARM icon is not initialized.";
        return false;
    }
    auto data = identity();
    data.uFlags |= NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = callback_message;
    data.hIcon = icon_;
    std::copy(desired_tip_.begin(), desired_tip_.end(), data.szTip);
    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        error = L"Windows did not add the ALARM icon to the system tray.";
        return false;
    }
    added_ = true;
    data = identity();
    data.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &data)) {
        Shell_NotifyIconW(NIM_DELETE, &data);
        added_ = false;
        error = L"Windows did not accept the ALARM icon protocol.";
        return false;
    }
    applied_tip_ = desired_tip_;
    return true;
}

bool Tray::update(const TrayStatus& status, std::wstring& error) {
    error.clear();
    if (status.hour < 0 || status.hour > 23 || status.minute < 0 || status.minute > 59) {
        error = L"The ALARM icon time is invalid.";
        return false;
    }
    status_ = status;
    desired_tip_ = tooltip(status);
    if (!added_) { error = L"The ALARM icon is unavailable."; return false; }
    if (desired_tip_ == applied_tip_) return true;
    auto data = identity();
    data.uFlags |= NIF_TIP | NIF_SHOWTIP;
    std::copy(desired_tip_.begin(), desired_tip_.end(), data.szTip);
    if (!Shell_NotifyIconW(NIM_MODIFY, &data)) {
        error = L"Windows did not update the ALARM icon state.";
        return false;
    }
    applied_tip_ = desired_tip_;
    return true;
}

bool Tray::recreate(std::wstring& error) {
    // Explorer discarded all notification icons; there is nothing to delete.
    added_ = false;
    applied_tip_.clear();
    return add_icon(error);
}

void Tray::remove() noexcept {
    if (added_) {
        auto data = identity();
        Shell_NotifyIconW(NIM_DELETE, &data);
    }
    added_ = false;
    applied_tip_.clear();
    if (icon_) DestroyIcon(icon_);
    icon_ = nullptr;
    owner_ = nullptr;
}

TrayAction Tray::handle_callback(WPARAM, LPARAM lparam, std::wstring& error) {
    error.clear();
    if (!added_ || HIWORD(lparam) != icon_id) return TrayAction::None;
    switch (LOWORD(lparam)) {
    case NIN_SELECT:
    case NIN_KEYSELECT:
    case WM_LBUTTONDBLCLK:
        return TrayAction::Open;
    case WM_CONTEXTMENU:
        return show_menu(error);
    default:
        return TrayAction::None;
    }
}

TrayAction Tray::show_menu(std::wstring& error) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        error = windows_error(L"Cannot create the ALARM menu", GetLastError());
        return TrayAction::None;
    }
    const bool complete = AppendMenuW(menu, MF_STRING, open_command, L"Open ALARM") &&
        AppendMenuW(menu, MF_STRING | (status_.enabled ? MF_CHECKED : MF_UNCHECKED),
            toggle_command, L"Daily alarm enabled") &&
        AppendMenuW(menu, MF_STRING | (status_.can_stop ? MF_ENABLED : MF_GRAYED),
            stop_command, L"Stop ringing") &&
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr) &&
        AppendMenuW(menu, MF_STRING, quit_command, L"Quit");
    if (!complete) {
        error = windows_error(L"Cannot create the ALARM menu", GetLastError());
        DestroyMenu(menu);
        return TrayAction::None;
    }
    SetMenuDefaultItem(menu, open_command, FALSE);
    POINT point{};
    GetCursorPos(&point);
    NOTIFYICONIDENTIFIER identifier{};
    identifier.cbSize = sizeof(identifier);
    identifier.hWnd = owner_;
    identifier.uID = icon_id;
    identifier.guidItem = guid_;
    RECT icon_location{};
    if (SUCCEEDED(Shell_NotifyIconGetRect(&identifier, &icon_location))) {
        point.x = icon_location.left;
        point.y = icon_location.bottom;
    }
    // Required for a tray menu to dismiss normally when clicked elsewhere.
    SetForegroundWindow(owner_);
    const auto command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        point.x, point.y, owner_, nullptr);
    DestroyMenu(menu);
    PostMessageW(owner_, WM_NULL, 0, 0);
    auto data = identity();
    Shell_NotifyIconW(NIM_SETFOCUS, &data);
    switch (command) {
    case open_command: return TrayAction::Open;
    case toggle_command: return TrayAction::ToggleEnabled;
    case stop_command: return TrayAction::Stop;
    case quit_command: return TrayAction::Quit;
    default: return TrayAction::None;
    }
}

bool query_current_user_startup(const std::filesystem::path& executable, bool& enabled, std::wstring& error) {
    error.clear();
    std::wstring expected;
    if (!startup_command(executable, expected, error)) return false;
    RegistryKey key;
    const auto code = RegOpenKeyExW(HKEY_CURRENT_USER, run_key, 0, KEY_QUERY_VALUE, &key.value);
    if (code == ERROR_FILE_NOT_FOUND) { enabled = false; return true; }
    if (code != ERROR_SUCCESS) {
        error = windows_error(L"Cannot read startup configuration", code);
        return false;
    }
    std::wstring actual;
    const auto registration = read_registration(key.value, actual, error);
    if (registration == Registration::Error) return false;
    enabled = registration == Registration::Present && actual == expected;
    if (registration == Registration::Present && !enabled)
        error = L"The ALARM startup entry points to another location.";
    return true;
}

bool set_current_user_startup(const std::filesystem::path& executable, bool enabled, std::wstring& error) {
    error.clear();
    std::wstring expected;
    if (enabled && !startup_command(executable, expected, error)) return false;
    if (enabled) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(executable, ec) || ec) {
            error = L"The ALARM startup executable was not found.";
            return false;
        }
    }
    RegistryKey key;
    const auto code = enabled
        ? RegCreateKeyExW(HKEY_CURRENT_USER, run_key, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
            nullptr, &key.value, nullptr)
        : RegOpenKeyExW(HKEY_CURRENT_USER, run_key, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key.value);
    if (!enabled && code == ERROR_FILE_NOT_FOUND) return true;
    if (code != ERROR_SUCCESS) {
        error = windows_error(L"Cannot change startup configuration", code);
        return false;
    }
    const auto changed = enabled
        ? RegSetValueExW(key.value, startup_name, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(expected.c_str()), static_cast<DWORD>((expected.size() + 1) * sizeof(wchar_t)))
        : RegDeleteValueW(key.value, startup_name);
    if (changed != ERROR_SUCCESS && !(changed == ERROR_FILE_NOT_FOUND && !enabled)) {
        error = windows_error(L"Cannot change startup configuration", changed);
        return false;
    }
    std::wstring actual;
    const auto registration = read_registration(key.value, actual, error);
    if (registration == Registration::Error) return false;
    if ((enabled && (registration != Registration::Present || actual != expected)) ||
        (!enabled && registration != Registration::Missing)) {
        error = L"Startup read-back does not confirm the change.";
        return false;
    }
    return true;
}

} // namespace alarmapp
