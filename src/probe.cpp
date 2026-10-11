// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#include "platform.h"
#include "audio.h"
#include <powrprof.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include <cmath>

namespace {
struct EndpointSnapshot { float volume=0; BOOL mute=FALSE; bool ok=false; std::wstring id; };
EndpointSnapshot snapshot(const std::wstring& id={}) {
    EndpointSnapshot s;
    IMMDeviceEnumerator* e=nullptr; IMMDevice* d=nullptr; IAudioEndpointVolume* v=nullptr;
    if(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&e))) &&
       SUCCEEDED(id.empty() ? e->GetDefaultAudioEndpoint(eRender,eMultimedia,&d) : e->GetDevice(id.c_str(),&d)) &&
       SUCCEEDED(d->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(&v)))) {
        s.ok=SUCCEEDED(v->GetMasterVolumeLevelScalar(&s.volume)) && SUCCEEDED(v->GetMute(&s.mute));
        LPWSTR endpoint_id=nullptr;
        if(SUCCEEDED(d->GetId(&endpoint_id))) { s.id=endpoint_id; CoTaskMemFree(endpoint_id); }
    }
    if(v) v->Release();
    if(d) d->Release();
    if(e) e->Release();
    return s;
}
}

int wmain(int argc,wchar_t** argv) {
    std::filesystem::path report;
    bool sound=false,restore_test=false;
    for(int i=1;i<argc;++i) {
        const std::wstring arg=argv[i];
        if(arg==L"--report" && i+1<argc) report=argv[++i];
        else if(arg==L"--audio-test") sound=true;
        else if(arg==L"--restore-test") { sound=true; restore_test=true; }
        else if(arg!=L"--no-audio") { std::wcerr << L"Usage: alarm_probe [--no-audio | --audio-test | --restore-test] [--report path]\n"; return 2; }
    }
    const HRESULT co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    std::wostringstream log;
    int failures=0;
    auto check=[&](bool ok,const std::wstring& message) { log << (ok ? L"PASS " : L"FAIL ") << message << L'\n'; if(!ok) ++failures; };
    SYSTEMTIME local{}; GetLocalTime(&local);
    log << L"ALARM PHASE 0 — API probes, not physical sleep validation\nLocal date: " << local.wYear << L'-' << local.wMonth << L'-' << local.wDay << L' ' << local.wHour << L':' << local.wMinute << L':' << local.wSecond << L"\n";
    log << alarmapp::wake_policy() << L"\n";
    SYSTEM_POWER_CAPABILITIES caps{};
    check(GetPwrCapabilities(&caps)!=FALSE,L"GetPwrCapabilities");
    log << L"S3=" << caps.SystemS3 << L" Hibernate=" << caps.SystemS4 << L" (Modern Standby: see powercfg /a)\n";
    try {
        alarmapp::WakeTimer timer; std::wstring status;
        const bool armed=timer.arm(alarmapp::Clock::now()+std::chrono::milliseconds(200),true,status);
        check(armed,L"SetWaitableTimer absolute UTC / fResume=TRUE : " + status);
        check(WaitForSingleObject(timer.handle(),2000)==WAIT_OBJECT_0,L"Waitable timer fired while awake");
        const bool rearmed=timer.arm(alarmapp::Clock::now()+std::chrono::seconds(30),true,status);
        timer.cancel();
        check(rearmed && WaitForSingleObject(timer.handle(),40)==WAIT_TIMEOUT,L"CancelWaitableTimer cancels pending wake");
    } catch(const std::exception& e) { check(false,L"Wake timer exception: " + std::wstring(e.what(),e.what()+std::char_traits<char>::length(e.what()))); }
    HWND hidden=CreateWindowExW(0,L"STATIC",L"ALARM power probe",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    HPOWERNOTIFY power=hidden ? RegisterSuspendResumeNotification(hidden,DEVICE_NOTIFY_WINDOW_HANDLE) : nullptr;
    check(power!=nullptr,L"RegisterSuspendResumeNotification HWND (real resume event pending)");
    if(power) UnregisterSuspendResumeNotification(power);
    if(hidden) DestroyWindow(hidden);
    std::wstring text;
    const bool tray=alarmapp::tray_probe(text); check(tray,text);
    const bool startup=alarmapp::startup_probe(text); check(startup,text);
    const bool audio=alarmapp::AudioPlayer::probe(text); check(audio,text);
    std::wstring device_error;
    const auto devices=alarmapp::AudioPlayer::devices(device_error);
    log << L"Active render endpoints: " << devices.size() << L'\n';
    for(const auto& device:devices) log << L"  " << device.name << L'\n';
    if(!device_error.empty()) log << device_error << L'\n';
    if(sound) {
        const auto before=snapshot();
        alarmapp::AudioOptions options;
        options.session_volume=.12f;
        options.override_endpoint=restore_test;
        options.device_id=before.id;
        options.endpoint_volume=before.ok ? std::min(before.volume,.15f) : .15f;
        alarmapp::AudioPlayer player;
        std::wstring error;
        const bool started=player.start(options,error);
        check(started,L"WASAPI tone started" + (error.empty()?L"":L" : " + error));
        if(started) {
            if(restore_test) {
                const auto during=snapshot(before.id);
                check(during.ok && std::abs(during.volume-options.endpoint_volume)<0.0001f && !during.mute,
                    L"Opt-in override applied to pinned endpoint (<=15%, unmuted)");
            }
            // A single blocking wait only in this short prototype, never an engine polling loop.
            Sleep(700);
            check(player.playing(),L"WASAPI render thread alive after 700 ms");
        }
        LARGE_INTEGER frequency{},a{},b{}; QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&a);
        player.stop(); QueryPerformanceCounter(&b);
        const double elapsed_ms=1000.*static_cast<double>(b.QuadPart-a.QuadPart)/static_cast<double>(frequency.QuadPart);
        check(!player.playing() && elapsed_ms<1000,L"STOP finished in " + std::to_wstring(elapsed_ms) + L" ms");
        if(!player.error().empty()) log << L"Audio status after STOP: " << player.error() << L'\n';
        const auto after=snapshot(before.id);
        check(before.ok && after.ok && std::abs(before.volume-after.volume)<0.0001f && before.mute==after.mute,
            restore_test ? L"Opt-in endpoint override restored (volume + mute)" : L"Endpoint volume + mute unchanged");
        log << L"Physical audibility and post-suspend playback still require human observation.\n";
    }
    log << L"Failures: " << failures << L"\n";
    const auto output=alarmapp::utf8(log.str());
    std::cout << output;
    if(!report.empty()) {
        std::ofstream file(report,std::ios::binary); file << output;
        if(!file) { std::cerr << "Cannot write report\n"; ++failures; }
    }
    if(SUCCEEDED(co)) CoUninitialize();
    return failures ? 1 : 0;
}
