#include "defaults.h"
#include "common.cpp"
#include "common_touch.cpp"
#include "common_stats.cpp"
#include "animals.h"
#include "animal_levels.h"

#include <algorithm>
#include <stdint.h>
#include <string.h>

namespace {
const bool SHOW_MENU = true;
const uint8_t LEVEL_MENU_VALUES[] = {
    1, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100
};
const uint8_t LEVEL_MENU_VALUE_COUNT =
    sizeof(LEVEL_MENU_VALUES) / sizeof(LEVEL_MENU_VALUES[0]);
const int MENU_BUTTON_WIDTH = 52;
const int MENU_BUTTON_HEIGHT = 42;
const int MENU_BUTTON_GAP_X = 8;
const int MENU_BUTTON_ROW_STEP = 56;
const int MENU_BUTTON_TOP = 58;
const int INFO_BUTTON_HEIGHT = 20;
const int PAR_RESTART_BUTTON_OFFSET = 50;
const uint8_t BOX_CAPACITY = 4;
const uint8_t BOXES_TOP_ROW = 5;
const uint8_t BOXES_BOTTOM_ROW = 4;
const uint8_t TOTAL_BOXES = BOXES_TOP_ROW + BOXES_BOTTOM_ROW;
const uint16_t BOX_WIDTH = 40;
const uint16_t BOX_HEIGHT = 100;
const uint8_t FRAME_THICKNESS = 2;
const uint8_t COMPLETED_FRAME_THICKNESS = 3;
const uint8_t IMAGE_VERTICAL_STEP = 25;
const uint8_t ANIMATION_FRAMES = 8;
const uint16_t TOUCH_DEBOUNCE_MS = 120;
const uint16_t LIGHT_BLUE = 0x867D;
const uint16_t GOLD = 0xFEA0;
const uint16_t OPAQUE_RUN_BUFFER_SIZE = 40;
const uint8_t NO_BOX = 0xFF;
const uint16_t MAX_TRACKED_BOARD_STATES = 128;

struct BoxPosition {
    int x;
    int y;
};

struct BoxState {
    uint8_t animals[BOX_CAPACITY];
    uint8_t count;
    bool completed;
};

struct BoardState {
    BoxState boxes[TOTAL_BOXES];
};

struct AnimalMetadata {
    uint16_t width;
    uint16_t height;
    const uint16_t *bitmap;
};

struct MovingAnimal {
    uint8_t animal;
    int x;
    int y;
    int width;
    int height;
};

BoxPosition boxPositions[TOTAL_BOXES];
BoardState gameBoard;
BoardState undoBoard;
BoardState seenBoardStates[MAX_TRACKED_BOARD_STATES];
uint16_t seenBoardStateCount = 0;
uint16_t nextSeenBoardState = 0;
uint16_t currentMoveCount = 0;
uint8_t expectedMoveCount = 0;
uint32_t currentLevel = 1;
uint16_t undoMoveCount = 0;
uint8_t selectedSource = NO_BOX;
uint16_t opaqueRunBuffer[OPAQUE_RUN_BUFFER_SIZE];
bool touchWasDown = false;
bool undoAvailable = false;
bool playerWon = false;
bool boardDeadlocked = false;
bool levelLoadFailed = false;
bool animationInProgress = false;
bool showingLevelMenu = SHOW_MENU;
bool repeatedBoardStateDetected = false;
uint32_t lastTouchTime = 0;

bool boardStatesEqual(const BoardState &left, const BoardState &right) {
    for (uint8_t box = 0; box < TOTAL_BOXES; ++box) {
        const BoxState &leftBox = left.boxes[box];
        const BoxState &rightBox = right.boxes[box];
        if (leftBox.count != rightBox.count ||
            leftBox.completed != rightBox.completed) {
            return false;
        }
        for (uint8_t item = 0; item < leftBox.count; ++item) {
            if (leftBox.animals[item] != rightBox.animals[item]) {
                return false;
            }
        }
    }
    return true;
}

bool rememberBoardState(const BoardState &state) {
    for (uint16_t index = 0; index < seenBoardStateCount; ++index) {
        if (boardStatesEqual(seenBoardStates[index], state)) {
            return true;
        }
    }

    if (seenBoardStateCount < MAX_TRACKED_BOARD_STATES) {
        seenBoardStates[seenBoardStateCount++] = state;
    } else {
        seenBoardStates[nextSeenBoardState] = state;
        nextSeenBoardState =
            (nextSeenBoardState + 1) % MAX_TRACKED_BOARD_STATES;
    }
    return false;
}

void resetBoardStateHistory() {
    seenBoardStateCount = 0;
    nextSeenBoardState = 0;
    repeatedBoardStateDetected = false;
    rememberBoardState(gameBoard);
}

bool restartButtonAvailable() {
    return currentMoveCount >= expectedMoveCount ||
        repeatedBoardStateDetected;
}

AnimalMetadata readAnimalMetadata(uint8_t index) {
    AnimalMetadata animal;
    animal.width = pgm_read_word(&animals[index].width);
    animal.height = pgm_read_word(&animals[index].height);
    animal.bitmap = reinterpret_cast<const uint16_t *>(
        pgm_read_ptr(&animals[index].bitmap));
    return animal;
}

uint16_t readAnimalPixel(const AnimalMetadata &animal, int x, int y) {
    return pgm_read_word(animal.bitmap +
        static_cast<size_t>(y) * animal.width + x);
}

void fillRectClipped(int x, int y, int width, int height, uint16_t color) {
    int left = std::max(0, x);
    int top = std::max(0, y);
    int right = std::min(SCREEN_WIDTH, x + width);
    int bottom = std::min(SCREEN_HEIGHT, y + height);
    if (left < right && top < bottom) {
        tft.fillRect(left, top, right - left, bottom - top, color);
    }
}

void drawTransparentAnimal(const AnimalMetadata &animal, int x, int y,
    int clipLeft, int clipRight) {
    int firstX = std::max(0, clipLeft - x);
    int lastX = std::min(static_cast<int>(animal.width), clipRight - x);
    int firstY = std::max(0, -y);
    int lastY = std::min(static_cast<int>(animal.height), SCREEN_HEIGHT - y);
    if (firstX >= lastX || firstY >= lastY) {
        return;
    }

    for (int sourceY = firstY; sourceY < lastY; ++sourceY) {
        int sourceX = firstX;
        while (sourceX < lastX) {
            if (readAnimalPixel(animal, sourceX, sourceY) ==
                ANIMAL_TRANSPARENT) {
                ++sourceX;
                continue;
            }

            int runStart = sourceX;
            int runLength = 0;
            while (sourceX < lastX &&
                runLength < OPAQUE_RUN_BUFFER_SIZE) {
                uint16_t color = readAnimalPixel(animal, sourceX, sourceY);
                if (color == ANIMAL_TRANSPARENT) {
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

void drawAnimalAt(uint8_t animalIndex, int x, int y, bool highlighted,
    int clipLeft, int clipRight) {
    AnimalMetadata animal = readAnimalMetadata(animalIndex);
    if (highlighted) {
        fillRectClipped(x, y, animal.width, animal.height, ILI9341_BLACK);
    }
    drawTransparentAnimal(animal, x, y, clipLeft, clipRight);
}

void createBoxLayout() {
    const int leftMargin = SCREEN_WIDTH / 40;
    const int boxGap = SCREEN_WIDTH / 40;
    const int rowTop = SCREEN_HEIGHT / 30;
    const int rowBottom = SCREEN_HEIGHT - BOX_HEIGHT - rowTop;

    for (uint8_t box = 0; box < BOXES_TOP_ROW; ++box) {
        boxPositions[box] = {
            leftMargin + box * (BOX_WIDTH + boxGap),
            rowTop
        };
    }
    for (uint8_t box = 0; box < BOXES_BOTTOM_ROW; ++box) {
        uint8_t index = BOXES_TOP_ROW + box;
        boxPositions[index] = {
            boxPositions[box].x,
            rowBottom
        };
    }
}

bool applyGameMove(BoardState &board, uint8_t source, uint8_t destination,
    uint8_t &amountMoved) {
    amountMoved = 0;
    if (source >= TOTAL_BOXES || destination >= TOTAL_BOXES ||
        source == destination) {
        return false;
    }

    BoxState &from = board.boxes[source];
    BoxState &to = board.boxes[destination];
    if (from.completed || to.completed || from.count == 0 ||
        to.count >= BOX_CAPACITY) {
        return false;
    }

    uint8_t animal = from.animals[from.count - 1];
    if (to.count != 0 && to.animals[to.count - 1] != animal) {
        return false;
    }

    uint8_t groupSize = 1;
    while (groupSize < from.count &&
        from.animals[from.count - groupSize - 1] == animal) {
        ++groupSize;
    }
    uint8_t freeSpaces = BOX_CAPACITY - to.count;
    amountMoved = std::min(groupSize, freeSpaces);

    for (uint8_t i = 0; i < amountMoved; ++i) {
        to.animals[to.count + i] = animal;
    }
    from.count -= amountMoved;
    to.count += amountMoved;
    return true;
}

uint8_t topGroupSize(const BoxState &box) {
    if (box.count == 0) {
        return 0;
    }
    uint8_t animal = box.animals[box.count - 1];
    uint8_t count = 1;
    while (count < box.count &&
        box.animals[box.count - count - 1] == animal) {
        ++count;
    }
    return count;
}

bool validAnimalInventory(const BoardState &board) {
    uint8_t totals[ANIMAL_COUNT] = {};
    uint8_t totalAnimals = 0;
    for (uint8_t i = 0; i < TOTAL_BOXES; ++i) {
        const BoxState &box = board.boxes[i];
        if (box.count > BOX_CAPACITY) {
            return false;
        }
        totalAnimals += box.count;
        for (uint8_t j = 0; j < box.count; ++j) {
            if (box.animals[j] >= ANIMAL_COUNT) {
                return false;
            }
            ++totals[box.animals[j]];
        }
    }
    if (totalAnimals != ANIMAL_COUNT * BOX_CAPACITY) {
        return false;
    }
    for (uint8_t animal = 0; animal < ANIMAL_COUNT; ++animal) {
        if (totals[animal] != BOX_CAPACITY) {
            return false;
        }
    }
    return true;
}

bool boxIsComplete(const BoxState &box) {
    if (box.count != BOX_CAPACITY) {
        return false;
    }
    for (uint8_t i = 1; i < BOX_CAPACITY; ++i) {
        if (box.animals[i] != box.animals[0]) {
            return false;
        }
    }
    return true;
}

bool loadLevelFromTable(uint32_t level) {
    if (level == 0 || level > NUM_ANIMAL_LEVELS) {
        return false;
    }

    uint16_t levelIndex = static_cast<uint16_t>(level - 1);
    memset(&gameBoard, 0, sizeof(gameBoard));
    expectedMoveCount = pgm_read_byte(&animal_levels[levelIndex].moves);
    if (expectedMoveCount == 0) {
        return false;
    }

    for (uint8_t box = 0; box < TOTAL_BOXES; ++box) {
        uint16_t encoded =
            pgm_read_word(&animal_levels[levelIndex].placements[box]);
        uint8_t digits[BOX_CAPACITY];
        uint8_t count = 0;
        while (encoded != 0) {
            if (count == BOX_CAPACITY) {
                return false;
            }
            uint8_t animal = encoded % 10;
            if (animal == 0 || animal > ANIMAL_COUNT) {
                return false;
            }
            digits[count++] = animal - 1;
            encoded /= 10;
        }

        BoxState &stack = gameBoard.boxes[box];
        stack.count = count;
        stack.completed = false;
        for (uint8_t item = 0; item < count; ++item) {
            stack.animals[item] = digits[count - item - 1];
        }
    }

    if (!validAnimalInventory(gameBoard)) {
        return false;
    }

    currentMoveCount = 0;
    selectedSource = NO_BOX;
    undoAvailable = false;
    playerWon = false;
    boardDeadlocked = false;
    animationInProgress = false;
    levelLoadFailed = false;
    resetBoardStateHistory();
    return true;
}

bool hasLegalMove(const BoardState &board) {
    for (uint8_t source = 0; source < TOTAL_BOXES; ++source) {
        const BoxState &from = board.boxes[source];
        if (from.completed || from.count == 0) {
            continue;
        }
        uint8_t animal = from.animals[from.count - 1];
        uint8_t movable = topGroupSize(from);
        for (uint8_t destination = 0; destination < TOTAL_BOXES;
            ++destination) {
            const BoxState &to = board.boxes[destination];
            if (source == destination || to.completed ||
                to.count >= BOX_CAPACITY ||
                (to.count != 0 && to.animals[to.count - 1] != animal)) {
                continue;
            }
            if (movable > 0 && BOX_CAPACITY - to.count > 0) {
                return true;
            }
        }
    }
    return false;
}

bool isWinningBoard(const BoardState &board) {
    bool completedAnimal[ANIMAL_COUNT] = {};
    uint8_t completedBoxes = 0;
    uint8_t emptyBoxes = 0;
    for (uint8_t i = 0; i < TOTAL_BOXES; ++i) {
        const BoxState &box = board.boxes[i];
        if (box.count == 0) {
            ++emptyBoxes;
        }
        if (!box.completed) {
            continue;
        }
        if (!boxIsComplete(box)) {
            return false;
        }
        uint8_t animal = box.animals[0];
        if (completedAnimal[animal]) {
            return false;
        }
        completedAnimal[animal] = true;
        ++completedBoxes;
    }
    if (completedBoxes != ANIMAL_COUNT || emptyBoxes != 1) {
        return false;
    }
    for (uint8_t animal = 0; animal < ANIMAL_COUNT; ++animal) {
        if (!completedAnimal[animal]) {
            return false;
        }
    }
    return true;
}

void drawBoxFrame(const BoxPosition &position, bool completed) {
    if (completed) {
        tft.fillRect(position.x, position.y, BOX_WIDTH,
            COMPLETED_FRAME_THICKNESS, GOLD);
        tft.fillRect(position.x, position.y + BOX_HEIGHT -
            COMPLETED_FRAME_THICKNESS, BOX_WIDTH,
            COMPLETED_FRAME_THICKNESS, GOLD);
        tft.fillRect(position.x, position.y, COMPLETED_FRAME_THICKNESS,
            BOX_HEIGHT, GOLD);
        tft.fillRect(position.x + BOX_WIDTH - COMPLETED_FRAME_THICKNESS,
            position.y, COMPLETED_FRAME_THICKNESS, BOX_HEIGHT, GOLD);
        return;
    }
    tft.fillRect(position.x, position.y, FRAME_THICKNESS,
        BOX_HEIGHT, ILI9341_BLACK);
    tft.fillRect(position.x + BOX_WIDTH - FRAME_THICKNESS, position.y,
        FRAME_THICKNESS, BOX_HEIGHT, ILI9341_BLACK);
    tft.fillRect(position.x, position.y + BOX_HEIGHT - FRAME_THICKNESS,
        BOX_WIDTH, FRAME_THICKNESS, ILI9341_BLACK);
}

void drawBox(uint8_t boxIndex, uint8_t visibleCount, bool highlightTop) {
    const BoxPosition &position = boxPositions[boxIndex];
    const BoxState &box = gameBoard.boxes[boxIndex];
    visibleCount = std::min(visibleCount, box.count);
    tft.fillRect(position.x, position.y, BOX_WIDTH, BOX_HEIGHT, LIGHT_BLUE);

    int clipLeft = position.x + FRAME_THICKNESS;
    int clipRight = position.x + BOX_WIDTH - FRAME_THICKNESS;
    for (uint8_t layer = 0; layer < visibleCount; ++layer) {
        AnimalMetadata animal = readAnimalMetadata(box.animals[layer]);
        int x = position.x + (BOX_WIDTH - animal.width) / 2;
        int bottom = position.y + BOX_HEIGHT - layer * IMAGE_VERTICAL_STEP;
        int y = bottom - animal.height;
        drawAnimalAt(box.animals[layer], x, y,
            highlightTop && layer == visibleCount - 1,
            clipLeft, clipRight);
    }
    drawBoxFrame(position, box.completed);
}

int informationX() {
    const int boxGap = SCREEN_WIDTH / 40;
    return boxPositions[BOXES_TOP_ROW + BOXES_BOTTOM_ROW - 1].x +
        BOX_WIDTH + boxGap;
}

int informationY() {
    return boxPositions[BOXES_TOP_ROW].y;
}

int informationWidth() {
    return SCREEN_WIDTH - informationX();
}

void drawButton(int y, const char *label) {
    const int x = informationX() + 4;
    const int width = informationWidth() - 8;
    tft.drawRect(x, y, width, INFO_BUTTON_HEIGHT, ILI9341_BLACK);
    tft.setTextColor(ILI9341_BLACK);
    tft.setTextSize(1);
    tft.setCursor(x + 4, y + 6);
    tft.print(label);
}

void drawGameInformation() {
    const int x = informationX();
    const int y = informationY();
    const int width = informationWidth();
    const int height = SCREEN_HEIGHT - y;
    tft.fillRect(x, y, width, height, LIGHT_BLUE);
    tft.setTextColor(ILI9341_BLACK);

    if (levelLoadFailed) {
        tft.setTextSize(1);
        tft.setCursor(x + 2, y + 6);
        tft.println("LEVEL DATA ERROR");
        tft.setCursor(x + 2, y + 22);
        tft.println("See serial log");
        return;
    }

    if (boardDeadlocked) {
        tft.setTextSize(1);
        tft.setCursor(x + 2, y + 2);
        tft.print("LEVEL ");
        tft.println(currentLevel);
        tft.setCursor(x + 2, y + 16);
        tft.print("MOVES ");
        tft.println(currentMoveCount);
        tft.setTextSize(2);
        tft.setCursor(x + 2, y + 32);
        tft.print("NO MOVES");
        if (undoAvailable) {
            drawButton(y + 58, "UNDO LAST");
            drawButton(y + 82, "RESTART");
        } else {
            drawButton(y + 82, "RESTART");
        }
        return;
    }

    if (playerWon) {
        tft.setTextSize(1);
        tft.setCursor(x + 2, y + 2);
        tft.print("LEVEL ");
        tft.println(currentLevel);
        tft.setCursor(x + 2, y + 16);
        tft.print("MOVES ");
        tft.println(currentMoveCount);
        tft.setTextSize(2);
        tft.setCursor(x + 2, y + 32);
        tft.print("YOU WIN");
        tft.setTextSize(1);
        tft.setCursor(x + 2, y + 48);
        tft.print("PAR ");
        tft.println(expectedMoveCount);
        drawButton(y + 58, "NEXT LEVEL");
        drawButton(y + 82, restartButtonAvailable() ?
            "RESTART" : "PLAY AGAIN");
        return;
    }

    if (restartButtonAvailable()) {
        tft.setTextSize(1);
        tft.setCursor(x + 2, y + 4);
        tft.print("LEVEL ");
        tft.println(currentLevel);
        tft.setCursor(x + 2, y + 18);
        tft.print("MOVES ");
        tft.println(currentMoveCount);
        tft.setCursor(x + 2, y + 32);
        tft.print("PAR ");
        tft.println(expectedMoveCount);
        drawButton(y + PAR_RESTART_BUTTON_OFFSET, "RESTART");
        return;
    }

    tft.setTextSize(2);
    tft.setCursor(x + 2, y + 4);
    tft.print("LEVEL");
    tft.setCursor(x + 2, y + 24);
    tft.println(currentLevel);
    tft.setCursor(x + 2, y + 52);
    tft.print("MOVES");
    tft.setCursor(x + 2, y + 72);
    tft.println(currentMoveCount);
    tft.setTextSize(1);
    tft.setCursor(x + 2, y + 92);
    tft.print("PAR ");
    tft.println(expectedMoveCount);
}

int levelMenuButtonX(uint8_t index) {
    const uint8_t row = index / 5;
    const uint8_t firstIndex = row * 5;
    const uint8_t itemsInRow = std::min<uint8_t>(
        5, LEVEL_MENU_VALUE_COUNT - firstIndex);
    const int rowWidth = itemsInRow * MENU_BUTTON_WIDTH +
        (itemsInRow - 1) * MENU_BUTTON_GAP_X;
    const int left = (SCREEN_WIDTH - rowWidth) / 2;
    return left + (index % 5) *
        (MENU_BUTTON_WIDTH + MENU_BUTTON_GAP_X);
}

int levelMenuButtonY(uint8_t index) {
    return MENU_BUTTON_TOP + (index / 5) * MENU_BUTTON_ROW_STEP;
}

void drawLevelMenu() {
    tft.fillScreen(LIGHT_BLUE);
    tft.setTextColor(ILI9341_BLACK);
    tft.setTextSize(2);
    tft.setCursor((SCREEN_WIDTH - 13 * 12) / 2, 18);
    tft.print("Jump to level");

    tft.setTextSize(2);
    for (uint8_t index = 0; index < LEVEL_MENU_VALUE_COUNT; ++index) {
        const int x = levelMenuButtonX(index);
        const int y = levelMenuButtonY(index);
        const uint8_t level = LEVEL_MENU_VALUES[index];
        const uint8_t digits = level >= 100 ? 3 : (level >= 10 ? 2 : 1);
        tft.drawRect(x, y, MENU_BUTTON_WIDTH, MENU_BUTTON_HEIGHT,
            ILI9341_BLACK);
        tft.setCursor(x + (MENU_BUTTON_WIDTH - digits * 12) / 2, y + 13);
        tft.print(level);
    }
}

void drawWholeScreen(int overrideBox = -1, int overrideCount = -1,
    bool suppressSelection = false) {
    tft.fillScreen(LIGHT_BLUE);
    for (uint8_t box = 0; box < TOTAL_BOXES; ++box) {
        uint8_t visibleCount = gameBoard.boxes[box].count;
        if (box == overrideBox && overrideCount >= 0) {
            visibleCount = static_cast<uint8_t>(overrideCount);
        }
        bool highlight = !suppressSelection && selectedSource == box;
        drawBox(box, visibleCount, highlight);
    }
    drawGameInformation();
}

void startNewGame(bool nextLevel) {
    if (nextLevel) {
        currentLevel = currentLevel >= NUM_ANIMAL_LEVELS ?
            1 : currentLevel + 1;
    }
    levelLoadFailed = !loadLevelFromTable(currentLevel);
    if (levelLoadFailed) {
        Serial.print("Unable to load animal level ");
        Serial.println(currentLevel);
    } else {
        Serial.print("Animal sorting level ");
        Serial.print(currentLevel);
        Serial.print(", expected moves ");
        Serial.println(expectedMoveCount);
    }
    drawWholeScreen();
}

void handleLevelMenuTap(int x, int y) {
    for (uint8_t index = 0; index < LEVEL_MENU_VALUE_COUNT; ++index) {
        const int left = levelMenuButtonX(index);
        const int top = levelMenuButtonY(index);
        if (x >= left && x < left + MENU_BUTTON_WIDTH &&
            y >= top && y < top + MENU_BUTTON_HEIGHT) {
            currentLevel = LEVEL_MENU_VALUES[index];
            showingLevelMenu = false;
            startNewGame(false);
            return;
        }
    }
}

void createStartingBoard() {
    createBoxLayout();
    startNewGame(false);
}

MovingAnimal movingAnimalAt(uint8_t source, uint8_t destination,
    uint8_t frame) {
    const BoxState &from = gameBoard.boxes[source];
    const BoxState &to = gameBoard.boxes[destination];
    const BoxPosition &fromPosition = boxPositions[source];
    const BoxPosition &toPosition = boxPositions[destination];
    const int denominator = ANIMATION_FRAMES - 1;
    uint8_t animalIndex = from.animals[from.count - 1];
    AnimalMetadata animal = readAnimalMetadata(animalIndex);
    int startX = fromPosition.x + (BOX_WIDTH - animal.width) / 2;
    int endX = toPosition.x + (BOX_WIDTH - animal.width) / 2;
    int startY = fromPosition.y + BOX_HEIGHT -
        (from.count - 1) * IMAGE_VERTICAL_STEP - animal.height;
    int endY = toPosition.y + BOX_HEIGHT -
        to.count * IMAGE_VERTICAL_STEP - animal.height;

    MovingAnimal position;
    position.animal = animalIndex;
    position.x = startX + (endX - startX) * frame / denominator;
    position.y = startY + (endY - startY) * frame / denominator;
    position.width = animal.width;
    position.height = animal.height;
    return position;
}

bool rectanglesIntersect(int leftA, int topA, int rightA, int bottomA,
    int leftB, int topB, int rightB, int bottomB) {
    return leftA < rightB && rightA > leftB &&
        topA < bottomB && bottomA > topB;
}

void restoreBoxesUnderMovingAnimal(const MovingAnimal &animal,
    uint8_t source) {
    fillRectClipped(animal.x, animal.y, animal.width, animal.height,
        LIGHT_BLUE);

    for (uint8_t box = 0; box < TOTAL_BOXES; ++box) {
        const BoxPosition &position = boxPositions[box];
        if (!rectanglesIntersect(animal.x, animal.y,
                animal.x + animal.width, animal.y + animal.height,
                position.x, position.y - 16,
                position.x + BOX_WIDTH, position.y + BOX_HEIGHT)) {
            continue;
        }
        uint8_t visibleCount = gameBoard.boxes[box].count;
        if (box == source) {
            --visibleCount;
        }
        drawBox(box, visibleCount, false);
    }

    if (rectanglesIntersect(animal.x, animal.y,
            animal.x + animal.width, animal.y + animal.height,
            informationX(), informationY(), SCREEN_WIDTH, SCREEN_HEIGHT)) {
        drawGameInformation();
    }
}

void redrawDeselectedAnimal(uint8_t boxIndex) {
    const BoxPosition &boxPosition = boxPositions[boxIndex];
    const BoxState &box = gameBoard.boxes[boxIndex];
    uint8_t animalIndex = box.animals[box.count - 1];
    AnimalMetadata animal = readAnimalMetadata(animalIndex);
    MovingAnimal bounds;
    bounds.animal = animalIndex;
    bounds.x = boxPosition.x + (BOX_WIDTH - animal.width) / 2;
    bounds.y = boxPosition.y + BOX_HEIGHT -
        (box.count - 1) * IMAGE_VERTICAL_STEP - animal.height;
    bounds.width = animal.width;
    bounds.height = animal.height;
    restoreBoxesUnderMovingAnimal(bounds, NO_BOX);
}

void animateSingleAnimal(uint8_t source, uint8_t destination) {
    bool hasPreviousPosition = false;
    MovingAnimal previousPosition;
    for (uint8_t frame = 0; frame < ANIMATION_FRAMES; ++frame) {
        if (hasPreviousPosition) {
            restoreBoxesUnderMovingAnimal(previousPosition, source);
        }
        drawBox(source, gameBoard.boxes[source].count - 1, false);
        drawBox(destination, gameBoard.boxes[destination].count, false);

        MovingAnimal position =
            movingAnimalAt(source, destination, frame);
        drawAnimalAt(position.animal, position.x, position.y, true,
            0, SCREEN_WIDTH);
        previousPosition = position;
        hasPreviousPosition = true;
        delay(16);
    }
    if (hasPreviousPosition) {
        restoreBoxesUnderMovingAnimal(previousPosition, source);
    }
}

bool transferOneAnimal(uint8_t source, uint8_t destination) {
    BoxState &from = gameBoard.boxes[source];
    BoxState &to = gameBoard.boxes[destination];
    if (from.count == 0 || to.count >= BOX_CAPACITY) {
        return false;
    }
    uint8_t animal = from.animals[from.count - 1];
    if (to.count != 0 && to.animals[to.count - 1] != animal) {
        return false;
    }
    to.animals[to.count++] = animal;
    --from.count;
    return true;
}

void updateCompletedBoxes() {
    for (uint8_t box = 0; box < TOTAL_BOXES; ++box) {
        if (!gameBoard.boxes[box].completed &&
            boxIsComplete(gameBoard.boxes[box])) {
            gameBoard.boxes[box].completed = true;
            drawBox(box, gameBoard.boxes[box].count, false);
        }
    }
}

void tryPlayerMove(uint8_t destination) {
    if (selectedSource >= TOTAL_BOXES || destination >= TOTAL_BOXES ||
        destination == selectedSource) {
        return;
    }

    BoardState preview = gameBoard;
    uint8_t amount = 0;
    if (!applyGameMove(preview, selectedSource, destination, amount)) {
        return;
    }

    const uint8_t source = selectedSource;
    undoBoard = gameBoard;
    undoMoveCount = currentMoveCount;
    animationInProgress = true;
    for (uint8_t moved = 0; moved < amount; ++moved) {
        animateSingleAnimal(source, destination);
        if (!transferOneAnimal(source, destination)) {
            Serial.println("Animal move failed during animation");
            gameBoard = undoBoard;
            animationInProgress = false;
            selectedSource = NO_BOX;
            drawWholeScreen();
            return;
        }
        drawBox(source, gameBoard.boxes[source].count, false);
        drawBox(destination, gameBoard.boxes[destination].count, false);
    }
    animationInProgress = false;

    undoAvailable = true;
    ++currentMoveCount;
    selectedSource = NO_BOX;
    updateCompletedBoxes();
    if (rememberBoardState(gameBoard)) {
        repeatedBoardStateDetected = true;
    }
    playerWon = isWinningBoard(gameBoard);
    boardDeadlocked = !playerWon && !hasLegalMove(gameBoard);
    drawBox(source, gameBoard.boxes[source].count, false);
    drawBox(destination, gameBoard.boxes[destination].count, false);
    drawGameInformation();
}

int hitTestBox(int x, int y) {
    for (uint8_t box = 0; box < TOTAL_BOXES; ++box) {
        const BoxPosition &position = boxPositions[box];
        if (x >= position.x && x < position.x + BOX_WIDTH &&
            y >= position.y && y < position.y + BOX_HEIGHT) {
            return box;
        }
    }
    return -1;
}

bool handleInformationTouch(int x, int y) {
    const int left = informationX() + 4;
    const int right = left + informationWidth() - 8;
    const int top = informationY();
    if (x < left || x >= right) {
        return false;
    }

    if (playerWon) {
        if (y >= top + 58 && y < top + 78) {
            startNewGame(true);
            return true;
        } else if (y >= top + 82 && y < top + 102) {
            startNewGame(false);
            return true;
        }
        return false;
    }

    if (boardDeadlocked) {
        if (undoAvailable && y >= top + 58 && y < top + 78) {
            gameBoard = undoBoard;
            currentMoveCount = undoMoveCount;
            selectedSource = NO_BOX;
            undoAvailable = false;
            boardDeadlocked = false;
            playerWon = false;
            if (rememberBoardState(gameBoard)) {
                repeatedBoardStateDetected = true;
            }
            drawWholeScreen();
            return true;
        } else if (y >= top + 82 && y < top + 102) {
            startNewGame(false);
            return true;
        }
        return false;
    }

    if (restartButtonAvailable() &&
        y >= top + PAR_RESTART_BUTTON_OFFSET &&
        y < top + PAR_RESTART_BUTTON_OFFSET + INFO_BUTTON_HEIGHT) {
        startNewGame(false);
        return true;
    }
    return false;
}

void handleTap(int x, int y) {
    if (showingLevelMenu) {
        handleLevelMenuTap(x, y);
        return;
    }
    if (levelLoadFailed) {
        return;
    }
    if (playerWon || boardDeadlocked) {
        handleInformationTouch(x, y);
        return;
    }
    if (animationInProgress) {
        return;
    }
    if (restartButtonAvailable() &&
        handleInformationTouch(x, y)) {
        return;
    }

    int boxIndex = hitTestBox(x, y);
    if (boxIndex < 0) {
        return;
    }
    uint8_t box = static_cast<uint8_t>(boxIndex);
    if (selectedSource == NO_BOX) {
        if (gameBoard.boxes[box].count == 0 ||
            gameBoard.boxes[box].completed) {
            return;
        }
        selectedSource = box;
        drawBox(box, gameBoard.boxes[box].count, true);
        return;
    }

    if (selectedSource == box) {
        uint8_t oldSource = selectedSource;
        selectedSource = NO_BOX;
        redrawDeselectedAnimal(oldSource);
        return;
    }
    tryPlayerMove(box);
}

void readTouchPosition(int &x, int &y) {
    TS_Point point = ts.getPoint();
    x = constrain(map(point.x, 200, 3800, 0, SCREEN_WIDTH - 1),
        0, SCREEN_WIDTH - 1);
    y = constrain(map(point.y, 200, 3800, 0, SCREEN_HEIGHT - 1),
        0, SCREEN_HEIGHT - 1);
}

void handleTouch() {
    bool touched = ts.touched();
    if (!touched) {
        touchWasDown = false;
        return;
    }

    if (touchWasDown || millis() - lastTouchTime < TOUCH_DEBOUNCE_MS) {
        return;
    }
    touchWasDown = true;
    lastTouchTime = millis();

    int x = 0;
    int y = 0;
    readTouchPosition(x, y);
    handleTap(x, y);
}

} // namespace

void setup() {
    setup_display();
    setup_touch();
    if (SHOW_MENU) {
        createBoxLayout();
        drawLevelMenu();
    } else {
        createStartingBoard();
    }
}

void loop() {
    handleTouch();
    delay(10);
}
