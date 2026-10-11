// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#include "tray.h"
#include "notifications.h"

#include <objbase.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace alarmapp;

namespace alarmapp {
class NotificationsTestAccess {
public:
    static HRESULT shortcut(const std::filesystem::path& executable,const std::filesystem::path& directory) {
        return Notifications::register_shortcut_for_testing(executable,directory);
    }
};
}

namespace {
using Microsoft::WRL::ComPtr;
int assertions = 0;
void require(bool condition, const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}
class OwnerWindow {
public:
    OwnerWindow() {
        value = CreateWindowExW(0, L"STATIC", L"ALARM tray smoke", WS_OVERLAPPED,
            0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!value) throw std::runtime_error("Cannot create hidden native owner");
    }
    ~OwnerWindow() { if (value) DestroyWindow(value); }
    HWND value = nullptr;
};
class ComScope {
public:
    ComScope() {
        result_=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        if(FAILED(result_)) throw std::runtime_error("Cannot initialize test COM apartment");
    }
    ~ComScope() { if(SUCCEEDED(result_)) CoUninitialize(); }
private:
    HRESULT result_=E_FAIL;
};
class ShortcutFixture {
public:
    std::filesystem::path directory;
    ShortcutFixture() {
        const auto parent=std::filesystem::canonical(std::filesystem::temp_directory_path());
        for(unsigned attempt=0;attempt<32;++attempt) {
            const auto candidate=parent/(L"alarm-shortcut-tests-"+std::to_wstring(GetCurrentProcessId())+
                L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(attempt));
            if(std::filesystem::create_directory(candidate)) { directory=candidate;break; }
        }
        require(!directory.empty() && directory.parent_path()==parent,"Create an isolated temporary shortcut folder");
    }
    ~ShortcutFixture() {
        // Remove only this fixture's two known files, then its empty directory.
        std::error_code error;
        std::filesystem::remove(directory/L"ALARM.lnk",error);
        std::filesystem::remove(directory/L"foreign.exe",error);
        std::filesystem::remove(directory,error);
    }
    std::filesystem::path link_path() const { return directory/L"ALARM.lnk"; }
    void make_link(const std::filesystem::path& target,const wchar_t* app_id=nullptr) {
        ComPtr<IShellLinkW> link;
        require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(link.GetAddressOf()))),
                "Create a real isolated Windows shortcut");
        require(SUCCEEDED(link->SetPath(target.c_str())),"Set isolated shortcut target");
        if(app_id) {
            ComPtr<IPropertyStore> properties;
            require(SUCCEEDED(link.As(&properties)),"Get isolated shortcut properties");
            PROPVARIANT value{};
            require(SUCCEEDED(InitPropVariantFromString(app_id,&value)),"Create isolated shortcut identity value");
            const HRESULT result=properties->SetValue(PKEY_AppUserModel_ID,value);
            PropVariantClear(&value);
            require(SUCCEEDED(result) && SUCCEEDED(properties->Commit()),"Commit isolated shortcut identity");
        }
        ComPtr<IPersistFile> file;
        require(SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Save(link_path().c_str(),TRUE)),"Save isolated shortcut");
    }
    std::string bytes() const {
        std::ifstream file(link_path(),std::ios::binary);
        require(bool(file),"Read isolated shortcut bytes");
        return {std::istreambuf_iterator<char>(file),{}};
    }
};
void notification_shortcut_tests(const std::filesystem::path& executable) {
    ShortcutFixture fixture;
    fixture.make_link(executable);
    require(SUCCEEDED(NotificationsTestAccess::shortcut(executable,fixture.directory)),
            "An ordinary installer shortcut to this executable acquires notification properties");
    {
        ComPtr<IShellLinkW> link;
        require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(link.GetAddressOf()))),
                "Reload the adopted shortcut");
        ComPtr<IPersistFile> file;
        ComPtr<IPropertyStore> properties;
        require(SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Load(fixture.link_path().c_str(),STGM_READ)) &&
                SUCCEEDED(link.As(&properties)),"Read persisted notification shortcut properties");
        PROPVARIANT value{};
        const HRESULT result=properties->GetValue(PKEY_AppUserModel_ID,&value);
        const bool identity_matches=SUCCEEDED(result) && value.vt==VT_LPWSTR && value.pwszVal &&
                                    std::wstring(value.pwszVal)==Notifications::app_id();
        PropVariantClear(&value);
        require(identity_matches,"Installer shortcut retains the native ALARM notification identity");
        wchar_t arguments[128]{};
        require(SUCCEEDED(link->GetArguments(arguments,static_cast<int>(std::size(arguments)))) &&
                std::wstring(arguments)==L"--open","Adopted shortcut uses the fixed safe open argument");
    }
    require(SUCCEEDED(NotificationsTestAccess::shortcut(executable,fixture.directory)),
            "An already owned notification shortcut can be registered again");
    const auto foreign=fixture.directory/L"foreign.exe";
    { std::ofstream file(foreign,std::ios::binary);file<<"fixture";require(bool(file),"Create an unrelated shortcut target fixture"); }
    fixture.make_link(foreign);
    const auto foreign_bytes=fixture.bytes();
    require(NotificationsTestAccess::shortcut(executable,fixture.directory)==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
            "An unclaimed shortcut to another file cannot be adopted");
    require(fixture.bytes()==foreign_bytes,"A shortcut to another file is preserved byte for byte");
    fixture.make_link(executable,L"Unrelated.Application");
    const auto claimed_bytes=fixture.bytes();
    require(NotificationsTestAccess::shortcut(executable,fixture.directory)==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
            "A foreign notification identity is preserved even when its target matches");
    require(fixture.bytes()==claimed_bytes,"A foreign notification shortcut is preserved byte for byte");
    std::cout<<"PASS isolated installer shortcut adoption and unrelated shortcut preservation\n";
}
}

int main() {
    try {
        ComScope com;
        OwnerWindow owner;
        GUID isolated_guid{};
        require(SUCCEEDED(CoCreateGuid(&isolated_guid)), "Generate an isolated test tray identity");
        Tray tray(isolated_guid);
        std::wstring error;
        require(!tray.available(), "A new tray must not claim to be installed");
        require(tray.create(owner.value, GetModuleHandleW(nullptr), error),
            "Actual logo resource 101 must install through Shell_NotifyIcon V4");
        require(tray.available() && error.empty() && tray.taskbar_created_message() != 0,
            "Successful tray registration and Explorer recovery message are retained");
        TrayStatus status;
        status.enabled = true;
        status.hour = 6;
        status.minute = 15;
        status.state = L"Active";
        require(tray.update(status, error), "Changed native tooltip must update");
        require(tray.update(status, error), "An unchanged tooltip must be accepted without mutation");
        const auto mouse_selection = MAKELPARAM(NIN_SELECT, 1);
        const auto keyboard_selection = MAKELPARAM(NIN_KEYSELECT, 1);
        require(tray.handle_callback(0, mouse_selection, error) == TrayAction::Open,
            "Version4 mouse activation uses LOWORD event and HIWORD icon identity");
        require(tray.handle_callback(0, keyboard_selection, error) == TrayAction::Open,
            "Version4 keyboard activation opens ALARM");
        require(tray.handle_callback(0, MAKELPARAM(NIN_SELECT, 2), error) == TrayAction::None,
            "A different icon identity cannot activate ALARM");
        require(tray.handle_callback(0, MAKELPARAM(WM_MOUSEMOVE, 1), error) == TrayAction::None,
            "Hover/movement does not activate ALARM");
        status.hour = 24;
        require(!tray.update(status, error) && !error.empty() && tray.available(),
            "An invalid tooltip time must not remove a working tray");
        NOTIFYICONDATAW lost{};
        lost.cbSize = sizeof(lost);
        lost.hWnd = owner.value;
        lost.uID = 1;
        lost.uFlags = NIF_GUID;
        lost.guidItem = isolated_guid;
        require(Shell_NotifyIconW(NIM_DELETE, &lost) != FALSE,
            "Simulate Explorer losing only this isolated test icon");
        require(tray.recreate(error) && tray.available(),
            "Explorer recovery restores the same icon and V4 protocol after native icon loss");
        tray.remove();
        require(!tray.available(), "Explicit removal clears tray availability");
        require(tray.handle_callback(0, mouse_selection, error) == TrayAction::None,
            "A stale activation after removal is ignored");
        tray.remove();
        require(!tray.available(), "Repeated cleanup is safe");
        require(tray.create(owner.value, GetModuleHandleW(nullptr), error),
            "A removed tray can be installed again with the same persistent identity");
        tray.remove();

        wchar_t executable[32768]{};
        const auto length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        require(length > 0 && length < std::size(executable), "Read native executable path");
        notification_shortcut_tests(std::filesystem::path(executable));
        bool enabled = true;
        require(query_current_user_startup(std::filesystem::path(executable), enabled, error),
            "Current-user startup registration can be inspected without changing it");
        require(!set_current_user_startup(std::filesystem::path(L"relative.exe"), true, error) && !error.empty(),
            "Invalid startup paths are rejected before registry mutation");
        const auto too_long = std::filesystem::path(L"C:\\" + std::wstring(245, L'x') + L"\\ALARM.exe");
        require(!set_current_user_startup(too_long, true, error) && !error.empty(),
            "The documented Run command limit includes the fixed background flag");
        std::cout << "PASS native logo tray create/update/V4 activation/remove\n"
                     "PASS read-only startup inspection and invalid path guard\n"
                  << "All " << assertions << " assertions passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << assertions << " assertions: " << error.what() << '\n';
        return 1;
    }
}
