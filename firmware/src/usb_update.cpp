#include "usb_update.h"
#include "core.h"
#include "link_version.h"

#include <esp_ota_ops.h>
#include <esp32-hal-tinyusb.h>
#include <mbedtls/sha256.h>
#include <esp32s3/rom/miniz.h>

namespace {
bool reply(const char* message) {
    for(size_t offset=0, size=strlen(message);offset<size;){
        if(!tud_mounted()||!tud_cdc_connected())return false;
        const size_t sent=tud_cdc_write(message+offset,size-offset);
        tud_cdc_write_flush();
        if(!sent)return false;
        offset+=sent;
    }
    return true;
}
bool readBytes(void* data,size_t size,uint32_t timeout=12000) {
    const uint32_t start=millis();
    auto* out=static_cast<uint8_t*>(data);
    while(size && millis()-start<timeout){
        const size_t count=tud_cdc_read(out,size);
        if(count){out+=count;size-=count;}
        else vTaskDelay(1);
    }
    return size==0;
}
bool readHeader(char* line,size_t capacity) {
    const auto start=millis();
    for(size_t count=0;count+1<capacity;){
        const uint32_t elapsed=millis()-start;
        if(elapsed>=12000 || !readBytes(line+count,1,12000-elapsed))return false;
        if(line[count]=='\n'){line[count]=0;return true;}
        ++count;
    }
    return false;
}
class Frames {
public:
    size_t read(uint8_t* frame, size_t capacity) {
        bool started=false, escaped=false, valid=false;
        size_t length=0;
        const uint32_t start=millis();
        while(millis()-start<15000){
            if(position==available){
                available=tud_cdc_read(batch,sizeof(batch));
                position=0;
                if(!available){vTaskDelay(1);continue;}
            }
            uint8_t byte=batch[position++];
            if(byte==0xc0){
                if(started&&valid&&!escaped&&length)return length;
                started=valid=true;
                escaped=false;
                length=0;
                continue;
            }
            if(!started||!valid)continue;
            if(escaped){
                if(byte==0xdc)byte=0xc0;
                else if(byte==0xdd)byte=0xdb;
                else{valid=false;continue;}
                escaped=false;
            }else if(byte==0xdb){escaped=true;continue;}
            if(length==capacity){valid=false;continue;}
            frame[length++]=byte;
        }
        return 0;
    }

private:
    uint8_t batch[64];
    size_t position=0,available=0;
};

bool acknowledge(unsigned written) {
    char line[24];
    snprintf(line,sizeof(line),"OK %u\n",written);
    return reply(line);
}

bool validVersion(const char* text) {
    for(unsigned segment=0;segment<3;++segment){
        if(*text<'0'||*text>'9')return false;
        while(*text>='0'&&*text<='9')++text;
        if(*text!=(segment<2?'.':'\0'))return false;
        ++text;
    }
    return true;
}
bool fail(const char* reason) {
    statusMessage(reason);
    char response[63];
    snprintf(response,sizeof(response),"ERROR %.54s\n",reason);
    reply(response);
    return false;
}
}
bool updateFromUsb() {
    // The host probes the 2400-baud port and waits for this line before sending update bytes.
    char ready[64];
    snprintf(ready,sizeof(ready),"PARALINK OTA2 %s %s\n",product::hardware,PARALINK_VERSION);
    const uint32_t start=millis();
    while(!tud_cdc_connected() && millis()-start<1500)vTaskDelay(1);
    if(!reply(ready))return false;

    char header[128], board[8], version[20], expectedSha[65], extra;
    unsigned size, assetSize, assetCrc;
    if(!readHeader(header,sizeof(header)) ||
       sscanf(header,"UPDATE %7s %19s %u %u %x %64s %c",board,version,&size,&assetSize,&assetCrc,expectedSha,&extra)!=6 ||
       strcmp(board,product::hardware)||!validVersion(version)||strlen(expectedSha)!=64 ||
       strspn(expectedSha,"0123456789abcdef")!=64)
        return fail("Invalid update header");
    statusUpdateVersion(version);
    if(!assetPackMatches(assetSize,assetCrc))return fail("Update needs new assets");
    const esp_partition_t* target=esp_ota_get_next_update_partition(nullptr);
    if(!target || !size || size>target->size)return fail("Firmware does not fit");
    esp_ota_handle_t handle;
    statusMessage("Preparing update");
    if(esp_ota_begin(target,size,&handle)!=ESP_OK)return fail("Could not open update partition");
    mbedtls_sha256_context digest;
    mbedtls_sha256_init(&digest);
    mbedtls_sha256_starts_ret(&digest,0);
    bool ok=acknowledge(0);
    Frames frames;
    uint8_t frame[1032];
    for(unsigned written=0;ok && written<size;){
        const size_t length=frames.read(frame,sizeof(frame));
        if(!length){
            esp_ota_abort(handle);
            mbedtls_sha256_free(&digest);
            char reason[48];snprintf(reason,sizeof(reason),"Read timed out at %u",written);
            return fail(reason);
        }
        if(length<=8)continue;
        const auto* block=frame+8;
        const unsigned chunk=length-8;
        uint32_t offset, crc;
        memcpy(&offset,frame,4);
        memcpy(&crc,frame+4,4);
        if(offset>=size || chunk!=std::min(1024u,size-offset) || mz_crc32(0,block,chunk)!=crc)continue;
        if(offset==written){
            const auto result=esp_ota_write(handle,block,chunk);
            if(result!=ESP_OK){
                esp_ota_abort(handle);
                mbedtls_sha256_free(&digest);
                char reason[54];
                snprintf(reason,sizeof(reason),"Write failed at %u: 0x%X",written,unsigned(result));
                return fail(reason);
            }
            mbedtls_sha256_update_ret(&digest,block,chunk);
            written+=chunk;
            statusMessage("Writing firmware",std::min(99u,100u*written/size));
        }
        if(offset+chunk==written)ok=acknowledge(written);
    }
    uint8_t hash[32];
    mbedtls_sha256_finish_ret(&digest,hash);
    mbedtls_sha256_free(&digest);
    if(!ok){esp_ota_abort(handle);return fail("Update interrupted");}
    statusMessage("Verifying firmware",99);
    char actual[65];
    for(unsigned i=0;i<sizeof(hash);++i)snprintf(actual+i*2,3,"%02x",hash[i]);
    if(strcmp(actual,expectedSha)){esp_ota_abort(handle);return fail("Firmware checksum failed");}
    if(esp_ota_end(handle)!=ESP_OK || esp_ota_set_boot_partition(target)!=ESP_OK)
        return fail("Firmware validation failed");
    statusMessage("Verified. Restarting",100);
    reply("DONE\n");
    return true;
}
