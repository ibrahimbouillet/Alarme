// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace alarmapp {

enum class WidgetPhase { Idle, Scheduled, Ringing, Silent, Error };

// Public presentation data only: no WAV paths, device IDs or diagnostic text.
struct WidgetState {
    bool enabled = false;
    int hour = 6;
    int minute = 0;
    WidgetPhase phase = WidgetPhase::Idle;
    std::wstring next_transition_utc;
    std::wstring updated_utc; // Empty: use the current UTC time.
    std::uint32_t process_id = 0; // Zero: use the current process ID.
    std::wstring sound_label = L"Built-in ALARM tone";
};

// Fixed ISO 8601 UTC form YYYY-MM-DDTHH:MM:SSZ; empty if not representable.
std::wstring widget_utc(std::chrono::system_clock::time_point value);

// Owns directory/widget-state.json. Unchanged logical data succeeds without a
// write (updatedUtc alone is ignored). Failure preserves the previous file.
// UTF-8 JSON, without a BOM; sound_label is limited to 160 UTF-16 code units.
bool publish_widget_state(const std::filesystem::path& directory,
                          const WidgetState& state, std::wstring& error);

} // namespace alarmapp
