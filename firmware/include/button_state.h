#pragma once
#include <stdint.h>

// One event per press: Short on release, Long at the hold threshold.
class ButtonState {
public:
    enum class Event { None, Short, Long };
    static constexpr uint32_t debounceMs=40, holdMs=800;
    void begin(bool released,uint32_t now){physical=stable=released;changed=now;armed=false;longFired=false;}
    Event update(bool released,uint32_t now){
        if(released!=physical){physical=released;changed=now;}
        if(released!=stable&&uint32_t(now-changed)>=debounceMs){
            stable=released;
            if(!released){pressed=now;armed=true;longFired=false;}
            else if(armed){armed=false;if(!longFired)return Event::Short;}
        }
        if(armed&&!physical&&!stable&&!longFired&&uint32_t(now-pressed)>=holdMs){longFired=true;return Event::Long;}
        return Event::None;
    }
private:
    bool physical=true,stable=true,armed=false,longFired=false;
    uint32_t changed=0,pressed=0;
};
