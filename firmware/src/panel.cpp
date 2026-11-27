#include "panel.h"

#include <Arduino.h>
#include <Wire.h>
#include <atomic>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>

// RM67162 initialization and QSPI framing follow the Capybara front end for this LILYGO board.
namespace {
constexpr int cs = 6, reset = 17, power = 38;
constexpr int touchSda = 3, touchScl = 2, touchIrq = 21;
constexpr uint8_t touchAddress = 0x15;
constexpr int stripRows = 8;

spi_device_handle_t display;
uint16_t* dmaStrip;
bool touchReady;
std::atomic<bool> touchPending{false};
constexpr uint8_t brightnessLevels[]{0x50, 0xA0, 0xFF};

void IRAM_ATTR notifyTouch() { touchPending.store(true, std::memory_order_relaxed); }

bool transfer(spi_transaction_t& transaction) {
    digitalWrite(cs, LOW);
    const bool ok = spi_device_polling_transmit(display, &transaction) == ESP_OK;
    digitalWrite(cs, HIGH);
    return ok;
}

bool command(uint8_t cmd, const uint8_t* data = nullptr, size_t size = 0) {
    spi_transaction_t transaction{};
    transaction.cmd = 0x02;
    transaction.addr = uint32_t(cmd) << 8;
    transaction.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
    transaction.tx_buffer = data;
    transaction.length = size * 8;
    return transfer(transaction);
}

bool reg(uint8_t cmd, uint8_t value) { return command(cmd, &value, 1); }

bool window(int y, int rows) {
    constexpr int lastX = panel::width - 1;
    const int lastY = y + rows - 1;
    const uint8_t col[]{0, 0, uint8_t(lastX >> 8), uint8_t(lastX)};
    const uint8_t row[]{uint8_t(y >> 8), uint8_t(y), uint8_t(lastY >> 8), uint8_t(lastY)};
    return command(0x2A, col, sizeof(col)) && command(0x2B, row, sizeof(row));
}

bool touchReg(uint8_t address, uint8_t value) {
    Wire.beginTransmission(touchAddress);
    Wire.write(address);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}
}

bool panel::begin() {
    pinMode(power, OUTPUT);
    digitalWrite(power, HIGH);
    pinMode(cs, OUTPUT);
    digitalWrite(cs, HIGH);
    pinMode(reset, OUTPUT);
    digitalWrite(reset, HIGH);
    delay(200);
    digitalWrite(reset, LOW);
    delay(300);
    digitalWrite(reset, HIGH);
    delay(200);

    constexpr size_t stripBytes = width * stripRows * sizeof(uint16_t);
    dmaStrip = static_cast<uint16_t*>(heap_caps_malloc(stripBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!dmaStrip) return false;
    spi_bus_config_t bus{};
    bus.data0_io_num = 18;
    bus.data1_io_num = 7;
    bus.data2_io_num = 48;
    bus.data3_io_num = 5;
    bus.data4_io_num = bus.data5_io_num = bus.data6_io_num = bus.data7_io_num = -1;
    bus.sclk_io_num = 47;
    bus.max_transfer_sz = stripBytes;
    bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS;
    if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

    spi_device_interface_config_t device{};
    device.command_bits = 8;
    device.address_bits = 24;
    device.clock_speed_hz = 20000000;
    device.spics_io_num = -1;
    device.flags = SPI_DEVICE_HALFDUPLEX;
    device.queue_size = 1;
    if (spi_bus_add_device(SPI3_HOST, &device, &display) != ESP_OK) return false;
    if (!reg(0xFE, 0) || !command(0x11)) return false;
    delay(120);
    if (!reg(0xFE, 5) || !reg(0x05, 5) || !reg(0xFE, 1) || !reg(0x73, 0x25)
        || !reg(0xFE, 0) || !reg(0x36, 0x60) || !reg(0x3A, 0x55)
        || !reg(0x51, 0) || !command(0x29)) return false;
    delay(120);

    pinMode(touchIrq, INPUT_PULLUP);
    if (Wire.begin(touchSda, touchScl, 100000)) {
        Wire.setTimeOut(4);
        // Keep register reads available, and report contact, movement and release.
        touchReg(0xFE, 1);
        touchReg(0xFA, 0x60);
        attachInterrupt(digitalPinToInterrupt(touchIrq), notifyTouch, FALLING);
        touchReady = true;
    }
    return true;
}

void panel::setBrightness(unsigned level) {
    setDisplayOn(true, level);
}

bool panel::setDisplayOn(bool on, unsigned brightness) {
    return brightness < sizeof(brightnessLevels) && reg(0x51, on ? brightnessLevels[brightness] : 0);
}

bool panel::takeTouchInterrupt() { return touchPending.exchange(false); }

void panel::present(const uint16_t* pixels, bool rotated) {
    presentRows(pixels, rotated, 0, height);
}

void panel::presentRows(const uint16_t* pixels, bool rotated, int top, int bottom) {
    const int physicalTop = rotated ? height - bottom : top;
    const int physicalBottom = rotated ? height - top : bottom;
    const int firstStrip = physicalTop / stripRows * stripRows;
    for (int y = firstStrip; y < physicalBottom; y += stripRows) {
        constexpr int count = width * stripRows;
        const uint16_t* source = pixels + y * width;
        const uint16_t* reverse = pixels + (height - y) * width - 1;
        for (int i = 0; i < count; ++i) {
            const uint16_t color = rotated ? reverse[-i] : source[i];
            dmaStrip[i] = (color << 8) | (color >> 8);
        }
        if (!window(y, stripRows)) return;
        spi_transaction_t transaction{};
        transaction.flags = SPI_TRANS_MODE_QIO;
        transaction.cmd = 0x32;
        transaction.addr = 0x002C00;
        transaction.tx_buffer = dmaStrip;
        transaction.length = count * 16;
        if (!transfer(transaction)) return;
    }
}

panel::Touch panel::touch(bool rotated) {
    if (!touchReady) return {Touch::None, 0, 0};
    static uint32_t lastPoll;
    const uint32_t now = millis();
    if (uint32_t(now - lastPoll) < 15) return {Touch::None, 0, 0};
    lastPoll = now;

    Wire.beginTransmission(touchAddress);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(touchAddress, uint8_t(5)) != 5)
        return {Touch::Invalid, 0, 0};

    uint8_t data[5];
    for (auto& byte : data) byte = Wire.read();
    const unsigned event = data[1] >> 6;
    const int x = ((data[1] & 0x0F) << 8) | data[2];
    const int y = ((data[3] & 0x0F) << 8) | data[4];
    const bool inBounds = x < width && y < height;
    // The touch axes match RM67162 MADCTL 0x60. The controller can
    // also report a virtual button outside the panel and all-FF idle samples.
    const int screenX = rotated ? width - 1 - x : x;
    const int screenY = rotated ? height - 1 - y : y;
    if (event == 1 && data[0] <= 1 && inBounds) return {Touch::Release, screenX, screenY};
    if (data[0] == 0 && event != 3) return {Touch::Release, -1, -1};
    if (data[0] != 1 || event == 3 || !inBounds) return {Touch::Invalid, 0, 0};
    return {event == 0 ? Touch::Down : Touch::Move, screenX, screenY};
}
