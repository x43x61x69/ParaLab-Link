#pragma once
#include <stdint.h>

constexpr uint16_t rgb565(unsigned rgb) {
    return ((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f);
}

constexpr auto BG = rgb565(0x0b131d);
constexpr auto PANEL = rgb565(0x182535);
constexpr auto INK = rgb565(0xf3f8fc);
constexpr auto MUTED = rgb565(0xa8bed0);
constexpr auto CYAN = rgb565(0x93d6ff);
constexpr auto ACCENT = rgb565(0x89efcf);
constexpr auto GREEN = rgb565(0x9ceabc);
constexpr auto LINE = rgb565(0x405a6e);
constexpr auto SELECT = rgb565(0x253b50);
constexpr auto PREVIEW = rgb565(0x101b29);
constexpr auto YELLOW = rgb565(0xffd88c);
constexpr auto ORANGE = rgb565(0xffac70);
constexpr auto DANGER = rgb565(0xffb5ac);
