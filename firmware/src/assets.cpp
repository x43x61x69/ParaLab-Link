#include "core.h"
#include "esp_partition.h"
#include "esp_spi_flash.h"
#include "esp32s3/rom/miniz.h"
#include "freertos/semphr.h"

namespace {
const uint8_t* mapped=nullptr;
spi_flash_mmap_handle_t handle;
SemaphoreHandle_t lock=nullptr;
tinfl_decompressor decoder;
uint8_t block[4096];
bool readLocked(unsigned id, uint8_t* out, size_t capacity) {
    if (!mapped || id>=assetCount) return false;
    const auto& r=assetRecords[id];
    if (r.raw>capacity || r.offset>assetPackBytes || r.packed>assetPackBytes-r.offset) return false;
    tinfl_init(&decoder);
    size_t input=r.packed, output=r.raw;
    auto result=tinfl_decompress(&decoder,mapped+r.offset,&input,out,out,&output,
                                TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    return result==TINFL_STATUS_DONE && output==r.raw && input==r.packed && mz_crc32(0,out,output)==r.crc;
}
}
bool assetsBegin() {
    lock=xSemaphoreCreateMutex();
    const auto* p=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,(esp_partition_subtype_t)0x40,"assets");
    if (!lock || !p || assetPackBytes>p->size || esp_partition_mmap(p,0,assetPackBytes,SPI_FLASH_MMAP_DATA,(const void**)&mapped,&handle)!=ESP_OK) return false;
    uint32_t size,crc;memcpy(&size,mapped+4,4);memcpy(&crc,mapped+12,4);
    return !memcmp(mapped,"KPA1",4) && size==assetPackBytes && mz_crc32(0,mapped+16,size-16)==crc;
}
bool assetRead(unsigned record, uint8_t* out, size_t capacity) {
    if (!lock) return false;
    xSemaphoreTake(lock,portMAX_DELAY);bool ok=readLocked(record,out,capacity);xSemaphoreGive(lock);return ok;
}
bool assetRange(const CatalogItem& item,size_t offset,uint8_t* out,size_t count) {
    if (!lock || offset>item.size || count>item.size-offset) return false;
    xSemaphoreTake(lock,portMAX_DELAY);bool ok=true;
    while(count) {
        const unsigned index=offset/4096, inside=offset%4096;
        if(index>=item.blocks || !readLocked(item.firstBlock+index,block,sizeof(block))){ok=false;break;}
        size_t n=std::min(count,size_t(assetRecords[item.firstBlock+index].raw-inside));
        if(!n){ok=false;break;}memcpy(out,block+inside,n);out+=n;offset+=n;count-=n;
    }
    xSemaphoreGive(lock);return ok;
}

bool assetPackMatches(size_t size, uint32_t crc) {
    if(!mapped || size!=assetPackBytes || !statusSnapshot().assetOK)return false;
    uint32_t stored;
    memcpy(&stored,mapped+12,sizeof(stored));
    return stored==crc;
}
