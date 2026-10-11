// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#include "audio.h"
#include "alarm_audio_policy.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace alarmapp {
struct AudioPlayerTestAccess {
    static void gate(AudioPlayer& player, HANDLE event, bool cancelable) {
        player.set_preparation_gate_for_testing(event, cancelable);
    }
};
}

using namespace alarmapp;
using namespace std::chrono_literals;

namespace {

int assertions = 0;

void require(bool condition, const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}

template<class T> class ComPtr {
public:
    ~ComPtr() { if (value_) value_->Release(); }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    T* operator->() const { return value_; }
    T** out() { return &value_; }
private:
    T* value_ = nullptr;
};

class ComScope {
public:
    ComScope() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {
        require(SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE,
                "COM must initialize for read-only endpoint verification");
    }
    ~ComScope() { if (SUCCEEDED(result_)) CoUninitialize(); }
private:
    HRESULT result_;
};

struct EndpointState {
    std::wstring id;
    float volume;
    BOOL mute;
};

std::vector<EndpointState> read_endpoints(const std::vector<AudioDevice>& devices) {
    ComScope com;
    ComPtr<IMMDeviceEnumerator> enumerator;
    require(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator.out()))),
            "Audio endpoint enumerator must be available");
    std::vector<EndpointState> result;
    for (const auto& device : devices) {
        ComPtr<IMMDevice> endpoint;
        ComPtr<IAudioEndpointVolume> volume;
        require(SUCCEEDED(enumerator->GetDevice(device.id.c_str(), endpoint.out())),
                "Enumerated endpoint must remain available during this test");
        require(SUCCEEDED(endpoint->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(volume.out()))), "Endpoint state must be readable");
        EndpointState state{device.id, 0, FALSE};
        require(SUCCEEDED(volume->GetMasterVolumeLevelScalar(&state.volume)),
                "Endpoint master volume must be readable");
        require(SUCCEEDED(volume->GetMute(&state.mute)), "Endpoint mute must be readable");
        result.push_back(state);
    }
    return result;
}

class TestDirectory {
public:
    TestDirectory() {
        base_ = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
        require(base_.is_absolute() && std::filesystem::is_directory(base_),
                "Test fixture base must be the resolved absolute Windows temp directory");
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            const auto name = L"alarm_audio_tests_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(attempt);
            root_ = (base_ / name).lexically_normal();
            require(root_.parent_path() == base_, "Fixture directory must stay directly under Windows temp");
            if (std::filesystem::create_directory(root_)) { created_ = true; return; }
        }
        throw std::runtime_error("Cannot create a unique audio fixture directory");
    }
    ~TestDirectory() {
        if (!cleanup()) std::cerr << "Audio fixture cleanup incomplete; temporary files preserved.\n";
    }
    std::filesystem::path missing_path() const { return root_ / L"missing.wav"; }
    std::filesystem::path write(const std::wstring& name, const std::vector<BYTE>& bytes) {
        const auto path = (root_ / name).lexically_normal();
        require(path.parent_path() == root_ && path.filename() == name,
                "Fixture names must stay inside the unique audio test directory");
        files_.push_back(path);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "Audio fixture must open for writing");
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
        require(static_cast<bool>(output), "Audio fixture must write and close completely");
        return path;
    }
    bool cleanup() noexcept {
        if (!created_) return true;
        try {
            // No recursive removal: delete only files created and tracked above,
            // then remove the now-empty unique directory. Check resolved parents
            // before each removal; unexpected files and paths stay untouched.
            if (root_.parent_path() != base_ || root_ == base_ ||
                root_.filename().wstring().find(L"alarm_audio_tests_") != 0 ||
                std::filesystem::weakly_canonical(root_).parent_path() != base_) return false;
            bool complete = true;
            for (const auto& path : files_) {
                if (path.parent_path() != root_ ||
                    std::filesystem::weakly_canonical(path).parent_path() != root_) return false;
                std::error_code error;
                std::filesystem::remove(path, error);
                if (error) complete = false;
            }
            std::error_code error;
            std::filesystem::remove(root_, error);
            if (error || std::filesystem::exists(root_)) complete = false;
            if (complete) created_ = false;
            return complete;
        } catch (...) { return false; }
    }
private:
    std::filesystem::path base_;
    std::filesystem::path root_;
    std::vector<std::filesystem::path> files_;
    bool created_ = false;
};

void append_u16(std::vector<BYTE>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<BYTE>(value));
    bytes.push_back(static_cast<BYTE>(value >> 8));
}
void append_u32(std::vector<BYTE>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<BYTE>(value >> shift));
}
void replace_u16(std::vector<BYTE>& bytes, std::size_t position, std::uint16_t value) {
    bytes.at(position) = static_cast<BYTE>(value);
    bytes.at(position + 1) = static_cast<BYTE>(value >> 8);
}
void replace_u32(std::vector<BYTE>& bytes, std::size_t position, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.at(position + shift / 8) = static_cast<BYTE>(value >> shift);
}
void append_tag(std::vector<BYTE>& bytes, const char* tag) {
    bytes.insert(bytes.end(), tag, tag + 4);
}
void append_chunk(std::vector<BYTE>& bytes, const char* tag, const std::vector<BYTE>& data) {
    append_tag(bytes, tag);
    append_u32(bytes, static_cast<std::uint32_t>(data.size()));
    bytes.insert(bytes.end(), data.begin(), data.end());
    if (data.size() & 1) bytes.push_back(0);
}

std::vector<BYTE> wav(unsigned tag, unsigned bits, unsigned channels = 1,
                      unsigned rate = 48000, bool extensible = false, bool metadata = false) {
    std::vector<BYTE> format;
    append_u16(format, static_cast<std::uint16_t>(extensible ? 0xfffe : tag));
    append_u16(format, static_cast<std::uint16_t>(channels));
    append_u32(format, rate);
    const unsigned alignment = channels * bits / 8;
    append_u32(format, rate * alignment);
    append_u16(format, static_cast<std::uint16_t>(alignment));
    append_u16(format, static_cast<std::uint16_t>(bits));
    if (extensible) {
        append_u16(format, 22);
        append_u16(format, static_cast<std::uint16_t>(bits));
        append_u32(format, channels == 1 ? 4 : 3);
        append_u32(format, tag);
        append_u16(format, 0);
        append_u16(format, 0x10);
        const BYTE suffix[]{0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
        format.insert(format.end(), std::begin(suffix), std::end(suffix));
    }
    std::vector<BYTE> data(256 * alignment, 0);
    for (std::size_t sample = 0; sample < 256 * channels; ++sample) {
        BYTE* position = data.data() + sample * bits / 8;
        if (tag == 1 && bits == 8) *position = 128;
        else if (tag == 3 && bits == 32) {
            const float value = sample % 2 ? -0.125f : 0.25f;
            std::memcpy(position, &value, sizeof(value));
        } else if (tag == 3 && bits == 64) {
            const double value = sample % 2 ? -0.125 : 0.25;
            std::memcpy(position, &value, sizeof(value));
        }
    }
    std::vector<BYTE> result;
    append_tag(result, "RIFF");
    append_u32(result, 0);
    append_tag(result, "WAVE");
    if (metadata) append_chunk(result, "JUNK", {1, 2, 3});
    append_chunk(result, "fmt ", format);
    append_chunk(result, "data", data);
    replace_u32(result, 4, static_cast<std::uint32_t>(result.size() - 8));
    return result;
}

AudioOptions silent_options() {
    AudioOptions options;
    options.session_volume = 0;
    options.override_endpoint = false;
    return options;
}

void audio_policy_tests() {
    for (const bool legacy_override : {false, true}) {
        for (const float legacy_endpoint : {0.0f, .5f, 1.0f}) {
            Config config;
            config.device_id = L"policy-only-endpoint";
            config.wav_path = L"policy-only-file.wav";
            config.session_volume = .37f;
            config.override_endpoint = legacy_override;
            config.endpoint_volume = legacy_endpoint;
            const auto alarm = make_alarm_audio_options(config, AudioPurpose::Alarm);
            require(alarm.override_endpoint && alarm.endpoint_volume == 1.0f,
                    "Every real alarm must force Windows endpoint maximum regardless of legacy preferences");
            require(alarm.session_volume == config.session_volume && alarm.device_id == config.device_id &&
                    alarm.wav_path == config.wav_path,
                    "Alarm policy must preserve configured session gain, selected output and sound");
            const auto preview = make_alarm_audio_options(config, AudioPurpose::Preview);
            require(!preview.override_endpoint && preview.session_volume == config.session_volume,
                    "Preview must preserve Windows volume/mute even when legacy override was enabled");
            require(preview.device_id == config.device_id && preview.wav_path == config.wav_path,
                    "Preview policy must preserve selected output and sound");
            const auto integration = make_alarm_audio_options(config, AudioPurpose::Integration);
            require(!integration.override_endpoint && integration.session_volume == 0.0f,
                    "Integration policy must be silent and never mutate Windows endpoint settings");
            require(integration.device_id == config.device_id && integration.wav_path == config.wav_path,
                    "Integration policy must preserve output/sound selection for end-to-end coverage");
            require(config.override_endpoint == legacy_override && config.endpoint_volume == legacy_endpoint &&
                    config.session_volume == .37f,
                    "Selecting an audio policy must not mutate or save the user's configuration");
        }
    }
    std::cout << "PASS: pure Alarm100/Preview/Integration policy; no system volume/mute mutation\n";
}

void reject(AudioPlayer& player, const AudioOptions& options, const char* message) {
    std::wstring error;
    require(!player.start(options, error), message);
    require(!player.playing(), "Rejected input must leave no playing stream");
    require(!error.empty() && !player.error().empty(), "Rejected input must report its error");
    player.stop();
    require(!player.playing(), "STOP after rejected input must stay stopped");
}

void invalid_options_tests(AudioPlayer& player, const TestDirectory& fixture) {
    const std::array<float, 5> invalid{-0.1f, 1.1f, std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
    for (const float value : invalid) {
        auto options = silent_options();
        options.session_volume = value;
        reject(player, options, "Out-of-range or non-finite session volume must be rejected before playback");
        options = silent_options();
        options.endpoint_volume = value;
        reject(player, options, "Out-of-range or non-finite endpoint volume must be rejected without mutation");
    }
    auto options = silent_options();
    options.wav_path = fixture.missing_path().wstring();
    reject(player, options, "Missing WAV file must be rejected");
    options = silent_options();
    options.device_id = L"alarm-audio-tests-missing-endpoint-" + std::to_wstring(GetCurrentProcessId());
    const auto before = std::chrono::steady_clock::now();
    reject(player, options, "Missing explicitly selected device must fail rather than fall back");
    const auto duration = std::chrono::steady_clock::now() - before;
    require(duration < 3s, "Missing-device retries must be bounded to less than three seconds");
    require(player.error().find(L"3 attempts") != std::wstring::npos,
            "Unavailable-device error must report exhausted retry limit");
    std::cout << "PASS: invalid finite/non-finite volumes, missing WAV, bounded missing-device retries ("
              << std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() << " ms)\n";
}

void playback_tests(AudioPlayer& player, const std::vector<AudioDevice>& devices) {
    std::wstring error;
    auto truncated_id = silent_options();
    truncated_id.device_id = devices.front().id;
    truncated_id.device_id.push_back(L'\0');
    truncated_id.device_id += L"concealed-endpoint";
    reject(player, truncated_id, "A valid endpoint ID with embedded NUL must not select its truncated prefix");
    for (unsigned cycle = 0; cycle < 5; ++cycle) {
        auto options = silent_options();
        if (cycle & 1) options.device_id = devices.front().id;
        require(player.start(options, error), "Built-in tone must start silently on default/pinned output");
        require(player.playing(), "A successful silent stream must be marked playing");
        const auto before = std::chrono::steady_clock::now();
        player.stop();
        const auto duration = std::chrono::steady_clock::now() - before;
        require(duration < 500ms, "STOP must interrupt event-driven playback promptly");
        require(!player.playing(), "STOP must finish stream cleanup before returning");
        player.stop();
        require(!player.playing(), "Repeated STOP must be safe and leave no stream");
        std::cout << "Silent STOP " << cycle + 1 << ": "
                  << std::chrono::duration_cast<std::chrono::microseconds>(duration).count() << " us\n";
    }
    auto options = silent_options();
    require(player.start(options, error), "Playback must restart after repeated STOP");
    require(player.start(options, error) && player.playing(),
            "Starting an already playing instance must replace/clean the previous stream");
    player.stop();
    {
        AudioPlayer scoped;
        require(scoped.start(options, error), "Scoped playback must start");
    }
    require(player.start(options, error), "Destruction of another player must not invalidate future playback");
    player.stop();
    std::cout << "PASS: default/pinned silent tone, immediate/idempotent STOP, restart, destruction\n";
}

void wav_tests(AudioPlayer& player, TestDirectory& fixture) {
    struct Format { unsigned tag; unsigned bits; };
    constexpr Format supported[]{{1, 8}, {1, 16}, {1, 24}, {1, 32}, {3, 32}, {3, 64}};
    std::wstring error;
    for (const auto format : supported) {
        for (unsigned channels : {1u, 2u}) {
            auto options = silent_options();
            const auto name = L"format_" + std::to_wstring(format.tag) + L"_" +
                std::to_wstring(format.bits) + L"_" + std::to_wstring(channels) + L".wav";
            options.wav_path = fixture.write(name, wav(format.tag, format.bits, channels,
                                                       channels == 1 ? 44100 : 48000)).wstring();
            require(player.start(options, error), "Supported mono/stereo WAV must open as a silent Windows stream");
            require(player.playing(), "Supported WAV must have a running stream");
            player.stop();
            require(!player.playing(), "WAV STOP must clean up its stream");
        }
    }
    for (const unsigned tag : {1u, 3u}) {
        auto options = silent_options();
        options.wav_path = fixture.write(L"extensible_" + std::to_wstring(tag) + L".wav",
            wav(tag, tag == 1 ? 16 : 32, 2, 96000, true, true)).wstring();
        require(player.start(options, error), "Extensible WAV and odd metadata padding must be accepted");
        player.stop();
        require(!player.playing(), "Extensible WAV must clean up after STOP");
    }
    auto truncated_path = silent_options();
    truncated_path.wav_path = fixture.write(L"nul_path.wav", wav(1, 16)).wstring();
    truncated_path.wav_path.push_back(L'\0');
    truncated_path.wav_path += L"concealed-suffix.wav";
    reject(player, truncated_path, "An existing WAV path with embedded NUL must not open its truncated prefix");
    unsigned invalid = 0;
    const auto reject_wav = [&](std::vector<BYTE> bytes, const char* reason) {
        auto options = silent_options();
        options.wav_path = fixture.write(L"invalid_" + std::to_wstring(invalid++) + L".wav", bytes).wstring();
        reject(player, options, reason);
    };
    reject_wav({'R', 'I', 'F', 'F'}, "Truncated WAV header must be rejected");
    auto bytes = wav(1, 16);
    bytes[0] = 'X';
    reject_wav(bytes, "Non-RIFF input must be rejected");
    reject_wav(wav(6, 16), "Compressed WAV must be rejected");
    bytes = wav(1, 16);
    replace_u32(bytes, 4, static_cast<std::uint32_t>(bytes.size() + 20));
    reject_wav(bytes, "Truncated RIFF body must be rejected");
    bytes = wav(1, 16);
    replace_u32(bytes, 40, 1000000);
    reject_wav(bytes, "Oversized data chunk must be rejected");
    bytes = wav(1, 16);
    replace_u16(bytes, 32, 3);
    reject_wav(bytes, "Invalid sample alignment must be rejected");
    bytes = wav(1, 16);
    replace_u32(bytes, 28, 1);
    reject_wav(bytes, "Invalid WAV byte rate must be rejected");
    reject_wav(wav(1, 16, 3), "Unsupported channel count must be rejected");
    reject_wav(wav(1, 16, 1, 1), "Unsupported sample rate must be rejected");
    for (float sample : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        bytes = wav(3, 32);
        std::memcpy(bytes.data() + 44, &sample, sizeof(sample));
        reject_wav(bytes, "Non-finite floating WAV sample must be rejected");
    }
    bytes = wav(1, 16, 2, 48000, true);
    replace_u16(bytes, 38, 0);
    reject_wav(bytes, "Zero valid bits in extensible WAV must be rejected");
    bytes = wav(1, 16);
    std::vector<BYTE> excessive_metadata;
    append_tag(excessive_metadata, "RIFF");
    append_u32(excessive_metadata, 0);
    append_tag(excessive_metadata, "WAVE");
    for (unsigned chunk = 0; chunk < 4097; ++chunk) append_chunk(excessive_metadata, "JUNK", {});
    excessive_metadata.insert(excessive_metadata.end(), bytes.begin() + 12, bytes.end());
    replace_u32(excessive_metadata, 4, static_cast<std::uint32_t>(excessive_metadata.size() - 8));
    const auto parsed_at = std::chrono::steady_clock::now();
    reject_wav(excessive_metadata, "Excess metadata chunks must be rejected without excessive parsing work");
    require(player.error().find(L"4096") != std::wstring::npos,
            "Chunk count rejection must identify its explicit parsing limit");
    require(std::chrono::steady_clock::now() - parsed_at < 2s,
            "Excess metadata rejection must remain bounded and prompt");
    // Removing three JUNK chunks leaves exactly 4096 total chunks, including
    // fmt/data. The accepted boundary protects ordinary RIFF metadata parsing.
    excessive_metadata.erase(excessive_metadata.begin() + 12, excessive_metadata.begin() + 36);
    replace_u32(excessive_metadata, 4, static_cast<std::uint32_t>(excessive_metadata.size() - 8));
    auto boundary = silent_options();
    boundary.wav_path = fixture.write(L"metadata_boundary.wav", excessive_metadata).wstring();
    require(player.start(boundary, error), "A WAV with exactly 4096 chunks must remain accepted");
    player.stop();
    require(!player.playing(), "The metadata-boundary stream must clean up after STOP");
    std::cout << "PASS: six mono/stereo WAV formats, extensible metadata, " << invalid
              << " malformed/unsupported WAV inputs\n";
}

// The held gate models a slow preparation call without touching any audio
// device. Its destructor always releases it before the owning player joins.
class PreparationEvent {
public:
    PreparationEvent() : value_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        require(value_ != nullptr, "Preparation test event must be created");
    }
    ~PreparationEvent() { if (value_) { SetEvent(value_); CloseHandle(value_); } }
    PreparationEvent(const PreparationEvent&) = delete;
    HANDLE get() const { return value_; }
    void release() { require(SetEvent(value_) != FALSE, "Preparation test event must release its worker"); }
    void close_original() { CloseHandle(value_); value_ = nullptr; }
private:
    HANDLE value_;
};

template<class Predicate>
AudioStatus wait_audio(AudioPlayer& player, Predicate done, const char* failure) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    AudioStatus snapshot;
    do {
        snapshot = player.status();
        if (done(snapshot)) { require(true, failure); return snapshot; }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    require(false, failure);
    return snapshot;
}

void asynchronous_tests(const TestDirectory& fixtures) {
    std::atomic<unsigned> notifications{0}; // Outlives player and all callbacks.
    AudioPlayer player;
    PreparationEvent held; // Unwind releases the gate before player destruction.
    AudioPlayerTestAccess::gate(player, held.get(), false);
    player.set_state_callback([&notifications] { ++notifications; });
    const auto original = player.status();
    require(original.state == AudioState::Idle && original.finished && !original.initialized,
            "A new player must have a finished idle generation");
    std::wstring error;
    auto began = std::chrono::steady_clock::now();
    require(player.begin_start(silent_options(), error), "Asynchronous preparation must be accepted");
    require(std::chrono::steady_clock::now() - began < 200ms,
            "begin_start must return while preparation is deliberately blocked");
    auto preparing = player.status();
    require(preparing.state == AudioState::Starting && !preparing.finished && !preparing.initialized &&
            !preparing.start_succeeded && !player.playing(),
            "Held preparation must not claim initialization or playback");
    require(preparing.generation == original.generation + 1,
            "An accepted begin must allocate one new generation");
    require(notifications == 1, "Beginning preparation must notify the state consumer once");
    began = std::chrono::steady_clock::now();
    require(!player.begin_start(silent_options(), error) && !error.empty(),
            "A begin during held preparation must explicitly refuse replacement");
    require(std::chrono::steady_clock::now() - began < 200ms,
            "Refusing a live preparation must not join or wait for it");
    require(player.status().generation == preparing.generation,
            "A refused begin must preserve the current generation");
    began = std::chrono::steady_clock::now();
    player.cancel();
    require(std::chrono::steady_clock::now() - began < 200ms,
            "cancel must return while an uncancelable preparation is still held");
    player.cancel();
    require(notifications == 2, "Repeated cancellation must coalesce a pending state notice");
    auto stopping = player.status();
    require(stopping.state == AudioState::Stopping && !stopping.finished && !stopping.initialized,
            "Cancellation must retain the worker until its real termination");
    require(!player.begin_start(silent_options(), error),
            "A canceled but still held worker must refuse restart");
    require(player.status().generation == preparing.generation,
            "A refused restart must not publish a stale new generation");
    held.release();
    const auto stopped = wait_audio(player, [](const AudioStatus& state) { return state.finished; },
                                   "Released cancellation must actually terminate its owned worker");
    require(stopped.state == AudioState::Idle && stopped.initialized && !stopped.start_succeeded &&
            stopped.generation == preparing.generation,
            "Canceled preparation must retain its failed first result in the same idle generation");
    require(notifications == 3, "Final cleanup must notify even when no visible UI polls its state");

    // The engine duplicates the test gate. Closing the caller's original event
    // must not create a dangling worker resource; cancellation wakes the copy.
    PreparationEvent cancelable;
    AudioPlayerTestAccess::gate(player, cancelable.get(), true);
    require(player.begin_start(silent_options(), error), "A finished generation must permit a new begin");
    const auto second = player.status();
    cancelable.close_original();
    player.cancel();
    const auto canceled_copy = wait_audio(player, [](const AudioStatus& state) { return state.finished; },
                                         "Cancellation must wake the owned gate after the original is closed");
    require(second.generation == stopped.generation + 1 && canceled_copy.generation == second.generation &&
            canceled_copy.state == AudioState::Idle && !canceled_copy.start_succeeded,
            "Restart and cancellation must isolate generations and own their preparation handles");

    AudioPlayerTestAccess::gate(player, nullptr, true);
    auto missing = silent_options();
    missing.wav_path = fixtures.missing_path().wstring();
    require(player.begin_start(missing, error), "A valid asynchronous request must defer missing-file failure");
    const auto failed = wait_audio(player, [](const AudioStatus& state) { return state.finished; },
                                  "A missing WAV must finish and publish its asynchronous error");
    require(failed.state == AudioState::Error && failed.initialized && !failed.start_succeeded &&
            !failed.message.empty() && failed.generation == second.generation + 1,
            "Asynchronous WAV failure must publish the current generation's failed first result");
    require(player.begin_start(silent_options(), error), "A finished error generation must permit silent restart");
    const auto playing = wait_audio(player, [](const AudioStatus& state) {
        return state.initialized && state.state == AudioState::Playing;
    }, "Asynchronous silent WASAPI restart must initialize and play");
    require(playing.start_succeeded && !playing.finished && playing.generation == failed.generation + 1,
            "A new success must replace the previous generation's failed result");
    player.cancel();
    const auto completed = wait_audio(player, [](const AudioStatus& state) { return state.finished; },
                                     "Asynchronous STOP must complete cleanup of a silent stream");
    require(completed.state == AudioState::Idle && completed.initialized && completed.start_succeeded &&
            completed.generation == playing.generation && !player.playing(),
            "A canceled successful generation must preserve its first result and stop playback");
    player.set_state_callback({});
    const auto before_clear = notifications.load();
    AudioPlayerTestAccess::gate(player, held.get(), false);
    ResetEvent(held.get());
    require(player.begin_start(silent_options(), error), "Restart after cleanup must remain available");
    player.cancel();
    held.release();
    wait_audio(player, [](const AudioStatus& state) { return state.finished; },
               "The final silent cancellation must terminate before callback ownership is released");
    require(notifications == before_clear, "Clearing the callback must suppress future state notifications");
    player.stop();
    require(player.status().finished && player.status().state == AudioState::Idle,
            "Legacy stop must join and retain a finished idle state after asynchronous use");
    std::cout << "PASS: nonblocking preparation, cancellation, coalesced notices, owned handles, restart generations\n";
}

void terminal_notification_test() {
    AudioPlayer player;
    PreparationEvent preparation, entered, completion;
    const auto caller = GetCurrentThreadId();
    AudioPlayerTestAccess::gate(player, preparation.get(), false);
    player.set_state_callback([&] {
        // Deliberately hold just the terminal notice in this test. The caller
        // can prove that publishing Idle is earlier than native termination.
        if (GetCurrentThreadId() != caller && player.status().state == AudioState::Idle) {
            SetEvent(entered.get());
            WaitForSingleObject(completion.get(), 2000);
        }
    });
    std::wstring error;
    require(player.begin_start(silent_options(), error), "Terminal-notice test must begin silent preparation");
    player.status(); // Acknowledge Starting so cancellation can notify separately.
    player.cancel();
    player.status(); // Acknowledge Stopping before the worker's terminal notice.
    preparation.release();
    require(WaitForSingleObject(entered.get(), 3000) == WAIT_OBJECT_0,
            "The worker must deliver its final notice while its resources are still owned");
    const auto terminal = player.status();
    require(terminal.state == AudioState::Idle && !terminal.finished && terminal.initialized,
            "A terminal state callback must not claim the native thread has already exited");
    require(!player.begin_start(silent_options(), error),
            "Restart must be refused until the terminal callback has actually returned and the worker exited");
    completion.release();
    wait_audio(player, [](const AudioStatus& state) { return state.finished; },
               "Native completion must become observable after the final callback returns");
    player.set_state_callback({});
    player.stop();
    std::cout << "PASS: final Idle notification precedes actual worker termination; restart waits for ownership cleanup\n";
}

} // namespace

int main() {
    try {
        audio_policy_tests();
        std::wstring report;
        const auto devices = AudioPlayer::devices(report);
        require(!devices.empty(), "Audio integration tests require an active Windows render endpoint");
        std::set<std::wstring> ids;
        for (const auto& device : devices) {
            require(!device.id.empty() && !device.name.empty(), "Each active output needs its ID and friendly name");
            require(ids.insert(device.id).second, "Active output IDs must be unique");
        }
        const auto original = read_endpoints(devices);
        require(AudioPlayer::probe(report), "Silent WASAPI probe must confirm event-driven buffer refills");
        std::wcout << report << L"\n";
        TestDirectory fixtures;
        AudioPlayer player;
        invalid_options_tests(player, fixtures);
        playback_tests(player, devices);
        wav_tests(player, fixtures);
        player.stop();
        asynchronous_tests(fixtures);
        terminal_notification_test();
        const auto after = read_endpoints(devices);
        require(after.size() == original.size(), "Endpoint count must remain stable during audio tests");
        for (std::size_t i = 0; i < original.size(); ++i) {
            require(after[i].id == original[i].id && after[i].mute == original[i].mute &&
                    std::fabs(after[i].volume - original[i].volume) < 0.000001f,
                    "All silent tests must preserve each endpoint's exact master volume and mute state");
        }
        require(fixtures.cleanup(), "Unique temporary WAV fixtures must be completely cleaned up");
        std::cout << "PASS: endpoint master/mute unchanged; temporary fixtures removed\n";
        std::cout << "All " << assertions << " audio assertions passed. Hardware audibility is not tested.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << assertions << " audio assertions: " << error.what() << '\n';
        return 1;
    }
}
