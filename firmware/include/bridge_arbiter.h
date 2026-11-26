#pragma once
#include <stdint.h>

// USB bridge is the default owner, independent of the displayed screen.
// A local transaction waits for a short gap in host input and drained TX bytes.
class BridgeArbiter {
public:
    static constexpr uint32_t quietMs=150, waitMs=5000;
    void hostInput(uint32_t now){lastHost=now;seenHost=true;}
    bool canPause(uint32_t now,bool pendingTx) const {
        return !local && !pendingTx && (!seenHost || uint32_t(now-lastHost)>=quietMs);
    }
    void pause(){local=true;}
    void resume(){local=false;}
    bool bridgeActive() const{return !local;}
private:
    uint32_t lastHost=0;
    bool seenHost=false,local=false;
};
