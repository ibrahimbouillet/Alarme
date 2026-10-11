// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#include "elapsed_cycle.h"
#include <iostream>
#include <stdexcept>
using namespace alarmapp;
using namespace std::chrono_literals;
int main() {
    try {
        auto require=[](bool value,const char* text) { if(!value) throw std::runtime_error(text); };
        const auto wall=TimePoint{1700000000s};
        constexpr std::uint64_t base=100000;
        ElapsedCycle cycle;
        cycle.start(wall,base);
        require(cycle.state()==Cycle::State::Ringing,"start rings");
        cycle.advance(base+60000);
        require(cycle.state()==Cycle::State::Ringing,"one minute rings");
        require(cycle.next_transition(wall+1h,base+60000)==wall+1h+4min,"forward wall jump preserves four remaining minutes");
        require(cycle.next_transition(wall-1h,base+60000)==wall-1h+4min,"backward wall jump preserves four remaining minutes");
        cycle.advance(base+300000);
        require(cycle.state()==Cycle::State::Silent,"exact 300 seconds starts silence");
        require(cycle.next_transition(wall+3h,base+300000)==wall+3h+10min,"ten minutes silence after arbitrary wall correction");
        cycle.advance(base+900000);
        require(cycle.state()==Cycle::State::Ringing,"exact 900 seconds starts second ring");
        cycle.advance(base+1200000);
        require(cycle.state()==Cycle::State::Silent,"second ring is exactly five minutes");
        cycle.advance(base+47*60000);
        require(cycle.state()==Cycle::State::Ringing,"resume after long sleep chooses present phase");
        require(cycle.next_transition(wall+24h,base+47*60000)==wall+24h+3min,"resume deadline uses current wall plus remaining elapsed duration");
        cycle.stop();
        require(!cycle.active() && cycle.next_transition(wall,base)==TimePoint::max(),"STOP cancels deadline");
        require(!cycle.advance(base+48*3600000ULL),"STOP remains stopped after resume");
        std::cout << "PASS elapsed durations, forward/backward clock corrections, suspend catch-up, STOP (12 assertions)\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
