
// ==========================================
// Common CYD configuration and setup
// ==========================================
#include "defaults.h"
#include "common.cpp"

// ==========================================
// Draw screen header
// ==========================================

void drawHeader() {

    tft.fillScreen(ILI9341_BLACK);

    tft.fillRect(
        0, 0,
        SCREEN_WIDTH, 45,
        ILI9341_BLUE
    );

    tft.setTextSize(2);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(8, 13);

    tft.print("System Info");

    tft.drawFastHLine(
        0, 48,
        SCREEN_WIDTH,
        ILI9341_DARKGREY
    );
}

// ==========================================
// Initialize
// ==========================================

void setup() {

    setup_display();

    drawHeader();
}

void printTaskStats() {

    char *stats = (char *)malloc(2048);

    if (!stats) return;

    vTaskGetRunTimeStats(stats);

    char *line = strtok(stats, "\n");

    tft.setTextSize(1);
    tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);

    while (line != NULL) {

        // Skip leading whitespace and control characters
        while (*line && (unsigned char)*line <= 32) {
            line++;
        }

        if (*line) {

            // Replace tabs and repeated whitespace with single spaces
            char cleaned[80];
            int out = 0;
            bool lastSpace = false;

            while (*line && out < sizeof(cleaned) - 1) {

                unsigned char c = *line++;

                if (c <= 32) {
                    if (!lastSpace) {
                        cleaned[out++] = ' ';
                        lastSpace = true;
                    }
                }
                else {
                    cleaned[out++] = c;
                    lastSpace = false;
                }
            }

            cleaned[out] = '\0';

            tft.println(cleaned);
        }

        line = strtok(NULL, "\n");
    }

    free(stats);
}

// ==========================================
// Main loop
// ==========================================

void loop() {

    // Clear previous readings
    tft.fillRect(
        0, 0,
        SCREEN_WIDTH, SCREEN_HEIGHT ,
        ILI9341_BLACK
    );

    tft.setTextSize(2);
    tft.setTextColor(ILI9341_WHITE);

    // --------------------------------------
    // ESP32 internal temperature
    // --------------------------------------

    float tempC = temperatureRead();
    float tempF = (tempC * 9.0 / 5.0) + 32.0;

    tft.setCursor(0, 0);

    tft.print("Temp: ");
    tft.print(tempC, 1);
    tft.print(" C / ");
    tft.print(tempF, 1);
    tft.println(" F");

    // --------------------------------------
    // Heap memory
    // --------------------------------------

    uint32_t heapTotal = ESP.getHeapSize();
    uint32_t heapFree = ESP.getFreeHeap();

    float heapPercent =
        (heapFree * 100.0) / heapTotal;

   

    tft.print("Heap:");
    tft.print(heapFree / 1024);
    tft.print("/");
    tft.print(heapTotal / 1024);
    tft.print("KB");

    

    tft.print(" Free: ");
    tft.print(heapPercent, 1);
    tft.println("%");

//Minimum free RAM	ESP.getMinFreeHeap()
uint32_t minfree = ESP.getMinFreeHeap();
//CPU frequency	ESP.getCpuFreqMHz()
uint32_t cpuFreq = ESP.getCpuFreqMHz();
//Flash size	ESP.getFlashChipSize()
uint32_t flashSize = ESP.getFlashChipSize();
//Wi-Fi RSSI	WiFi.RSSI()
//Uptime	millis()
uint32_t uptime = millis() / 1000; // in seconds
//Chip revision	ESP.getChipRevision()
uint32_t chipRevision = ESP.getChipRevision();

tft.print("Min Free: ");
tft.print(minfree / 1024);  

tft.print("KB | \nCPU: ");
tft.print(cpuFreq);
tft.print(" MHz | Flash: ");
tft.print(flashSize / (1024 * 1024));   
tft.print(" MB\nUptime: ");
tft.print(uptime / 3600); // hours
tft.print("h ");
tft.print((uptime % 3600) / 60); // minutes 
//tft.print("m\nchip Rev: ");
//tft.print(chipRevision);
tft.println("m");
printTaskStats();


    // --------------------------------------
    // Serial diagnostics
    // --------------------------------------

    Serial.printf(
        "Temp: %.1f C / %.1f F | "
        "Heap: %u / %u bytes (%.1f%% free) \n"
        "CPU: %u MHz | Flash: %u MB\n"
        "Uptime: %u h %u m\n"
        "Chip Rev: %u\n",
        tempC,
        tempF,
        heapFree,
        heapTotal,
        heapPercent,
        cpuFreq, 
        flashSize,
        uptime/3600,
        (uptime % 3600) / 60,
        chipRevision
    );

    delay(10000);
}
