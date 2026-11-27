#include "defaults.h"
#include "common.cpp"
#include "common_touch.cpp"
#include "common_stats.cpp"
#include "artwork.h"

#include <algorithm>
#include <stdint.h>

namespace {

const int DISPLAY_WIDTH = 240;
const int DISPLAY_HEIGHT = 320;
const uint8_t PORTRAIT_ROTATION = 0;
const int CHARACTER_AREA_WIDTH = 140;
const int PALETTE_LEFT = CHARACTER_AREA_WIDTH;
const int PALETTE_WIDTH = DISPLAY_WIDTH - PALETTE_LEFT;
const int PALETTE_VIEW_TOP = 40;
const int PALETTE_VIEW_BOTTOM = 282;
const int PALETTE_ITEM_GAP = 8;
const int PALETTE_SCROLL_STEP = 100;
const int SCROLL_BUTTON_SIZE = 32;
const int SCROLL_BUTTON_MARGIN = 4;
const int MAX_PLACED_ITEMS = 30;
const int BITMAP_ROW_BUFFER_SIZE = WOMAN_WIDTH;
const unsigned long TOUCH_DEBOUNCE_MS = 25;

struct Rect {
    int left;
    int top;
    int right;
    int bottom;
};

struct TouchPoint {
    int x;
    int y;
};

struct PlacedClothing {
    uint8_t clothingIndex;
    int16_t x;
    int16_t y;
};

enum DragOrigin : uint8_t {
    DRAG_NONE,
    DRAG_PALETTE,
    DRAG_PLACED
};

struct DragState {
    DragOrigin origin;
    uint8_t clothingIndex;
    int x;
    int y;
    int offsetX;
    int offsetY;
};

const int CHARACTER_X = (CHARACTER_AREA_WIDTH - WOMAN_WIDTH) / 2;
const int CHARACTER_Y = (DISPLAY_HEIGHT - WOMAN_HEIGHT) / 2;
const Rect SCREEN_RECT = {0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT};
const Rect PALETTE_CONTENT_RECT = {
    PALETTE_LEFT, PALETTE_VIEW_TOP, DISPLAY_WIDTH, PALETTE_VIEW_BOTTOM
};

PlacedClothing placedClothes[MAX_PLACED_ITEMS];
size_t placedClothesCount = 0;
int paletteScrollOffset = 0;
DragState dragState = {};
uint16_t opaqueRunBuffer[BITMAP_ROW_BUFFER_SIZE];
bool rawTouchState = false;
bool stableTouchState = false;
unsigned long rawTouchChangedAt = 0;
TouchPoint lastTouchPoint = {0, 0};

Rect intersectRect(const Rect &first, const Rect &second) {
    return {
        std::max(first.left, second.left),
        std::max(first.top, second.top),
        std::min(first.right, second.right),
        std::min(first.bottom, second.bottom)
    };
}

bool rectIsEmpty(const Rect &rect) {
    return rect.left >= rect.right || rect.top >= rect.bottom;
}

bool rectContains(const Rect &rect, int x, int y) {
    return x >= rect.left && x < rect.right &&
        y >= rect.top && y < rect.bottom;
}

bool rectsOverlap(const Rect &first, const Rect &second) {
    return first.left < second.right && first.right > second.left &&
        first.top < second.bottom && first.bottom > second.top;
}

Rect bitmapRect(int x, int y, int width, int height) {
    return {x, y, x + width, y + height};
}

ClothingItem readClothingMetadata(uint8_t index) {
    ClothingItem item;
    item.width = pgm_read_word(&clothes[index].width);
    item.height = pgm_read_word(&clothes[index].height);
    item.bitmap = reinterpret_cast<const uint16_t *>(
        pgm_read_ptr(&clothes[index].bitmap));
    return item;
}

uint16_t readBitmapPixel(const uint16_t *bitmap, int width, int x, int y) {
    return pgm_read_word(bitmap + static_cast<size_t>(y) * width + x);
}

bool bitmapPixelIsOpaque(const uint16_t *bitmap, int width, int height,
    int bitmapX, int bitmapY, int screenX, int screenY) {
    int x = screenX - bitmapX;
    int y = screenY - bitmapY;
    if (x < 0 || x >= width || y < 0 || y >= height) {
        return false;
    }
    return readBitmapPixel(bitmap, width, x, y) != ARTWORK_TRANSPARENT;
}

void drawTransparentBitmap(const uint16_t *bitmap, int width, int height,
    int x, int y, const Rect &requestedClip) {
    Rect bounds = bitmapRect(x, y, width, height);
    Rect clip = intersectRect(intersectRect(requestedClip, SCREEN_RECT), bounds);
    if (rectIsEmpty(clip)) {
        return;
    }

    int firstX = clip.left - x;
    int lastX = clip.right - x;
    int firstY = clip.top - y;
    int lastY = clip.bottom - y;
    for (int sourceY = firstY; sourceY < lastY; ++sourceY) {
        int sourceX = firstX;
        while (sourceX < lastX) {
            if (readBitmapPixel(bitmap, width, sourceX, sourceY) ==
                ARTWORK_TRANSPARENT) {
                ++sourceX;
                continue;
            }

            int runStart = sourceX;
            int runLength = 0;
            while (sourceX < lastX &&
                runLength < BITMAP_ROW_BUFFER_SIZE) {
                uint16_t color = readBitmapPixel(
                    bitmap, width, sourceX, sourceY);
                if (color == ARTWORK_TRANSPARENT) {
                    break;
                }
                opaqueRunBuffer[runLength++] = color;
                ++sourceX;
            }

            tft.drawRGBBitmap(x + runStart, y + sourceY,
                opaqueRunBuffer, runLength, 1);
        }
    }
}

Rect upButtonRect() {
    int left = DISPLAY_WIDTH - SCROLL_BUTTON_SIZE - SCROLL_BUTTON_MARGIN;
    return {left, SCROLL_BUTTON_MARGIN,
        left + SCROLL_BUTTON_SIZE, SCROLL_BUTTON_MARGIN + SCROLL_BUTTON_SIZE};
}

Rect downButtonRect() {
    int left = DISPLAY_WIDTH - SCROLL_BUTTON_SIZE - SCROLL_BUTTON_MARGIN;
    int top = DISPLAY_HEIGHT - SCROLL_BUTTON_SIZE - SCROLL_BUTTON_MARGIN;
    return {left, top, left + SCROLL_BUTTON_SIZE, top + SCROLL_BUTTON_SIZE};
}

int paletteContentHeight() {
    int height = 0;
    for (uint8_t index = 0; index < CLOTHING_COUNT; ++index) {
        ClothingItem item = readClothingMetadata(index);
        height += item.height;
        if (index + 1 < CLOTHING_COUNT) {
            height += PALETTE_ITEM_GAP;
        }
    }
    return height;
}

int maximumPaletteScroll() {
    return std::max(0, paletteContentHeight() -
        (PALETTE_VIEW_BOTTOM - PALETTE_VIEW_TOP));
}

void paletteItemPosition(uint8_t index, const ClothingItem &item,
    int &x, int &y) {
    int contentY = PALETTE_VIEW_TOP - paletteScrollOffset;
    for (uint8_t previous = 0; previous < index; ++previous) {
        ClothingItem previousItem = readClothingMetadata(previous);
        contentY += previousItem.height + PALETTE_ITEM_GAP;
    }
    x = PALETTE_LEFT + (PALETTE_WIDTH - item.width) / 2;
    y = contentY;
}

void drawClothingPalette(const Rect &requestedClip) {
    Rect clip = intersectRect(requestedClip, PALETTE_CONTENT_RECT);
    if (rectIsEmpty(clip)) {
        return;
    }

    int y = PALETTE_VIEW_TOP - paletteScrollOffset;
    for (uint8_t index = 0; index < CLOTHING_COUNT; ++index) {
        ClothingItem item = readClothingMetadata(index);
        int x = PALETTE_LEFT + (PALETTE_WIDTH - item.width) / 2;
        drawTransparentBitmap(item.bitmap, item.width, item.height,
            x, y, clip);
        y += item.height + PALETTE_ITEM_GAP;
    }
}

void drawScrollButton(const Rect &button, bool pointsUp, bool enabled) {
    uint16_t background = enabled ? ILI9341_DARKGREY : ILI9341_BLACK;
    uint16_t foreground = enabled ? ILI9341_WHITE : ILI9341_DARKGREY;
    int centerX = (button.left + button.right) / 2;
    int centerY = (button.top + button.bottom) / 2;
    tft.fillRect(button.left, button.top,
        button.right - button.left, button.bottom - button.top, background);
    tft.drawRect(button.left, button.top,
        button.right - button.left, button.bottom - button.top,
        ILI9341_LIGHTGREY);
    if (pointsUp) {
        tft.fillTriangle(centerX, centerY - 6, centerX - 7, centerY + 5,
            centerX + 7, centerY + 5, foreground);
    } else {
        tft.fillTriangle(centerX - 7, centerY - 5, centerX + 7,
            centerY - 5, centerX, centerY + 6, foreground);
    }
}

void drawScrollButtons(const Rect &clip) {
    Rect up = upButtonRect();
    Rect down = downButtonRect();
    if (rectsOverlap(clip, up)) {
        drawScrollButton(up, true, paletteScrollOffset > 0);
    }
    if (rectsOverlap(clip, down)) {
        drawScrollButton(down, false,
            paletteScrollOffset < maximumPaletteScroll());
    }
}

void drawCharacter(const Rect &clip) {
    drawTransparentBitmap(womanBitmap, WOMAN_WIDTH, WOMAN_HEIGHT,
        CHARACTER_X, CHARACTER_Y, clip);
}

void drawPlacedClothes(const Rect &clip) {
    for (size_t index = 0; index < placedClothesCount; ++index) {
        const PlacedClothing &placed = placedClothes[index];
        ClothingItem item = readClothingMetadata(placed.clothingIndex);
        drawTransparentBitmap(item.bitmap, item.width, item.height,
            placed.x, placed.y, clip);
    }
}

void redrawRegion(const Rect &requestedRegion) {
    Rect clip = intersectRect(requestedRegion, SCREEN_RECT);
    if (rectIsEmpty(clip)) {
        return;
    }

    tft.fillRect(clip.left, clip.top, clip.right - clip.left,
        clip.bottom - clip.top, ILI9341_BLACK);
    drawClothingPalette(clip);
    if (clip.left <= CHARACTER_AREA_WIDTH - 1 &&
        clip.right > CHARACTER_AREA_WIDTH - 1) {
        int firstY = std::max(clip.top, 0);
        tft.drawFastVLine(CHARACTER_AREA_WIDTH - 1, firstY,
            clip.bottom - firstY, ILI9341_DARKGREY);
    }
    drawCharacter(clip);
    drawPlacedClothes(clip);
    drawScrollButtons(clip);
}

void drawPaperDoll() {
    redrawRegion(SCREEN_RECT);
}

bool hitTestClothing(int x, int y, uint8_t &clothingIndex,
    int &itemX, int &itemY) {
    if (!rectContains(PALETTE_CONTENT_RECT, x, y)) {
        return false;
    }

    for (uint8_t index = 0; index < CLOTHING_COUNT; ++index) {
        ClothingItem item = readClothingMetadata(index);
        paletteItemPosition(index, item, itemX, itemY);
        if (bitmapPixelIsOpaque(item.bitmap, item.width, item.height,
            itemX, itemY, x, y)) {
            clothingIndex = index;
            return true;
        }
    }
    return false;
}

int hitTestPlacedClothing(int x, int y) {
    for (size_t reverse = placedClothesCount; reverse > 0; --reverse) {
        size_t index = reverse - 1;
        const PlacedClothing &placed = placedClothes[index];
        ClothingItem item = readClothingMetadata(placed.clothingIndex);
        if (bitmapPixelIsOpaque(item.bitmap, item.width, item.height,
            placed.x, placed.y, x, y)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

bool pointOnCharacter(int x, int y) {
    return bitmapPixelIsOpaque(womanBitmap, WOMAN_WIDTH, WOMAN_HEIGHT,
        CHARACTER_X, CHARACTER_Y, x, y);
}

void handlePaletteScroll(int direction) {
    int newOffset = paletteScrollOffset + direction * PALETTE_SCROLL_STEP;
    newOffset = std::max(0, std::min(newOffset, maximumPaletteScroll()));
    if (newOffset == paletteScrollOffset) {
        return;
    }
    paletteScrollOffset = newOffset;
    redrawRegion({PALETTE_LEFT, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT});
}

Rect dragBounds() {
    ClothingItem item = readClothingMetadata(dragState.clothingIndex);
    return bitmapRect(dragState.x, dragState.y, item.width, item.height);
}

void drawDragPreview() {
    if (dragState.origin == DRAG_NONE) {
        return;
    }
    ClothingItem item = readClothingMetadata(dragState.clothingIndex);
    drawTransparentBitmap(item.bitmap, item.width, item.height,
        dragState.x, dragState.y, SCREEN_RECT);
}

PlacedClothing removePlacedClothing(size_t index) {
    PlacedClothing removed = placedClothes[index];
    for (size_t move = index + 1; move < placedClothesCount; ++move) {
        placedClothes[move - 1] = placedClothes[move];
    }
    --placedClothesCount;
    return removed;
}

void startDrag(const TouchPoint &point) {
    if (rectContains(upButtonRect(), point.x, point.y)) {
        handlePaletteScroll(-1);
        return;
    }
    if (rectContains(downButtonRect(), point.x, point.y)) {
        handlePaletteScroll(1);
        return;
    }

    int placedIndex = hitTestPlacedClothing(point.x, point.y);
    if (placedIndex >= 0) {
        PlacedClothing placed = removePlacedClothing(
            static_cast<size_t>(placedIndex));
        dragState.origin = DRAG_PLACED;
        dragState.clothingIndex = placed.clothingIndex;
        dragState.x = placed.x;
        dragState.y = placed.y;
        dragState.offsetX = point.x - placed.x;
        dragState.offsetY = point.y - placed.y;
        redrawRegion(bitmapRect(placed.x, placed.y,
            readClothingMetadata(placed.clothingIndex).width,
            readClothingMetadata(placed.clothingIndex).height));
        drawDragPreview();
        return;
    }

    uint8_t clothingIndex;
    int itemX;
    int itemY;
    if (hitTestClothing(point.x, point.y,
        clothingIndex, itemX, itemY)) {
        dragState.origin = DRAG_PALETTE;
        dragState.clothingIndex = clothingIndex;
        dragState.x = itemX;
        dragState.y = itemY;
        dragState.offsetX = point.x - itemX;
        dragState.offsetY = point.y - itemY;
        drawDragPreview();
    }
}

void updateDrag(const TouchPoint &point) {
    if (dragState.origin == DRAG_NONE) {
        return;
    }

    Rect oldBounds = dragBounds();
    dragState.x = point.x - dragState.offsetX;
    dragState.y = point.y - dragState.offsetY;
    Rect newBounds = dragBounds();
    redrawRegion(oldBounds);
    redrawRegion(newBounds);
    drawDragPreview();
}

void finishDrag(const TouchPoint &point) {
    if (dragState.origin == DRAG_NONE) {
        return;
    }

    Rect oldBounds = dragBounds();
    DragOrigin origin = dragState.origin;
    PlacedClothing placed = {
        dragState.clothingIndex,
        static_cast<int16_t>(dragState.x),
        static_cast<int16_t>(dragState.y)
    };
    bool addPlacedItem = false;

    if (origin == DRAG_PALETTE) {
        if (pointOnCharacter(point.x, point.y)) {
            if (placedClothesCount < MAX_PLACED_ITEMS) {
                addPlacedItem = true;
            } else {
                Serial.println("Paper doll placement limit reached");
            }
        }
    } else if (point.x < PALETTE_LEFT) {
        addPlacedItem = true;
    }

    if (addPlacedItem) {
        placedClothes[placedClothesCount++] = placed;
    }
    dragState.origin = DRAG_NONE;
    redrawRegion(oldBounds);
    if (addPlacedItem) {
        ClothingItem item = readClothingMetadata(placed.clothingIndex);
        redrawRegion(bitmapRect(placed.x, placed.y, item.width, item.height));
    }
}

TouchPoint readTouchPoint() {
    TS_Point rawPoint = ts.getPoint();
    TouchPoint point;
    point.x = constrain(map(rawPoint.x, 200, 3800, 0, DISPLAY_WIDTH - 1),
        0, DISPLAY_WIDTH - 1);
    point.y = constrain(map(rawPoint.y, 200, 3800, 0, DISPLAY_HEIGHT - 1),
        0, DISPLAY_HEIGHT - 1);
    return point;
}

void handleTouch() {
    bool rawTouched = ts.touched();
    unsigned long now = millis();
    if (rawTouched != rawTouchState) {
        rawTouchState = rawTouched;
        rawTouchChangedAt = now;
    }

    if (now - rawTouchChangedAt >= TOUCH_DEBOUNCE_MS &&
        stableTouchState != rawTouchState) {
        stableTouchState = rawTouchState;
        if (stableTouchState) {
            lastTouchPoint = readTouchPoint();
            startDrag(lastTouchPoint);
        } else {
            finishDrag(lastTouchPoint);
        }
    }

    if (stableTouchState && rawTouchState) {
        TouchPoint point = readTouchPoint();
        if (point.x != lastTouchPoint.x || point.y != lastTouchPoint.y) {
            updateDrag(point);
            lastTouchPoint = point;
        }
    }
}

void initPaperDoll() {
    tft.setRotation(PORTRAIT_ROTATION);
    ts.setRotation(PORTRAIT_ROTATION);
    placedClothesCount = 0;
    paletteScrollOffset = 0;
    dragState = {};
    rawTouchState = false;
    stableTouchState = false;
    rawTouchChangedAt = millis();
    drawPaperDoll();
}

} // namespace

void setup() {
    setup_display();
    setup_touch();
    initPaperDoll();
}

void loop() {
    handleTouch();
    delay(5);
}
