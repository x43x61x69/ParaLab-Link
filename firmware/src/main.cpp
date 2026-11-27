#include "core.h"
#include "esp_log.h"
#include "link_version.h"

void setup() {
    esp_log_level_set("*", ESP_LOG_NONE);
    product::begin();
    communicationBegin();
    uiBegin();
    communicationUsbBegin();
}

void loop() {
    communicationUsbTick();
    uiTick();
    vTaskDelay(pdMS_TO_TICKS(10));
}
