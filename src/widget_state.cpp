// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#include "widget_state.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <atomic>
#include <ctime>
#include <cwchar>
#include <fstream>
#include <mutex>
#include <string_view>

namespace alarmapp {
namespace {

constexpr std::size_t maximum_bytes = 4096;
std::atomic<unsigned long> sequence{0};
std::mutex publish_mutex;

bool valid_utc(std::wstring_view value) {
    if (value.size() != 20 || value[4] != L'-' || value[7] != L'-' ||
        value[10] != L'T' || value[13] != L':' || value[16] != L':' || value[19] != L'Z') return false;
    constexpr std::array digits{0,1,2,3,5,6,8,9,11,12,14,15,17,18};
    for (int i : digits) if (value[i] < L'0' || value[i] > L'9') return false;
    const auto pair = [&](int i) { return (value[i]-L'0')*10+value[i+1]-L'0'; };
    const int year = pair(0)*100+pair(2), month = pair(5), day = pair(8);
    if (year < 1601 || month < 1 || month > 12 || pair(11) > 23 || pair(14) > 59 || pair(17) > 59) return false;
    constexpr std::array days{31,28,31,30,31,30,31,31,30,31,30,31};
    const bool leap = year%4 == 0 && (year%100 != 0 || year%400 == 0);
    return day >= 1 && day <= days[month-1] + (month == 2 && leap ? 1 : 0);
}

// ASCII is valid UTF-8; escaping non-ASCII as UTF-16 also makes the contract
// independent of a consumer's source encoding and handles every JSON control.
bool json_string(std::wstring_view value, std::string& output) {
    constexpr char hex[] = "0123456789abcdef";
    const auto escape = [&](std::uint32_t unit) {
        output += "\\u";
        for (int shift : {12,8,4,0}) output.push_back(hex[(unit >> shift)&15]);
    };
    output.push_back('"');
    for (std::size_t i = 0; i < value.size(); ++i) {
        auto unit = static_cast<std::uint32_t>(value[i]);
        if (unit >= 0xd800 && unit <= 0xdbff) {
            if (i+1 == value.size()) return false;
            const auto low = static_cast<std::uint32_t>(value[++i]);
            if (low < 0xdc00 || low > 0xdfff) return false;
            escape(unit); escape(low); continue;
        }
        if ((unit >= 0xdc00 && unit <= 0xdfff) || unit > 0x10ffff) return false;
        if (unit > 0xffff) { unit -= 0x10000; escape(0xd800+(unit>>10)); escape(0xdc00+(unit&1023)); }
        else if (unit == '"') output += "\\\"";
        else if (unit == '\\') output += "\\\\";
        else if (unit < 0x20 || unit >= 0x7f) escape(unit);
        else output.push_back(static_cast<char>(unit));
    }
    output.push_back('"');
    return true;
}

const char* phase_name(WidgetPhase phase) {
    switch (phase) {
    case WidgetPhase::Idle: return "idle";
    case WidgetPhase::Scheduled: return "scheduled";
    case WidgetPhase::Ringing: return "ringing";
    case WidgetPhase::Silent: return "silent";
    case WidgetPhase::Error: return "error";
    }
    return nullptr;
}

bool serialize(const WidgetState& state, std::string& output, std::size_t& timestamp) {
    const auto phase = phase_name(state.phase);
    if (!phase || state.hour < 0 || state.hour > 23 || state.minute < 0 || state.minute > 59 ||
        state.sound_label.size() > 160 ||
        (!state.next_transition_utc.empty() && !valid_utc(state.next_transition_utc))) return false;
    const auto updated = state.updated_utc.empty() ? widget_utc(std::chrono::system_clock::now()) : state.updated_utc;
    if (!valid_utc(updated)) return false;
    const auto pid = state.process_id ? state.process_id : GetCurrentProcessId();
    output = "{\n  \"enabled\": " + std::string(state.enabled ? "true" : "false") +
        ",\n  \"hour\": " + std::to_string(state.hour) + ",\n  \"minute\": " + std::to_string(state.minute) +
        ",\n  \"phase\": \"" + phase + "\",\n  \"nextTransitionUtc\": ";
    if (!json_string(state.next_transition_utc, output)) return false;
    output += ",\n  \"updatedUtc\": \"";
    timestamp = output.size();
    for (wchar_t c : updated) output.push_back(static_cast<char>(c));
    output += "\",\n  \"processId\": " + std::to_string(pid) + ",\n  \"soundLabel\": ";
    if (!json_string(state.sound_label, output)) return false;
    output += "\n}\n";
    return output.size() <= maximum_bytes;
}

bool unchanged(const std::filesystem::path& path, const std::string& data, std::size_t timestamp) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::string existing(maximum_bytes+1, '\0');
    file.read(existing.data(), static_cast<std::streamsize>(existing.size()));
    existing.resize(static_cast<std::size_t>(file.gcount()));
    if (file.bad() || existing.size() != data.size()) return false;
    std::wstring old_stamp;
    for (std::size_t i = timestamp; i < timestamp+20; ++i)
        old_stamp.push_back(static_cast<unsigned char>(existing[i]));
    return valid_utc(old_stamp) && existing.compare(0,timestamp,data,0,timestamp) == 0 &&
        existing.compare(timestamp+20,std::string::npos,data,timestamp+20,std::string::npos) == 0;
}

std::wstring io_error(const wchar_t* action, DWORD code) {
    return std::wstring(action) + L" (Windows error " + std::to_wstring(code) + L").";
}

} // namespace

std::wstring widget_utc(std::chrono::system_clock::time_point value) {
    const auto seconds = std::chrono::floor<std::chrono::seconds>(value);
    const auto time = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
    if (gmtime_s(&utc, &time) != 0 || utc.tm_year < -299 || utc.tm_year > 8099) return {};
    wchar_t text[21]{};
    if (std::wcsftime(text,std::size(text),L"%Y-%m-%dT%H:%M:%SZ",&utc) != 20) return {};
    return text;
}

bool publish_widget_state(const std::filesystem::path& directory, const WidgetState& state, std::wstring& error) {
    std::lock_guard lock(publish_mutex);
    error.clear();
    std::string data;
    std::size_t timestamp = 0;
    if (!serialize(state,data,timestamp) || directory.empty() || directory.wstring().find(L'\0') != std::wstring::npos) {
        error = L"Invalid widget state: time, phase, UTC, Unicode, or size.";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(directory,ec);
    if (ec) { error = L"Cannot create the widget state directory."; return false; }
    const auto destination = directory/L"widget-state.json";
    if (unchanged(destination,data,timestamp)) return true;
    std::filesystem::path temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        temporary = directory/(L"widget-state.json.tmp."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(++sequence));
        file = CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        const auto code = GetLastError();
        if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) {
            error = io_error(L"Cannot create temporary widget state",code); return false;
        }
    }
    if (file == INVALID_HANDLE_VALUE) { error = L"Too many temporary widget files already exist."; return false; }
    DWORD written = 0;
    bool ok = WriteFile(file,data.data(),static_cast<DWORD>(data.size()),&written,nullptr) != FALSE;
    DWORD code = GetLastError();
    if (ok && written != data.size()) { ok = false; code = ERROR_WRITE_FAULT; }
    if (ok && !FlushFileBuffers(file)) { ok = false; code = GetLastError(); }
    if (!CloseHandle(file) && ok) { ok = false; code = GetLastError(); }
    if (ok && !MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        ok = false; code = GetLastError();
    }
    if (!ok) {
        DeleteFileW(temporary.c_str());
        error = io_error(L"Cannot publish widget state atomically",code);
    }
    return ok;
}

} // namespace alarmapp
