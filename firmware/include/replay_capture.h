#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// USB packet boundaries are not command boundaries. End a burst on silence;
// reject an oversized burst rather than replaying a truncated prefix/suffix.
class ReplayCapture {
public:
    void clear() { pending_size=valid_size=0;last_byte=0;recording=overflow=false; }
    static constexpr size_t capacity = 4096;
    static constexpr uint32_t quiet_ms = 150;
    void finish(uint32_t now) {
        if (!recording || uint32_t(now - last_byte) < quiet_ms) return;
        valid_size = overflow ? 0 : pending_size;
        if (valid_size) memcpy(saved, pending, valid_size);
        recording = false;
    }
    void append(const uint8_t* bytes, size_t size, uint32_t now) {
        finish(now);
        if (!recording) { recording = true; pending_size = 0; overflow = false; }
        last_byte = now;
        if (size > capacity - pending_size) overflow = true;
        if (!overflow) { memcpy(pending + pending_size, bytes, size); pending_size += size; }
    }
    bool busy() const { return recording; }
    bool too_large() const { return overflow; }
    size_t size() const { return valid_size; }
    const uint8_t* data() const { return saved; }
private:
    uint8_t pending[capacity]{}, saved[capacity]{};
    size_t pending_size = 0, valid_size = 0;
    uint32_t last_byte = 0;
    bool recording = false, overflow = false;
};
