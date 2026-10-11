// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#pragma once

#include "audio.h"
#include "config.h"

namespace alarmapp {

enum class AudioPurpose { Alarm, Preview, Integration };

// Pure policy: only a real alarm may raise/unmute the Windows endpoint.
// Preview preserves Windows volume/mute; integration is deliberately silent.
inline AudioOptions make_alarm_audio_options(const Config& config, AudioPurpose purpose) {
    AudioOptions options;
    options.device_id = config.device_id;
    options.wav_path = config.wav_path;
    options.session_volume = purpose == AudioPurpose::Integration ? 0.0f : config.session_volume;
    options.override_endpoint = purpose == AudioPurpose::Alarm;
    options.endpoint_volume = purpose == AudioPurpose::Alarm ? 1.0f : config.endpoint_volume;
    return options;
}

} // namespace alarmapp
