// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#include "config.h"
#include "engine.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace std::chrono_literals;
using namespace alarmapp;

namespace {

int assertions = 0;
void require(bool condition, const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}

TimePoint local_date(int year, int month, int day, int hour, int minute, int dst = -1) {
    std::tm local{};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_isdst = dst;
    const auto value = std::mktime(&local);
    if (value == std::time_t{-1}) throw std::runtime_error("Test local date could not be converted");
    return Clock::from_time_t(value);
}

class TimezoneGuard {
public:
    explicit TimezoneGuard(const char* zone) {
        if (const auto* current = std::getenv("TZ")) { previous_ = current; had_previous_ = true; }
        set(zone);
    }
    ~TimezoneGuard() { set(had_previous_ ? previous_.c_str() : nullptr); }
private:
    static void set(const char* zone) {
#ifdef _WIN32
        _putenv_s("TZ", zone ? zone : "");
        _tzset();
#else
        if (zone) setenv("TZ", zone, 1); else unsetenv("TZ");
        tzset();
#endif
    }
    bool had_previous_ = false;
    std::string previous_;
};

void cycle_tests() {
    const auto start = TimePoint{std::chrono::seconds{1700000000}};
    Cycle cycle;
    require(!cycle.active() && cycle.state() == Cycle::State::Idle, "New cycle must be idle");
    require(cycle.next_transition() == TimePoint::max(), "Idle cycle has no deadline");
    require(!cycle.advance(start), "An idle cycle cannot restart itself");
    cycle.start(start);
    require(cycle.active() && cycle.state() == Cycle::State::Ringing, "Start must ring immediately");
    require(cycle.next_transition() == start + 5min, "First ringing deadline is exactly five minutes");
    require(!cycle.advance(start + 5min - 1ns), "Ringing lasts until the five-minute boundary");
    require(cycle.advance(start + 5min) && cycle.state() == Cycle::State::Silent,
        "The five-minute boundary starts silence");
    require(cycle.next_transition() == start + 15min, "Silence deadline is anchored to start");
    require(!cycle.advance(start + 15min - 1ns), "Silence lasts a full ten minutes");
    require(cycle.advance(start + 15min) && cycle.state() == Cycle::State::Ringing,
        "The fifteen-minute boundary restarts ringing");
    require(cycle.next_transition() == start + 20min, "Second ringing interval ends at minute twenty");
    require(cycle.advance(start + 20min) && cycle.state() == Cycle::State::Silent &&
        cycle.next_transition() == start + 30min,
        "Repeated cycle uses the same full ten-minute silence");
    require(cycle.advance(start + 30min) && cycle.state() == Cycle::State::Ringing &&
        cycle.next_transition() == start + 35min,
        "The third ring begins exactly at minute thirty");
    cycle.advance(start + 47min);
    require(cycle.state() == Cycle::State::Ringing && cycle.next_transition() == start + 50min,
        "Delayed resume computes the present ringing phase without accumulated drift");
    cycle.advance(start + 59min + 900ms);
    require(cycle.state() == Cycle::State::Silent && cycle.next_transition() == start + 60min,
        "Delayed resume skips missed transitions and preserves the present silent phase");
    cycle.advance(start + 24h + 7min);
    require(cycle.state() == Cycle::State::Silent && cycle.next_transition() == start + 24h + 15min,
        "Long delays use absolute cycle time");
    cycle.stop();
    require(!cycle.active() && cycle.state() == Cycle::State::Idle, "STOP returns to idle immediately");
    require(cycle.next_transition() == TimePoint::max(), "STOP cancels the deadline");
    require(!cycle.advance(start + 48h), "STOP remains stopped after a late callback");
    cycle.start(start + 48h);
    cycle.advance(start + 48h - 1h);
    require(cycle.state() == Cycle::State::Ringing && cycle.next_transition() == start + 48h + 5min,
        "A backward clock change does not create a negative cycle");

    require(!alarm_due(start, start - 1ns), "A future alarm is not due");
    require(alarm_due(start, start), "An exactly due alarm is accepted");
    require(alarm_due(start, start + 24h), "Catch-up includes its twenty-four-hour boundary");
    require(!alarm_due(start, start + 24h + 1ns), "An older missed alarm is not caught up");
    require(!alarm_due(start, start, -1s), "Negative catch-up duration is rejected");
    cycle.start(start + 3h);
    require(cycle.state() == Cycle::State::Ringing && cycle.next_transition() == start + 3h + 5min,
        "A missed alarm begins a fresh ringing cycle at resume time");
    Cycle accelerated(200ms, 300ms);
    accelerated.start(start);
    require(accelerated.next_transition() == start + 200ms,
        "Explicit accelerated test uses its requested ringing duration");
    require(accelerated.advance(start + 200ms) && accelerated.state() == Cycle::State::Silent &&
        accelerated.next_transition() == start + 500ms,
        "Accelerated test retains the silent boundary semantics");
    require(accelerated.advance(start + 500ms) && accelerated.state() == Cycle::State::Ringing &&
        accelerated.next_transition() == start + 700ms,
        "Accelerated test retains repeated ringing and anchored deadlines");
    bool invalid_duration = false;
    try { Cycle invalid(0s, 1s); } catch (const std::invalid_argument&) { invalid_duration = true; }
    require(invalid_duration, "Zero cycle durations are rejected");
    invalid_duration = false;
    try { Cycle invalid(1s, -1s); } catch (const std::invalid_argument&) { invalid_duration = true; }
    require(invalid_duration, "Negative cycle durations are rejected");
    invalid_duration = false;
    try { Cycle invalid(Clock::duration::max(), 1s); }
    catch (const std::invalid_argument&) { invalid_duration = true; }
    require(invalid_duration, "Overflowing cycle duration sums are rejected");
    std::cout << "PASS: 5/10-minute boundaries, STOP, delayed resume, bounded catch-up\n";
}

void scheduling_tests() {
    TimezoneGuard zone("EST5EDT");
    require(next_daily_alarm(local_date(2026, 9, 30, 5, 59), 6, 0) == local_date(2026, 9, 30, 6, 0),
        "Default 06:00 is scheduled on the current local date when future");
    require(next_daily_alarm(local_date(2026, 9, 30, 6, 0), 6, 0) == local_date(2026, 10, 1, 6, 0),
        "An occurrence equal to now is consumed; month rollovers use a civil day");
    require(next_daily_alarm(local_date(2026, 12, 31, 23, 59), 6, 0) == local_date(2027, 1, 1, 6, 0),
        "Scheduling crosses the year boundary");
    require(next_daily_alarm(local_date(2028, 2, 28, 7, 0), 6, 0) == local_date(2028, 2, 29, 6, 0),
        "Scheduling includes the leap day");
    const auto spring_before = local_date(2026, 3, 7, 6, 0);
    require(next_daily_alarm(spring_before, 6, 0) - spring_before == 23h,
        "Spring next local day is twenty-three hours rather than a fixed twenty-four");
    const auto autumn_before = local_date(2026, 10, 31, 6, 0);
    require(next_daily_alarm(autumn_before, 6, 0) - autumn_before == 25h,
        "Autumn next local day is twenty-five hours rather than a fixed twenty-four");
    require(next_daily_alarm(local_date(2026, 3, 8, 1, 50), 2, 30) == local_date(2026, 3, 8, 3, 0),
        "A skipped spring time moves to the first valid minute");
    require(next_daily_alarm(local_date(2026, 11, 1, 1, 0, 1), 1, 30) == local_date(2026, 11, 1, 1, 30, 1),
        "A repeated autumn time uses its earlier occurrence");
    require(next_daily_alarm(local_date(2026, 11, 1, 1, 45, 1), 1, 30) == local_date(2026, 11, 2, 1, 30),
        "A repeated autumn hour does not schedule the daily alarm twice");
    bool threw = false;
    try { (void)next_daily_alarm(local_date(2026, 9, 30, 0, 0), 24, 0); }
    catch (const std::invalid_argument&) { threw = true; }
    require(threw, "Invalid schedule hour is rejected");
    threw = false;
    try { (void)next_daily_alarm(local_date(2026, 9, 30, 0, 0), 6, -1); }
    catch (const std::invalid_argument&) { threw = true; }
    require(threw, "Invalid schedule minute is rejected");

    const auto fixed_now = local_date(2026, 9, 30, 5, 0);
    const auto eastern = next_daily_alarm(fixed_now, 6, 0);
    {
        TimezoneGuard changed("UTC0");
        require(next_daily_alarm(fixed_now, 6, 0) != eastern,
            "A fresh daily scheduling call respects a changed local timezone");
    }
    std::cout << "PASS: local dates, DST gap/fold, timezone changes, invalid times\n";
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void write_bytes(const std::filesystem::path& path, const std::string& content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << content;
    if (!stream) throw std::runtime_error("Cannot write test fixture");
}

bool equal_config(const Config& a, const Config& b) {
    return a.enabled == b.enabled && a.hour == b.hour && a.minute == b.minute &&
        a.device_id == b.device_id && a.wav_path == b.wav_path &&
        a.session_volume == b.session_volume && a.override_endpoint == b.override_endpoint &&
        a.endpoint_volume == b.endpoint_volume && a.startup == b.startup;
}

class TestDirectory {
public:
    TestDirectory() {
        root = std::filesystem::temp_directory_path() /
            ("alarm_core_tests_" + std::to_string(Clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("Test directory already exists");
    }
    ~TestDirectory() {
        // root is exclusively a directory created above under the system temp directory.
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root;
};

void config_tests() {
    TestDirectory fixture;
    const auto path = fixture.root / "nested" / "config.ini";
    Config untouched;
    untouched.hour = 17;
    std::wstring error;
    require(load_config(path, untouched, error) == ConfigLoadResult::Missing && untouched.hour == 17,
        "A missing configuration leaves caller defaults unchanged");
    require(!Config{}.enabled && Config{}.hour == 6 && Config{}.minute == 0,
        "First run is OFF with the default local time 06:00");
    Config saved;
    saved.enabled = true;
    saved.hour = 22;
    saved.minute = 37;
    saved.device_id = L"{endpoint}=Headphones \u00e9t\u00e9 \U0001f3a7\n";
    saved.wav_path = L"C:\\Music\\Wakeup \u00e9 \U0001f31e.wav";
    saved.session_volume = .12345679f;
    saved.override_endpoint = true;
    saved.endpoint_volume = .91f;
    saved.startup = true;
    require(save_config(path, saved, error) && error.empty(), "Saving creates its parent and configuration atomically");
    Config loaded;
    require(load_config(path, loaded, error) == ConfigLoadResult::Loaded && equal_config(saved, loaded),
        "All fields, UTF-16 characters and float precision survive round-trip");
    saved.hour = 5;
    saved.enabled = false;
    require(save_config(path, saved, error), "An existing valid configuration can be replaced");
    require(load_config(path, loaded, error) == ConfigLoadResult::Loaded && equal_config(saved, loaded),
        "Atomic replacement produces the complete new configuration");
    const auto valid = read_bytes(path);
    Config invalid = saved;
    invalid.hour = 24;
    require(!save_config(path, invalid, error) && read_bytes(path) == valid,
        "Invalid values cannot replace an existing valid file");
    invalid = saved;
    invalid.endpoint_volume = std::numeric_limits<float>::quiet_NaN();
    require(!save_config(path, invalid, error) && read_bytes(path) == valid,
        "NaN volume cannot be persisted");
    invalid = saved;
    invalid.wav_path.push_back(L'\0');
    invalid.wav_path += L"concealed-suffix.wav";
    require(!save_config(path, invalid, error) && read_bytes(path) == valid,
        "Embedded NUL cannot truncate a persisted WAV path at the Windows API boundary");
    invalid = saved;
    invalid.device_id.push_back(L'\0');
    invalid.device_id += L"concealed-endpoint";
    require(!save_config(path, invalid, error) && read_bytes(path) == valid,
        "Embedded NUL cannot truncate a persisted endpoint ID at the Windows API boundary");

    const auto reject = [&](std::string bytes, const char* message) {
        write_bytes(path, bytes);
        Config sentinel;
        sentinel.hour = 17;
        const auto status = load_config(path, sentinel, error);
        require(status == ConfigLoadResult::Invalid && sentinel.hour == 17 && !error.empty(), message);
        require(!save_config(path, saved, error) && read_bytes(path) == bytes,
            "Automatic save preserves malformed original bytes");
    };
    reject("broken\n", "A corrupt file is rejected without altering caller state");
    reject(valid + "hour=6\n", "Duplicate keys are rejected");
    reject(valid + "unexpected=1\n", "Unknown keys are rejected");
    auto modified = valid;
    auto position = modified.find("enabled=0");
    modified.replace(position, 9, "enabled=yes");
    reject(modified, "Noncanonical booleans are rejected");
    modified = valid;
    position = modified.find("session_volume=");
    const auto line_end = modified.find('\n', position);
    modified.replace(position, line_end - position, "session_volume=nan");
    reject(modified, "Non-finite parsed volumes are rejected");
    modified = valid;
    position = modified.find("device_id=");
    modified.replace(position, modified.find('\n', position) - position, "device_id=c080");
    reject(modified, "Overlong malformed UTF-8 is rejected");
    modified = valid;
    position = modified.find("wav_path=");
    modified.insert(modified.find('\n', position), "00");
    reject(modified, "A decoded NUL in a WAV path is rejected without replacing its original file");
    modified = valid;
    position = modified.find("device_id=");
    modified.insert(modified.find('\n', position), "00");
    reject(modified, "A decoded NUL in an endpoint ID is rejected without replacing its original file");
    reject(std::string(64 * 1024 + 1, 'x'), "Oversized configurations are rejected");
    write_bytes(path, valid);
    const auto impossible = fixture.root / "directory_as_file";
    std::filesystem::create_directory(impossible);
    require(!save_config(impossible, saved, error) && !error.empty(),
        "An IO failure is reported rather than claimed as a saved configuration");
#ifdef _WIN32
    require(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != 0, "Set read-only test fixture");
    saved.minute = 11;
    const auto replacement = save_config(path, saved, error);
    const auto preserved = read_bytes(path);
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    require(!replacement && preserved == valid, "A failed atomic replacement preserves the original file");
#endif
    int temporary_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
        if (entry.path().filename().wstring().find(L".tmp.") != std::wstring::npos) ++temporary_files;
    }
    require(temporary_files == 0, "Successful and failed writes clean temporary siblings");
    std::cout << "PASS: configuration round-trip, validation, malformed preservation, atomic replacement\n";
}

} // namespace

int main() {
    try {
        cycle_tests();
        scheduling_tests();
        config_tests();
        std::cout << "All " << assertions << " assertions passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << assertions << " assertions: " << error.what() << '\n';
        return 1;
    }
}
