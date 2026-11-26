#include "defaults.h"
#include "common.cpp"
#include "common_touch.cpp"
#include "common_stats.cpp"

#include <algorithm>
#include <math.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace {

const uint16_t ITERATION_INCREMENT = 256;
const uint8_t BLACK_ITERATION_MARKER = 0xFF;
const int TILE_SIZE = 20;
const size_t PIXEL_COUNT = SCREEN_WIDTH * SCREEN_HEIGHT;
const size_t COMPUTED_MAP_BYTES = (PIXEL_COUNT + 7) / 8;
const size_t MAX_BOX_STACK = 256;
const size_t BOX_STACK_HEADROOM = 24;
const unsigned long STATS_INTERVAL_MS = 10000;
const uint8_t BOXES_PER_LOOP = 48;
const UBaseType_t WORK_QUEUE_CAPACITY = 32;
const UBaseType_t WORK_QUEUE_TARGET_DEPTH = 10;
const UBaseType_t RESULT_QUEUE_CAPACITY = 48;
const uint32_t MAX_OUTSTANDING_JOBS = 48;
const uint8_t DISPATCHES_PER_LOOP = 12;
const uint8_t RESULTS_PER_LOOP = 4;
const uint32_t CALCULATION_WORKER_STACK_SIZE = 4096;
const int ZOOM_ANCHOR_X = SCREEN_WIDTH / 2;
const int ZOOM_ANCHOR_Y = SCREEN_HEIGHT / 2;
const int AUTO_BUTTON_SIZE = 30;
const int AUTO_BUTTON_X = SCREEN_WIDTH - AUTO_BUTTON_SIZE;
const int AUTO_BUTTON_Y = SCREEN_HEIGHT - AUTO_BUTTON_SIZE;
const int ITERATION_BUTTON_Y = AUTO_BUTTON_Y - AUTO_BUTTON_SIZE;
const uint8_t CORNER_SAMPLE_MASK = 0x0F;
const uint8_t CENTER_SAMPLE_MASK = 0x10;

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

struct PendingBox {
    Box box;
    uint8_t waitingSamples;
    bool deferred;
};

struct CalculationRequest {
    Box box;
    Viewport viewport;
    uint32_t generation;
    uint16_t sampleX[5];
    uint16_t sampleY[5];
    uint8_t sampleMask;
};

struct CalculationResult {
    Box box;
    uint32_t generation;
    uint16_t sampleX[5];
    uint16_t sampleY[5];
    uint8_t sampleValues[5];
    uint8_t sampleMask;
};

Viewport viewport = {-2.0, 2.0, -2.0, 2.0};
uint8_t iterationValues[PIXEL_COUNT];
uint8_t computedMap[COMPUTED_MAP_BYTES];
uint8_t inFlightMap[COMPUTED_MAP_BYTES];
PendingBox boxStack[MAX_BOX_STACK];
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
bool queueProcessingEnabled = false;
bool primaryResultPending = false;
volatile uint16_t iterationThreshold = 255;
uint32_t renderGeneration = 0;
uint32_t outstandingJobs = 0;
uint32_t currentGenerationJobs = 0;
uint32_t staleResultsDiscarded = 0;
uint32_t cacheHits = 0;
uint32_t newPixelCalculations = 0;
uint32_t duplicateCalculationsAvoided = 0;
uint32_t workRequestsDispatched = 0;
uint32_t workRequestsDeferredInFlight = 0;
uint32_t workRequestsDeferredQueueFull = 0;
uint32_t core0Calculations = 0;
uint32_t core1Calculations = 0;
QueueHandle_t calculationWorkQueue = nullptr;
QueueHandle_t calculationResultQueue = nullptr;
TaskHandle_t calculationWorkerHandle = nullptr;
CalculationResult pendingPrimaryResult = {};
portMUX_TYPE calculationStatsMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE workerFailureMux = portMUX_INITIALIZER_UNLOCKED;
CalculationResult workerFailureResult = {};
bool workerFailurePending = false;

void recordCalculations(uint32_t core, uint32_t count) {
    portENTER_CRITICAL(&calculationStatsMux);
    if (core == 0) {
        core0Calculations += count;
    } else {
        core1Calculations += count;
    }
    newPixelCalculations += count;
    portEXIT_CRITICAL(&calculationStatsMux);
}

uint8_t mandelbrotValue(double real, double imaginary, uint32_t core) {
    recordCalculations(core, 1);
    double zReal = 0.0;
    double zImaginary = 0.0;
    uint16_t iteration = 0;
    uint16_t threshold = iterationThreshold;

    while (iteration < threshold &&
        zReal * zReal + zImaginary * zImaginary <= 4.0) {
        double nextReal = zReal * zReal - zImaginary * zImaginary + real;
        zImaginary = 2.0 * zReal * zImaginary + imaginary;
        zReal = nextReal;
        ++iteration;
    }

    return static_cast<uint8_t>(iteration);
}

uint8_t mandelbrotValueAt(const Viewport &calculationViewport, int x, int y,
    uint32_t core) {
    double real = calculationViewport.realMin +
        (calculationViewport.realMax - calculationViewport.realMin) * x /
            (SCREEN_WIDTH - 1);
    double imaginary = calculationViewport.imaginaryMin +
        (calculationViewport.imaginaryMax -
            calculationViewport.imaginaryMin) * y / (SCREEN_HEIGHT - 1);
    return mandelbrotValue(real, imaginary, core);
}

CalculationResult calculateRequest(const CalculationRequest &request,
    uint32_t core) {
    CalculationResult result = {};
    result.box = request.box;
    result.generation = request.generation;
    result.sampleMask = request.sampleMask;
    for (uint8_t i = 0; i < 5; ++i) {
        result.sampleX[i] = request.sampleX[i];
        result.sampleY[i] = request.sampleY[i];
        if ((request.sampleMask & (1U << i)) != 0) {
            result.sampleValues[i] = mandelbrotValueAt(request.viewport,
                request.sampleX[i], request.sampleY[i], core);
        }
    }
    return result;
}

void pixelCoordinatesToComplex(int x, int y, double &real,
    double &imaginary) {
    real = viewport.realMin +
        (viewport.realMax - viewport.realMin) * x / (SCREEN_WIDTH - 1);
    imaginary = viewport.imaginaryMin +
        (viewport.imaginaryMax - viewport.imaginaryMin) * y /
            (SCREEN_HEIGHT - 1);
}

double coordinatePrecision(double value) {
    double upper = nextafter(value, INFINITY);
    double lower = nextafter(value, -INFINITY);
    return std::max(fabs(upper - value), fabs(value - lower));
}

bool axisHasDistinctTileCoordinates(double minimum, double maximum,
    int dimension, double &precisionLimit) {
    for (int tileStart = 0; tileStart < dimension; tileStart += TILE_SIZE) {
        int tileEnd = min(tileStart + TILE_SIZE - 1, dimension - 1);
        double previous = minimum + (maximum - minimum) * tileStart /
            (dimension - 1);
        for (int pixel = tileStart + 1; pixel <= tileEnd; ++pixel) {
            double coordinate = minimum + (maximum - minimum) * pixel /
                (dimension - 1);
            if (coordinate == previous) {
                precisionLimit = std::max(precisionLimit,
                    coordinatePrecision(previous));
                return false;
            }
            previous = coordinate;
        }
    }
    return true;
}

bool viewportBelowTilePrecision(const Viewport &candidate,
    double &precisionLimit) {
    precisionLimit = 0.0;
    bool realCoordinatesDistinct = axisHasDistinctTileCoordinates(
        candidate.realMin, candidate.realMax, SCREEN_WIDTH, precisionLimit);
    bool imaginaryCoordinatesDistinct = axisHasDistinctTileCoordinates(
        candidate.imaginaryMin, candidate.imaginaryMax, SCREEN_HEIGHT,
        precisionLimit);
    return !realCoordinatesDistinct || !imaginaryCoordinatesDistinct;
}

Viewport viewportAfterZoomAtPixel(int x, int y, double &centerReal,
    double &centerImaginary) {
    pixelCoordinatesToComplex(x, y, centerReal, centerImaginary);
    double realRange = (viewport.realMax - viewport.realMin) * 0.5;
    double imaginaryRange =
        (viewport.imaginaryMax - viewport.imaginaryMin) * 0.5;
    double realStep = realRange / (SCREEN_WIDTH - 1);
    double imaginaryStep = imaginaryRange / (SCREEN_HEIGHT - 1);

    Viewport zoomed;
    zoomed.realMin = centerReal - ZOOM_ANCHOR_X * realStep;
    zoomed.realMax = zoomed.realMin + realRange;
    zoomed.imaginaryMin =
        centerImaginary - ZOOM_ANCHOR_Y * imaginaryStep;
    zoomed.imaginaryMax = zoomed.imaginaryMin + imaginaryRange;
    return zoomed;
}

bool pixelIsComputed(size_t index) {
    return (computedMap[index >> 3] & (1U << (index & 7))) != 0;
}

bool pixelIsInFlight(size_t index) {
    return (inFlightMap[index >> 3] & (1U << (index & 7))) != 0;
}

void markPixelComputed(size_t index) {
    computedMap[index >> 3] |= static_cast<uint8_t>(1U << (index & 7));
}

void clearPixelComputed(size_t index) {
    computedMap[index >> 3] &=
        static_cast<uint8_t>(~(1U << (index & 7)));
}

void markPixelInFlight(size_t index) {
    inFlightMap[index >> 3] |= static_cast<uint8_t>(1U << (index & 7));
}

void clearPixelInFlight(size_t index) {
    inFlightMap[index >> 3] &=
        static_cast<uint8_t>(~(1U << (index & 7)));
}

uint8_t iterationAtPixel(int x, int y) {
    size_t index = static_cast<size_t>(y) * SCREEN_WIDTH + x;
    if (pixelIsComputed(index)) {
        ++cacheHits;
        return iterationValues[index];
    }

    double real;
    double imaginary;
    pixelCoordinatesToComplex(x, y, real, imaginary);

    uint8_t value = mandelbrotValue(real, imaginary, 1);
    iterationValues[index] = value;
    markPixelComputed(index);
    return value;
}

uint16_t colorForIteration(uint8_t iteration) {
    if (iteration == BLACK_ITERATION_MARKER) {
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

void drawIterationButton() {
    tft.fillRect(AUTO_BUTTON_X, ITERATION_BUTTON_Y, AUTO_BUTTON_SIZE,
        AUTO_BUTTON_SIZE, ILI9341_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
    tft.setCursor(AUTO_BUTTON_X + 8, ITERATION_BUTTON_Y + 6);
    tft.print('+');
}

void drawFractalBox(const Box &box, uint16_t color) {
    if (box.x1 < AUTO_BUTTON_X || box.y1 < ITERATION_BUTTON_Y) {
        tft.fillRect(box.x0, box.y0, box.x1 - box.x0 + 1,
            box.y1 - box.y0 + 1, color);
        return;
    }

    if (box.y0 < ITERATION_BUTTON_Y) {
        int topHeight = ITERATION_BUTTON_Y - box.y0;
        tft.fillRect(box.x0, box.y0, box.x1 - box.x0 + 1,
            topHeight, color);
    }

    if (box.x0 < AUTO_BUTTON_X) {
        int leftWidth = AUTO_BUTTON_X - box.x0;
        int bottomY = max(box.y0, ITERATION_BUTTON_Y);
        tft.fillRect(box.x0, bottomY, leftWidth,
            box.y1 - bottomY + 1, color);
    }
}

void drawFractalPixel(int x, int y, uint16_t color) {
    if (x < AUTO_BUTTON_X || y < ITERATION_BUTTON_Y) {
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

void pushBox(const Box &box) {
    if (box.x0 > box.x1 || box.y0 > box.y1) {
        return;
    }
    if (boxStackSize >= MAX_BOX_STACK) {
        Serial.println("Fractal subdivision stack capacity exceeded");
        return;
    }
    boxStack[boxStackSize++] = {box, 0, false};
}

void deferBox(const Box &box, uint8_t waitingSamples) {
    if (boxStackSize >= MAX_BOX_STACK) {
        Serial.println("Deferred fractal box stack capacity exceeded");
        return;
    }
    boxStack[boxStackSize++] = {box, waitingSamples, true};
    ++workRequestsDeferredInFlight;
}

bool popReadyBox(Box &box) {
    for (size_t position = boxStackSize; position > 0; --position) {
        size_t index = position - 1;
        if (boxStack[index].deferred) {
            continue;
        }

        box = boxStack[index].box;
        for (size_t move = index + 1; move < boxStackSize; ++move) {
            boxStack[move - 1] = boxStack[move];
        }
        --boxStackSize;
        return true;
    }
    return false;
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

void setSampleCoordinates(const Box &box, uint16_t sampleX[5],
    uint16_t sampleY[5]) {
    sampleX[0] = box.x0;
    sampleY[0] = box.y0;
    sampleX[1] = box.x1;
    sampleY[1] = box.y0;
    sampleX[2] = box.x0;
    sampleY[2] = box.y1;
    sampleX[3] = box.x1;
    sampleY[3] = box.y1;
    sampleX[4] = (box.x0 + box.x1) / 2;
    sampleY[4] = (box.y0 + box.y1) / 2;
}

void wakeDeferredBoxes() {
    for (size_t entry = 0; entry < boxStackSize; ++entry) {
        PendingBox &pending = boxStack[entry];
        if (!pending.deferred) {
            continue;
        }

        uint16_t sampleX[5];
        uint16_t sampleY[5];
        setSampleCoordinates(pending.box, sampleX, sampleY);
        bool stillWaiting = false;
        for (uint8_t sample = 0; sample < 5; ++sample) {
            if ((pending.waitingSamples & (1U << sample)) == 0) {
                continue;
            }
            size_t index = static_cast<size_t>(sampleY[sample]) *
                SCREEN_WIDTH + sampleX[sample];
            if (pixelIsInFlight(index)) {
                stillWaiting = true;
                break;
            }
        }

        if (!stillWaiting) {
            pending.deferred = false;
            pending.waitingSamples = 0;
        }
    }
}

void subdivideBox(const Box &box, uint8_t center) {
    int middleX = (box.x0 + box.x1) / 2;
    int middleY = (box.y0 + box.y1) / 2;
    drawFractalPixel(middleX, middleY, colorForIteration(center));

    pushBox({middleX + 1, middleY + 1, box.x1, box.y1});
    pushBox({box.x0, middleY + 1, middleX, box.y1});
    pushBox({middleX + 1, box.y0, box.x1, middleY});
    pushBox({box.x0, box.y0, middleX, middleY});
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

    uint8_t center = iterationAtPixel(
        (box.x0 + box.x1) / 2, (box.y0 + box.y1) / 2);
    subdivideBox(box, center);
}

bool readCachedPixel(int x, int y, uint8_t &value) {
    size_t index = static_cast<size_t>(y) * SCREEN_WIDTH + x;
    if (!pixelIsComputed(index)) {
        return false;
    }

    ++cacheHits;
    value = iterationValues[index];
    return true;
}

void storeCalculationSample(int x, int y, uint8_t value) {
    size_t index = static_cast<size_t>(y) * SCREEN_WIDTH + x;
    if (pixelIsComputed(index)) {
        ++cacheHits;
        return;
    }

    iterationValues[index] = value;
    markPixelComputed(index);
}

void resolveCalculationBox(const Box &box) {
    uint8_t topLeft;
    uint8_t topRight;
    uint8_t bottomLeft;
    uint8_t bottomRight;
    if (!readCachedPixel(box.x0, box.y0, topLeft) ||
        !readCachedPixel(box.x1, box.y0, topRight) ||
        !readCachedPixel(box.x0, box.y1, bottomLeft) ||
        !readCachedPixel(box.x1, box.y1, bottomRight)) {
        pushBox(box);
        return;
    }

    if (topLeft == topRight && topLeft == bottomLeft &&
        topLeft == bottomRight) {
        fillBox(box, topLeft);
        return;
    }

    uint8_t center;
    int centerX = (box.x0 + box.x1) / 2;
    int centerY = (box.y0 + box.y1) / 2;
    if (!readCachedPixel(centerX, centerY, center)) {
        pushBox(box);
        return;
    }
    subdivideBox(box, center);
}

void processCalculationResult(const CalculationResult &result) {
    for (uint8_t sample = 0; sample < 5; ++sample) {
        if ((result.sampleMask & (1U << sample)) == 0) {
            continue;
        }

        size_t index = static_cast<size_t>(result.sampleY[sample]) *
            SCREEN_WIDTH + result.sampleX[sample];
        clearPixelInFlight(index);
        storeCalculationSample(result.sampleX[sample],
            result.sampleY[sample], result.sampleValues[sample]);
    }

    wakeDeferredBoxes();
    resolveCalculationBox(result.box);
}

void processWorkerFailure() {
    CalculationResult result = {};
    bool hasFailure = false;
    portENTER_CRITICAL(&workerFailureMux);
    if (workerFailurePending) {
        result = workerFailureResult;
        workerFailurePending = false;
        hasFailure = true;
    }
    portEXIT_CRITICAL(&workerFailureMux);

    if (!hasFailure) {
        return;
    }

    if (result.generation == renderGeneration) {
        if (currentGenerationJobs > 0) {
            --currentGenerationJobs;
        } else {
            Serial.println("Current-generation job count underflow");
        }
        processCalculationResult(result);
    } else {
        ++staleResultsDiscarded;
    }

    if (outstandingJobs > 0) {
        --outstandingJobs;
    } else {
        Serial.println("Outstanding calculation count underflow");
    }
    Serial.println("Worker result queue send failed; recovered result locally");
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
    double elapsedSeconds = (millis() - imageStartedAt) / 1000.0;

    Serial.printf(
        "Mandelbrot image complete in %.1f seconds\n"
        "Iteration threshold: %u\n"
        "Center: real=%.10e imaginary=%.10e | visible radius=%.8e\n",
        elapsedSeconds,
        static_cast<unsigned int>(iterationThreshold),
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

bool takeNextBox(Box &box) {
    wakeDeferredBoxes();
    if (popReadyBox(box)) {
        return true;
    }
    return nextRootBox(box);
}

void processSynchronousImage(uint8_t boxBudget) {
    for (uint8_t processed = 0; processed < boxBudget && !imageComplete;
        ++processed) {
        Box box;
        if (!takeNextBox(box)) {
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

void processCalculationResults(uint8_t resultBudget) {
    processWorkerFailure();
    for (uint8_t processed = 0; processed < resultBudget; ++processed) {
        CalculationResult result = {};
        if (xQueueReceive(calculationResultQueue, &result, 0) != pdTRUE) {
            break;
        }

        if (result.generation == renderGeneration) {
            if (currentGenerationJobs > 0) {
                --currentGenerationJobs;
            } else {
                Serial.println("Current-generation job count underflow");
            }
            processCalculationResult(result);
        } else {
            ++staleResultsDiscarded;
        }

        if (outstandingJobs > 0) {
            --outstandingJobs;
        } else {
            Serial.println("Outstanding calculation count underflow");
        }
    }
}

bool dispatchBoxCalculation(const Box &box) {
    uint16_t sampleX[5];
    uint16_t sampleY[5];
    setSampleCoordinates(box, sampleX, sampleY);

    uint8_t cornerValues[4] = {};
    bool cornerCached[4] = {};
    uint8_t waitingSamples = 0;
    bool allCornersCached = true;
    for (uint8_t sample = 0; sample < 4; ++sample) {
        cornerCached[sample] = readCachedPixel(sampleX[sample],
            sampleY[sample], cornerValues[sample]);
        if (cornerCached[sample]) {
            continue;
        }

        allCornersCached = false;
        size_t index = static_cast<size_t>(sampleY[sample]) *
            SCREEN_WIDTH + sampleX[sample];
        if (pixelIsInFlight(index)) {
            waitingSamples |= static_cast<uint8_t>(1U << sample);
            ++duplicateCalculationsAvoided;
        }
    }

    if (waitingSamples != 0) {
        deferBox(box, waitingSamples);
        return true;
    }

    uint8_t requestMask = 0;
    if (allCornersCached) {
        if (cornerValues[0] == cornerValues[1] &&
            cornerValues[0] == cornerValues[2] &&
            cornerValues[0] == cornerValues[3]) {
            fillBox(box, cornerValues[0]);
            return true;
        }

        uint8_t centerValue;
        if (readCachedPixel(sampleX[4], sampleY[4], centerValue)) {
            subdivideBox(box, centerValue);
            return true;
        }

        size_t centerIndex = static_cast<size_t>(sampleY[4]) *
            SCREEN_WIDTH + sampleX[4];
        if (pixelIsInFlight(centerIndex)) {
            ++duplicateCalculationsAvoided;
            deferBox(box, CENTER_SAMPLE_MASK);
            return true;
        }
        requestMask = CENTER_SAMPLE_MASK;
    } else {
        for (uint8_t sample = 0; sample < 4; ++sample) {
            if (cornerCached[sample]) {
                continue;
            }

            size_t index = static_cast<size_t>(sampleY[sample]) *
                SCREEN_WIDTH + sampleX[sample];
            bool duplicateCoordinate = false;
            for (uint8_t prior = 0; prior < sample; ++prior) {
                size_t priorIndex = static_cast<size_t>(sampleY[prior]) *
                    SCREEN_WIDTH + sampleX[prior];
                if (priorIndex == index) {
                    duplicateCoordinate = true;
                    break;
                }
            }
            if (duplicateCoordinate) {
                ++duplicateCalculationsAvoided;
                continue;
            }
            requestMask |= static_cast<uint8_t>(1U << sample);
        }
    }

    CalculationRequest request = {};
    request.box = box;
    request.viewport = viewport;
    request.generation = renderGeneration;
    request.sampleMask = requestMask;
    for (uint8_t sample = 0; sample < 5; ++sample) {
        request.sampleX[sample] = sampleX[sample];
        request.sampleY[sample] = sampleY[sample];
        if ((requestMask & (1U << sample)) != 0) {
            markPixelInFlight(static_cast<size_t>(sampleY[sample]) *
                SCREEN_WIDTH + sampleX[sample]);
        }
    }

    if (xQueueSend(calculationWorkQueue, &request, 0) != pdTRUE) {
        for (uint8_t sample = 0; sample < 5; ++sample) {
            if ((requestMask & (1U << sample)) == 0) {
                continue;
            }
            size_t index = static_cast<size_t>(sampleY[sample]) *
                SCREEN_WIDTH + sampleX[sample];
            clearPixelInFlight(index);
        }
        ++workRequestsDeferredQueueFull;
        pushBox(box);
        return false;
    }

    ++outstandingJobs;
    ++currentGenerationJobs;
    ++workRequestsDispatched;
    return true;
}

void dispatchCalculationRequests(uint8_t dispatchBudget) {
    for (uint8_t dispatched = 0;
        dispatched < dispatchBudget && !imageComplete; ++dispatched) {
        if (uxQueueMessagesWaiting(calculationWorkQueue) >=
            WORK_QUEUE_TARGET_DEPTH) {
            return;
        }
        if (outstandingJobs >= MAX_OUTSTANDING_JOBS) {
            return;
        }

        size_t pendingStackSize = boxStackSize > 0
            ? boxStackSize - 1 : 0;
        size_t reservedChildren =
            4 * static_cast<size_t>(outstandingJobs + 1);
        if (pendingStackSize + reservedChildren >
            MAX_BOX_STACK - BOX_STACK_HEADROOM) {
            return;
        }

        Box box;
        if (!takeNextBox(box)) {
            return;
        }

        if (!dispatchBoxCalculation(box)) {
            return;
        }
    }
}

bool flushPendingPrimaryResult() {
    if (!primaryResultPending) {
        return true;
    }
    if (xQueueSend(calculationResultQueue, &pendingPrimaryResult, 0) != pdTRUE) {
        return false;
    }
    primaryResultPending = false;
    return true;
}

void processOnePrimaryCalculation() {
    if (!flushPendingPrimaryResult()) {
        return;
    }

    CalculationRequest request = {};
    if (xQueueReceive(calculationWorkQueue, &request, 0) != pdTRUE) {
        return;
    }

    CalculationResult result = calculateRequest(request, 1);
    if (xQueueSend(calculationResultQueue, &result, 0) != pdTRUE) {
        pendingPrimaryResult = result;
        primaryResultPending = true;
    }
}

void tryCompleteImage() {
    if (!imageComplete && nextTileY >= SCREEN_HEIGHT &&
        boxStackSize == 0 && currentGenerationJobs == 0) {
        reportImageComplete();
    }
}

void processImage(uint8_t boxBudget, bool touchHandled) {
    if (!queueProcessingEnabled) {
        processSynchronousImage(boxBudget);
        return;
    }

    processCalculationResults(RESULTS_PER_LOOP);
    flushPendingPrimaryResult();
    dispatchCalculationRequests(DISPATCHES_PER_LOOP);

    if (!touchHandled &&
        uxQueueMessagesWaiting(calculationResultQueue) == 0) {
        if (outstandingJobs == 0 && boxStackSize > 0 &&
            boxStackSize + 4 >
                MAX_BOX_STACK - BOX_STACK_HEADROOM) {
            processSynchronousImage(boxBudget);
        } else {
            processOnePrimaryCalculation();
        }
    }

    tryCompleteImage();
}

void calculationWorkerTask(void *) {
    for (;;) {
        CalculationRequest request = {};
        if (xQueueReceive(calculationWorkQueue, &request,
            portMAX_DELAY) != pdTRUE) {
            continue;
        }

        CalculationResult result = calculateRequest(request, 0);
        if (xQueueSend(calculationResultQueue, &result,
            portMAX_DELAY) != pdTRUE) {
            portENTER_CRITICAL(&workerFailureMux);
            workerFailureResult = result;
            workerFailurePending = true;
            portEXIT_CRITICAL(&workerFailureMux);
            vTaskSuspend(nullptr);
        }
    }
}

void initializeCalculationQueues() {
    calculationWorkQueue = xQueueCreate(
        WORK_QUEUE_CAPACITY, sizeof(CalculationRequest));
    calculationResultQueue = xQueueCreate(
        RESULT_QUEUE_CAPACITY, sizeof(CalculationResult));

    if (calculationWorkQueue == nullptr ||
        calculationResultQueue == nullptr) {
        Serial.println("Calculation queue creation failed; using synchronous mode");
        if (calculationWorkQueue != nullptr) {
            vQueueDelete(calculationWorkQueue);
            calculationWorkQueue = nullptr;
        }
        if (calculationResultQueue != nullptr) {
            vQueueDelete(calculationResultQueue);
            calculationResultQueue = nullptr;
        }
        return;
    }

    queueProcessingEnabled = true;
    BaseType_t taskCreated = xTaskCreatePinnedToCore(
        calculationWorkerTask,
        "mandelbrot-core0",
        CALCULATION_WORKER_STACK_SIZE,
        nullptr,
        tskIDLE_PRIORITY + 1,
        &calculationWorkerHandle,
        0
    );

    if (taskCreated != pdPASS) {
        calculationWorkerHandle = nullptr;
        Serial.println("Core 0 worker creation failed; primary will calculate jobs");
    } else {
        Serial.println("Mandelbrot worker pinned to core 0");
    }
}

void dumpCalculationStats() {
    UBaseType_t workQueueLength = calculationWorkQueue == nullptr
        ? 0 : uxQueueMessagesWaiting(calculationWorkQueue);
    UBaseType_t resultQueueLength = calculationResultQueue == nullptr
        ? 0 : uxQueueMessagesWaiting(calculationResultQueue);

    uint32_t core0Count;
    uint32_t core1Count;
    uint32_t calculationCount;
    portENTER_CRITICAL(&calculationStatsMux);
    core0Count = core0Calculations;
    core1Count = core1Calculations;
    calculationCount = newPixelCalculations;
    portEXIT_CRITICAL(&calculationStatsMux);

    Serial.printf(
        "Iteration threshold=%u\n"
        "Calculation queues: work=%lu/%lu results=%lu/%lu "
        "outstanding=%lu/%lu\n"
        "Mandelbrot point calculations: core0=%lu core1=%lu total=%lu "
        "cache hits=%lu duplicate calculations avoided=%lu\n"
        "Requests: dispatched=%lu deferred-in-flight=%lu "
        "deferred-queue-full=%lu stale results=%lu\n",
        static_cast<unsigned int>(iterationThreshold),
        static_cast<unsigned long>(workQueueLength),
        static_cast<unsigned long>(WORK_QUEUE_CAPACITY),
        static_cast<unsigned long>(resultQueueLength),
        static_cast<unsigned long>(RESULT_QUEUE_CAPACITY),
        static_cast<unsigned long>(outstandingJobs),
        static_cast<unsigned long>(MAX_OUTSTANDING_JOBS),
        static_cast<unsigned long>(core0Count),
        static_cast<unsigned long>(core1Count),
        static_cast<unsigned long>(calculationCount),
        static_cast<unsigned long>(cacheHits),
        static_cast<unsigned long>(duplicateCalculationsAvoided),
        static_cast<unsigned long>(workRequestsDispatched),
        static_cast<unsigned long>(workRequestsDeferredInFlight),
        static_cast<unsigned long>(workRequestsDeferredQueueFull),
        static_cast<unsigned long>(staleResultsDiscarded)
    );
}

void resetRenderState() {
    boxStackSize = 0;
    memset(inFlightMap, 0, sizeof(inFlightMap));
    nextTileX = 0;
    nextTileY = 0;
    imageComplete = false;
    tft.fillScreen(ILI9341_BLACK);
    drawIterationButton();
    drawAutoZoomButton();
    imageStartedAt = millis();
}

void beginImage() {
    ++renderGeneration;
    currentGenerationJobs = 0;
    memset(inFlightMap, 0, sizeof(inFlightMap));
    memset(computedMap, 0, sizeof(computedMap));
    resetRenderState();
}

void increaseIterationThreshold() {
    autoZoomEnabled = false;
    drawAutoZoomButton();

    if (iterationThreshold >
        static_cast<uint16_t>(UINT16_MAX - ITERATION_INCREMENT)) {
        Serial.printf("Iteration threshold is already at maximum (%u)\n",
            static_cast<unsigned int>(iterationThreshold));
        return;
    }

    uint16_t previousThreshold = iterationThreshold;
    iterationThreshold = static_cast<uint16_t>(
        iterationThreshold + ITERATION_INCREMENT);
    size_t invalidatedPixels = 0;
    for (size_t index = 0; index < PIXEL_COUNT; ++index) {
        if (pixelIsComputed(index) &&
            iterationValues[index] == BLACK_ITERATION_MARKER) {
            clearPixelComputed(index);
            ++invalidatedPixels;
        }
    }

    ++renderGeneration;
    currentGenerationJobs = 0;
    Serial.printf(
        "Iteration threshold increased to %u; recomputing %lu "
        "black-marked pixels (previous threshold %u)\n",
        static_cast<unsigned int>(iterationThreshold),
        static_cast<unsigned long>(invalidatedPixels),
        static_cast<unsigned int>(previousThreshold));
    resetRenderState();
}

void zoomAtPixel(int x, int y) {
    double centerReal;
    double centerImaginary;
    Viewport zoomedViewport = viewportAfterZoomAtPixel(
        x, y, centerReal, centerImaginary);
    ++renderGeneration;
    currentGenerationJobs = 0;
    memset(inFlightMap, 0, sizeof(inFlightMap));

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

    viewport = zoomedViewport;
    double realRadius = (viewport.realMax - viewport.realMin) * 0.5;
    double imaginaryRadius =
        (viewport.imaginaryMax - viewport.imaginaryMin) * 0.5;
    double visibleRadius = std::min(realRadius, imaginaryRadius);
    Serial.printf(
        "Zooming at pixel (%d,%d): real=%.12e imag=%.12e "
        "visible radius=%.8e\n",
        x, y, centerReal, centerImaginary, visibleRadius);
    resetRenderState();
}

bool updateTouch() {
    bool touched = ts.touched();
    bool touchHandled = false;
    if (touched && !touchWasDown) {
        TS_Point point = ts.getPoint();
        int x = constrain(map(point.x, 200, 3800, 0, SCREEN_WIDTH - 1),
            0, SCREEN_WIDTH - 1);
        int y = constrain(map(point.y, 200, 3800, 0, SCREEN_HEIGHT - 1),
            0, SCREEN_HEIGHT - 1);

        if (x >= AUTO_BUTTON_X && y >= ITERATION_BUTTON_Y &&
            y < AUTO_BUTTON_Y) {
            increaseIterationThreshold();
            touchHandled = true;
        } else if (x >= AUTO_BUTTON_X && y >= AUTO_BUTTON_Y) {
            autoZoomEnabled = !autoZoomEnabled;
            Serial.printf("Auto zoom %s\n",
                autoZoomEnabled ? "enabled" : "disabled");
            drawAutoZoomButton();
            touchHandled = true;
        } else if (imageComplete) {
            zoomAtPixel(x, y);
            touchHandled = true;
        }
    }
    touchWasDown = touched;
    return touchHandled;
}

} // namespace

void setup() {
    setup_display();
    setup_touch();
    Serial.printf("Fractal UI running on core %d\n", xPortGetCoreID());
    initializeCalculationQueues();
    beginImage();
    lastStatsDump = millis();
}

void loop() {
    bool touchHandled = updateTouch();
    processImage(BOXES_PER_LOOP, touchHandled);

    unsigned long now = millis();
    if (now - lastStatsDump >= STATS_INTERVAL_MS) {
        dump_stats_serial();
        dumpCalculationStats();
        lastStatsDump = millis();
    }

    if (imageComplete && autoZoomEnabled) {
        double precisionLimit;
        if (viewportBelowTilePrecision(viewport, precisionLimit)) {
            autoZoomEnabled = false;
            drawAutoZoomButton();
            Serial.printf(
                "Stopping autozoom because pixel size is below available "
                "precision %.8e\n",
                precisionLimit);
        } else {
            int zoomX;
            int zoomY;
            int score;
            findMostComplexTile(zoomX, zoomY, score);
            Serial.printf(
                "Auto zoom: %d distinct iteration results near (%d,%d)\n",
                score, zoomX, zoomY);

            double centerReal;
            double centerImaginary;
            Viewport nextViewport = viewportAfterZoomAtPixel(zoomX, zoomY,
                centerReal, centerImaginary);
            if (viewportBelowTilePrecision(nextViewport, precisionLimit)) {
                autoZoomEnabled = false;
                drawAutoZoomButton();
                Serial.printf(
                    "Stopping autozoom because pixel size is below "
                    "available precision %.8e\n",
                    precisionLimit);
            } else {
                zoomAtPixel(zoomX, zoomY);
            }
        }
    }

    delay(1);
}


