// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#include "engine.h"

#include <algorithm>
#include <ctime>
#include <optional>
#include <stdexcept>

namespace alarmapp {

Cycle::Cycle(Clock::duration ringing, Clock::duration silent)
    : ringing_(ringing), silent_(silent) {
    if (ringing <= Clock::duration::zero() || silent <= Clock::duration::zero() ||
        ringing > Clock::duration::max() - silent) {
        throw std::invalid_argument("Cycle durations must be positive and their sum representable");
    }
}

void Cycle::start(TimePoint now) noexcept {
    started_ = now;
    state_ = State::Ringing;
    next_ = started_ + ringing_;
}

void Cycle::stop() noexcept {
    state_ = State::Idle;
    next_ = TimePoint::max();
}

bool Cycle::advance(TimePoint now) noexcept {
    if (!active()) return false;
    const auto old_state = state_;
    const auto elapsed = std::max(Clock::duration::zero(), now - started_);
    const auto period = ringing_ + silent_;
    const auto completed = elapsed / period;
    const auto cycle_start = started_ + completed * period;
    const auto phase = elapsed % period;
    if (phase < ringing_) {
        state_ = State::Ringing;
        next_ = cycle_start + ringing_;
    } else {
        state_ = State::Silent;
        next_ = cycle_start + period;
    }
    return state_ != old_state;
}

namespace {

bool local_time(std::time_t value, std::tm& out) {
#ifdef _WIN32
    return localtime_s(&out, &value) == 0;
#else
    return localtime_r(&value, &out) != nullptr;
#endif
}

bool same_minute(const std::tm& a, const std::tm& b) {
    return a.tm_year == b.tm_year && a.tm_mon == b.tm_mon &&
           a.tm_mday == b.tm_mday && a.tm_hour == b.tm_hour &&
           a.tm_min == b.tm_min && b.tm_sec == 0;
}

std::optional<std::time_t> first_occurrence(std::tm requested) {
    std::optional<std::time_t> earliest;
    // Probe both interpretations and validate the round trip. This avoids the
    // implementation-defined mktime choice in an autumn repeated hour.
    for (int dst : {-1, 0, 1}) {
        auto candidate = requested;
        candidate.tm_isdst = dst;
        const auto value = std::mktime(&candidate);
        std::tm round_trip{};
        if (value != std::time_t{-1} && local_time(value, round_trip) &&
            same_minute(requested, round_trip) && (!earliest || value < *earliest)) {
            earliest = value;
        }
    }
    return earliest;
}

} // namespace

TimePoint next_daily_alarm(TimePoint now, int hour, int minute) {
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        throw std::invalid_argument("Alarm time must be 00:00 through 23:59");
    }
#ifdef _WIN32
    _tzset();
#else
    tzset();
#endif
    std::tm date{};
    if (!local_time(Clock::to_time_t(now), date)) {
        throw std::runtime_error("Unable to convert the current local date");
    }

    for (int day = 0; day < 4; ++day) {
        auto requested = date;
        requested.tm_hour = hour;
        requested.tm_min = minute;
        requested.tm_sec = 0;
        auto occurrence = first_occurrence(requested);
        // A civil time inside a forward DST jump does not exist. Advance civil
        // minutes to the first real minute, without adding 24 absolute hours.
        for (int civil_minute = hour * 60 + minute + 1;
             !occurrence && civil_minute < 24 * 60; ++civil_minute) {
            requested.tm_hour = civil_minute / 60;
            requested.tm_min = civil_minute % 60;
            occurrence = first_occurrence(requested);
        }
        if (occurrence) {
            const auto result = Clock::from_time_t(*occurrence);
            if (result > now) return result;
        }

        // Noon avoids the usual DST transition hour. mktime normalizes the
        // calendar day and consults the timezone rules for that new date.
        auto following_day = date;
        ++following_day.tm_mday;
        following_day.tm_hour = 12;
        following_day.tm_min = 0;
        following_day.tm_sec = 0;
        following_day.tm_isdst = -1;
        const auto normalized = std::mktime(&following_day);
        if (normalized == std::time_t{-1} || !local_time(normalized, date)) {
            throw std::runtime_error("Unable to convert the next local date");
        }
    }
    throw std::runtime_error("No valid alarm occurrence in the next four local dates");
}

bool alarm_due(TimePoint scheduled, TimePoint now, Clock::duration max_lateness) noexcept {
    return max_lateness >= Clock::duration::zero() &&
           now >= scheduled && now - scheduled <= max_lateness;
}

} // namespace alarmapp
