
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

// SPI display
SPIClass displaySPI(HSPI);

Adafruit_ILI9341 tft(
  &displaySPI, TFT_DC, TFT_CS, TFT_RST
);

// Scan interval
const unsigned long SCAN_INTERVAL = 30000;
unsigned long lastScan = 0;

// Display dimensions (landscape)
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

// ==========================================
// Convert RSSI to percentage
// ==========================================

int signalPercent(int rssi) {
  if (rssi <= -100) {
    return 0;
  }
  if (rssi >= -50) {
    return 100;
  }
  return 2 * (rssi + 100);
}

const char* securityName(wifi_auth_mode_t security) {
  switch (security) {
    case WIFI_AUTH_OPEN:
      return "Open";
    case WIFI_AUTH_WEP:
      return "WEP";
    case WIFI_AUTH_WPA_PSK:
      return "WPA";
    case WIFI_AUTH_WPA2_PSK:
      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "WPA/WPA2";
    case WIFI_AUTH_ENTERPRISE:
      return "Enterprise";
    case WIFI_AUTH_WPA3_PSK:
      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "WPA2/3";
    case WIFI_AUTH_WAPI_PSK:
      return "WAPI";
    case WIFI_AUTH_OWE:
      return "OWE";
    case WIFI_AUTH_WPA3_ENT_192:
      return "WPA3-192";
    case WIFI_AUTH_WPA3_EXT_PSK:
      return "WPA3-EXT";
    case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE:
      return "WPA3-EXT2";
    default:
      return "Unknown";
  }
}

// ==========================================
// Draw screen header
// ==========================================

void drawHeader() {

  tft.fillScreen(ILI9341_BLACK);

  tft.fillRect(0, 0, 320, 30, ILI9341_BLUE);

  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(8, 7);

  tft.print("System Info");

  tft.setTextSize(1);
  tft.setTextColor(ILI9341_YELLOW);

  tft.setCursor(8, 36);
  tft.print("SSID");

  tft.setCursor(184, 36);
  tft.print("SIGNAL");

  tft.setCursor(263, 36);
  tft.print("SECURITY");

  tft.drawFastHLine(0, 48, 320, ILI9341_DARKGREY);
}

// ==========================================
// Display scan results
// ==========================================

void displayNetworks(int count) {

  drawHeader();

  tft.setTextSize(1);
  tft.setTextColor(ILI9341_CYAN);

  tft.setCursor(8, 224);
  tft.printf("Networks found: %d", count);

  if (count == 0) {

    tft.setTextSize(2);
    tft.setTextColor(ILI9341_RED);
    tft.setCursor(25, 105);
    tft.print("No networks found");

    return;
  }

  // Maximum rows fitting on screen
  int rows = min(count, 7);

  for (int i = 0; i < rows; i++) {

    int y = 53 + (i * 24);

    String ssid = WiFi.SSID(i);

    if (ssid.length() == 0) {
      ssid = "<Hidden>";
    }

    // Keep SSID within its column
    if (ssid.length() > 18) {
      ssid = ssid.substring(0, 17) + "~";
    }

    int rssi = WiFi.RSSI(i);
    int percent = signalPercent(rssi);

    const char* security =
      securityName(WiFi.encryptionType(i));

    // Network name
    tft.setTextSize(2);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(8, y);

    tft.print(ssid);

    // Signal strength
    tft.setTextSize(1);
    tft.setTextColor(
      rssi > -65 ? ILI9341_GREEN :
      rssi > -80 ? ILI9341_YELLOW :
                   ILI9341_RED
    );

    tft.setCursor(184, y + 5);
    tft.printf("%d%%", percent);

    // Security
    tft.setTextColor(ILI9341_CYAN);
    tft.setCursor(263, y + 5);
    tft.print(security);

    // Row separator
    tft.drawFastHLine(
      5, y + 21, 310, ILI9341_DARKGREY
    );
  }

  if (count > 7) {
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(200, 224);
    tft.printf("+ %d more", count - 7);
  }
}

// ==========================================
// Perform Wi-Fi scan
// ==========================================

void scanNetworks() {

  Serial.println();
  Serial.println("Scanning for Wi-Fi networks...");

  tft.fillScreen(ILI9341_BLACK);

  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(25, 95);
  tft.print("Scanning Wi-Fi...");

  int count = WiFi.scanNetworks(false, true);

  Serial.printf("Found %d networks\n", count);

  if (count >= 0) {

    for (int i = 0; i < count; i++) {

      Serial.printf(
        "%2d. %-32s RSSI: %4d dBm  %s\n",
        i + 1,
        WiFi.SSID(i).c_str(),
        (int)WiFi.RSSI(i),
        securityName(WiFi.encryptionType(i))
      );
    }

    displayNetworks(count);

  } else {

    tft.fillScreen(ILI9341_BLACK);
    tft.setTextColor(ILI9341_RED);
    tft.setTextSize(2);
    tft.setCursor(20, 100);
    tft.print("Scan failed");

    Serial.printf("Wi-Fi scan error: %d\n", count);
  }

  WiFi.scanDelete();

  lastScan = millis();
}

// ==========================================
// Setup
// ==========================================

void setup() {

  Serial.begin(115200);

  // Backlight
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  // Initialize display
  displaySPI.begin(
    TFT_SCLK,
    TFT_MISO,
    TFT_MOSI,
    TFT_CS
  );

  tft.begin();
  tft.setRotation(1);

  // Initialize Wi-Fi in station mode
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  delay(500);

  Serial.println("ESP32 CYD Wi-Fi Scanner");

  // Initial scan
  scanNetworks();
}

// ==========================================
// Main loop
// ==========================================

void loop() {

  if (millis() - lastScan >= SCAN_INTERVAL) {
    scanNetworks();
  }

  delay(100);
}
