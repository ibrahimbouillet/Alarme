// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#include "audio.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <process.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace alarmapp {
namespace {

template<class T> class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T* operator->() const { return ptr_; }
    T* get() const { return ptr_; }
    T** out() { reset(); return &ptr_; }
    void reset() { if (ptr_) ptr_->Release(); ptr_ = nullptr; }
private:
    T* ptr_ = nullptr;
};

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    HANDLE get() const { return value_; }
    void reset(HANDLE value = nullptr) { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); value_ = value; }
private:
    HANDLE value_;
};

class ComScope {
public:
    ComScope() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComScope() { if (SUCCEEDED(result_)) CoUninitialize(); }
    bool ok() const { return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE; }
    HRESULT result() const { return result_; }
private:
    HRESULT result_;
};

std::wstring hresult_text(const wchar_t* operation, HRESULT hr) {
    std::wostringstream text;
    text << operation << L" (0x" << std::hex << std::uppercase << std::setw(8)
         << std::setfill(L'0') << static_cast<unsigned long>(hr) << L")";
    wchar_t* message = nullptr;
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, hr, 0,
                       reinterpret_cast<wchar_t*>(&message), 0, nullptr)) {
        std::wstring detail(message);
        LocalFree(message);
        while (!detail.empty() && (detail.back() == L'\r' || detail.back() == L'\n' ||
                                   detail.back() == L' ')) detail.pop_back();
        text << L": " << detail;
    }
    return text.str();
}

void require(HRESULT hr, const wchar_t* operation) {
    if (FAILED(hr)) throw hresult_text(operation, hr);
}

bool cancelled(HANDLE stop) { return WaitForSingleObject(stop, 0) == WAIT_OBJECT_0; }

constexpr GUID kPcm = {1, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
constexpr GUID kFloat = {3, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
constexpr GUID kAudioContext = {0xbbfb5ac2, 0x4b81, 0x4ad0,
                              {0xa4, 0x9f, 0xe7, 0x1d, 0x46, 0x54, 0x0e, 0x12}};

struct Source {
    WORD channels = 1;
    DWORD rate = 48000;
    std::vector<float> samples;
    std::size_t cursor = 0;
    std::uint64_t tone_frame = 0;

    void fill(BYTE* destination, UINT32 frames) {
        auto* output = reinterpret_cast<float*>(destination);
        if (!samples.empty()) {
            for (std::size_t i = 0; i < static_cast<std::size_t>(frames) * channels; ++i) {
                output[i] = samples[cursor];
                if (++cursor == samples.size()) cursor = 0;
            }
            return;
        }
        constexpr double pi = 3.14159265358979323846;
        for (UINT32 i = 0; i < frames; ++i, ++tone_frame) {
            const double t = static_cast<double>(tone_frame % (rate * 6u)) / rate;
            const double pulse = std::fmod(t, 0.6);
            const double ramp = std::min({1.0, pulse / 0.01, (0.45 - pulse) / 0.01});
            const double envelope = pulse < 0.45 ? std::max(0.0, ramp) : 0.0;
            const double frequency = static_cast<unsigned>(t / 0.6) % 2 ? 660.0 : 880.0;
            output[i] = static_cast<float>(0.22 * envelope * std::sin(2 * pi * frequency * t));
        }
    }
};

WORD u16(const BYTE* p) { return static_cast<WORD>(p[0] | (p[1] << 8)); }
DWORD u32(const BYTE* p) {
    return static_cast<DWORD>(p[0]) | (static_cast<DWORD>(p[1]) << 8) |
           (static_cast<DWORD>(p[2]) << 16) | (static_cast<DWORD>(p[3]) << 24);
}

void read_exact(HANDLE file, BYTE* output, std::size_t count, HANDLE stop) {
    while (count) {
        if (cancelled(stop)) throw std::wstring(L"Audio playback cancelled.");
        DWORD received = 0;
        const DWORD part = static_cast<DWORD>(std::min<std::size_t>(count, 1024 * 1024));
        if (!ReadFile(file, output, part, &received, nullptr))
            throw hresult_text(L"Read WAV file", HRESULT_FROM_WIN32(GetLastError()));
        if (received != part) throw std::wstring(L"Truncated WAV file.");
        output += received;
        count -= received;
    }
}

void seek_file(HANDLE file, std::uint64_t offset) {
    LARGE_INTEGER location{};
    location.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, location, nullptr, FILE_BEGIN))
        throw hresult_text(L"Seek within WAV file", HRESULT_FROM_WIN32(GetLastError()));
}

Source load_source(const std::wstring& path, HANDLE stop) {
    Source source;
    if (path.empty()) return source;
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                            nullptr));
    if (file.get() == INVALID_HANDLE_VALUE)
        throw hresult_text(L"Open WAV file", HRESULT_FROM_WIN32(GetLastError()));
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size))
        throw hresult_text(L"WAV file size", HRESULT_FROM_WIN32(GetLastError()));
    if (size.QuadPart < 12 || size.QuadPart > 512ll * 1024 * 1024)
        throw std::wstring(L"The WAV file must be between 12 bytes and 512 MiB.");
    BYTE header[12]{};
    read_exact(file.get(), header, sizeof(header), stop);
    if (std::memcmp(header, "RIFF", 4) || std::memcmp(header + 8, "WAVE", 4))
        throw std::wstring(L"Unsupported format: choose a RIFF WAV file with PCM or floating-point audio.");
    const std::uint64_t end = static_cast<std::uint64_t>(u32(header + 4)) + 8;
    if (end < 12 || end > static_cast<std::uint64_t>(size.QuadPart))
        throw std::wstring(L"Invalid or truncated WAV header.");
    std::vector<BYTE> format, data;
    unsigned chunks = 0;
    for (std::uint64_t position = 12; position + 8 <= end;) {
        // File size alone does not bound parsing work: zero-length metadata can
        // otherwise force millions of seek/read calls before playback starts.
        if (++chunks > 4096)
            throw std::wstring(L"The WAV file contains too many chunks (maximum 4096).");
        seek_file(file.get(), position);
        BYTE chunk[8]{};
        read_exact(file.get(), chunk, sizeof(chunk), stop);
        const DWORD bytes = u32(chunk + 4);
        const std::uint64_t next = position + 8 + bytes + (bytes & 1u);
        if (next > end) throw std::wstring(L"Invalid or truncated WAV chunk.");
        if (!std::memcmp(chunk, "fmt ", 4) && format.empty()) {
            if (bytes < 16 || bytes > 4096) throw std::wstring(L"Invalid WAV format.");
            format.resize(bytes);
            read_exact(file.get(), format.data(), bytes, stop);
        } else if (!std::memcmp(chunk, "data", 4) && data.empty()) {
            if (!bytes || bytes > 128u * 1024 * 1024)
                throw std::wstring(L"WAV audio data must be nonempty and smaller than 128 MiB.");
            data.resize(bytes);
            read_exact(file.get(), data.data(), bytes, stop);
        }
        position = next;
    }
    if (format.empty() || data.empty()) throw std::wstring(L"Missing WAV fmt/data chunks.");
    WORD tag = u16(format.data());
    source.channels = u16(format.data() + 2);
    source.rate = u32(format.data() + 4);
    const WORD alignment = u16(format.data() + 12);
    const WORD bits = u16(format.data() + 14);
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        if (format.size() < 40 || u16(format.data() + 16) < 22)
            throw std::wstring(L"Invalid extensible WAV format.");
        GUID subtype{};
        std::memcpy(&subtype, format.data() + 24, sizeof(subtype));
        if (IsEqualGUID(subtype, kPcm)) tag = WAVE_FORMAT_PCM;
        else if (IsEqualGUID(subtype, kFloat)) tag = WAVE_FORMAT_IEEE_FLOAT;
        else throw std::wstring(L"Unsupported compressed WAV subformat.");
        const WORD valid_bits = u16(format.data() + 18);
        if (!valid_bits || valid_bits > bits)
            throw std::wstring(L"Invalid WAV valid-bit count.");
        const DWORD mask = u32(format.data() + 20);
        if (mask && !((source.channels == 1 && (mask == 1 || mask == 4)) ||
                      (source.channels == 2 && mask == 3)))
            throw std::wstring(L"Unsupported WAV channel layout.");
    }
    if ((source.channels != 1 && source.channels != 2) || source.rate < 8000 ||
        source.rate > 192000)
        throw std::wstring(L"The WAV must be mono or stereo, from 8000 to 192000 Hz.");
    if (!((tag == WAVE_FORMAT_PCM && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
          (tag == WAVE_FORMAT_IEEE_FLOAT && (bits == 32 || bits == 64))))
        throw std::wstring(L"Supported WAV formats: 8/16/24/32-bit PCM or 32/64-bit floating point.");
    const WORD sample_bytes = bits / 8;
    if (alignment != source.channels * sample_bytes || data.size() % alignment ||
        u32(format.data() + 8) != source.rate * alignment)
        throw std::wstring(L"Invalid WAV alignment or byte rate.");
    const std::size_t samples = data.size() / sample_bytes;
    if (samples > 32u * 1024 * 1024)
        throw std::wstring(L"WAV is too long: maximum 128 MiB after decoding.");
    source.samples.resize(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        if ((i & 0xffffu) == 0 && cancelled(stop))
            throw std::wstring(L"Audio playback cancelled.");
        const BYTE* sample = data.data() + i * sample_bytes;
        double value = 0;
        if (tag == WAVE_FORMAT_IEEE_FLOAT) {
            if (bits == 32) { float v; std::memcpy(&v, sample, sizeof(v)); value = v; }
            else std::memcpy(&value, sample, sizeof(value));
            if (!std::isfinite(value)) throw std::wstring(L"The WAV contains a nonfinite audio sample.");
        } else if (bits == 8) value = (static_cast<int>(sample[0]) - 128) / 128.0;
        else if (bits == 16) value = static_cast<std::int16_t>(u16(sample)) / 32768.0;
        else if (bits == 24) {
            std::int32_t v = sample[0] | (sample[1] << 8) | (sample[2] << 16);
            if (v & 0x800000) v |= static_cast<std::int32_t>(0xff000000u);
            value = v / 8388608.0;
        } else value = static_cast<std::int32_t>(u32(sample)) / 2147483648.0;
        source.samples[i] = static_cast<float>(std::clamp(value, -1.0, 1.0));
    }
    return source;
}

std::wstring device_id(IMMDevice* device) {
    wchar_t* id = nullptr;
    require(device->GetId(&id), L"Audio device ID");
    std::wstring result(id);
    CoTaskMemFree(id);
    return result;
}

class DeviceNotifications final : public IMMNotificationClient {
public:
    DeviceNotifications(HANDLE changed, std::wstring id, bool follow_default)
        : changed_(changed), id_(std::move(id)), follow_default_(follow_default) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(IMMNotificationClient))) {
            *value = static_cast<IMMNotificationClient*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (follow_default_ && flow == eRender && role == eMultimedia) SetEvent(changed_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD) override {
        if (id && id_ == id) SetEvent(changed_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
        if (id && id_ == id) SetEvent(changed_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
private:
    std::atomic<ULONG> references_{1};
    HANDLE changed_;
    const std::wstring id_;
    const bool follow_default_;
};

struct EndpointSnapshot {
    std::wstring id;
    float volume = 0;
    BOOL mute = FALSE;
    bool volume_changed = false;
    bool mute_changed = false;
};

// Owns all COM interfaces on the audio worker's apartment. Stop/reset precede
// endpoint restoration, including paths where only part of setup succeeded.
struct Stream {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    ComPtr<ISimpleAudioVolume> session;
    ComPtr<IAudioEndpointVolume> endpoint;
    ComPtr<IMMNotificationClient> notifications;
    IMMDeviceEnumerator* enumerator = nullptr;
    bool registered = false;
    EndpointSnapshot snapshot;
    UINT32 capacity = 0;
    ~Stream() {
        if (client.get()) { client->Stop(); client->Reset(); }
        if (registered) enumerator->UnregisterEndpointNotificationCallback(notifications.get());
    }
};

std::wstring restore_endpoint(IAudioEndpointVolume* endpoint, EndpointSnapshot& snapshot) {
    std::wstring failures;
    if (snapshot.volume_changed) {
        const HRESULT hr = endpoint->SetMasterVolumeLevelScalar(snapshot.volume, &kAudioContext);
        if (SUCCEEDED(hr)) snapshot.volume_changed = false;
        else failures = hresult_text(L"Restore volume", hr);
    }
    if (snapshot.mute_changed) {
        const HRESULT hr = endpoint->SetMute(snapshot.mute, &kAudioContext);
        if (SUCCEEDED(hr)) snapshot.mute_changed = false;
        else {
            if (!failures.empty()) failures += L"; ";
            failures += hresult_text(L"Restore mute state", hr);
        }
    }
    return failures;
}

} // namespace

struct AudioPlayer::Impl {
    Handle stop_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle sample_event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    Handle changed_event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::mutex operation_mutex;
    mutable std::mutex state_mutex;
    std::condition_variable initialized;
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> submitted_buffers{0};
    AudioState phase = AudioState::Idle;
    std::uint64_t generation = 0;
    HANDLE worker_handle = nullptr;
    bool worker_expected = false;
    std::function<void()> state_callback;
    mutable bool notification_pending = false;
    Handle preparation_gate;
    bool preparation_cancelable = true;
    bool ready = false;
    bool start_result = false;
    std::wstring message;
    // Data only, never COM pointers, survives device removal and subsequent starts.
    std::vector<EndpointSnapshot> pending_restores;

    ~Impl() { if (worker_handle) CloseHandle(worker_handle); }

    void publish_state(AudioState value, std::initializer_list<AudioState> eligible = {}) noexcept {
        std::function<void()> callback;
        try {
            std::lock_guard lock(state_mutex);
            if (eligible.size() && std::find(eligible.begin(), eligible.end(), phase) == eligible.end()) return;
            if (phase == value) return;
            phase = value;
            if (state_callback && !notification_pending) {
                callback = state_callback; notification_pending = true;
            }
        } catch (...) { return; }
        if (callback) { try { callback(); } catch (...) {} }
    }
    bool thread_finished_locked() const {
        return !worker_expected || (worker_handle && WaitForSingleObject(worker_handle, 0) == WAIT_OBJECT_0);
    }
    bool reap_finished() {
        { std::lock_guard lock(state_mutex); if (!thread_finished_locked()) return false; }
        std::lock_guard lock(state_mutex);
        if (worker_handle) CloseHandle(worker_handle);
        worker_handle = nullptr;
        worker_expected = false;
        return true;
    }
    void request_cancel() {
        SetEvent(stop_event.get());
        {
            std::lock_guard lock(state_mutex);
            if (worker_handle) CancelSynchronousIo(worker_handle);
        }
        publish_state(AudioState::Stopping, {AudioState::Starting, AudioState::Playing});
        publish_state(AudioState::Idle, {AudioState::Error});
    }
    bool validate(const AudioOptions& options, std::wstring& error) {
        if (options.device_id.find(L'\0') != std::wstring::npos || options.wav_path.find(L'\0') != std::wstring::npos)
            error = L"The WAV path and audio device ID must not contain NUL characters.";
        else if (!std::isfinite(options.session_volume) || options.session_volume < 0 || options.session_volume > 1 ||
                 !std::isfinite(options.endpoint_volume) || options.endpoint_volume < 0 || options.endpoint_volume > 1)
            error = L"Volume levels must be between 0 and 1.";
        else if (!stop_event.get() || !sample_event.get() || !changed_event.get())
            error = L"Cannot create audio engine events.";
        else return true;
        return false;
    }
    bool launch(const AudioOptions& options, std::wstring& error) {
        ResetEvent(stop_event.get()); submitted_buffers = 0;
        {
            std::lock_guard lock(state_mutex);
            ++generation; ready = false; start_result = false; message.clear(); worker_expected = true;
        }
        publish_state(AudioState::Starting);
        try {
            auto parameters = std::make_unique<WorkerParameters>(WorkerParameters{this, options});
            // _beginthreadex returns an owned, real Windows thread handle even
            // if the worker exits before this call returns. Unlike MinGW's
            // std::thread native_handle, it supports CancelSynchronousIo and a
            // wait for actual native termination. No second handle allocation
            // can strand an already-created worker in a false unfinished state.
            auto* argument = parameters.release();
            const auto native = _beginthreadex(nullptr, 0, worker_entry, argument, 0, nullptr);
            if (!native) { delete argument; throw std::runtime_error("Cannot create audio thread"); }
            std::lock_guard lock(state_mutex);
            worker_handle = reinterpret_cast<HANDLE>(native);
        }
        catch (...) {
            { std::lock_guard lock(state_mutex); worker_expected = false; }
            error = L"Cannot start the audio worker thread.";
            set_message(error); signal_ready(false); publish_state(AudioState::Error); return false;
        }
        return true;
    }

    struct WorkerParameters { Impl* owner; AudioOptions options; };
    static unsigned __stdcall worker_entry(void* argument) noexcept {
        std::unique_ptr<WorkerParameters> parameters(static_cast<WorkerParameters*>(argument));
        parameters->owner->run(std::move(parameters->options));
        return 0; // CRT ends the thread; the owning player closes its handle.
    }

    void set_message(std::wstring value) {
        std::lock_guard lock(state_mutex);
        message = std::move(value);
    }
    void append_message(const std::wstring& value) {
        if (value.empty()) return;
        std::lock_guard lock(state_mutex);
        if (!message.empty()) message += L"; ";
        message += value;
    }
    void signal_ready(bool result) {
        std::lock_guard lock(state_mutex);
        if (ready) return;
        start_result = result;
        ready = true;
        initialized.notify_all();
    }

    void restore_pending(IMMDeviceEnumerator* enumerator) {
        for (auto iterator = pending_restores.begin(); iterator != pending_restores.end();) {
            ComPtr<IMMDevice> device;
            ComPtr<IAudioEndpointVolume> endpoint;
            HRESULT hr = enumerator->GetDevice(iterator->id.c_str(), device.out());
            if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                                                    nullptr, reinterpret_cast<void**>(endpoint.out()));
            if (SUCCEEDED(hr)) restore_endpoint(endpoint.get(), *iterator);
            if (!iterator->volume_changed && !iterator->mute_changed)
                iterator = pending_restores.erase(iterator);
            else ++iterator;
        }
    }

    void close_stream(Stream& stream) {
        if (stream.client.get()) { stream.client->Stop(); stream.client->Reset(); }
        if (stream.endpoint.get() && (stream.snapshot.volume_changed || stream.snapshot.mute_changed)) {
            const auto issue = restore_endpoint(stream.endpoint.get(), stream.snapshot);
            if (!issue.empty()) {
                pending_restores.push_back(stream.snapshot);
                append_message(issue + L"; restoration retained for this device.");
            }
        }
    }

    void open(Stream& stream, IMMDeviceEnumerator* enumerator, const AudioOptions& options,
              const Source& source) {
        if (cancelled(stop_event.get())) throw std::wstring(L"Audio playback cancelled.");
        if (options.device_id.empty())
            require(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, stream.device.out()),
                    L"No default Windows audio output is available");
        else require(enumerator->GetDevice(options.device_id.c_str(), stream.device.out()),
                     L"The selected audio device is unavailable");
        DWORD state = 0;
        require(stream.device->GetState(&state), L"Audio device state");
        if (!(state & DEVICE_STATE_ACTIVE)) throw std::wstring(L"The selected audio device is inactive or disconnected.");
        stream.snapshot.id = device_id(stream.device.get());
        if (options.override_endpoint && std::any_of(pending_restores.begin(), pending_restores.end(),
                [&](const auto& snapshot) { return snapshot.id == stream.snapshot.id; }))
            throw std::wstring(L"This device's original audio state is still awaiting restoration.");
        require(stream.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                         reinterpret_cast<void**>(stream.client.out())),
                L"Activate WASAPI");
        // Windows maps Alerts to alarm/ringtone/notification processing. Set the
        // category before Initialize; this does not override Modern Standby's
        // policy that can suppress sound after explicit user-initiated sleep.
        ComPtr<IAudioClient2> categorized_client;
        require(stream.client->QueryInterface(__uuidof(IAudioClient2),
                    reinterpret_cast<void**>(categorized_client.out())),
                L"Windows audio category interface");
        AudioClientProperties properties{};
        properties.cbSize = sizeof(properties);
        properties.bIsOffload = FALSE;
        properties.eCategory = AudioCategory_Alerts;
        properties.Options = AUDCLNT_STREAMOPTIONS_NONE;
        require(categorized_client->SetClientProperties(&properties),
                L"Windows Alerts audio category (alarm)");
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = source.channels;
        format.nSamplesPerSec = source.rate;
        format.wBitsPerSample = 32;
        format.nBlockAlign = format.nChannels * sizeof(float);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        GUID session_id{};
        require(CoCreateGuid(&session_id), L"Create audio session");
        require(stream.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST,
                    1000000, 0, &format, &session_id), L"Initialize shared WASAPI");
        // Required before any subsequent fallible setup, even if Start is never reached.
        require(stream.client->SetEventHandle(sample_event.get()), L"WASAPI event");
        require(stream.client->GetBufferSize(&stream.capacity), L"WASAPI buffer size");
        require(stream.client->GetService(__uuidof(IAudioRenderClient),
                                          reinterpret_cast<void**>(stream.render.out())),
                L"WASAPI render client");
        require(stream.client->GetService(__uuidof(ISimpleAudioVolume),
                                          reinterpret_cast<void**>(stream.session.out())),
                L"Audio session volume");
        require(stream.session->SetMasterVolume(options.session_volume, &kAudioContext),
                L"ALARM session volume");
        require(stream.session->SetMute(FALSE, &kAudioContext), L"Unmute ALARM session");
        if (options.session_volume <= 0.0001f)
            append_message(L"ALARM session volume is zero: this stream is deliberately silent.");
        const HRESULT endpoint_hr = stream.device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                nullptr, reinterpret_cast<void**>(stream.endpoint.out()));
        if (FAILED(endpoint_hr)) {
            if (options.override_endpoint) require(endpoint_hr, L"Device volume control");
            append_message(L"Cannot verify the device volume/mute state; audibility is not guaranteed.");
        } else {
            const HRESULT volume_hr = stream.endpoint->GetMasterVolumeLevelScalar(&stream.snapshot.volume);
            const HRESULT mute_hr = stream.endpoint->GetMute(&stream.snapshot.mute);
            if (options.override_endpoint) {
                require(volume_hr, L"Save device volume");
                require(mute_hr, L"Save device mute state");
                // Mark before mutation: failed COM setters may still have changed the device.
                stream.snapshot.volume_changed = true;
                require(stream.endpoint->SetMasterVolumeLevelScalar(options.endpoint_volume, &kAudioContext),
                        L"Temporary device volume");
                stream.snapshot.mute_changed = true;
                require(stream.endpoint->SetMute(FALSE, &kAudioContext), L"Temporarily unmute device");
                // A successful setter alone does not prove the device accepted
                // maximum volume or unmute. Read back before starting/announcing
                // playback; failure follows the same snapshot rollback path.
                float actual_volume = 0;
                BOOL actual_mute = TRUE;
                require(stream.endpoint->GetMasterVolumeLevelScalar(&actual_volume),
                        L"Verify temporary device volume");
                require(stream.endpoint->GetMute(&actual_mute),
                        L"Verify temporary device unmute");
                if (actual_mute || (options.endpoint_volume == 1.0f &&
                    (!std::isfinite(actual_volume) || actual_volume != 1.0f)))
                    throw std::wstring(L"Windows did not confirm maximum device volume or unmute.");
                if (options.endpoint_volume <= 0.0001f)
                    append_message(L"Temporary device volume is zero: the output is inaudible.");
            } else if (FAILED(volume_hr) || FAILED(mute_hr))
                append_message(L"Cannot verify the device volume/mute state; audibility is not guaranteed.");
            else if (stream.snapshot.mute || stream.snapshot.volume <= 0.0001f)
                append_message(L"Windows output is muted or at zero volume: the stream may be inaudible. System volume is preserved.");
        }
        stream.enumerator = enumerator;
        *stream.notifications.out() = new DeviceNotifications(changed_event.get(), stream.snapshot.id,
                                                               options.device_id.empty());
        require(enumerator->RegisterEndpointNotificationCallback(stream.notifications.get()),
                L"Track audio devices");
        stream.registered = true;
    }

    void run(AudioOptions options) noexcept {
        try {
            if (preparation_gate.get()) {
                if (preparation_cancelable) {
                    HANDLE gates[]{stop_event.get(), preparation_gate.get()};
                    const auto result = WaitForMultipleObjects(2, gates, FALSE, INFINITE);
                    if (result != WAIT_OBJECT_0 && result != WAIT_OBJECT_0 + 1)
                        throw std::wstring(L"Invalid audio preparation event.");
                } else if (WaitForSingleObject(preparation_gate.get(), INFINITE) != WAIT_OBJECT_0)
                    throw std::wstring(L"Invalid audio preparation event.");
            }
            if (cancelled(stop_event.get())) throw std::wstring(L"Audio playback cancelled.");
            ComScope com;
            if (!com.ok()) throw hresult_text(L"Initialize audio COM", com.result());
            ComPtr<IMMDeviceEnumerator> enumerator;
            require(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator.out())),
                    L"Enumerate Core Audio devices");
            Source source = load_source(options.wav_path, stop_event.get());
            unsigned attempts = 0;
            std::wstring last_failure;
            while (!cancelled(stop_event.get())) {
                restore_pending(enumerator.get());
                Stream stream;
                bool device_changed = false;
                try {
                    ResetEvent(sample_event.get());
                    ResetEvent(changed_event.get());
                    open(stream, enumerator.get(), options, source);
                    if (cancelled(stop_event.get())) throw std::wstring(L"Audio playback cancelled.");
                    BYTE* buffer = nullptr;
                    require(stream.render->GetBuffer(stream.capacity, &buffer), L"Initial WASAPI buffer");
                    source.fill(buffer, stream.capacity);
                    require(stream.render->ReleaseBuffer(stream.capacity, 0), L"Initial audio buffer");
                    ++submitted_buffers;
                    require(stream.client->Start(), L"Start audio playback");
                    running = true;
                    signal_ready(true);
                    publish_state(AudioState::Playing, {AudioState::Starting});
                    const ULONGLONG healthy_since = GetTickCount64();
                    HANDLE events[]{stop_event.get(), changed_event.get(), sample_event.get()};
                    while (!cancelled(stop_event.get())) {
                        const DWORD result = WaitForMultipleObjects(3, events, FALSE, 3000);
                        if (result == WAIT_OBJECT_0) break;
                        if (result == WAIT_OBJECT_0 + 1) { device_changed = true; break; }
                        if (result == WAIT_TIMEOUT)
                            throw std::wstring(L"Windows has not provided an audio buffer for 3 seconds.");
                        if (result != WAIT_OBJECT_0 + 2)
                            throw hresult_text(L"Wait for WASAPI", HRESULT_FROM_WIN32(GetLastError()));
                        UINT32 padding = 0;
                        require(stream.client->GetCurrentPadding(&padding), L"Audio buffer state");
                        if (padding > stream.capacity) throw std::wstring(L"Inconsistent audio buffer size.");
                        const UINT32 available = stream.capacity - padding;
                        if (!available) continue;
                        require(stream.render->GetBuffer(available, &buffer), L"Get audio buffer");
                        source.fill(buffer, available);
                        require(stream.render->ReleaseBuffer(available, 0), L"Submit audio buffer");
                        ++submitted_buffers;
                        // Only a stable stream resets consecutive failures. Merely
                        // starting must not create endless retries on a broken driver.
                        if (GetTickCount64() - healthy_since >= 30000) attempts = 0;
                    }
                } catch (const std::wstring& failure) { last_failure = failure; }
                catch (...) { last_failure = L"Internal error during audio playback."; }
                running = false;
                close_stream(stream);
                if (cancelled(stop_event.get())) break;
                if (device_changed) {
                    set_message(L"Audio output changed: reopening the device.");
                    attempts = 0;
                } else if (++attempts >= 3) {
                    append_message(last_failure + L"; stopped after 3 attempts.");
                    break;
                } else set_message(last_failure + L"; retrying audio.");
                publish_state(AudioState::Starting, {AudioState::Playing});
                // Stop can interrupt retries; no CPU polling, no unbounded retry loop.
                if (WaitForSingleObject(stop_event.get(), 250) == WAIT_OBJECT_0) break;
            }
            for (unsigned attempt = 0; attempt < 3 && !pending_restores.empty(); ++attempt) {
                restore_pending(enumerator.get());
                if (!pending_restores.empty() && attempt < 2) {
                    // A separate timer avoids a signaled stop event turning this into a spin.
                    Handle retry(CreateWaitableTimerW(nullptr, TRUE, nullptr));
                    LARGE_INTEGER due{}; due.QuadPart = -1000000;
                    if (!retry.get() || !SetWaitableTimer(retry.get(), &due, 0, nullptr, nullptr, FALSE)) break;
                    WaitForSingleObject(retry.get(), 150);
                }
            }
            if (!pending_restores.empty())
                append_message(L"Audio state was not restored: device unavailable. Restoration will be retried when audio next starts.");
        } catch (const std::wstring& failure) {
            if (!cancelled(stop_event.get())) append_message(failure);
        } catch (const std::exception&) { append_message(L"Internal error or insufficient memory in the audio engine."); }
        catch (...) { append_message(L"Unexpected audio engine error."); }
        running = false;
        signal_ready(false);
        publish_state(cancelled(stop_event.get()) ? AudioState::Idle : AudioState::Error);
    }

    void stop_locked() {
        request_cancel();
        // Legacy stop/destruction deliberately wait; the UI uses cancel() and
        // status(). The handle remains owned until Windows reports termination.
        if (worker_handle) WaitForSingleObject(worker_handle, INFINITE);
        running = false;
        { std::lock_guard lock(state_mutex); if (worker_handle) CloseHandle(worker_handle); worker_handle = nullptr; worker_expected = false; }
    }
};

AudioPlayer::AudioPlayer() : impl_(std::make_unique<Impl>()) {}
AudioPlayer::~AudioPlayer() { set_state_callback({}); stop(); }

bool AudioPlayer::start(const AudioOptions& options, std::wstring& error) {
    std::lock_guard operation(impl_->operation_mutex);
    impl_->stop_locked();
    error.clear();
    if (!impl_->validate(options, error)) {
        impl_->set_message(error); impl_->publish_state(AudioState::Error);
        return false;
    }
    if (!impl_->launch(options, error)) return false;
    std::unique_lock state(impl_->state_mutex);
    impl_->initialized.wait(state, [&] { return impl_->ready; });
    error = impl_->message;
    return impl_->start_result;
}

void AudioPlayer::stop() {
    // Signal before waiting for serialization so a concurrent file load/setup
    // can observe cancellation while start() is waiting for its worker.
    impl_->request_cancel();
    std::lock_guard operation(impl_->operation_mutex);
    impl_->stop_locked();
    impl_->publish_state(AudioState::Idle);
}
bool AudioPlayer::begin_start(const AudioOptions& options, std::wstring& error) {
    error.clear();
    std::unique_lock operation(impl_->operation_mutex, std::try_to_lock);
    if (!operation.owns_lock()) { error = L"An audio operation is already in progress; retry after cleanup."; return false; }
    if (!impl_->reap_finished()) {
        error = L"The previous audio worker has not finished; check its status before retrying."; return false;
    }
    if (!impl_->validate(options, error)) return false;
    return impl_->launch(options, error);
}
AudioStatus AudioPlayer::status() const {
    std::lock_guard lock(impl_->state_mutex);
    AudioStatus result;
    result.state = impl_->phase; result.generation = impl_->generation; result.message = impl_->message;
    result.initialized = impl_->ready; result.start_succeeded = impl_->start_result;
    result.finished = impl_->thread_finished_locked();
    impl_->notification_pending = false;
    return result;
}
void AudioPlayer::cancel() { impl_->request_cancel(); }
void AudioPlayer::set_state_callback(std::function<void()> callback) {
    std::lock_guard lock(impl_->state_mutex);
    impl_->state_callback = std::move(callback); impl_->notification_pending = false;
}
void AudioPlayer::set_preparation_gate_for_testing(void* event, bool cancelable) {
    std::lock_guard operation(impl_->operation_mutex);
    if (!impl_->reap_finished()) throw std::logic_error("Preparation gate requires an idle worker");
    HANDLE duplicate = nullptr;
    if (event && !DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &duplicate,
                                 0, FALSE, DUPLICATE_SAME_ACCESS))
        throw std::runtime_error("Cannot duplicate preparation gate");
    impl_->preparation_gate.reset(duplicate); impl_->preparation_cancelable = cancelable;
}
bool AudioPlayer::playing() const { return impl_->running.load(); }
std::wstring AudioPlayer::error() const {
    std::lock_guard lock(impl_->state_mutex);
    return impl_->message;
}

std::vector<AudioDevice> AudioPlayer::devices(std::wstring& error) {
    error.clear();
    std::vector<AudioDevice> outputs;
    try {
        ComScope com;
        if (!com.ok()) throw hresult_text(L"Initialize audio COM", com.result());
        ComPtr<IMMDeviceEnumerator> enumerator;
        require(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                 __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator.out())),
                L"Enumerate Core Audio devices");
        ComPtr<IMMDeviceCollection> collection;
        require(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.out()),
                L"List active audio outputs");
        UINT count = 0;
        require(collection->GetCount(&count), L"Audio output count");
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            require(collection->Item(i, device.out()), L"Audio output");
            AudioDevice output{device_id(device.get()), L"Audio output"};
            ComPtr<IPropertyStore> properties;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, properties.out()))) {
                PROPVARIANT name{};
                PropVariantInit(&name);
                if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) &&
                    name.vt == VT_LPWSTR && name.pwszVal) output.name = name.pwszVal;
                PropVariantClear(&name);
            }
            outputs.push_back(std::move(output));
        }
        if (outputs.empty()) error = L"No active audio output was detected.";
    } catch (const std::wstring& failure) { error = failure; }
    catch (...) { error = L"Error while enumerating audio outputs."; }
    return outputs;
}

bool AudioPlayer::probe(std::wstring& report) {
    std::wstring issue;
    const auto outputs = devices(issue);
    std::wostringstream details;
    details << L"Active audio outputs: " << outputs.size() << L"\n";
    for (const auto& output : outputs) details << L"- " << output.name << L" [" << output.id << L"]\n";
    if (!issue.empty()) details << issue << L"\n";
    AudioPlayer player;
    AudioOptions options;
    options.session_volume = 0;
    bool success = player.start(options, issue);
    if (success) {
        // Confirm event-driven refills, not only Initialize/Start success. This
        // timer yields the CPU while the deliberately inaudible stream renders.
        Handle timer(CreateWaitableTimerW(nullptr, TRUE, nullptr));
        LARGE_INTEGER due{}; due.QuadPart = -2000000;
        if (!timer.get() || !SetWaitableTimer(timer.get(), &due, 0, nullptr, nullptr, FALSE) ||
            WaitForSingleObject(timer.get(), 350) != WAIT_OBJECT_0) {
            success = false;
            player.impl_->append_message(L"Cannot schedule the audio buffer check.");
        } else if (!player.playing() || player.impl_->submitted_buffers.load() < 2) {
            success = false;
            player.impl_->append_message(L"The stream started, but no event-driven audio buffer refill was confirmed.");
        }
    }
    player.stop();
    details << (success ? L"Shared WASAPI: silent stream started, event-driven buffers confirmed, then stopped.\n" : L"Shared WASAPI: failed.\n");
    const auto final_issue = player.error();
    if (!final_issue.empty()) details << final_issue << L"\n";
    details << L"System volume/mute unchanged. This test does not prove physical speaker audibility.";
    report = details.str();
    return success;
}

} // namespace alarmapp
