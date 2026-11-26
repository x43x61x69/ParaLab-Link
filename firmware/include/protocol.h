#pragma once
#include "core.h"
#include <FS.h>
class ParadiseProtocol {
public:
    void reset();
    bool echo();
    bool download(const CatalogItem& item);
    bool gift(uint16_t id);
    bool playdate(const CatalogItem& item);
    const char* error() const { return failure; }
private:
    uint32_t session=0;
    bool hasSession=false;
    uint8_t frame[4112];
    uint8_t input[128];
    size_t inputOffset=0, inputSize=0;
    const char* failure="Transfer failed";
    bool fail(const char* message);
    bool line(char* out,size_t capacity,uint32_t timeout);
    bool exact(uint8_t* out,size_t count,uint32_t timeout);
    bool command(const char* text);
    int ack(uint32_t timeout,int* enq=nullptr);
    bool sync(const char* number);
    bool send(uint8_t type,const uint8_t* bytes,size_t count,const CatalogItem* item=nullptr,bool trailer=false);
    bool receive(uint8_t expected,uint8_t* small,size_t cap,size_t* received,fs::File* file=nullptr,uint32_t wait=5000,size_t fileLimit=SIZE_MAX,bool optional=false);
};
