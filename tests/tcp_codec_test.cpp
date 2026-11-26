#include "../firmware/include/tcp_codec.h"
#include "protocol_vectors.h"
#include <cassert>
#include <vector>
#include <string>
#include <cstring>
std::vector<uint8_t> unhex(const char* s){std::vector<uint8_t> out;while(*s){char b[]={s[0],s[1],0};out.push_back(strtoul(b,nullptr,16));s+=2;}return out;}
int main(){
    const uint8_t digits[]="123456789";assert(tcp::crc16(digits,9)==0xbb3d);
    for(const auto& v:vectors){
        std::vector<uint8_t> payload(v.size),frame(12+v.size);
        for(unsigned i=0;i<v.size;i++)payload[i]=(i*37+11)&255;
        tcp::makeHeader(frame.data(),0x12345678,3,17,payload.data(),payload.size());
        if(v.size)memcpy(frame.data()+12,payload.data(),v.size);
        assert(frame==unhex(v.plain));assert(tcp::checkFrame(frame.data(),frame.size()));
        const uint8_t nonce[]={3,5,7,11};tcp::crypt(nonce,frame.data(),frame.size());assert(frame==unhex(v.encrypted));
        tcp::crypt(nonce,frame.data(),frame.size());assert(frame==unhex(v.plain));
        auto bad=frame;bad[9]^=1;assert(!tcp::checkFrame(bad.data(),bad.size()));
        bad=frame;bad[4]^=1;assert(!tcp::checkFrame(bad.data(),bad.size()));
        if(v.size){bad=frame;bad.back()^=1;assert(!tcp::checkFrame(bad.data(),bad.size()));}
        assert(!tcp::checkFrame(frame.data(),11));
    }
}
