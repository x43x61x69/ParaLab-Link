#include "core.h"
#include "board_pins.h"
#include "protocol.h"
#include "replay_capture.h"
#include "bridge_arbiter.h"
#include "driver/uart.h"
#include "hal/usb_serial_jtag_ll.h"
#include "link_version.h"
#include "usb_update.h"
#include <USB.h>
#include "esp32-hal-tinyusb.h"
#include <LittleFS.h>
#include "freertos/semphr.h"

std::atomic<bool> cancelled{false};
std::atomic<uint32_t> usbInBytes{0},uartTxBytes{0},uartRxBytes{0},usbOutBytes{0},lastTxMs{0},lastRxMs{0};
namespace {
Status status;
SemaphoreHandle_t statusLock;
QueueHandle_t requests;
QueueHandle_t uartEvents;
std::atomic<bool> trafficPending{false};
ParadiseProtocol protocol;
ReplayCapture capture;
BridgeArbiter arbiter;
std::atomic<bool> rebootForUpload{false};
std::atomic<bool> requestUsbUpdate{false};
uint8_t pending[64];size_t pendingSize=0,pendingSent=0;
uint8_t hostPending[64];size_t hostSize=0,hostSent=0;

uint16_t loadBridgeCdcDescriptor(uint8_t* destination, uint8_t* interfaceNumber) {
    const uint8_t name = tinyusb_add_string_descriptor(product::name);
    const uint8_t descriptor[] = {
        TUD_CDC_DESCRIPTOR(*interfaceNumber, name, 0x85, 64, 0x03, 0x84, 64)
    };
    memcpy(destination, descriptor, sizeof(descriptor));
    *interfaceNumber += 2;
    return sizeof(descriptor);
}

int bridgeUsbRead(uint8_t* data, size_t size) {
    // Leave RX in TinyUSB's FIFO until the bridge owns the UART. A full FIFO
    // backpressures the host instead of dropping bytes into a full Arduino queue.
    return tud_mounted() ? tud_cdc_read(data, size) : 0;
}

bool bridgeUsbHasInput() {
    return tud_mounted() && tud_cdc_available() != 0;
}

size_t bridgeUsbWrite(const uint8_t* data, size_t size) {
    if (!tud_mounted() || !tud_cdc_connected()) return 0;
    const size_t written = tud_cdc_write(data, size);
    tud_cdc_write_flush();
    return written;
}

void mode(Mode value){xSemaphoreTake(statusLock,portMAX_DELAY);status.mode=value;status.progress=0;xSemaphoreGive(statusLock);}
void bridgeTick(){
            int n=0;
            if(hostSent==hostSize){
                n=bridgeUsbRead(hostPending,sizeof(hostPending));hostSize=n>0?n:0;hostSent=0;
                if(n>0){usbInBytes.fetch_add(n);trafficPending.store(true);capture.append(hostPending,n,millis());arbiter.hostInput(millis());}
            }
            // Nonblocking FIFO write keeps UART RX serviced during full-duplex traffic.
            if(hostSent<hostSize){
                n=uart_tx_chars(UART_NUM_1,(const char*)hostPending+hostSent,hostSize-hostSent);
                if(n>0){hostSent+=n;uartTxBytes.fetch_add(n);lastTxMs.store(millis());trafficPending.store(true);}
            }
            capture.finish(millis());
            xSemaphoreTake(statusLock,portMAX_DELAY);status.replayBytes=capture.busy()?0:capture.size();xSemaphoreGive(statusLock);
            if (!tud_mounted() || !tud_cdc_connected()) {
                // Without a USB reader, leave the Paradise handshake in UART RX.
                pendingSize=pendingSent=0;
                return;
            }
            if(pendingSent==pendingSize){
                n=uart_read_bytes(UART_NUM_1,pending,sizeof(pending),0);pendingSize=n>0?n:0;pendingSent=0;
                if(n>0){uartRxBytes.fetch_add(n);lastRxMs.store(millis());trafficPending.store(true);}
            }
            if(pendingSent<pendingSize){
                auto wrote=bridgeUsbWrite(pending+pendingSent,pendingSize-pendingSent);
                pendingSent+=wrote;usbOutBytes.fetch_add(wrote);
                if(wrote)trafficPending.store(true);
            }
}
enum class Acquire { Ready, Cancelled, Busy };
Acquire acquireLocal(){
    const uint32_t start=millis();statusMessage("Waiting for computer");
    while(!cancelled.load()){
        // Grant before bridgeTick so an already-queued Tama handshake remains
        // available to the local protocol when the computer has been idle.
        if(arbiter.canPause(millis(),hostSent<hostSize||bridgeUsbHasInput())){
            if(uart_wait_tx_done(UART_NUM_1,pdMS_TO_TICKS(300))!=ESP_OK)return Acquire::Busy;
            arbiter.pause();mode(Mode::Standalone);return Acquire::Ready;
        }
        if(uint32_t(millis()-start)>=BridgeArbiter::waitMs)return Acquire::Busy;
        bridgeTick();vTaskDelay(1);
    }
    return Acquire::Cancelled;
}
void releaseLocal(){
    uart_wait_tx_done(UART_NUM_1,pdMS_TO_TICKS(300));
    // Discard only leftover local responses, never RX while forwarding bridge traffic.
    uart_flush_input(UART_NUM_1);
    arbiter.resume();mode(Mode::Bridge);
    // USB FIFO is never read during a local transaction: host data stays under
    // USB backpressure and is forwarded on the next bridgeTick, rather than lost.
}
void task(void*){
    Request req;
    for(;;){
        if(xQueueReceive(requests,&req,0)==pdTRUE){
            if(req.job==Job::UsbUpdate){
                trafficPending.store(true);
                const bool ok=updateFromUsb();
                vTaskDelay(pdMS_TO_TICKS(ok?1200:1600));
                if(ok)esp_restart();
                tud_cdc_read_flush();
                xSemaphoreTake(statusLock,portMAX_DELAY);
                status.usbUpdate=false;
                status.busy=false;
                xSemaphoreGive(statusLock);
                continue;
            }
            const auto acquired=acquireLocal();
            if(acquired!=Acquire::Ready){
                statusResult(false,acquired==Acquire::Cancelled?"Cancelled":"Computer busy. Try later.");
            }else{
                bool ok=false;const char* error="Timeout / check failed";
                if(req.job==Job::ReplayBytes){
                    capture.finish(millis());
                    if(capture.busy()||!capture.size())error="No complete data";
                    else ok=uartSend(capture.data(),capture.size());
                }else if(req.job==Job::Echo){
                    uart_flush_input(UART_NUM_1);statusMessage("Waiting for Tama");protocol.reset();ok=protocol.echo();
                }else if(req.item<catalogCount && statusSnapshot().assetOK){
                    const auto& item=catalogItems[req.item];
                    if(item.kind==0)ok=protocol.gift(item.id);
                    else if(item.kind==1||item.kind==4){uart_flush_input(UART_NUM_1);ok=protocol.download(item);}
                    else if(item.kind==2)ok=protocol.playdate(item);
                    if(!ok)error=protocol.error();
                    if(ok){xSemaphoreTake(statusLock,portMAX_DELAY);status.lastItem=req.item;xSemaphoreGive(statusLock);}
                }
                const bool wasCancelled=cancelled.load();
                if(wasCancelled){const uint8_t cancel[]={'C','A','N','\r','\n'};uartSend(cancel,sizeof(cancel));}
                releaseLocal(); // Always return UART to the background bridge.
                statusResult(ok&&!wasCancelled,wasCancelled?"Cancelled":ok?(req.job==Job::ReplayBytes?"Replay complete":"Send complete"):error);
            }
        }
        if(arbiter.bridgeActive())bridgeTick();
        vTaskDelay(1);
    }
}
}
Status statusSnapshot(){xSemaphoreTake(statusLock,portMAX_DELAY);auto copy=status;xSemaphoreGive(statusLock);return copy;}
bool takeCommunicationActivity(){
    bool active=trafficPending.exchange(false);
    // UART events report data even when no computer is draining the RX buffer.
    uart_event_t event;
    for(unsigned i=0;i<16 && xQueueReceive(uartEvents,&event,0)==pdTRUE;++i){
        if(event.type==UART_DATA || event.type==UART_BUFFER_FULL)active=true;
    }
    return active;
}
void statusMessage(const char* message,unsigned progress){xSemaphoreTake(statusLock,portMAX_DELAY);snprintf(status.message,sizeof(status.message),"%s",message);status.progress=progress;xSemaphoreGive(statusLock);}
void statusResult(bool success,const char* message){xSemaphoreTake(statusLock,portMAX_DELAY);status.success=success;status.busy=false;status.progress=success?100:status.progress;snprintf(status.message,sizeof(status.message),"%s",message);xSemaphoreGive(statusLock);}
void statusUpdateVersion(const char* version){
    xSemaphoreTake(statusLock,portMAX_DELAY);
    snprintf(status.updateVersion,sizeof(status.updateVersion),"%s",version);
    xSemaphoreGive(statusLock);
}
bool submit(Job job,uint16_t item){
    xSemaphoreTake(statusLock,portMAX_DELAY);
    if(status.busy){xSemaphoreGive(statusLock);return false;}
    status.busy=true;
    status.usbUpdate=job==Job::UsbUpdate;
    status.updateVersion[0]=0;
    status.success=false;
    status.progress=0;
    snprintf(status.message,sizeof(status.message),"%s","Starting");
    xSemaphoreGive(statusLock);
    cancelled=false;
    Request req{job,item};if(xQueueSend(requests,&req,0)==pdTRUE)return true;
    xSemaphoreTake(statusLock,portMAX_DELAY);
    status.usbUpdate=false;
    status.busy=false;
    xSemaphoreGive(statusLock);
    return false;
}
bool uartSend(const uint8_t* bytes,size_t count){
    if (!count) return true;
    const int written=uart_write_bytes(UART_NUM_1,bytes,count);
    if (written<=0) return false;
    uartTxBytes.fetch_add(written);
    lastTxMs.store(millis());
    trafficPending.store(true);
    return size_t(written)==count;
}
void communicationBegin(){
    statusLock=xSemaphoreCreateMutex();requests=xQueueCreate(1,sizeof(Request));configASSERT(statusLock&&requests);
    status.assetOK=assetsBegin();
    // Mount only: never auto-format a user's received ghost partition on failure.
    status.storageOK=LittleFS.begin(false,"/userdata",5,"userdata");
    uart_config_t config{};config.baud_rate=460800;config.data_bits=UART_DATA_8_BITS;config.parity=UART_PARITY_DISABLE;config.stop_bits=UART_STOP_BITS_1;config.flow_ctrl=UART_HW_FLOWCTRL_DISABLE;config.source_clk=UART_SCLK_XTAL;
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1,16384,0,16,&uartEvents,0));ESP_ERROR_CHECK(uart_param_config(UART_NUM_1,&config));ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1,pins::uartTx,pins::uartRx,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE));
    statusMessage(status.assetOK?"Bridge active":"Assets missing / corrupt");
    ESP_ERROR_CHECK(xTaskCreate(task,"tama-communication",12288,nullptr,2,nullptr)==pdPASS?ESP_OK:ESP_ERR_NO_MEM);
}

void communicationUsbBegin() {
    // Keep the Arduino TinyUSB PID distinct from the ROM USB Serial/JTAG PID.
    USB.PID(0x0002);
    USB.productName(product::name);
    if (tinyusb_enable_interface(USB_INTERFACE_CDC, TUD_CDC_DESC_LEN, loadBridgeCdcDescriptor) != ESP_OK) {
        statusMessage("USB bridge unavailable");
        return;
    }

    // Let the host register a disconnect before the new controller takes over D+/D-.
    const bool padWasEnabled = usb_serial_jtag_ll_pad_backup_and_disable();
    delay(300);
    if (!USB.begin()) {
        usb_serial_jtag_ll_enable_pad(padWasEnabled);
        statusMessage("USB bridge unavailable");
    }
}

extern "C" void tud_cdc_line_coding_cb(uint8_t interface, const cdc_line_coding_t* coding) {
    // The bridge uses a fixed UART rate. A 1200-baud USB request is for uploading.
    if (interface != 0) return;
    if (coding->bit_rate == 1200) rebootForUpload.store(true);
    if (coding->bit_rate == 2400) requestUsbUpdate.store(true);
}

void communicationUsbTick() {
    if(requestUsbUpdate.exchange(false))submit(Job::UsbUpdate);
    if (!rebootForUpload.exchange(false) || statusSnapshot().busy) return;
    uiPreparingUpdate();
    delay(600);
    usb_persist_restart(RESTART_BOOTLOADER);
}
