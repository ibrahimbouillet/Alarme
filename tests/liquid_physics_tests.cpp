// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Ibrahim Bouillet.
// ALARM was programmed by Ibrahim Bouillet.

#include "liquid_physics.h"
#include <iostream>
#include <stdexcept>

namespace {
using namespace alarmapp::liquid;
int assertions=0;
void require(bool value,const char* message){++assertions;if(!value)throw std::runtime_error(message);}
bool close(double a,double b,double tolerance=1e-8){return std::abs(a-b)<=tolerance;}
const Drop& first(const World& world){for(const auto& d:world.drops())if(d.alive)return d;throw std::runtime_error("No live drop");}
void physics() {
    World idle;idle.seed({400,250});idle.advance(20);
    require(first(idle).phase==Phase::Preview&&!idle.animated(),"Seed remains visible with no continuous animation");
    idle.begin_drag({400,250});idle.drag_to({440,280},.05);
    require(close(first(idle).position.x,440)&&close(first(idle).position.y,280),"Captured drag follows the pointer exactly");
    idle.advance(.5);require(first(idle).phase==Phase::Dragging,"Held drop does not expire");
    idle.release();require(first(idle).phase==Phase::Falling&&!idle.dragging(),"Release starts falling and ends capture state");
    idle.advance(3.4);require(idle.count()==1&&first(idle).opacity()<1,"Released drop fades near its four-second lifetime");
    idle.advance(.6);require(idle.count()==0&&!idle.animated(),"Exactly four seconds expires the released drop and rendering need");

    World sixty(2000,3000),one_twenty(2000,3000);
    sixty.begin_drag({500,200});sixty.release();one_twenty.begin_drag({500,200});one_twenty.release();
    for(int i=0;i<60;++i)sixty.advance(1.0/60);
    for(int i=0;i<120;++i)one_twenty.advance(1.0/120);
    require(close(first(sixty).position.y,first(one_twenty).position.y),"Fixed timestep produces matching 60/120 Hz trajectories");
    require(close(first(sixty).velocity.y,980),"One second of gravity adds 980 logical pixels/second");
    require(first(sixty).position.y>690&&first(sixty).position.y<700,"Free fall matches the fixed-step analytic trajectory");
    require(close(first(sixty).radius_x()*first(sixty).radius_y(),64*64),"Deformation conserves projected water area");

    World bounded(500,400);bounded.begin_drag({250,100});bounded.release();
    for(int i=0;i<420;++i){bounded.advance(1.0/120);if(bounded.count()){
        const auto& d=first(bounded);
        require(finite(d.position)&&finite(d.velocity)&&std::isfinite(d.stretch),"Floor impacts keep all physics finite");
        require(d.stretch>=.48&&d.stretch<=2.05&&close(d.radius_x()*d.radius_y()*d.radius_z(),64*64*64,1e-6),"Impact deformation remains bounded and volume-conserving");
    }}
    for(int i=0;i<12;++i){bounded.begin_drag({70+(i%3)*180.0,100});bounded.release();}
    require(bounded.count()==maximum_drops,"Rapid creation stays bounded at three drops");
    bounded.advance(30);require(!bounded.count(),"Delayed frame expires droplets without unbounded simulation catch-up");
    bounded.clear();require(!bounded.dragging()&&!bounded.animated(),"Clear cancels all capture and animation state");
    World small(128,128);small.begin_drag({64,40});small.drag_to({120,50},.04);small.release();
    for(int i=0;i<180;++i){small.advance(fixed_step);const auto& d=first(small);
        require(d.position.x>=d.extent_x()-1e-8&&d.position.x<=128-d.extent_x()+1e-8,"Rotated drops stay within even the smallest supported width");
        require(d.position.y+d.extent_y()<=116+1e-8,"Rotated drop extents respect the floor after deformation");
    }
    bool rejected=false;try{bounded.advance(std::numeric_limits<double>::quiet_NaN());}catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"Invalid elapsed time is rejected");
}
void puddles() {
    World water(900,500);water.begin_drag({450,120});water.release();
    for(int i=0;i<240&&first(water).phase!=Phase::Puddle;++i)water.advance(fixed_step);
    require(first(water).phase==Phase::Puddle,"Impact enters a fluid contact phase instead of bouncing");
    const double initial_width=first(water).radius_x(),initial_height=first(water).radius_y();
    double previous_width=initial_width,previous_height=initial_height;
    for(int i=0;i<120;++i) {
        water.advance(fixed_step);const auto& d=first(water);
        require(d.phase==Phase::Puddle&&close(d.velocity.y,0),"Water stays attached to the floor with no rebound");
        require(close(d.position.y+d.extent_y(),488),"Spreading meniscus remains on the contact plane");
        require(d.radius_x()>=previous_width-1e-8&&d.radius_y()<=previous_height+1e-8,"Contact spreads progressively while vertical thickness decreases");
        require(close(d.radius_x()*d.radius_y()*d.radius_z(),std::pow(d.radius,3),1e-6),"Spread conserves the complete 3D water volume");
        previous_width=d.radius_x();previous_height=d.radius_y();
    }
    require(first(water).radius_x()>initial_width*3&&first(water).radius_y()<initial_height*.12,"Impact makes a long thin puddle rather than a spherical ball");
    require(first(water).radius_x()/first(water).radius_y()>40,"Settled fluid has a visibly flat aspect ratio");
    const double remaining=lifetime-first(water).age;
    water.advance(remaining);
    require(water.count()==0,"Puddle expiration remains four seconds from release, not from impact");

    World sixty(900,500),one_twenty(900,500);
    sixty.begin_drag({450,120});sixty.release();one_twenty.begin_drag({450,120});one_twenty.release();
    for(int i=0;i<120;++i)sixty.advance(1.0/60);
    for(int i=0;i<240;++i)one_twenty.advance(fixed_step);
    require(first(sixty).phase==Phase::Puddle&&first(one_twenty).phase==Phase::Puddle,"Both frame rates reach the fluid contact phase");
    require(close(first(sixty).radius_x(),first(one_twenty).radius_x())&&close(first(sixty).radius_y(),first(one_twenty).radius_y()),"60/120 Hz fluid spread has identical dimensions");

    for(const double spawn_x:{8.0,892.0}) {
        World edge(900,500);edge.begin_drag({spawn_x,120});edge.release();
        for(int i=0;i<300;++i) {
            edge.advance(fixed_step);const auto& d=first(edge);
            require(d.position.x>=d.extent_x()-1e-8&&d.position.x+d.extent_x()<=900+1e-8,"Spreading water stays inside both work-area sides");
            require(d.position.y+d.extent_y()<=488+1e-8,"Side contact never pushes fluid below the floor");
        }
    }
}
void optics() {
    const auto normal=refract({0,0,-1},{0,0,1},1/water_ior);
    require(normal&&close(normal->x,0)&&close(normal->z,-1),"Normal incidence does not bend the ray");
    const double sine=std::sqrt(.5);
    const auto tilted=refract({sine,0,-sine},{0,0,1},1/water_ior);
    require(tilted&&close(tilted->x,sine/water_ior),"Air to water satisfies Snell's sine ratio");
    require(tilted&&close(std::sqrt(tilted->x*tilted->x+tilted->z*tilted->z),1),"Transmitted ray remains a unit vector");
    require(!refract({std::sqrt(.75),0,-.5},{0,0,1},water_ior),"Water to air above critical angle produces total internal reflection");
    const double f0=std::pow((1-water_ior)/(1+water_ior),2);
    require(close(fresnel(1),f0)&&f0>.02&&f0<.021,"Normal water Fresnel reflectance is approximately two percent");
    require(close(fresnel(0),1)&&fresnel(.3)>fresnel(.8),"Fresnel reflection increases toward grazing incidence");
    const auto left=light_direction({100,300},{300,300}),right=light_direction({500,300},{300,300});
    require(left.x<0&&right.x>0&&left.z>0,"Light direction follows the pointer on either side of a drop");
    require(shadow_offset(left,64).x>0&&shadow_offset(right,64).x<0,"Shadow lies opposite the actual mouse light direction");
}
}
int main(){try{physics();puddles();optics();std::cout<<"PASS fixed-step drag/gravity, fluid impact/spread/volume/no-rebound, four-second expiry, bounds, Snell/Fresnel/mouse shadow\nAll "<<assertions<<" liquid assertions passed.\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL after "<<assertions<<" assertions: "<<e.what()<<'\n';return 1;}}
