#pragma once
#include <Arduino.h>
#include <atomic>
#include "catalog.h"

enum class Mode : uint8_t { Standalone, Bridge };
enum class Job : uint8_t { Item, Echo, ReplayBytes };
struct Request { Job job; uint16_t item; };
struct Status {
    Mode mode = Mode::Bridge;
    bool busy=false, assetOK=false, storageOK=false, success=false;
    unsigned progress=0, replayBytes=0;
    int lastItem=-1;
    char message[80]="Ready";
};
extern std::atomic<bool> cancelled;
extern std::atomic<uint32_t> usbInBytes, uartTxBytes, uartRxBytes, usbOutBytes;
extern std::atomic<uint32_t> lastTxMs, lastRxMs;
Status statusSnapshot();
void statusMessage(const char* message, unsigned progress=0);
void statusResult(bool success, const char* message);
bool submit(Job job, uint16_t item=0);
void communicationBegin();
void communicationUsbBegin();
void communicationUsbTick();
bool takeCommunicationActivity();
void uiBegin();
void uiTick();
bool assetsBegin();
bool assetRead(unsigned record, uint8_t* out, size_t capacity);
bool assetRange(const CatalogItem& item, size_t offset, uint8_t* out, size_t count);
bool uartSend(const uint8_t* bytes, size_t count);
