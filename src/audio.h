// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#pragma once

#include <memory>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace alarmapp {

struct AudioDevice {
    std::wstring id;
    std::wstring name;
};

struct AudioOptions {
    // Empty ID follows the Windows multimedia default output.
    std::wstring device_id;
    // Empty path uses the built-in, pulsed alarm tone.
    std::wstring wav_path;
    float session_volume = 0.5f;
    // Only explicit opt-in changes the selected endpoint's master volume/mute.
    bool override_endpoint = false;
    float endpoint_volume = 0.5f;
};

enum class AudioState { Idle, Starting, Playing, Stopping, Error };
struct AudioStatus {
    AudioState state = AudioState::Idle;
    std::uint64_t generation = 0;
    std::wstring message;
    // First startup result of this generation, stable across device retries.
    bool initialized = false;
    bool start_succeeded = false;
    // The native worker thread has actually ended, so a new begin may reap it.
    bool finished = true;
};

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // True means Windows started the stream, not that speakers are audible.
    // On success, error may contain a mute/volume warning. On failure it gives
    // the reason. Asynchronous errors and restoration failures use error().
    bool start(const AudioOptions& options, std::wstring& error);
    void stop();
    // UI path: no readiness wait or join of a live worker. An unfinished prior
    // generation is refused explicitly; poll status() and retry after cleanup.
    bool begin_start(const AudioOptions& options, std::wstring& error);
    AudioStatus status() const;
    // Requests stop and cancellation of synchronous WAV I/O; completion is
    // asynchronous and depends on Windows/drivers. Never terminates a thread.
    void cancel();
    // Coalesced until status() is read. Called outside the state lock, possibly
    // on the worker. Capture a value such as HWND and PostMessage; never perform
    // UI work or synchronous start/stop from the callback.
    // Clear before shutdown, then stop/join before destroying captured owners.
    void set_state_callback(std::function<void()> callback);
    bool playing() const;
    std::wstring error() const;
    static std::vector<AudioDevice> devices(std::wstring& error);
    // Opens and stops a deliberately silent stream; never alters master volume.
    static bool probe(std::wstring& report);

private:
    friend struct AudioPlayerTestAccess;
    // Private deterministic preparation seam; duplicate owns the event until
    // cleanup. Normal callers cannot enable it and no audio is involved.
    void set_preparation_gate_for_testing(void* event, bool cancelable);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace alarmapp
