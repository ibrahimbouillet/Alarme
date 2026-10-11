// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>

namespace alarmapp::liquid {
constexpr double fixed_step = 1.0 / 120.0;
constexpr double lifetime = 4.0;
constexpr double water_ior = 1.333;
constexpr std::size_t maximum_drops = 3;
struct Vec2 { double x = 0, y = 0; };
struct Vec3 { double x = 0, y = 0, z = 0; };
inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x+b.x,a.y+b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x-b.x,a.y-b.y}; }
inline Vec2 operator*(Vec2 a, double s) { return {a.x*s,a.y*s}; }
inline double length(Vec2 a) { return std::hypot(a.x,a.y); }
inline bool finite(Vec2 a) { return std::isfinite(a.x)&&std::isfinite(a.y); }
inline Vec3 normalized(Vec3 a) {
    const double n=std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z);
    return n>1e-12?Vec3{a.x/n,a.y/n,a.z/n}:Vec3{};
}
// incoming points toward the interface; normal points back into the incident
// medium. eta=n_incident/n_transmitted. No transmitted ray exists during TIR.
inline std::optional<Vec3> refract(Vec3 incoming,Vec3 normal,double eta) {
    incoming=normalized(incoming);normal=normalized(normal);
    if(!std::isfinite(eta)||eta<=0) throw std::invalid_argument("Invalid refraction index ratio");
    const double dot=incoming.x*normal.x+incoming.y*normal.y+incoming.z*normal.z;
    const double k=1-eta*eta*(1-dot*dot);
    if(k<0) return std::nullopt;
    const double f=eta*dot+std::sqrt(k);
    return Vec3{eta*incoming.x-f*normal.x,eta*incoming.y-f*normal.y,eta*incoming.z-f*normal.z};
}
inline double fresnel(double cosine,double first=1,double second=water_ior) {
    const double ratio=(first-second)/(first+second), base=ratio*ratio;
    return base+(1-base)*std::pow(1-std::clamp(cosine,0.0,1.0),5);
}
inline Vec3 light_direction(Vec2 mouse,Vec2 center,double height=240) {
    return normalized({mouse.x-center.x,mouse.y-center.y,height});
}
inline Vec2 shadow_offset(Vec3 light,double radius) {
    return {-light.x*radius*.65,-light.y*radius*.65};
}
enum class Phase { Preview,Dragging,Falling,Puddle };
struct Drop {
    bool alive=false;
    Phase phase=Phase::Preview;
    Vec2 position{},velocity{};
    double radius=64,stretch=1,stretch_velocity=0,angle=0,age=0,spread=1,puddle_age=0;
    // Impact spreads in both axes of the floor (x/z), while the vertical
    // thickness falls as spread^2. rx*ry*rz remains exactly radius^3.
    double radius_x() const {return radius/std::sqrt(stretch)*spread;}
    double radius_y() const {return radius*std::sqrt(stretch)/(spread*spread);}
    double radius_z() const {return radius*spread;}
    double extent_x() const {return std::hypot(radius_x()*std::cos(angle),radius_y()*std::sin(angle));}
    double extent_y() const {return std::hypot(radius_x()*std::sin(angle),radius_y()*std::cos(angle));}
    double opacity() const {return phase==Phase::Falling||phase==Phase::Puddle?std::clamp((lifetime-age)/.65,0.0,1.0):1;}
};
class World {
public:
    explicit World(double width=1200,double height=800) {bounds(width,height);}
    void bounds(double width,double height) {
        if(!std::isfinite(width)||!std::isfinite(height)||width<128||height<128)
            throw std::invalid_argument("Invalid liquid world bounds");
        width_=width;height_=height;
        for(auto& drop:drops_) {
            drop.radius=std::min(drop.radius,std::min(width_,height_)*.30);
            drop.spread=std::min(drop.spread,std::max(1.0,(width_-4)*std::sqrt(drop.stretch)/(2*drop.radius)));
        }
    }
    void clear() {drops_={};drag_=-1;accumulator_=0;}
    void seed(Vec2 point) {clear();spawn(point).phase=Phase::Preview;}
    void pointer(Vec2 point) {if(!finite(point))throw std::invalid_argument("Invalid pointer");pointer_=point;}
    bool hit(const Drop& drop,Vec2 point) const {
        const auto delta=point-drop.position;
        const auto c=std::cos(drop.angle),s=std::sin(drop.angle);
        const auto x=(delta.x*c+delta.y*s)/drop.radius_x(),y=(-delta.x*s+delta.y*c)/drop.radius_y();
        return drop.alive&&x*x+y*y<=1;
    }
    void begin_drag(Vec2 point) {
        pointer(point);
        if(drag_>=0) release();
        drag_=-1;
        for(std::size_t i=0;i<drops_.size();++i) if(hit(drops_[i],point))drag_=static_cast<int>(i);
        if(drag_<0) {
            auto& drop=spawn(point);
            drag_=static_cast<int>(&drop-drops_.data());
        }
        auto& drop=drops_[static_cast<std::size_t>(drag_)];
        if(drop.phase==Phase::Puddle){drop.spread=1;drop.stretch=1;drop.stretch_velocity=0;drop.angle=0;drop.puddle_age=0;}
        offset_=drop.position-point;drop.phase=Phase::Dragging;drop.age=0;drop.velocity={};
    }
    void drag_to(Vec2 point,double elapsed) {
        pointer(point);
        if(drag_<0)return;
        auto& drop=drops_[static_cast<std::size_t>(drag_)];
        const auto next=point+offset_,delta=next-drop.position;
        if(std::isfinite(elapsed)&&elapsed>1e-5&&elapsed<.25) {
            auto velocity=delta*(1/elapsed);
            const auto speed=length(velocity);
            if(speed>1400)velocity=velocity*(1400/speed);
            drop.velocity=drop.velocity*.65+velocity*.35;
        }
        drop.position=next;
    }
    void release() {
        if(drag_<0)return;
        auto& drop=drops_[static_cast<std::size_t>(drag_)];
        drop.phase=Phase::Falling;drop.age=0;
        drag_=-1;
    }
    bool dragging()const{return drag_>=0;}
    bool animated()const {
        return std::any_of(drops_.begin(),drops_.end(),[](const Drop& d){return d.alive&&d.phase!=Phase::Preview;});
    }
    const std::array<Drop,maximum_drops>& drops()const{return drops_;}
    Vec2 mouse()const{return pointer_;}
    std::size_t count()const {
        return static_cast<std::size_t>(std::count_if(drops_.begin(),drops_.end(),[](const Drop& d){return d.alive;}));
    }
    void advance(double elapsed) {
        if(!std::isfinite(elapsed)||elapsed<0)throw std::invalid_argument("Invalid elapsed time");
        // Lifetime uses the full real interval; integration has a bounded
        // catch-up budget so a delayed frame cannot enter an unbounded loop.
        for(auto& drop:drops_) if(drop.alive&&(drop.phase==Phase::Falling||drop.phase==Phase::Puddle)) {
            drop.age+=elapsed;if(drop.age+1e-10>=lifetime)drop.alive=false;
        }
        accumulator_+=std::min(elapsed,.25);
        while(accumulator_+1e-12>=fixed_step) {step();accumulator_=std::max(0.0,accumulator_-fixed_step);}
    }
private:
    std::array<Drop,maximum_drops> drops_{};
    Vec2 pointer_{},offset_{};
    double width_=1200,height_=800,accumulator_=0;
    int drag_=-1;
    Drop& spawn(Vec2 point) {
        if(!finite(point))throw std::invalid_argument("Invalid drop position");
        auto slot=std::find_if(drops_.begin(),drops_.end(),[](const Drop& drop){return !drop.alive;});
        if(slot==drops_.end()) slot=std::max_element(drops_.begin(),drops_.end(),[](const Drop& a,const Drop& b){return a.age<b.age;});
        *slot={};slot->alive=true;slot->position=point;slot->radius=std::min(64.0,std::min(width_,height_)*.30);
        return *slot;
    }
    void step() {
        for(auto& drop:drops_) if(drop.alive) {
            if(drop.phase==Phase::Preview)continue;
            if(drop.phase==Phase::Puddle) {
                drop.puddle_age+=fixed_step;
                drop.velocity.y=0;drop.velocity.x*=std::exp(-7*fixed_step);
                drop.position.x+=drop.velocity.x*fixed_step;
                drop.stretch+=(1-drop.stretch)*(1-std::exp(-12*fixed_step));
                drop.stretch_velocity=0;drop.angle=0;
                const double limit=std::max(1.0,(width_-4)*std::sqrt(drop.stretch)/(2*drop.radius));
                const double target=std::min(4.0,limit);
                drop.spread=std::min(limit,drop.spread+(target-drop.spread)*(1-std::exp(-5.5*fixed_step)));
                const double margin=drop.extent_x();
                drop.position.x=std::clamp(drop.position.x,margin,width_-margin);
                drop.position.y=height_-12-drop.extent_y();
                continue;
            }
            if(drop.phase==Phase::Falling) {
                drop.velocity.y+=980*fixed_step;
                drop.velocity.x*=std::exp(-.22*fixed_step);
                drop.position=drop.position+drop.velocity*fixed_step;
            }
            const double speed=length(drop.velocity);
            const double target=1+std::min(.7,speed*.00055);
            drop.stretch_velocity+=(42*(target-drop.stretch)-7.5*drop.stretch_velocity)*fixed_step;
            drop.stretch=std::clamp(drop.stretch+drop.stretch_velocity*fixed_step,.48,2.05);
            if(speed>25) {
                constexpr double pi=3.14159265358979323846;
                const double target_angle=std::atan2(drop.velocity.y,drop.velocity.x)-pi/2;
                const double delta=std::remainder(target_angle-drop.angle,2*pi);
                drop.angle+=delta*(1-std::exp(-7*fixed_step));
            }
            if(drop.phase==Phase::Falling) {
                // Collision uses the rotated ellipsoid's projected extents,
                // after deformation, so horizontal throws cannot clip edges.
                const double floor=height_-12-drop.extent_y();
                if(drop.position.y>floor) {
                    const double impact=std::max(0.0,drop.velocity.y);
                    drop.position.y=floor;
                    if(impact>0) {
                        drop.phase=Phase::Puddle;drop.puddle_age=0;drop.angle=0;
                        drop.velocity.y=0;drop.velocity.x*=.25;drop.stretch_velocity=0;
                        drop.position.y=height_-12-drop.extent_y();
                    }
                }
                const double margin=drop.extent_x();
                if(drop.position.x<margin){drop.position.x=margin;drop.velocity.x=std::abs(drop.velocity.x)*.35;}
                if(drop.position.x>width_-margin){drop.position.x=width_-margin;drop.velocity.x=-std::abs(drop.velocity.x)*.35;}
            }
        }
    }
};
}
