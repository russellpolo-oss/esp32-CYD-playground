#include "defaults.h"
#include "common.cpp"
#include "SECRETS.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <TJpg_Decoder.h>
#include <time.h>
#include <vector>

namespace {

const unsigned long IMAGE_DISPLAY_MS = 10000;
const unsigned long WIFI_RETRY_MS = 15000;
const unsigned long PLAYLIST_RETRY_MS = 5000;
const unsigned long HTTP_TIMEOUT_MS = 20000;
const unsigned long CLOCK_SYNC_TIMEOUT_MS = 15000;
const unsigned long CLOCK_SYNC_RETRY_MS = 60000;
const size_t MAX_PLAYLIST_BYTES = 16 * 1024;
const size_t MAX_IMAGE_BYTES = 96 * 1024;
const size_t MAX_PLAYLIST_ITEMS = 128;

std::vector<String> imageUrls;
size_t currentImage = 0;
unsigned long lastImageDisplayed = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastPlaylistAttempt = 0;
bool playlistReady = false;
bool hasDisplayedImage = false;
bool wifiWasConnected = false;
bool clockSyncAttempted = false;
unsigned long lastClockSyncAttempt = 0;

void showMessage(const char *title, const char *detail = nullptr) {
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(2);
    tft.setTextWrap(true);
    tft.setCursor(8, 12);
    tft.println(title);
    if (detail != nullptr) {
        tft.println();
        tft.println(detail);
    }
}

bool ensureClockSynchronized() {
    if (time(nullptr) >= 1700000000) {
        return true;
    }

    unsigned long now = millis();
    if (clockSyncAttempted &&
        now - lastClockSyncAttempt < CLOCK_SYNC_RETRY_MS) {
        return false;
    }

    clockSyncAttempted = true;
    lastClockSyncAttempt = now;
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    unsigned long started = millis();
    while (millis() - started < CLOCK_SYNC_TIMEOUT_MS) {
        if (time(nullptr) >= 1700000000) {
            return true;
        }
        delay(250);
    }
    Serial.println("Clock sync failed; HTTPS requires valid system time");
    return false;
}

bool beginRequest(const String &url, HTTPClient &http,
    WiFiClient &plainClient, NetworkClientSecure &secureClient) {
    if (url.startsWith("https://")) {
        if (!ensureClockSynchronized()) {
            return false;
        }
        secureClient.useBuiltinCACertBundle();
        return http.begin(secureClient, url);
    }
    if (url.startsWith("http://")) {
        return http.begin(plainClient, url);
    }
    return false;
}

class VectorSink : public Stream {
public:
    VectorSink(std::vector<uint8_t> &output, size_t maxBytes)
        : output_(output), maxBytes_(maxBytes), overflowed_(false) {}

    size_t write(uint8_t value) override {
        return write(&value, 1);
    }

    size_t write(const uint8_t *data, size_t length) override {
        if (length > maxBytes_ - output_.size()) {
            overflowed_ = true;
            return 0;
        }
        output_.insert(output_.end(), data, data + length);
        return length;
    }

    int available() override {
        return 0;
    }

    int read() override {
        return -1;
    }

    int peek() override {
        return -1;
    }

    void flush() override {}

    bool overflowed() const {
        return overflowed_;
    }

private:
    std::vector<uint8_t> &output_;
    size_t maxBytes_;
    bool overflowed_;
};

bool readResponseBody(HTTPClient &http, size_t maxBytes,
    std::vector<uint8_t> &body) {
    int contentLength = http.getSize();
    if (contentLength > 0) {
        if ((size_t)contentLength > maxBytes) {
            Serial.println("Response exceeds configured size limit");
            return false;
        }
        body.reserve((size_t)contentLength);
    }

    VectorSink sink(body, maxBytes);
    int bytesWritten = http.writeToStream(&sink);
    if (sink.overflowed()) {
        Serial.println("Response exceeds configured size limit");
        return false;
    }
    if (bytesWritten < 0) {
        Serial.printf("Failed reading response body (%d)\n", bytesWritten);
        return false;
    }

    if (contentLength >= 0 && body.size() != (size_t)contentLength) {
        Serial.println("Incomplete response body");
        return false;
    }
    return !body.empty();
}

bool fetchResponse(const String &url, size_t maxBytes,
    std::vector<uint8_t> &body) {
    WiFiClient plainClient;
    NetworkClientSecure secureClient;
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!beginRequest(url, http, plainClient, secureClient)) {
        Serial.println("Invalid URL or failed to start HTTP request");
        return false;
    }

    int status = http.GET();
    if (status != HTTP_CODE_OK) {
        Serial.printf("HTTP request failed (status %d)\n", status);
        http.end();
        return false;
    }

    bool success = readResponseBody(http, maxBytes, body);
    http.end();
    return success;
}

bool fetchPlaylist() {
    std::vector<uint8_t> response;
    if (!fetchResponse(String(IMAGE_URL), MAX_PLAYLIST_BYTES, response)) {
        showMessage("Could not load playlist", "Retrying shortly");
        return false;
    }

    JsonDocument document;
    DeserializationError error = deserializeJson(
        document, response.data(), response.size());
    if (error) {
        Serial.printf("Playlist JSON error: %s\n", error.c_str());
        showMessage("Invalid playlist JSON", "Check the playlist URL");
        return false;
    }

    JsonArrayConst urls = document.as<JsonArrayConst>();
    if (urls.isNull() || urls.size() == 0 ||
        urls.size() > MAX_PLAYLIST_ITEMS) {
        Serial.println("Playlist must be a non-empty JSON array of URLs");
        showMessage("Invalid playlist", "Expected an array of image URLs");
        return false;
    }

    std::vector<String> loadedUrls;
    loadedUrls.reserve(urls.size());
    for (JsonVariantConst entry : urls) {
        if (!entry.is<const char *>()) {
            Serial.println("Playlist contains a non-string entry");
            showMessage("Invalid playlist", "Entries must be URL strings");
            return false;
        }

        String url = entry.as<String>();
        if (!url.startsWith("http://") && !url.startsWith("https://")) {
            Serial.println("Playlist contains an unsupported URL");
            showMessage("Invalid image URL", "Use HTTP or HTTPS URLs");
            return false;
        }
        loadedUrls.push_back(url);
    }

    imageUrls.swap(loadedUrls);
    Serial.printf("Loaded %u image URLs\n", (unsigned)imageUrls.size());
    return true;
}

bool jpgOutput(int16_t x, int16_t y, uint16_t width, uint16_t height,
    uint16_t *pixels) {
    tft.drawRGBBitmap(x, y, pixels, width, height);
    return true;
}

bool displayImage(const String &url) {
    showMessage("Loading image");
    std::vector<uint8_t> image;
    if (!fetchResponse(url, MAX_IMAGE_BYTES, image)) {
        showMessage("Image download failed", "Skipping to next image");
        return false;
    }

    tft.fillScreen(ILI9341_BLACK);
    JRESULT result = TJpgDec.drawJpg(
        0, 0, image.data(), (uint32_t)image.size());
    if (result != JDR_OK) {
        Serial.printf("JPEG decode failed (result %d)\n", (int)result);
        showMessage("Unsupported image", "Expected a JPEG image");
        return false;
    }
    return true;
}

void startWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(SSID, PASSWORD);
    lastWifiAttempt = millis();
    Serial.println("Connecting to Wi-Fi");
    showMessage("Connecting to Wi-Fi");
}

} // namespace

void setup() {
    setup_display();
    TJpgDec.setJpgScale(1);
    TJpgDec.setCallback(jpgOutput);
    startWifi();
}

void loop() {
    unsigned long now = millis();
    if (WiFi.status() != WL_CONNECTED) {
        wifiWasConnected = false;
        if (now - lastWifiAttempt >= WIFI_RETRY_MS) {
            WiFi.disconnect();
            WiFi.begin(SSID, PASSWORD);
            lastWifiAttempt = now;
            Serial.println("Retrying Wi-Fi connection");
            showMessage("Connecting to Wi-Fi");
        }
        delay(100);
        return;
    }

    if (!wifiWasConnected) {
        wifiWasConnected = true;
        Serial.print("Wi-Fi connected; IP: ");
        Serial.println(WiFi.localIP());
        playlistReady = false;
        lastPlaylistAttempt = 0;
    }

    if (!playlistReady) {
        if (now - lastPlaylistAttempt < PLAYLIST_RETRY_MS) {
            delay(100);
            return;
        }
        lastPlaylistAttempt = now;
        playlistReady = fetchPlaylist();
        if (playlistReady) {
            currentImage = 0;
            hasDisplayedImage = false;
        } else {
            delay(100);
            return;
        }
    }

    if (hasDisplayedImage &&
        now - lastImageDisplayed < IMAGE_DISPLAY_MS) {
        delay(25);
        return;
    }

    displayImage(imageUrls[currentImage]);
    lastImageDisplayed = millis();
    hasDisplayedImage = true;
    currentImage = (currentImage + 1) % imageUrls.size();
}