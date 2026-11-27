#include "esp32-hal-tinyusb.h"
#include <cstring>

namespace {
constexpr uint8_t vendorRequest = 0x01;
constexpr uint8_t landingPage = 0x01;
constexpr uint16_t getUrl = 0x02;
constexpr uint16_t bosLength = TUD_BOS_DESC_LEN + TUD_BOS_WEBUSB_DESC_LEN;

constexpr uint8_t bosDescriptor[] = {
    TUD_BOS_DESCRIPTOR(bosLength, 1),
    TUD_BOS_WEBUSB_DESCRIPTOR(vendorRequest, landingPage),
};

constexpr char landingUrl[] = "x43x61x69.github.io/ParaLab/#device";
constexpr uint8_t urlLength = 3 + sizeof(landingUrl) - 1;
}

// Arduino's BOS descriptor advertises WinUSB on interface 0, which is our CDC
// port. Advertise only the landing page so Windows can keep its serial driver.
extern "C" const uint8_t* __wrap_tud_descriptor_bos_cb() {
    return bosDescriptor;
}

extern "C" bool tinyusb_vendor_control_request_cb(uint8_t rhport, uint8_t stage,
                                                   const tusb_control_request_t* request) {
    if (request->bmRequestType != 0xc0 || request->bRequest != vendorRequest ||
        request->wIndex != getUrl || request->wValue != landingPage) return false;
    if (stage != CONTROL_STAGE_SETUP) return true;

    // TinyUSB can read this buffer after the setup callback returns.
    static uint8_t descriptor[urlLength] = {urlLength, 0x03, 0x01}; // URL, HTTPS
    std::memcpy(descriptor + 3, landingUrl, sizeof(landingUrl) - 1);
    return tud_control_xfer(rhport, request, descriptor, sizeof(descriptor));
}
