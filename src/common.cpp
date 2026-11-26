
#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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



void setup_display() {

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
    tft.setRotation(DEFAULT_ROTATION);

    delay(500);

   // drawHeader();

    Serial.println("System Info initialized");
}

