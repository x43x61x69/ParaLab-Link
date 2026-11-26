#include "tcp_codec.h"
#include <string.h>
#ifdef TCP_NATIVE_TEST
#include <openssl/sha.h>
#else
#include "mbedtls/sha256.h"
#endif
namespace tcp {
uint16_t crc16(const uint8_t* p,size_t n){uint16_t v=0;while(n--){v^=*p++;for(int i=0;i<8;i++)v=(v>>1)^((v&1)?0xa001:0);}return v;}
uint32_t load32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
void store32(uint8_t* p,uint32_t v){for(int i=0;i<4;i++)p[i]=(v>>(8*i))&255;}
void makeHeader(uint8_t* out,uint32_t session,uint8_t type,uint8_t index,const uint8_t* payload,size_t count){
    store32(out,session);memcpy(out+4,"TCP",3);out[7]=type;out[8]=index;out[9]=255-index;
    auto crc=crc16(payload,count);out[10]=crc&255;out[11]=crc>>8;
}
bool checkFrame(const uint8_t* f,size_t n){return n>=12 && !memcmp(f+4,"TCP",3) && uint8_t(f[8]+f[9])==255 && crc16(f+12,n-12)==uint16_t(f[10]|(f[11]<<8));}
void crypt(const uint8_t nonce[4],uint8_t* bytes,size_t count){
    static const char key[]="SPqREQqtuhvgJuRexqMfG8FzstAgmnf7";
    uint8_t seed[4+sizeof(key)-1],mask[32];memcpy(seed,nonce,4);memcpy(seed+4,key,sizeof(key)-1);
#ifdef TCP_NATIVE_TEST
    SHA256(seed,sizeof(seed),mask);
#else
    mbedtls_sha256_ret(seed,sizeof(seed),mask,0);
#endif
    for(size_t i=0;i<count;i++){const size_t j=i%32;bytes[i]^=mask[j];mask[j]=uint8_t(mask[j]*2+1);}
}
}
