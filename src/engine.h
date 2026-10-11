// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#pragma once

#include <chrono>

namespace alarmapp {

using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

// Deadlines stay anchored to start(), including after sleep or a delayed callback.
class Cycle {
public:
    enum class State { Idle, Ringing, Silent };
    static constexpr auto ringing_duration = std::chrono::minutes{5};
    static constexpr auto silent_duration = std::chrono::minutes{10};

    // Alternate durations are for an explicit accelerated integration test.
    explicit Cycle(Clock::duration ringing = ringing_duration,
                   Clock::duration silent = silent_duration);

    void start(TimePoint now) noexcept;
    void stop() noexcept;
    bool advance(TimePoint now) noexcept;
    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] TimePoint next_transition() const noexcept { return next_; }
    [[nodiscard]] bool active() const noexcept { return state_ != State::Idle; }

private:
    State state_ = State::Idle;
    TimePoint started_{};
    TimePoint next_ = TimePoint::max();
    Clock::duration ringing_;
    Clock::duration silent_;
};

// Returns the next local-calendar occurrence strictly after now. A skipped DST
// time moves to the first valid minute that day; a repeated time occurs once,
// at its earlier occurrence. Recompute after a Windows time/zone change.
// Throws invalid_argument for an invalid HH:MM, runtime_error on conversion failure.
[[nodiscard]] TimePoint next_daily_alarm(TimePoint now, int hour, int minute);

// Caller consumes each scheduled deadline once, and starts a NEW cycle at now
// for a missed alarm. An already active cycle instead uses advance(now).
[[nodiscard]] bool alarm_due(
    TimePoint scheduled,
    TimePoint now,
    Clock::duration max_lateness = std::chrono::hours{24}) noexcept;

} // namespace alarmapp
