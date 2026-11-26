#pragma once
#include <stddef.h>
#include <stdint.h>
namespace tcp {
constexpr size_t chunkSize=4096, frameHeader=12, nonceSize=4;
uint16_t crc16(const uint8_t* bytes,size_t count);
uint32_t load32(const uint8_t* bytes);
void store32(uint8_t* bytes,uint32_t value);
void makeHeader(uint8_t* out,uint32_t session,uint8_t type,uint8_t index,const uint8_t* payload,size_t count);
bool checkFrame(const uint8_t* frame,size_t count);
void crypt(const uint8_t nonce[4],uint8_t* bytes,size_t count);
}
