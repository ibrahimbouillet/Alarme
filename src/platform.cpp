// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#include "platform.h"
#include <powrprof.h>
#include <shlobj.h>
#include <shellapi.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <stdexcept>

namespace alarmapp {
std::wstring system_error(DWORD code) {
    wchar_t* buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring result = buffer ? buffer : L"Windows error";
    if (buffer) LocalFree(buffer);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    return result + L" (" + std::to_wstring(code) + L")";
}
WakeTimer::WakeTimer() {
    handle_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!handle_) throw std::runtime_error("CreateWaitableTimer failed");
}
WakeTimer::~WakeTimer() { if (handle_) { cancel(); CloseHandle(handle_); } }
void WakeTimer::cancel() { if (handle_) CancelWaitableTimer(handle_); }
bool WakeTimer::arm(TimePoint deadline, bool request_wake, std::wstring& status) {
    // Positive absolute UTC FILETIME includes time spent suspended (unlike relative timers).
    constexpr long long filetime_epoch = 116444736000000000LL;
    const auto ticks = std::chrono::duration_cast<std::chrono::duration<long long, std::ratio<1,10000000>>>(deadline.time_since_epoch()).count();
    LARGE_INTEGER due{};
    due.QuadPart = ticks + filetime_epoch;
    SetLastError(ERROR_SUCCESS);
    const BOOL ok = SetWaitableTimer(handle_, &due, 0, nullptr, nullptr, request_wake ? TRUE : FALSE);
    const DWORD code = GetLastError();
    if (!ok) { status = system_error(code); return false; }
    if (request_wake && code == ERROR_NOT_SUPPORTED)
        status = L"Timer armed; hardware wake is not supported.";
    else if (request_wake)
        status = L"Timer armed; wake request accepted. " + wake_policy();
    else status = L"Timer armed (test without waking from sleep).";
    return true;
}
namespace {
struct Policy {
    DWORD ac=0, dc=0;
    bool known=false;
    BYTE ac_source=255;
};
Policy read_policy() {
    Policy p;
    GUID* scheme=nullptr;
    if (PowerGetActiveScheme(nullptr,&scheme)==ERROR_SUCCESS) {
        const auto a=PowerReadACValueIndex(nullptr,scheme,&GUID_SLEEP_SUBGROUP,&GUID_ALLOW_RTC_WAKE,&p.ac);
        const auto d=PowerReadDCValueIndex(nullptr,scheme,&GUID_SLEEP_SUBGROUP,&GUID_ALLOW_RTC_WAKE,&p.dc);
        p.known=a==ERROR_SUCCESS && d==ERROR_SUCCESS;
        LocalFree(scheme);
    }
    SYSTEM_POWER_STATUS power{};
    if(GetSystemPowerStatus(&power)) p.ac_source=power.ACLineStatus;
    return p;
}
std::wstring policy_label(DWORD value) {
    return value==1 ? L"allowed" : value==2 ? L"system timers only" : L"disabled";
}
}
std::wstring wake_policy() {
    const auto p=read_policy();
    if(!p.known) return L"Wake policy unknown; check powercfg.";
    return L"Wake: AC power " + policy_label(p.ac) + L", battery " + policy_label(p.dc) + L". Physical wake still needs verification.";
}
bool wake_policy_allows() { const auto p=read_policy(); return p.known && (p.ac_source==1 ? p.ac==1 : p.ac_source==0 ? p.dc==1 : p.ac==1 && p.dc==1); }
std::filesystem::path config_directory() {
    PWSTR path=nullptr;
    if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_CREATE,nullptr,&path)))
        throw std::runtime_error("LOCALAPPDATA unavailable");
    std::filesystem::path result=std::filesystem::path(path)/L"Ibrahim"/L"ALARM";
    CoTaskMemFree(path);
    return result;
}
std::wstring executable_path() {
    std::wstring path(32768,L'\0');
    const DWORD n=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(!n || n>=path.size()) throw std::runtime_error("Executable path unavailable");
    path.resize(n); return path;
}
bool startup_probe(std::wstring& result) {
    // Temporary, uniquely named Run value. It is removed before this process exits.
    HKEY key=nullptr;
    auto code=RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,nullptr,0,KEY_SET_VALUE|KEY_QUERY_VALUE,nullptr,&key,nullptr);
    if(code!=ERROR_SUCCESS) { result=L"HKCU Run: " + system_error(code); return false; }
    const auto name=L"ALARM_Feasibility_" + std::to_wstring(GetCurrentProcessId());
    const auto command=L"\"" + executable_path() + L"\" --no-audio";
    code=RegSetValueExW(key,name.c_str(),0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),static_cast<DWORD>((command.size()+1)*sizeof(wchar_t)));
    wchar_t readback[32768]{}; DWORD bytes=sizeof(readback),type=0;
    const auto read=code==ERROR_SUCCESS ? RegQueryValueExW(key,name.c_str(),nullptr,&type,reinterpret_cast<BYTE*>(readback),&bytes) : code;
    const auto removed=RegDeleteValueW(key,name.c_str());
    RegCloseKey(key);
    const bool ok=code==ERROR_SUCCESS && read==ERROR_SUCCESS && type==REG_SZ && command==readback && removed==ERROR_SUCCESS;
    result=ok ? L"HKCU Run: prototype write, read-back and removal succeeded. Next-login execution was not tested." : L"HKCU Run: prototype or cleanup failed; check ALARM_Feasibility_" + std::to_wstring(GetCurrentProcessId());
    return ok;
}
bool tray_probe(std::wstring& result) {
    HWND window=CreateWindowExW(0,L"STATIC",L"ALARM probe",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!window) { result=L"Tray: cannot create prototype window."; return false; }
    NOTIFYICONDATAW n{}; n.cbSize=sizeof(n); n.hWnd=window; n.uID=1;
    n.uFlags=NIF_ICON|NIF_TIP; n.hIcon=LoadIconW(nullptr,IDI_INFORMATION);
    lstrcpyW(n.szTip,L"ALARM — internal prototype");
    const bool add=Shell_NotifyIconW(NIM_ADD,&n)!=FALSE;
    n.uVersion=NOTIFYICON_VERSION_4;
    const bool version=add && Shell_NotifyIconW(NIM_SETVERSION,&n)!=FALSE;
    const bool removed=add && Shell_NotifyIconW(NIM_DELETE,&n)!=FALSE;
    DestroyWindow(window);
    result=add && version && removed ? L"Tray: NIM_ADD / NIM_SETVERSION / NIM_DELETE succeeded. Visual notification was not tested." : L"Tray: Shell_NotifyIcon prototype failed.";
    return add && version && removed;
}
std::string utf8(const std::wstring& text) {
    if(text.empty()) return {};
    const int size=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string result(size,'\0');
    WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),size,nullptr,nullptr);
    return result;
}
void append_log(const std::filesystem::path& directory,const std::wstring& message) {
    static std::mutex log_mutex;
    std::lock_guard lock(log_mutex);
    std::error_code ec; std::filesystem::create_directories(directory,ec);
    const auto path=directory/L"alarm.log";
    // Bound disk use; keep the previous journal for troubleshooting.
    if(std::filesystem::exists(path,ec) && std::filesystem::file_size(path,ec)>2*1024*1024) {
        const auto old=directory/L"alarm.previous.log";
        std::filesystem::remove(old,ec); std::filesystem::rename(path,old,ec);
    }
    std::ofstream out(path,std::ios::app|std::ios::binary);
    SYSTEMTIME now{}; GetLocalTime(&now);
    out << now.wYear << '-' << std::setw(2) << std::setfill('0') << now.wMonth << '-' << std::setw(2) << now.wDay
        << ' ' << std::setw(2) << now.wHour << ':' << std::setw(2) << now.wMinute << ':' << std::setw(2) << now.wSecond
        << ' ' << utf8(message) << '\n';
}
}
