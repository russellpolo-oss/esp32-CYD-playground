#include "defaults.h"
#include "common.cpp"
#include "common_touch.cpp"
#include "common_stats.cpp"

#include <algorithm>
#include <math.h>
#include <stdint.h>

namespace {

const uint16_t MAX_ITERATIONS = 255;
const int TILE_SIZE = 20;
const size_t PIXEL_COUNT = SCREEN_WIDTH * SCREEN_HEIGHT;
const size_t COMPUTED_MAP_BYTES = (PIXEL_COUNT + 7) / 8;
const size_t MAX_BOX_STACK = 64;
const unsigned long STATS_INTERVAL_MS = 10000;
const uint8_t BOXES_PER_LOOP = 48;
const int ZOOM_ANCHOR_X = SCREEN_WIDTH / 2;
const int ZOOM_ANCHOR_Y = SCREEN_HEIGHT / 2;
const int AUTO_BUTTON_SIZE = 30;
const int AUTO_BUTTON_X = SCREEN_WIDTH - AUTO_BUTTON_SIZE;
const int AUTO_BUTTON_Y = SCREEN_HEIGHT - AUTO_BUTTON_SIZE;

struct Viewport {
    double realMin;
    double realMax;
    double imaginaryMin;
    double imaginaryMax;
};

struct Box {
    int x0;
    int y0;
    int x1;
    int y1;
};

Viewport viewport = {-2.0, 2.0, -2.0, 2.0};
uint8_t iterationValues[PIXEL_COUNT];
uint8_t computedMap[COMPUTED_MAP_BYTES];
Box boxStack[MAX_BOX_STACK];
size_t boxStackSize = 0;
int nextTileX = 0;
int nextTileY = 0;
int reuseStartX = 0;
int reuseStartY = 0;
int reuseWidth = 0;
int reuseHeight = 0;
unsigned long lastStatsDump = 0;
unsigned long imageStartedAt = 0;
bool imageComplete = false;
bool touchWasDown = false;
bool autoZoomEnabled = false;

uint8_t mandelbrotValue(double real, double imaginary) {
    double zReal = 0.0;
    double zImaginary = 0.0;
    uint16_t iteration = 0;

    while (iteration < MAX_ITERATIONS &&
        zReal * zReal + zImaginary * zImaginary <= 4.0) {
        double nextReal = zReal * zReal - zImaginary * zImaginary + real;
        zImaginary = 2.0 * zReal * zImaginary + imaginary;
        zReal = nextReal;
        ++iteration;
    }

    return static_cast<uint8_t>(iteration);
}

void pixelCoordinatesToComplex(int x, int y, double &real,
    double &imaginary) {
    real = viewport.realMin +
        (viewport.realMax - viewport.realMin) * x / (SCREEN_WIDTH - 1);
    imaginary = viewport.imaginaryMin +
        (viewport.imaginaryMax - viewport.imaginaryMin) * y /
            (SCREEN_HEIGHT - 1);
}

bool pixelIsComputed(size_t index) {
    return (computedMap[index >> 3] & (1U << (index & 7))) != 0;
}

void markPixelComputed(size_t index) {
    computedMap[index >> 3] |= static_cast<uint8_t>(1U << (index & 7));
}

uint8_t iterationAtPixel(int x, int y) {
    size_t index = static_cast<size_t>(y) * SCREEN_WIDTH + x;
    if (pixelIsComputed(index)) {
        return iterationValues[index];
    }

    double real;
    double imaginary;
    pixelCoordinatesToComplex(x, y, real, imaginary);

    uint8_t value = mandelbrotValue(real, imaginary);
    iterationValues[index] = value;
    markPixelComputed(index);
    return value;
}

uint16_t colorForIteration(uint8_t iteration) {
    if (iteration == MAX_ITERATIONS) {
        return ILI9341_BLACK;
    }

    uint8_t red = static_cast<uint8_t>(iteration * 13);
    uint8_t green = static_cast<uint8_t>(iteration * 7);
    uint8_t blue = static_cast<uint8_t>(iteration * 3);
    return tft.color565(red, green, blue);
}

void drawAutoZoomButton() {
    uint16_t background = autoZoomEnabled ? ILI9341_WHITE : ILI9341_BLACK;
    uint16_t foreground = autoZoomEnabled ? ILI9341_BLACK : ILI9341_WHITE;
    tft.fillRect(AUTO_BUTTON_X, AUTO_BUTTON_Y, AUTO_BUTTON_SIZE,
        AUTO_BUTTON_SIZE, background);
    tft.setTextSize(2);
    tft.setTextColor(foreground, background);
    tft.setCursor(AUTO_BUTTON_X + 9, AUTO_BUTTON_Y + 6);
    tft.print('A');
}

void drawFractalBox(const Box &box, uint16_t color) {
    if (box.x1 < AUTO_BUTTON_X || box.y1 < AUTO_BUTTON_Y) {
        tft.fillRect(box.x0, box.y0, box.x1 - box.x0 + 1,
            box.y1 - box.y0 + 1, color);
        return;
    }

    if (box.y0 < AUTO_BUTTON_Y) {
        int topHeight = AUTO_BUTTON_Y - box.y0;
        tft.fillRect(box.x0, box.y0, box.x1 - box.x0 + 1,
            topHeight, color);
    }

    if (box.x0 < AUTO_BUTTON_X) {
        int leftWidth = AUTO_BUTTON_X - box.x0;
        int bottomY = max(box.y0, AUTO_BUTTON_Y);
        tft.fillRect(box.x0, bottomY, leftWidth,
            box.y1 - bottomY + 1, color);
    }
}

void drawFractalPixel(int x, int y, uint16_t color) {
    if (x < AUTO_BUTTON_X || y < AUTO_BUTTON_Y) {
        tft.drawPixel(x, y, color);
    }
}

void fillBox(const Box &box, uint8_t iteration) {
    for (int y = box.y0; y <= box.y1; ++y) {
        size_t row = static_cast<size_t>(y) * SCREEN_WIDTH;
        for (int x = box.x0; x <= box.x1; ++x) {
            size_t index = row + x;
            iterationValues[index] = iteration;
            markPixelComputed(index);
        }
    }
    drawFractalBox(box, colorForIteration(iteration));
}

void pushBox(int x0, int y0, int x1, int y1) {
    if (x0 > x1 || y0 > y1) {
        return;
    }
    if (boxStackSize >= MAX_BOX_STACK) {
        Serial.println("Fractal subdivision stack capacity exceeded");
        return;
    }
    boxStack[boxStackSize++] = {x0, y0, x1, y1};
}

bool nextRootBox(Box &box) {
    if (nextTileY >= SCREEN_HEIGHT) {
        return false;
    }

    box.x0 = nextTileX;
    box.y0 = nextTileY;
    box.x1 = min(nextTileX + TILE_SIZE - 1, SCREEN_WIDTH - 1);
    box.y1 = min(nextTileY + TILE_SIZE - 1, SCREEN_HEIGHT - 1);

    nextTileX += TILE_SIZE;
    if (nextTileX >= SCREEN_WIDTH) {
        nextTileX = 0;
        nextTileY += TILE_SIZE;
    }
    return true;
}

void processBox(const Box &box) {
    uint8_t topLeft = iterationAtPixel(box.x0, box.y0);
    uint8_t topRight = iterationAtPixel(box.x1, box.y0);
    uint8_t bottomLeft = iterationAtPixel(box.x0, box.y1);
    uint8_t bottomRight = iterationAtPixel(box.x1, box.y1);

    if (topLeft == topRight && topLeft == bottomLeft &&
        topLeft == bottomRight) {
        fillBox(box, topLeft);
        return;
    }

    int middleX = (box.x0 + box.x1) / 2;
    int middleY = (box.y0 + box.y1) / 2;
    uint8_t center = iterationAtPixel(middleX, middleY);
    drawFractalPixel(middleX, middleY, colorForIteration(center));

    pushBox(middleX + 1, middleY + 1, box.x1, box.y1);
    pushBox(box.x0, middleY + 1, middleX, box.y1);
    pushBox(middleX + 1, box.y0, box.x1, middleY);
    pushBox(box.x0, box.y0, middleX, middleY);
}

void reportImageComplete() {
    imageComplete = true;

    double centerReal = (viewport.realMin + viewport.realMax) * 0.5;
    double centerImaginary =
        (viewport.imaginaryMin + viewport.imaginaryMax) * 0.5;
    double realRadius = (viewport.realMax - viewport.realMin) * 0.5;
    double imaginaryRadius =
        (viewport.imaginaryMax - viewport.imaginaryMin) * 0.5;
    double visibleRadius = realRadius < imaginaryRadius
        ? realRadius : imaginaryRadius;

    Serial.printf(
        "Mandelbrot image complete in %lu ms\n"
        "Center: real=%.10f imaginary=%.10f | visible radius=%.8e\n",
        millis() - imageStartedAt,
        centerReal,
        centerImaginary,
        visibleRadius
    );
}

int countDistinctResults(int x0, int y0, int x1, int y1) {
    uint32_t seen[8] = {};
    int distinctCount = 0;
    for (int y = y0; y <= y1; ++y) {
        size_t row = static_cast<size_t>(y) * SCREEN_WIDTH;
        for (int x = x0; x <= x1; ++x) {
            uint8_t iteration = iterationValues[row + x];
            uint32_t mask = 1UL << (iteration & 31);
            uint32_t &bucket = seen[iteration >> 5];
            if ((bucket & mask) == 0) {
                bucket |= mask;
                ++distinctCount;
            }
        }
    }
    return distinctCount;
}

void findMostComplexTile(int &zoomX, int &zoomY, int &bestScore) {
    bestScore = -1;
    long bestDistance = LONG_MAX;

    for (int y0 = 0; y0 < SCREEN_HEIGHT; y0 += TILE_SIZE) {
        int y1 = min(y0 + TILE_SIZE - 1, SCREEN_HEIGHT - 1);
        for (int x0 = 0; x0 < SCREEN_WIDTH; x0 += TILE_SIZE) {
            int x1 = min(x0 + TILE_SIZE - 1, SCREEN_WIDTH - 1);
            int score = countDistinctResults(x0, y0, x1, y1);
            long dx = x0 + x1 - (SCREEN_WIDTH - 1);
            long dy = y0 + y1 - (SCREEN_HEIGHT - 1);
            long distance = dx * dx + dy * dy;

            if (score > bestScore ||
                (score == bestScore && distance < bestDistance)) {
                bestScore = score;
                bestDistance = distance;
                zoomX = (x0 + x1) / 2;
                zoomY = (y0 + y1) / 2;
            }
        }
    }
}

void processImage(uint8_t boxBudget) {
    for (uint8_t processed = 0; processed < boxBudget && !imageComplete;
        ++processed) {
        Box box;
        if (boxStackSize > 0) {
            box = boxStack[--boxStackSize];
        } else if (!nextRootBox(box)) {
            reportImageComplete();
            break;
        }
        processBox(box);
    }

    if (!imageComplete && boxStackSize == 0 &&
        nextTileY >= SCREEN_HEIGHT) {
        reportImageComplete();
    }
}

void resetRenderState() {
    boxStackSize = 0;
    nextTileX = 0;
    nextTileY = 0;
    imageComplete = false;
    tft.fillScreen(ILI9341_BLACK);
    drawAutoZoomButton();
    imageStartedAt = millis();
}

void beginImage() {
    memset(computedMap, 0, sizeof(computedMap));
    resetRenderState();
}

void zoomAtPixel(int x, int y) {
    double centerReal;
    double centerImaginary;
    pixelCoordinatesToComplex(x, y, centerReal, centerImaginary);

    reuseStartX = max(0, ZOOM_ANCHOR_X / 2 - x);
    int lastReuseX = min(SCREEN_WIDTH / 2 - 1,
        SCREEN_WIDTH - 1 - x + ZOOM_ANCHOR_X / 2);
    reuseStartY = max(0, ZOOM_ANCHOR_Y / 2 - y);
    int lastReuseY = min(SCREEN_HEIGHT / 2 - 1,
        SCREEN_HEIGHT - 1 - y + ZOOM_ANCHOR_Y / 2);
    reuseWidth = lastReuseX - reuseStartX + 1;
    reuseHeight = lastReuseY - reuseStartY + 1;

    size_t packedCount = 0;
    for (int cacheY = reuseStartY; cacheY <= lastReuseY; ++cacheY) {
        int oldY = y + cacheY - ZOOM_ANCHOR_Y / 2;
        for (int cacheX = reuseStartX; cacheX <= lastReuseX; ++cacheX) {
            int oldX = x + cacheX - ZOOM_ANCHOR_X / 2;
            iterationValues[packedCount++] =
                iterationValues[oldY * SCREEN_WIDTH + oldX];
        }
    }

    std::fill(computedMap, computedMap + COMPUTED_MAP_BYTES, 0);
    for (size_t packedIndex = packedCount; packedIndex > 0; --packedIndex) {
        size_t cacheIndex = packedIndex - 1;
        int cacheX = reuseStartX + cacheIndex % reuseWidth;
        int cacheY = reuseStartY + cacheIndex / reuseWidth;
        int screenX = cacheX * 2;
        int screenY = cacheY * 2;
        size_t screenIndex = static_cast<size_t>(screenY) * SCREEN_WIDTH +
            screenX;
        iterationValues[screenIndex] = iterationValues[cacheIndex];
        markPixelComputed(screenIndex);
    }

    double realRange = (viewport.realMax - viewport.realMin) * 0.5;
    double imaginaryRange =
        (viewport.imaginaryMax - viewport.imaginaryMin) * 0.5;
    double realStep = realRange / (SCREEN_WIDTH - 1);
    double imaginaryStep = imaginaryRange / (SCREEN_HEIGHT - 1);
    viewport.realMin = centerReal - ZOOM_ANCHOR_X * realStep;
    viewport.realMax = viewport.realMin + realRange;
    viewport.imaginaryMin = centerImaginary - ZOOM_ANCHOR_Y * imaginaryStep;
    viewport.imaginaryMax = viewport.imaginaryMin + imaginaryRange;

    Serial.printf("Zooming at pixel (%d,%d): real=%.8f imag=%.8f\n",
        x, y, centerReal, centerImaginary);
    resetRenderState();
}

void updateTouch() {
    bool touched = ts.touched();
    if (touched && !touchWasDown) {
        TS_Point point = ts.getPoint();
        int x = constrain(map(point.x, 200, 3800, 0, SCREEN_WIDTH - 1),
            0, SCREEN_WIDTH - 1);
        int y = constrain(map(point.y, 200, 3800, 0, SCREEN_HEIGHT - 1),
            0, SCREEN_HEIGHT - 1);

        if (x >= AUTO_BUTTON_X && y >= AUTO_BUTTON_Y) {
            autoZoomEnabled = !autoZoomEnabled;
            Serial.printf("Auto zoom %s\n",
                autoZoomEnabled ? "enabled" : "disabled");
            drawAutoZoomButton();
        } else if (imageComplete) {
            zoomAtPixel(x, y);
        }
    }
    touchWasDown = touched;
}

} // namespace

void setup() {
    setup_display();
    setup_touch();
    beginImage();
    lastStatsDump = millis();
}

void loop() {
    updateTouch();
    processImage(BOXES_PER_LOOP);

    unsigned long now = millis();
    if (now - lastStatsDump >= STATS_INTERVAL_MS) {
        dump_stats_serial();
        lastStatsDump = now;
    }

    if (imageComplete && autoZoomEnabled) {
        int zoomX;
        int zoomY;
        int score;
        findMostComplexTile(zoomX, zoomY, score);
        Serial.printf("Auto zoom: %d distinct iteration results near (%d,%d)\n",
            score, zoomX, zoomY);
        zoomAtPixel(zoomX, zoomY);
    }

    delay(1);
}


