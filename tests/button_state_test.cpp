#include "../firmware/include/button_state.h"
#include <cassert>
using Event=ButtonState::Event;
int main(){
    ButtonState b;b.begin(true,0);
    assert(b.update(false,100)==Event::None);
    assert(b.update(false,139)==Event::None);
    assert(b.update(false,140)==Event::None);
    assert(b.update(false,939)==Event::None);
    assert(b.update(false,940)==Event::Long); // Fires while still held.
    assert(b.update(false,1600)==Event::None); // Never repeats.
    assert(b.update(true,1610)==Event::None);
    assert(b.update(true,1650)==Event::None); // No confirm/click on release.
    assert(b.update(false,1800)==Event::None);
    assert(b.update(false,1840)==Event::None);
    assert(b.update(true,1900)==Event::None);
    assert(b.update(true,1940)==Event::Short);
    assert(b.update(true,2000)==Event::None);
    // Contact bounce must not produce a click.
    b.begin(true,0);b.update(false,10);b.update(true,20);
    assert(b.update(true,100)==Event::None);
    // A key held at startup must be released before it is armed.
    b.begin(false,0);assert(b.update(false,2000)==Event::None);
    b.update(true,2010);assert(b.update(true,2050)==Event::None);
    // Unsigned timing remains valid across millis() rollover.
    b.begin(true,UINT32_MAX-200);b.update(false,UINT32_MAX-100);b.update(false,UINT32_MAX-60);
    assert(b.update(false,738)==Event::None);assert(b.update(false,739)==Event::Long);
}
