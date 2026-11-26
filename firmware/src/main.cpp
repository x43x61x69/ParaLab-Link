#include "core.h"
#include "esp_log.h"

void setup() {
    esp_log_level_set("*", ESP_LOG_NONE);
    communicationBegin();
    uiBegin();
    communicationUsbBegin();
}

void loop() {
    communicationUsbTick();
    uiTick();
    vTaskDelay(pdMS_TO_TICKS(10));
}
