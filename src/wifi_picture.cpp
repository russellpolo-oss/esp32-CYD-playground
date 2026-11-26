//requires cydimages.php to be running on a web server
//url, wifi ssid and password must be set in SECRETS.h
// pulls json file from webserver, parses it and displays the images in a slideshow



#include "defaults.h"
#include "common.cpp"
#include "common_stats.cpp"
#include "SECRETS.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <TJpg_Decoder.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <new>
#include <time.h>
#include <vector>

namespace {

const unsigned long IMAGE_DISPLAY_MS = 10000;
const unsigned long STATS_DUMP_INTERVAL_MS = 10000;
const unsigned long WIFI_RETRY_MS = 15000;
const unsigned long PLAYLIST_RETRY_MS = 5000;
const unsigned long PLAYLIST_REFRESH_MS = 60UL * 60UL * 1000UL;
const unsigned long HTTP_TIMEOUT_MS = 20000;
const unsigned long CLOCK_SYNC_TIMEOUT_MS = 15000;
const unsigned long CLOCK_SYNC_RETRY_MS = 60000;
const size_t MAX_PLAYLIST_BYTES = 16 * 1024;
const size_t MAX_IMAGE_BYTES = 96 * 1024;
const size_t MAX_PLAYLIST_ITEMS = 128;
const unsigned long PREFETCH_RETRY_MS = 1000;

struct PrefetchRequest {
    size_t imageIndex;
    String url;
};

struct PrefetchResult {
    size_t imageIndex;
    bool success;
    std::vector<uint8_t> *image;
};

std::vector<String> imageUrls;
size_t nextImageIndex = 0;
unsigned long lastImageDisplayed = 0;
unsigned long lastStatsDump = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastPlaylistAttempt = 0;
unsigned long lastPlaylistRefresh = 0;
unsigned long lastPrefetchFailure = 0;
QueueHandle_t prefetchQueue = nullptr;
PrefetchResult readyImage = {0, false, nullptr};
bool playlistReady = false;
bool hasDisplayedImage = false;
bool wifiWasConnected = false;
bool prefetchInProgress = false;
bool hasReadyImage = false;
bool prefetchRetryScheduled = false;
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

void prefetchImageTask(void *parameter) {
    PrefetchRequest *request = static_cast<PrefetchRequest *>(parameter);
    std::vector<uint8_t> *image =
        new (std::nothrow) std::vector<uint8_t>();
    bool success = image != nullptr &&
        fetchResponse(request->url, MAX_IMAGE_BYTES, *image);

    if (!success) {
        delete image;
        image = nullptr;
    }

    PrefetchResult result = {request->imageIndex, success, image};
    if (xQueueSend(prefetchQueue, &result, portMAX_DELAY) != pdTRUE) {
        delete image;
    }

    delete request;
    vTaskDelete(nullptr);
}

void startImagePrefetch() {
    if (prefetchInProgress || hasReadyImage || imageUrls.empty() ||
        (prefetchRetryScheduled &&
            millis() - lastPrefetchFailure < PREFETCH_RETRY_MS)) {
        return;
    }

    PrefetchRequest *request = new (std::nothrow) PrefetchRequest();
    if (request == nullptr) {
        Serial.println("Unable to allocate image prefetch request");
        lastPrefetchFailure = millis();
        prefetchRetryScheduled = true;
        return;
    }

    request->imageIndex = nextImageIndex;
    request->url = imageUrls[nextImageIndex];
    if (xTaskCreate(prefetchImageTask, "image-prefetch", 12288,
        request, 1, nullptr) != pdPASS) {
        delete request;
        Serial.println("Unable to start image prefetch task");
        lastPrefetchFailure = millis();
        prefetchRetryScheduled = true;
        return;
    }

    prefetchInProgress = true;
    prefetchRetryScheduled = false;
}

void receivePrefetchResult() {
    PrefetchResult result = {0, false, nullptr};
    if (xQueueReceive(prefetchQueue, &result, 0) != pdTRUE) {
        return;
    }
    prefetchInProgress = false;

    if (result.success && result.image != nullptr) {
        readyImage = result;
        hasReadyImage = true;
        return;
    }

    delete result.image;
    nextImageIndex = (result.imageIndex + 1) % imageUrls.size();
    lastPrefetchFailure = millis();
    prefetchRetryScheduled = true;
    Serial.println("Image prefetch failed; trying the next URL");
    if (!hasDisplayedImage) {
        showMessage("Image download failed", "Trying next image");
    }
}

bool fetchPlaylist(bool showErrors = true) {
    std::vector<uint8_t> response;
    if (!fetchResponse(String(IMAGE_URL), MAX_PLAYLIST_BYTES, response)) {
        if (showErrors) {
            showMessage("Could not load playlist", "Retrying shortly");
        }
        return false;
    }

    JsonDocument document;
    DeserializationError error = deserializeJson(
        document, response.data(), response.size());
    if (error) {
        Serial.printf("Playlist JSON error: %s\n", error.c_str());
        if (showErrors) {
            showMessage("Invalid playlist JSON", "Check the playlist URL");
        }
        return false;
    }

    JsonArrayConst urls = document.as<JsonArrayConst>();
    if (urls.isNull() || urls.size() == 0 ||
        urls.size() > MAX_PLAYLIST_ITEMS) {
        Serial.println("Playlist must be a non-empty JSON array of URLs");
        if (showErrors) {
            showMessage("Invalid playlist", "Expected an array of image URLs");
        }
        return false;
    }

    std::vector<String> loadedUrls;
    loadedUrls.reserve(urls.size());
    for (JsonVariantConst entry : urls) {
        if (!entry.is<const char *>()) {
            Serial.println("Playlist contains a non-string entry");
            if (showErrors) {
                showMessage("Invalid playlist", "Entries must be URL strings");
            }
            return false;
        }

        String url = entry.as<String>();
        if (!url.startsWith("http://") && !url.startsWith("https://")) {
            Serial.println("Playlist contains an unsupported URL");
            if (showErrors) {
                showMessage("Invalid image URL", "Use HTTP or HTTPS URLs");
            }
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

bool displayImage(std::vector<uint8_t> &image) {
    if (!hasDisplayedImage) {
        tft.fillScreen(ILI9341_BLACK);
    }
    JRESULT result = TJpgDec.drawJpg(
        0, 0, image.data(), (uint32_t)image.size());
    if (result != JDR_OK) {
        Serial.printf("JPEG decode failed (result %d)\n", (int)result);
        return false;
    }
    return true;
}

void displayReadyImage() {
    if (!hasReadyImage ||
        (hasDisplayedImage &&
            millis() - lastImageDisplayed < IMAGE_DISPLAY_MS)) {
        return;
    }

    size_t displayedIndex = readyImage.imageIndex;
    bool displayed = readyImage.success && readyImage.image != nullptr &&
        displayImage(*readyImage.image);
    delete readyImage.image;
    readyImage = {0, false, nullptr};
    hasReadyImage = false;

    if (displayed) {
        hasDisplayedImage = true;
        lastImageDisplayed = millis();
        nextImageIndex = (displayedIndex + 1) % imageUrls.size();
        prefetchRetryScheduled = false;
        return;
    }

    nextImageIndex = (displayedIndex + 1) % imageUrls.size();
    lastPrefetchFailure = millis();
    prefetchRetryScheduled = true;
    Serial.println("Unable to decode image; trying the next URL");
    if (!hasDisplayedImage) {
        showMessage("Unsupported image", "Expected a JPEG image");
    }
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
    prefetchQueue = xQueueCreate(1, sizeof(PrefetchResult));
    if (prefetchQueue == nullptr) {
        Serial.println("Unable to create image prefetch queue");
        showMessage("Image viewer error", "Could not create queue");
        return;
    }
    startWifi();
}

void loop() {
    if (prefetchQueue == nullptr) {
        delay(1000);
        return;
    }

    receivePrefetchResult();
    displayReadyImage();

    unsigned long now = millis();
    if (now - lastStatsDump >= STATS_DUMP_INTERVAL_MS) {
        dump_stats_serial();
        lastStatsDump = now;
    }

    if (WiFi.status() != WL_CONNECTED) {
        wifiWasConnected = false;
        if (now - lastWifiAttempt >= WIFI_RETRY_MS) {
            WiFi.disconnect();
            WiFi.begin(SSID, PASSWORD);
            lastWifiAttempt = now;
            Serial.println("Retrying Wi-Fi connection");
            if (!hasDisplayedImage) {
                showMessage("Connecting to Wi-Fi");
            }
        }
        delay(100);
        return;
    }

    if (!wifiWasConnected) {
        wifiWasConnected = true;
        Serial.print("Wi-Fi connected; IP: ");
        Serial.println(WiFi.localIP());
        if (!playlistReady) {
            lastPlaylistAttempt = 0;
        }
    }

    if (!playlistReady) {
        if (now - lastPlaylistAttempt < PLAYLIST_RETRY_MS) {
            delay(100);
            return;
        }
        lastPlaylistAttempt = now;
        playlistReady = fetchPlaylist();
        if (playlistReady) {
            nextImageIndex = 0;
            hasDisplayedImage = false;
            lastPlaylistRefresh = millis();
        } else {
            delay(100);
            return;
        }
    }

    if (now - lastPlaylistRefresh >= PLAYLIST_REFRESH_MS &&
        !prefetchInProgress && !hasReadyImage) {
        lastPlaylistRefresh = now;
        if (fetchPlaylist(false)) {
            nextImageIndex %= imageUrls.size();
            Serial.println("Playlist refreshed");
        } else {
            Serial.println("Playlist refresh failed; retaining current list");
        }
    }

    if (playlistReady) {
        startImagePrefetch();
    }
    delay(25);
}