#pragma once
#include <cstdint>

namespace panel {
constexpr int width = 536;
constexpr int height = 240;

struct Touch {
    enum Event { None, Down, Move, Release, Invalid } event;
    int x, y; // A release may lack a final coordinate.
};

bool begin();
bool setDisplayOn(bool on, unsigned brightness);
bool takeTouchInterrupt();
void setBrightness(unsigned level);
void present(const uint16_t* pixels, bool rotated);
void presentRows(const uint16_t* pixels, bool rotated, int top, int bottom);
Touch touch(bool rotated);
}
