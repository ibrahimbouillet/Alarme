// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#pragma once

#include <filesystem>
#include <string>

namespace alarmapp {

struct Config {
    bool enabled = false;
    int hour = 6;
    int minute = 0;
    std::wstring device_id;
    std::wstring wav_path;
    float session_volume = .5f;
    bool override_endpoint = false;
    float endpoint_volume = .5f;
    bool startup = false;
};

enum class ConfigLoadResult { Loaded, Missing, Invalid, IoError };

// On error or Missing, out remains unchanged. A malformed configuration must
// be preserved for diagnosis; save_config also refuses to replace one.
ConfigLoadResult load_config(
    const std::filesystem::path& path, Config& out, std::wstring& error);

// Validates, writes/flushed a temporary sibling, then replaces the target.
// The caller supplies a path inside its Windows LocalAppData directory.
bool save_config(
    const std::filesystem::path& path, const Config& config, std::wstring& error);

} // namespace alarmapp
