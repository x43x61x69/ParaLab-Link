#include "../firmware/include/bridge_arbiter.h"
#include <cassert>
int main(){
    BridgeArbiter a;
    assert(a.bridgeActive());assert(a.canPause(0,false));
    a.hostInput(100);
    assert(!a.canPause(249,false));assert(a.canPause(250,false));
    assert(!a.canPause(300,true)); // staged bytes or data waiting in USB FIFO.
    a.pause();assert(!a.bridgeActive());assert(!a.canPause(500,false));
    a.resume();assert(a.bridgeActive());
    for(unsigned outcome=0;outcome<3;outcome++){
        a.pause(); // success, failure and cancellation all release the owner.
        assert(!a.bridgeActive());a.resume();assert(a.bridgeActive());
    }
    a.hostInput(UINT32_MAX-50);assert(!a.canPause(98,false));assert(a.canPause(99,false));
}
