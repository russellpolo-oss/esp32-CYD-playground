#include <Arduino.h>
#include <stdlib.h>

void dump_stats_serial() {
    float tempC = temperatureRead();
    float tempF = (tempC * 9.0 / 5.0) + 32.0;
    uint32_t heapTotal = ESP.getHeapSize();
    uint32_t heapFree = ESP.getFreeHeap();
    float heapPercent = (heapFree * 100.0) / heapTotal;
    uint32_t cpuFreq = ESP.getCpuFreqMHz();
    uint32_t flashSizeMb = ESP.getFlashChipSize() / (1024 * 1024);
    uint32_t uptime = millis() / 1000;
    uint32_t chipRevision = ESP.getChipRevision();

    Serial.printf(
        "Temp: %.1f C / %.1f F | "
        "Heap: %lu / %lu bytes (%.1f%% free)\n"
        "CPU: %lu MHz | Flash: %lu MB\n"
        "Uptime: %lu h %lu m\n"
        "Chip Rev: %lu\n",
        tempC,
        tempF,
        static_cast<unsigned long>(heapFree),
        static_cast<unsigned long>(heapTotal),
        heapPercent,
        static_cast<unsigned long>(cpuFreq),
        static_cast<unsigned long>(flashSizeMb),
        uptime / 3600UL,
        (uptime % 3600UL) / 60UL,
        static_cast<unsigned long>(chipRevision)
    );

    char *taskStats = static_cast<char *>(malloc(2048));
    if (taskStats == nullptr) {
        Serial.println("Task runtime stats unavailable: allocation failed");
        return;
    }

    vTaskGetRunTimeStats(taskStats);
    Serial.println("Task runtime stats:");
    Serial.println(taskStats);
    free(taskStats);
}
