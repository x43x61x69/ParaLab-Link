#include "protocol.h"
#include "tcp_codec.h"
#include "driver/uart.h"
#include "esp_system.h"
#include <LittleFS.h>

void ParadiseProtocol::reset(){
    session=0;
    hasSession=false;
    inputOffset=inputSize=0;
    failure="Transfer failed";
}
bool ParadiseProtocol::fail(const char* message){failure=message;return false;}
bool ParadiseProtocol::exact(uint8_t* out,size_t count,uint32_t timeout){
    const auto start=millis();
    while(count && !cancelled.load()){
        if(inputOffset==inputSize){
            const uint32_t elapsed=millis()-start;
            if(elapsed>=timeout)return false;
            int read=uart_read_bytes(UART_NUM_1,input,sizeof(input),0);
            if(read==0){
                const auto ticks=pdMS_TO_TICKS(std::min(uint32_t(20),timeout-elapsed));
                read=uart_read_bytes(UART_NUM_1,input,1,std::max(TickType_t(1),ticks));
            }
            if(read<0)return false;
            if(!read)continue;
            inputOffset=0;
            inputSize=read;
            uartRxBytes.fetch_add(read);
            lastRxMs.store(millis());
        }
        const size_t copied=std::min(count,inputSize-inputOffset);
        memcpy(out,input+inputOffset,copied);
        inputOffset+=copied;
        out+=copied;
        count-=copied;
    }
    return !count;
}
bool ParadiseProtocol::line(char* out,size_t capacity,uint32_t timeout){
    const auto start=millis();size_t used=0;bool overflow=false;
    while(!cancelled.load()){
        const uint32_t elapsed=millis()-start;
        if(elapsed>=timeout)break;
        uint8_t c;if(!exact(&c,1,timeout-elapsed))break;
        if(c=='\n'){
            if(overflow){used=0;overflow=false;continue;}
            while(used && (out[used-1]==' '||out[used-1]=='\t'))--used;
            if(!used)continue;
            out[used]=0;
            return true;
        }
        if(c=='\r')continue;
        if(!used && (c==0 || c==' ' || c=='\t'))continue;
        if((c<32 && c!='\t') || c>126 || used+1>=capacity){overflow=true;continue;}
        if(!overflow)out[used++]=char(c);
    }out[0]=0;return false;
}
bool ParadiseProtocol::command(const char* text){
    char wire[40];
    const int size=snprintf(wire,sizeof(wire),"%s\r\n",text);
    return !cancelled.load() && size>0 && size<int(sizeof(wire)) && uartSend((const uint8_t*)wire,size);
}
int ParadiseProtocol::ack(uint32_t timeout,int* enq){
    const auto start=millis();char text[128];
    while(!cancelled.load()){
        const uint32_t elapsed=millis()-start;
        if(elapsed>=timeout || !line(text,sizeof(text),timeout-elapsed))break;
        if(!strcmp(text,"ACK"))return 1;if(!strcmp(text,"CAN"))return -1;if(!strcmp(text,"NAK"))return 0;
        if(!strncmp(text,"ENQ ",4)&&enq){char* end;unsigned long n=strtoul(text+4,&end,10);if(!*end&&n<256){*enq=int(n);return 2;}}
        if(!strcmp(text,"ECHO REQ"))command("ECHO REP");
    }return -2;
}
bool ParadiseProtocol::echo(){
    char text[128];for(int retry=0;retry<3&&!cancelled.load();retry++){
        if(!command("ECHO REQ"))return fail("UART write failed");
        const auto start=millis();
        while(!cancelled.load()){
            const uint32_t elapsed=millis()-start;
            if(elapsed>=3000 || !line(text,sizeof(text),3000-elapsed))break;
            if(!strcmp(text,"ECHO REP"))return true;
            if(!strcmp(text,"ECHO REQ"))return command("ECHO REP")||fail("UART write failed");
            if(!strcmp(text,"CAN"))return fail("Paradise cancelled the transfer");
        }
    }return fail("Paradise did not answer Echo");
}
bool ParadiseProtocol::send(uint8_t type,const uint8_t* bytes,size_t count,const CatalogItem* item,bool trailer){
    if(count>4096*256 || (!bytes&&!item))return false;
    char text[40];snprintf(text,sizeof(text),"PKT %u",unsigned(count));bool accepted=false;
    for(int retry=0;retry<3&&!cancelled.load();retry++){
        if(!command(text))return fail("UART write failed");
        int a=ack(2000);if(a==1){accepted=true;break;}if(a==-1)return fail("Paradise cancelled the transfer");
    }if(!accepted)return fail("No packet ACK. Check Paradise screen");
    const unsigned chunks=std::max(size_t(1),(count+4095)/4096);unsigned index=0,rewinds=0;
    while(index<chunks&&!cancelled.load()){
        size_t offset=index*4096,n=std::min(size_t(4096),count-offset);
        if(bytes)memcpy(frame+16,bytes+offset,n);
        else{
            size_t raw=offset<item->size?std::min(n,size_t(item->size-offset)):0;
            if(raw&&!assetRange(*item,offset,frame+16,raw))return fail("Could not read item data");
            if(raw<n){
                if(!trailer)return false;
                uint8_t tail[20]{};tail[2]=item->field;tcp::store32(tail+4,item->uid);tcp::store32(tail+8,item->charUid);
                tail[12]=4;tail[16]=6;tail[17]=20;tail[18]=1;
                size_t at=offset+raw-item->size;if(at+n-raw>sizeof(tail))return false;
                memcpy(frame+16+raw,tail+at,n-raw);
            }
        }
        esp_fill_random(frame,4);tcp::makeHeader(frame+4,hasSession?session:0,type,index,frame+16,n);
        tcp::crypt(frame,frame+4,12+n);bool done=false,jumped=false;
        int reply=-2;
        for(int retry=0;retry<3&&!cancelled.load();retry++){
            if(!uartSend(frame,n+16))return fail("UART write failed");
            int request=-1;
            reply=ack(5000,&request);
            if(reply==1){index++;done=true;break;}
            if(reply==-1)return fail("Paradise cancelled the transfer");
            if(reply==2){if(unsigned(request)>=chunks||++rewinds>chunks*3)return fail("Invalid block retry from Paradise");index=request;jumped=true;break;}
        }if(!done&&!jumped)return fail(reply==0?"Paradise rejected the data":"No block ACK. Check Paradise before retrying");
        statusMessage("Sending",100*index/chunks);
    }return !cancelled.load();
}
bool ParadiseProtocol::receive(uint8_t expected,uint8_t* small,size_t cap,size_t* received,fs::File* file,uint32_t wait,size_t fileLimit,bool optional){
    char text[128];uint32_t length=0;bool found=false;
    const auto start=millis();
    while(!cancelled.load()){
        const uint32_t elapsed=millis()-start;
        if(elapsed>=wait || !line(text,sizeof(text),wait-elapsed))break;
        if(!strcmp(text,"ECHO REQ")){if(!command("ECHO REP"))return fail("UART write failed");continue;}
        if(!strcmp(text,"CAN"))return fail("Paradise cancelled the transfer");
        if(strncmp(text,"PKT ",4))continue;
        char* end;unsigned long n=strtoul(text+4,&end,10);
        if(text[4]<'0'||text[4]>'9'||*end||n>cap){command("CAN");return fail("Invalid packet size from Paradise");}length=n;found=true;break;
    }
    if(!found){
        if(optional && !cancelled.load()){if(received)*received=0;return true;}
        return fail("No packet from Paradise. Check its screen");
    }
    if(!command("ACK"))return fail("UART write failed");
    const unsigned chunks=std::max(size_t(1),(size_t(length)+4095)/4096);size_t written=0;
    for(unsigned index=0;index<chunks;index++){
        size_t n=std::min(size_t(4096),size_t(length)-written);bool valid=false;
        for(int retry=0;retry<3&&!cancelled.load();retry++){
            if(!exact(frame,n+16,5000)){command("CAN");return fail("Incomplete data block from Paradise");}
            tcp::crypt(frame,frame+4,n+12);const uint8_t* decoded=frame+4;
            if(!tcp::checkFrame(decoded,n+12)||(hasSession&&tcp::load32(decoded)!=session)){command("NAK");continue;}
            if(decoded[8]!=index){char msg[32];snprintf(msg,sizeof(msg),"ENQ %u",index);command(msg);continue;}
            const uint8_t type=decoded[7];
            if(type&16){session=tcp::load32(decoded);hasSession=true;}
            else if(expected&&(type&15)&&(type&15)!=expected){command("NAK");continue;}
            if(file){size_t save=written<fileLimit?std::min(n,fileLimit-written):0;if(save&&file->write(frame+16,save)!=save){command("CAN");return fail("Could not save received data");}}
            else if(small)memcpy(small+written,frame+16,n);
            written+=n;if(!command("ACK"))return fail("UART write failed");valid=true;break;
        }if(!valid)return fail("Could not verify Paradise data");statusMessage("Receiving",100*(index+1)/chunks);
    }if(received)*received=written;return !cancelled.load();
}
bool ParadiseProtocol::download(const CatalogItem& item){
    reset();
    statusMessage("Waiting for Paradise Download");
    return send(3,nullptr,item.size,&item);
}
bool ParadiseProtocol::gift(uint16_t id){
    reset();uint8_t small[4096];size_t size;
    statusMessage("On Paradise, open Gift and press B");
    if(!receive(0,small,sizeof(small),&size,nullptr,60000))return false;
    if(!receive(0,small,sizeof(small),&size))return false;
    delay(100);
    const uint8_t hello[]={1,0,0,0};if(!send(2,hello,4))return false;delay(100);
    if(!echo())return false;delay(100);uint8_t item[]={uint8_t(id),uint8_t(id>>8)};
    if(!send(2,item,2))return false;
    statusMessage("Gift acknowledged. Check Paradise",100);
    delay(100);
    // Listen for the returned gift and heartbeat together; either may arrive first.
    if(!command("ECHO REQ"))return fail("Gift acknowledged. Check Paradise");
    return receive(0,small,sizeof(small),&size,nullptr,12000,SIZE_MAX,true);
}
bool ParadiseProtocol::sync(const char* number){
    char expected[32],text[128];snprintf(expected,sizeof(expected),"SYNC %s",number);
    if(!command(expected))return fail("UART write failed");
    const auto start=millis();
    while(!cancelled.load()){
        const uint32_t elapsed=millis()-start;
        if(elapsed>=60000 || !line(text,sizeof(text),60000-elapsed))break;
        if(!strcmp(text,expected))return true;
        if(!strcmp(text,"CAN"))return fail("Paradise cancelled the transfer");
        if(!strcmp(text,"ECHO REQ"))command("ECHO REP");
    }return fail("Playdate confirmation timed out");
}
bool ParadiseProtocol::playdate(const CatalogItem& item){
    reset();uint8_t small[4096];size_t size=0;
    statusMessage("Waiting for playdate");if(!receive(0,small,sizeof(small),&size,nullptr,40000))return false;
    if(!statusSnapshot().storageOK){statusMessage("Storage unavailable");return false;}
    auto temp=LittleFS.open("/received.tmp","w");if(!temp)return false;
    statusMessage("Receiving friend");bool ok=receive(1,nullptr,131092,&size,&temp,30000,131072);temp.flush();temp.close();
    if(!ok||size<131072){LittleFS.remove("/received.tmp");return false;}
    delay(1000);statusMessage("Sending friend");if(!send(1,nullptr,item.size+20,&item,true))return false;
    if(!receive(1,small,sizeof(small),&size,nullptr,15000)||size<2)return false;
    if(!receive(1,small,sizeof(small),&size,nullptr,20000)||size<4)return false;
    // Default is to decline breeding rather than change the user's Tama.
    if((small[2]&1) && (!command("BREED 0") || !sync("1")))return false;
    if(!LittleFS.rename("/received.tmp","/received.bin"))return false;
    return !cancelled.load();
}
