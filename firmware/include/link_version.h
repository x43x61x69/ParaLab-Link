#pragma once

#define PARALINK_VERSION "0.1.15"

#ifdef PARALINK_K191
#define PARALINK_HARDWARE "K191"
#else
#define PARALINK_HARDWARE "H717"
#endif

namespace product {
constexpr char hardware[] = PARALINK_HARDWARE;
extern char name[20];
extern char hardwareVersion[24];
void begin();
constexpr char title[] = "ParaLink";

} // namespace product
