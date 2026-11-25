
#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

// ==========================================
// CYD ESP32-2432S028R configuration
// ==========================================

// Display pins
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  -1
#define TFT_BL   21

// Display dimensions
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

// SPI display
SPIClass displaySPI(HSPI);

Adafruit_ILI9341 tft(
    &displaySPI,
    TFT_DC,
    TFT_CS,
    TFT_RST
);

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

    Serial.begin(115200);

    // Backlight
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    // Initialize SPI
    displaySPI.begin(
        TFT_SCLK,
        TFT_MISO,
        TFT_MOSI,
        TFT_CS
    );

    // Initialize display
    tft.begin();
    tft.setRotation(1);

    delay(500);

    drawHeader();

    Serial.println("System Info initialized");
}

// ==========================================
// Main loop
// ==========================================

void loop() {

    // Clear previous readings
    tft.fillRect(
        0, 55,
        SCREEN_WIDTH, 100,
        ILI9341_BLACK
    );

    tft.setTextSize(2);
    tft.setTextColor(ILI9341_WHITE);

    // --------------------------------------
    // ESP32 internal temperature
    // --------------------------------------

    float tempC = temperatureRead();
    float tempF = (tempC * 9.0 / 5.0) + 32.0;

    tft.setCursor(8, 65);

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
tft.print("m\nchip Rev: ");
tft.print(chipRevision);



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

    delay(1000);
}
