// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#include "audio.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(__MINGW32__) && !defined(__IAudioMeterInformation_INTERFACE_DEFINED__)
// This MinGW SDK supplies only a forward declaration. Match the documented
// Windows COM ABI; the diagnostic still uses the native Windows implementation.
struct IAudioMeterInformation : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* channels) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT channels, float* values) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* support) = 0;
};
#endif

namespace {

constexpr GUID kMeterInterface{0xc02216f6, 0x8c67, 0x4b5b,
                              {0x9d, 0, 0xd0, 0x08, 0xe7, 0x3e, 0, 0x64}};

template<class T> class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : pointer_(std::exchange(other.pointer_, nullptr)) {}
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) { reset(); pointer_ = std::exchange(other.pointer_, nullptr); }
        return *this;
    }
    T* operator->() const { return pointer_; }
    T* get() const { return pointer_; }
    T** out() { reset(); return &pointer_; }
private:
    void reset() { if (pointer_) pointer_->Release(); pointer_ = nullptr; }
    T* pointer_ = nullptr;
};

class ComScope {
public:
    ComScope() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {
        if (FAILED(result_) && result_ != RPC_E_CHANGED_MODE)
            throw std::runtime_error("COM initialization failed");
    }
    ~ComScope() { if (SUCCEEDED(result_)) CoUninitialize(); }
private:
    HRESULT result_;
};

class Timer {
public:
    Timer() : handle_(CreateWaitableTimerW(nullptr, FALSE, nullptr)) {
        LARGE_INTEGER due{}; due.QuadPart = -1000000;
        if (!handle_ || !SetWaitableTimer(handle_, &due, 100, nullptr, nullptr, FALSE)) {
            if (handle_) CloseHandle(handle_);
            handle_ = nullptr;
            throw std::runtime_error("Diagnostic sampling timer failed");
        }
    }
    ~Timer() { if (handle_) { CancelWaitableTimer(handle_); CloseHandle(handle_); } }
    void wait() const {
        if (WaitForSingleObject(handle_, 1000) != WAIT_OBJECT_0)
            throw std::runtime_error("Diagnostic sampling timer stalled");
    }
private:
    HANDLE handle_;
};

std::wstring hr_text(const wchar_t* action, HRESULT result) {
    std::wostringstream text;
    text << action << L" (HRESULT 0x" << std::hex << std::uppercase
         << std::setw(8) << std::setfill(L'0') << static_cast<unsigned long>(result) << L")";
    return text.str();
}
void require(HRESULT result, const wchar_t* action) {
    if (FAILED(result)) throw hr_text(action, result);
}

std::string utf8(const std::wstring& text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

struct EndpointState {
    float scalar = 0;
    float decibels = 0;
    BOOL mute = FALSE;
};
EndpointState read_endpoint(IAudioEndpointVolume* volume) {
    EndpointState state;
    require(volume->GetMasterVolumeLevelScalar(&state.scalar), L"Read endpoint master volume");
    require(volume->GetMasterVolumeLevel(&state.decibels), L"Read endpoint master decibels");
    require(volume->GetMute(&state.mute), L"Read endpoint mute");
    return state;
}
void show_endpoint(std::wostream& output, const wchar_t* phase, const EndpointState& state) {
    output << phase << L": endpoint_master=" << state.scalar << L" (" << state.decibels
           << L" dB), endpoint_mute=" << (state.mute ? L"true" : L"false") << L"\n";
}

struct Peaks {
    float maximum = 0;
    unsigned samples = 0;
    unsigned nonzero = 0;
    unsigned errors = 0;
    float record(IAudioMeterInformation* meter) {
        float value = 0;
        if (!meter || FAILED(meter->GetPeakValue(&value)) || !std::isfinite(value)) {
            ++errors;
            return 0;
        }
        ++samples;
        if (value > 0.000001f) ++nonzero;
        maximum = std::max(maximum, value);
        return value;
    }
};
void show_peaks(std::wostream& output, const wchar_t* phase, const Peaks& peaks) {
    output << phase << L": meter_peak_max=" << peaks.maximum << L", nonzero_samples="
           << peaks.nonzero << L"/" << peaks.samples << L", read_errors=" << peaks.errors << L"\n";
}

ComPtr<IAudioSessionControl> find_own_session(IAudioSessionManager2* manager) {
    ComPtr<IAudioSessionEnumerator> sessions;
    require(manager->GetSessionEnumerator(sessions.out()), L"Enumerate diagnostic audio session");
    int count = 0;
    require(sessions->GetCount(&count), L"Count audio sessions");
    for (int index = 0; index < count; ++index) {
        ComPtr<IAudioSessionControl> control;
        ComPtr<IAudioSessionControl2> information;
        if (FAILED(sessions->GetSession(index, control.out())) ||
            FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                           reinterpret_cast<void**>(information.out())))) continue;
        DWORD pid = 0;
        if (SUCCEEDED(information->GetProcessId(&pid)) && pid == GetCurrentProcessId()) return control;
    }
    return {};
}

struct SessionState {
    bool found = false;
    bool readable = false;
    float gain = 0;
    BOOL mute = FALSE;
    AudioSessionState state = AudioSessionStateExpired;
};
SessionState read_session(IAudioSessionControl* control) {
    SessionState result;
    if (!control) return result;
    result.found = true;
    ComPtr<ISimpleAudioVolume> volume;
    if (SUCCEEDED(control->GetState(&result.state)) &&
        SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(volume.out()))) &&
        SUCCEEDED(volume->GetMasterVolume(&result.gain)) && SUCCEEDED(volume->GetMute(&result.mute)))
        result.readable = true;
    return result;
}
void show_session(std::wostream& output, const wchar_t* phase, const SessionState& state) {
    output << phase << L": own_session_found=" << (state.found ? L"true" : L"false");
    if (state.readable) {
        const wchar_t* name = state.state == AudioSessionStateActive ? L"active" :
            state.state == AudioSessionStateInactive ? L"inactive" : L"expired";
        output << L", state=" << name << L", session_gain=" << state.gain
               << L", session_mute=" << (state.mute ? L"true" : L"false");
    } else if (state.found) output << L", state/gain/mute=unavailable";
    output << L"\n";
}

float parse_level(const wchar_t* text) {
    wchar_t* end = nullptr;
    const float value = std::wcstof(text, &end);
    if (!end || end == text || *end || !std::isfinite(value) || value < 0 || value > 1)
        throw std::runtime_error("Gain and override must be finite values between 0 and 1");
    return value;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    alarmapp::AudioOptions options;
    options.session_volume = 0.5f;
    std::filesystem::path report;
    try {
        for (int index = 1; index < argc; ++index) {
            const std::wstring argument = argv[index];
            if (argument == L"--gain" && index + 1 < argc) options.session_volume = parse_level(argv[++index]);
            else if (argument == L"--override" && index + 1 < argc) {
                options.override_endpoint = true;
                options.endpoint_volume = parse_level(argv[++index]);
            } else if (argument == L"--device" && index + 1 < argc) options.device_id = argv[++index];
            else if (argument == L"--report" && index + 1 < argc) report = argv[++index];
            else throw std::runtime_error("Usage: alarm_audio_diagnostic [--gain .5|.75] [--override .5] [--device endpoint-ID] [--report path]");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }

    std::wostringstream log;
    log << std::fixed << std::setprecision(6);
    int failures = 0;
    try {
        ComScope com;
        ComPtr<IMMDeviceEnumerator> enumerator;
        require(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                  __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator.out())),
                L"Create endpoint enumerator");
        ComPtr<IMMDevice> device;
        if (options.device_id.empty())
            require(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.out()),
                    L"Select Windows multimedia default output");
        else require(enumerator->GetDevice(options.device_id.c_str(), device.out()), L"Select explicit output");
        wchar_t* id = nullptr;
        require(device->GetId(&id), L"Read selected endpoint ID");
        options.device_id = id;
        CoTaskMemFree(id);
        std::wstring friendly_name = L"Unknown output";
        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, properties.out()))) {
            PROPVARIANT name{};
            PropVariantInit(&name);
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) &&
                name.vt == VT_LPWSTR && name.pwszVal) friendly_name = name.pwszVal;
            PropVariantClear(&name);
        }
        ComPtr<IAudioEndpointVolume> endpoint;
        ComPtr<IAudioMeterInformation> meter;
        require(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(endpoint.out())), L"Read-only endpoint volume access");
        require(device->Activate(kMeterInterface, CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(meter.out())), L"Read-only endpoint peak meter access");
        log << L"ALARM audio diagnostic: five-second built-in tone\nEndpoint: " << friendly_name
            << L"\nPinned endpoint ID: " << options.device_id << L"\nRequested session gain: "
            << options.session_volume << L"\nEndpoint override: "
            << (options.override_endpoint ? L"explicit opt-in" : L"disabled (master/mute preserved)") << L"\n";
        if (options.override_endpoint) log << L"Requested temporary endpoint level: " << options.endpoint_volume << L"\n";
        DWORD support = 0;
        if (SUCCEEDED(meter->QueryHardwareSupport(&support)))
            log << L"Endpoint hardware peak meter: " << ((support & ENDPOINT_HARDWARE_SUPPORT_METER) ? L"yes" : L"no (Windows software meter)") << L"\n";
        const auto before = read_endpoint(endpoint.get());
        show_endpoint(log, L"BEFORE", before);
        Timer timer;
        Peaks baseline;
        for (unsigned sample = 0; sample < 5; ++sample) { timer.wait(); baseline.record(meter.get()); }
        show_peaks(log, L"BEFORE", baseline);
        std::cout << "Playing a five-second diagnostic tone on " << utf8(friendly_name)
                  << " at session gain " << options.session_volume << "; endpoint master "
                  << before.scalar << ", mute " << (before.mute ? "true" : "false") << ".\n" << std::flush;
        alarmapp::AudioPlayer player;
        std::wstring status;
        const bool started = player.start(options, status);
        log << L"AudioPlayer.start: " << (started ? L"true" : L"false") << L"\n";
        if (!status.empty()) log << L"Audio status: " << status << L"\n";
        Peaks during, session_peaks;
        ComPtr<IAudioSessionManager2> manager;
        ComPtr<IAudioSessionControl> own_session;
        ComPtr<IAudioMeterInformation> session_meter;
        SessionState running_session;
        const HRESULT manager_result = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                nullptr, reinterpret_cast<void**>(manager.out()));
        if (FAILED(manager_result)) log << hr_text(L"Session manager unavailable", manager_result) << L"\n";
        if (started) {
            show_endpoint(log, L"DURING_START", read_endpoint(endpoint.get()));
            const ULONGLONG began = GetTickCount64();
            unsigned samples = 0;
            while (GetTickCount64() - began < 5000 && player.playing()) {
                timer.wait();
                const float value = during.record(meter.get());
                if (!own_session.get() && manager.get() && samples % 5 == 0) {
                    own_session = find_own_session(manager.get());
                    if (own_session.get()) {
                        running_session = read_session(own_session.get());
                        show_session(log, L"DURING", running_session);
                        const HRESULT result = own_session->QueryInterface(kMeterInterface,
                                                       reinterpret_cast<void**>(session_meter.out()));
                        if (FAILED(result)) log << hr_text(L"Optional session peak meter unavailable", result) << L"\n";
                    }
                }
                const float session_value = session_meter.get() ? session_peaks.record(session_meter.get()) : 0;
                if (++samples % 5 == 0)
                    log << L"DURING_SAMPLE: elapsed_ms=" << GetTickCount64() - began
                        << L", endpoint_peak=" << value << L", own_session_peak=" << session_value << L"\n";
            }
            log << L"Player still running before STOP: " << (player.playing() ? L"true" : L"false") << L"\n";
            if (!player.playing()) ++failures;
            show_endpoint(log, L"DURING_END", read_endpoint(endpoint.get()));
            if (own_session.get()) running_session = read_session(own_session.get());
            show_session(log, L"DURING_END", running_session);
        } else ++failures;
        const ULONGLONG stopped_at = GetTickCount64();
        player.stop();
        log << L"STOP: playing=" << (player.playing() ? L"true" : L"false")
            << L", elapsed_ms=" << GetTickCount64() - stopped_at << L"\n";
        if (!player.error().empty()) log << L"Audio status after STOP: " << player.error() << L"\n";
        const auto after = read_endpoint(endpoint.get());
        show_endpoint(log, L"AFTER", after);
        Peaks after_peaks;
        for (unsigned sample = 0; sample < 3; ++sample) { timer.wait(); after_peaks.record(meter.get()); }
        show_peaks(log, L"DURING_ENDPOINT", during);
        if (session_meter.get()) show_peaks(log, L"DURING_OWN_SESSION", session_peaks);
        show_peaks(log, L"AFTER", after_peaks);
        if (own_session.get()) show_session(log, L"AFTER", read_session(own_session.get()));
        const bool restored = std::fabs(before.scalar - after.scalar) < 0.000001f && before.mute == after.mute;
        log << L"Selected endpoint master/mute " << (options.override_endpoint ? L"restored" : L"unchanged")
            << L": " << (restored ? L"true" : L"false") << L"\n";
        if (!restored) ++failures;
        const bool gain_verified = running_session.readable && !running_session.mute &&
            running_session.state == AudioSessionStateActive && std::fabs(running_session.gain - options.session_volume) < 0.000001f;
        log << L"Requested active/unmuted session gain verified: " << (gain_verified ? L"true" : L"false") << L"\n";
        if (!gain_verified) ++failures;
        if (options.session_volume > 0 && during.maximum <= 0.000001f) {
            log << L"FAIL: Windows endpoint meter remained zero during the playing tone.\n";
            ++failures;
        }
        if (session_meter.get() && options.session_volume > 0 && session_peaks.maximum <= 0.000001f) {
            log << L"FAIL: The diagnostic process's session meter remained zero.\n";
            ++failures;
        }
        log << L"Endpoint meter measures the Windows output mix before endpoint attenuation; other applications can contribute.\n"
            << L"Nonzero metering confirms a Windows audio signal, not physical speaker audibility. No audio recording was made.\n";
    } catch (const std::wstring& error) { log << L"FAIL: " << error << L"\n"; ++failures; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; log << L"FAIL: diagnostic exception\n"; ++failures; }
    log << L"Failures: " << failures << L"\n";
    const std::string output = utf8(log.str());
    // Emit the complete report only after STOP; saved/redirected output is
    // self-contained without duplicating the progress introduction's full log.
    std::cout << output;
    if (!report.empty()) {
        std::ofstream file(report, std::ios::binary | std::ios::trunc);
        file << output;
        file.close();
        if (!file) { std::cerr << "Cannot save diagnostic report\n"; ++failures; }
    }
    return failures ? 1 : 0;
}
