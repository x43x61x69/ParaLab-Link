#include "link_version.h"

#include <cstdint>
#include <cstdio>
#include <esp_mac.h>

char product::name[20] = "ParaLink";
char product::hardwareVersion[24] = "v" PARALINK_VERSION;

void product::begin() {
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) != ESP_OK) return;
    snprintf(name, sizeof(name), "ParaLink %02X%02X", unsigned(mac[4]), unsigned(mac[5]));
    snprintf(hardwareVersion, sizeof(hardwareVersion), "v%s  %02X%02X",
             PARALINK_VERSION, unsigned(mac[4]), unsigned(mac[5]));
}
