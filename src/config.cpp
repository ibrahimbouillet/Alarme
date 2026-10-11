// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#include "config.h"

#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace alarmapp {
namespace {

constexpr std::uintmax_t max_config_bytes = 64 * 1024;
constexpr std::string_view header = "ALARM_CONFIG_V1";
std::atomic<unsigned long> save_sequence{0};

bool valid_config(const Config& c) {
    return c.hour >= 0 && c.hour <= 23 && c.minute >= 0 && c.minute <= 59 &&
           std::isfinite(c.session_volume) && c.session_volume >= 0.f && c.session_volume <= 1.f &&
           std::isfinite(c.endpoint_volume) && c.endpoint_volume >= 0.f && c.endpoint_volume <= 1.f &&
           c.device_id.find(L'\0') == std::wstring::npos &&
           c.wav_path.find(L'\0') == std::wstring::npos;
}

// Strict UTF-8 keeps device names and paths portable, and accepts UTF-16
// surrogate pairs on Windows without lossy replacement of malformed strings.
bool to_utf8(const std::wstring& input, std::string& output) {
    output.clear();
    for (std::size_t i = 0; i < input.size(); ++i) {
        auto cp = static_cast<std::uint32_t>(input[i]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if (++i == input.size()) return false;
                const auto low = static_cast<std::uint32_t>(input[i]);
                if (low < 0xdc00 || low > 0xdfff) return false;
                cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
            } else if (cp >= 0xdc00 && cp <= 0xdfff) return false;
        }
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        if (cp < 0x80) output.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            output.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            output.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp < 0x10000) {
            output.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            output.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            output.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }
    return true;
}

bool from_utf8(std::string_view input, std::wstring& output) {
    output.clear();
    for (std::size_t i = 0; i < input.size();) {
        const auto first = static_cast<unsigned char>(input[i++]);
        std::uint32_t cp = 0;
        int extra = 0;
        std::uint32_t minimum = 0;
        if (first < 0x80) cp = first;
        else if (first >= 0xc2 && first <= 0xdf) { cp = first & 0x1f; extra = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { cp = first & 0x0f; extra = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { cp = first & 0x07; extra = 3; minimum = 0x10000; }
        else return false;
        for (int n = 0; n < extra; ++n) {
            if (i == input.size()) return false;
            const auto next = static_cast<unsigned char>(input[i++]);
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0x10000) {
                cp -= 0x10000;
                output.push_back(static_cast<wchar_t>(0xd800 + (cp >> 10)));
                output.push_back(static_cast<wchar_t>(0xdc00 + (cp & 0x3ff)));
            } else output.push_back(static_cast<wchar_t>(cp));
        } else output.push_back(static_cast<wchar_t>(cp));
    }
    return true;
}

std::string hex_encode(std::string_view input) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(input.size() * 2);
    for (unsigned char byte : input) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

bool hex_decode(std::string_view input, std::wstring& result) {
    if (input.size() % 2) return false;
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string decoded;
    decoded.reserve(input.size() / 2);
    for (std::size_t i = 0; i < input.size(); i += 2) {
        const auto a = digit(input[i]), b = digit(input[i + 1]);
        if (a < 0 || b < 0) return false;
        decoded.push_back(static_cast<char>((a << 4) | b));
    }
    return from_utf8(decoded, result);
}

template<class T> bool number(std::string_view text, T& result) {
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool boolean(std::string_view text, bool& result) {
    if (text != "0" && text != "1") return false;
    result = text == "1";
    return true;
}

std::string float_text(float value) {
    std::array<char, 64> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
        value, std::chars_format::general, std::numeric_limits<float>::max_digits10);
    return {buffer.data(), converted.ptr};
}

bool serialize(const Config& c, std::string& data) {
    if (!valid_config(c)) return false;
    std::string device, wav;
    if (!to_utf8(c.device_id, device) || !to_utf8(c.wav_path, wav)) return false;
    data = std::string(header) + "\n" +
        "enabled=" + (c.enabled ? "1" : "0") + "\n" +
        "hour=" + std::to_string(c.hour) + "\n" +
        "minute=" + std::to_string(c.minute) + "\n" +
        "device_id=" + hex_encode(device) + "\n" +
        "wav_path=" + hex_encode(wav) + "\n" +
        "session_volume=" + float_text(c.session_volume) + "\n" +
        "override_endpoint=" + (c.override_endpoint ? "1" : "0") + "\n" +
        "endpoint_volume=" + float_text(c.endpoint_volume) + "\n" +
        "startup=" + (c.startup ? "1" : "0") + "\n";
    return data.size() <= max_config_bytes;
}

bool parse(const std::string& data, Config& result) {
    std::istringstream stream(data);
    std::string line;
    if (!std::getline(stream, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != header) return false;
    std::map<std::string, std::string> values;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto equals = line.find('=');
        if (equals == std::string::npos || equals == 0) return false;
        if (!values.emplace(line.substr(0, equals), line.substr(equals + 1)).second) return false;
    }
    constexpr std::array keys{"enabled", "hour", "minute", "device_id", "wav_path",
        "session_volume", "override_endpoint", "endpoint_volume", "startup"};
    if (values.size() != keys.size()) return false;
    for (const auto* key : keys) if (!values.contains(key)) return false;
    Config c;
    if (!boolean(values.at("enabled"), c.enabled) ||
        !number(values.at("hour"), c.hour) || !number(values.at("minute"), c.minute) ||
        !hex_decode(values.at("device_id"), c.device_id) ||
        !hex_decode(values.at("wav_path"), c.wav_path) ||
        !number(values.at("session_volume"), c.session_volume) ||
        !boolean(values.at("override_endpoint"), c.override_endpoint) ||
        !number(values.at("endpoint_volume"), c.endpoint_volume) ||
        !boolean(values.at("startup"), c.startup) || !valid_config(c)) return false;
    result = std::move(c);
    return true;
}

std::wstring system_error_text(const std::wstring& action, unsigned long code) {
    return action + L" (system error " + std::to_wstring(code) + L").";
}

} // namespace

ConfigLoadResult load_config(const std::filesystem::path& path, Config& out, std::wstring& error) {
    error.clear();
    std::error_code ec;
    const auto present = std::filesystem::exists(path, ec);
    if (ec) { error = L"Cannot inspect the configuration file."; return ConfigLoadResult::IoError; }
    if (!present) return ConfigLoadResult::Missing;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) { error = L"Cannot read the configuration file size."; return ConfigLoadResult::IoError; }
    if (size > max_config_bytes) {
        error = L"Configuration exceeds the allowed size; original file preserved.";
        return ConfigLoadResult::Invalid;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { error = L"Cannot open the configuration file."; return ConfigLoadResult::IoError; }
    std::string data(static_cast<std::size_t>(size), '\0');
    stream.read(data.data(), static_cast<std::streamsize>(data.size()));
    if (!stream || stream.peek() != std::char_traits<char>::eof()) {
        error = L"Cannot read a complete stable configuration file.";
        return ConfigLoadResult::IoError;
    }
    Config loaded;
    if (!parse(data, loaded)) {
        error = L"Configuration is malformed; original file preserved.";
        return ConfigLoadResult::Invalid;
    }
    out = std::move(loaded);
    return ConfigLoadResult::Loaded;
}

bool save_config(const std::filesystem::path& path, const Config& config, std::wstring& error) {
    error.clear();
    std::string data;
    if (!serialize(config, data)) { error = L"Configuration values are invalid."; return false; }
    Config existing;
    const auto status = load_config(path, existing, error);
    if (status == ConfigLoadResult::Invalid || status == ConfigLoadResult::IoError) return false;

    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) { error = L"Cannot create the configuration directory."; return false; }
#ifdef _WIN32
    const auto process_id = GetCurrentProcessId();
#else
    const auto process_id = getpid();
#endif
    auto temporary = path;
    temporary += L".tmp." + std::to_wstring(process_id) + L"." + std::to_wstring(++save_sequence);
    const auto cleanup = [&] { std::error_code ignored; std::filesystem::remove(temporary, ignored); };
#ifdef _WIN32
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = system_error_text(L"Cannot create the temporary configuration", GetLastError());
        return false;
    }
    DWORD written = 0;
    const bool write_ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr)
        && written == data.size();
    const auto write_error = GetLastError();
    const bool flush_ok = write_ok && FlushFileBuffers(file);
    const auto flush_error = GetLastError();
    const bool close_ok = CloseHandle(file);
    const auto close_error = GetLastError();
    if (!write_ok || !flush_ok || !close_ok) {
        error = system_error_text(L"Cannot flush the temporary configuration",
            !write_ok ? write_error : (!flush_ok ? flush_error : close_error));
        cleanup();
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = system_error_text(L"Cannot replace the configuration", GetLastError());
        cleanup();
        return false;
    }
#else
    const int file = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (file < 0) { error = system_error_text(L"Cannot create temporary configuration", errno); return false; }
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto written = write(file, data.data() + offset, data.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) break;
        offset += static_cast<std::size_t>(written);
    }
    const bool flushed = offset == data.size() && fsync(file) == 0;
    const auto flush_error = errno;
    const bool closed = close(file) == 0;
    if (!flushed || !closed) {
        error = system_error_text(L"Cannot flush temporary configuration", flush_error);
        cleanup();
        return false;
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) { error = L"Cannot replace the configuration."; cleanup(); return false; }
    const auto parent = path.parent_path().empty() ? std::filesystem::path{"."} : path.parent_path();
    const int directory = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (directory >= 0) { fsync(directory); close(directory); }
#endif
    return true;
}

} // namespace alarmapp
