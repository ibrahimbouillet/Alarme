// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#pragma once
#include "engine.h"
#include <algorithm>
#include <cstdint>

namespace alarmapp {
// Uptime is supplied by GetTickCount64 on Windows. It includes suspended time
// and is independent of civil-clock changes. The pure adapter is testable
// without changing the host's clock or suspending it.
class ElapsedCycle {
public:
    using State = Cycle::State;
    explicit ElapsedCycle(Clock::duration ringing=std::chrono::minutes(5),
                          Clock::duration silent=std::chrono::minutes(10)) : cycle_(ringing,silent) {}
    void start(TimePoint wall_now,std::uint64_t uptime_ms) {
        anchor_=wall_now; uptime_origin_=uptime_ms; cycle_.start(anchor_);
    }
    void stop() { cycle_.stop(); }
    bool advance(std::uint64_t uptime_ms) { return cycle_.advance(synthetic_now(uptime_ms)); }
    State state() const { return cycle_.state(); }
    bool active() const { return cycle_.active(); }
    TimePoint next_transition(TimePoint wall_now,std::uint64_t uptime_ms) const {
        if(!active()) return TimePoint::max();
        const auto remaining=std::max(Clock::duration::zero(),cycle_.next_transition()-synthetic_now(uptime_ms));
        return wall_now+remaining;
    }
private:
    TimePoint synthetic_now(std::uint64_t uptime_ms) const {
        const auto elapsed=uptime_ms>=uptime_origin_ ? uptime_ms-uptime_origin_ : 0;
        return anchor_+std::chrono::milliseconds(elapsed);
    }
    Cycle cycle_;
    TimePoint anchor_{};
    std::uint64_t uptime_origin_=0;
};
}
