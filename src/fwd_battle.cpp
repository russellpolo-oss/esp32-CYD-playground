#include "defaults.h"
#include "common.cpp"
#include "common_touch.cpp"
#include "common_stats.cpp"
#include "fwd_art.h"

#include <algorithm>
#include <esp_system.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {

const uint8_t NO_CHARACTER = 0xFF;
const size_t NAME_BUFFER_SIZE = 16;
const size_t MESSAGE_BUFFER_SIZE = 72;
const uint16_t SCREEN_BACKGROUND = 0x0841;
const uint16_t PANEL_BACKGROUND = 0x2104;
const uint16_t DISABLED_GREY = 0x7BEF;
const uint16_t ACCENT_CYAN = 0x07FF;
const uint16_t PLAYER_GREEN = 0x07E0;
const uint16_t HP_BAR_TOP = 30;
const uint16_t HP_BAR_BOTTOM = 166;
const uint16_t HP_BAR_HEIGHT = HP_BAR_BOTTOM - HP_BAR_TOP;
const uint16_t COMBAT_MESSAGE_Y = 178;
const uint16_t COMBAT_MESSAGE_HEIGHT = 20;
const uint16_t COMBAT_MESSAGE_DURATION_MS = 10000;
const uint16_t TOUCH_DEBOUNCE_MS = 120;
const uint16_t CHARACTER_CELL_TOP = 32;
const uint16_t CHARACTER_CELL_BOTTOM = 205;
const uint16_t ACTION_BUTTON_Y = 205;
const uint16_t ACTION_BUTTON_HEIGHT = 28;
const uint16_t ACTION_BUTTON_WIDTH = 92;

enum class Screen : uint8_t {
    CharacterSelection,
    Explore,
    Combat,
    GameOver
};

struct CharacterMetadata {
    bool player;
    const char *nameInFlash;
    uint8_t startingHp;
    uint16_t width;
    uint16_t height;
    const uint16_t *bitmap;
};

struct Button {
    int x;
    int y;
    int width;
    int height;
};

struct GameState {
    Screen screen;
    uint8_t playerIndex;
    uint8_t monsterIndex;
    uint8_t playerStartingHp;
    uint8_t playerHp;
    uint8_t monsterStartingHp;
    uint8_t monsterHp;
    char playerName[NAME_BUFFER_SIZE];
    char monsterName[NAME_BUFFER_SIZE];
    char combatMessage[MESSAGE_BUFFER_SIZE];
    uint32_t combatMessageStartedAt;
    bool combatMessageActive;
    bool counterattackPending;
    bool transitionPending;
    Screen transitionTarget;
};

GameState game = {};
uint8_t playableIndices[FWD_COUNT];
uint8_t monsterIndices[FWD_COUNT];
uint8_t playableCount = 0;
uint8_t monsterCount = 0;
bool touchWasDown = false;
uint32_t lastTouchTime = 0;
uint16_t bitmapRow[SCREEN_WIDTH];

CharacterMetadata readCharacterMetadata(uint8_t index) {
    const CharacterItem *item = &fwd_character[index];
    CharacterMetadata metadata;
    metadata.player = pgm_read_byte(&item->player) != 0;
    metadata.nameInFlash = reinterpret_cast<const char *>(
        pgm_read_ptr(&item->name));
    metadata.startingHp = pgm_read_byte(&item->starting_hp);
    metadata.width = pgm_read_word(&item->width);
    metadata.height = pgm_read_word(&item->height);
    metadata.bitmap = reinterpret_cast<const uint16_t *>(
        pgm_read_ptr(&item->bitmap));
    return metadata;
}

void copyCharacterName(uint8_t index, char *destination, size_t capacity) {
    if (capacity == 0) {
        return;
    }

    CharacterMetadata metadata = readCharacterMetadata(index);
    size_t position = 0;
    while (position + 1 < capacity) {
        char character = static_cast<char>(
            pgm_read_byte(metadata.nameInFlash + position));
        destination[position++] = character;
        if (character == '\0') {
            return;
        }
    }
    destination[capacity - 1] = '\0';
}

void collectCharacterIndices() {
    playableCount = 0;
    monsterCount = 0;
    for (uint8_t index = 0; index < FWD_COUNT; ++index) {
        CharacterMetadata metadata = readCharacterMetadata(index);
        if (metadata.player) {
            playableIndices[playableCount++] = index;
        } else {
            monsterIndices[monsterCount++] = index;
        }
    }
}

void drawCenteredText(const char *text, int y, uint8_t textSize,
    uint16_t color) {
    tft.setTextSize(textSize);
    tft.setTextColor(color);
    int textWidth = static_cast<int>(strlen(text)) * 6 * textSize;
    int x = std::max(0, (SCREEN_WIDTH - textWidth) / 2);
    tft.setCursor(x, y);
    tft.print(text);
}

void drawTextCenteredInCell(const char *text, int left, int width, int y,
    uint8_t textSize, uint16_t color) {
    tft.setTextSize(textSize);
    tft.setTextColor(color);
    int textWidth = static_cast<int>(strlen(text)) * 6 * textSize;
    tft.setCursor(left + (width - textWidth) / 2, y);
    tft.print(text);
}

void fitImageDimensions(const CharacterMetadata &metadata, int maxWidth,
    int maxHeight, int &drawWidth, int &drawHeight) {
    if (metadata.width == 0 || metadata.height == 0) {
        drawWidth = 0;
        drawHeight = 0;
        return;
    }

    if (maxWidth * metadata.height <= maxHeight * metadata.width) {
        drawWidth = maxWidth;
        drawHeight = std::max(1, static_cast<int>(
            metadata.height * maxWidth / metadata.width));
    } else {
        drawHeight = maxHeight;
        drawWidth = std::max(1, static_cast<int>(
            metadata.width * maxHeight / metadata.height));
    }
}

void drawScaledBitmap(const CharacterMetadata &metadata, int x, int y,
    int drawWidth, int drawHeight) {
    for (int outputY = 0; outputY < drawHeight; ++outputY) {
        int sourceY = outputY * metadata.height / drawHeight;
        int outputX = 0;
        while (outputX < drawWidth) {
            int sourceX = outputX * metadata.width / drawWidth;
            uint16_t color = pgm_read_word(
                metadata.bitmap +
                static_cast<size_t>(sourceY) * metadata.width + sourceX);
            if (color == TRANSPARENT) {
                ++outputX;
                continue;
            }

            int runStart = outputX;
            int runLength = 0;
            while (outputX < drawWidth && runLength < SCREEN_WIDTH) {
                sourceX = outputX * metadata.width / drawWidth;
                color = pgm_read_word(
                    metadata.bitmap +
                    static_cast<size_t>(sourceY) * metadata.width + sourceX);
                if (color == TRANSPARENT) {
                    break;
                }
                bitmapRow[runLength++] = color;
                ++outputX;
            }
            tft.drawRGBBitmap(x + runStart, y + outputY, bitmapRow,
                runLength, 1);
        }
    }
}

void drawCharacterInArea(uint8_t index, int x, int y, int maxWidth,
    int maxHeight) {
    CharacterMetadata metadata = readCharacterMetadata(index);
    int drawWidth = 0;
    int drawHeight = 0;
    fitImageDimensions(metadata, maxWidth, maxHeight, drawWidth, drawHeight);
    if (drawWidth == 0 || drawHeight == 0) {
        return;
    }
    int drawX = x + (maxWidth - drawWidth) / 2;
    int drawY = y + (maxHeight - drawHeight) / 2;
    drawScaledBitmap(metadata, drawX, drawY, drawWidth, drawHeight);
}

void drawButton(const Button &button, const char *label, uint16_t fillColor,
    uint16_t textColor) {
    tft.fillRect(button.x, button.y, button.width, button.height, fillColor);
    tft.drawRect(button.x, button.y, button.width, button.height, textColor);
    drawTextCenteredInCell(label, button.x, button.width,
        button.y + (button.height - 8) / 2, 1, textColor);
}

Button exploreButton() {
    Button button = {24, 198, 126, 30};
    return button;
}

Button shopButton() {
    Button button = {170, 198, 126, 30};
    return button;
}

Button runButton() {
    Button button = {48, ACTION_BUTTON_Y, ACTION_BUTTON_WIDTH,
        ACTION_BUTTON_HEIGHT};
    return button;
}

Button attackButton() {
    Button button = {180, ACTION_BUTTON_Y, ACTION_BUTTON_WIDTH,
        ACTION_BUTTON_HEIGHT};
    return button;
}

Button playAgainButton() {
    Button button = {80, 177, 160, 34};
    return button;
}

bool pointInButton(int x, int y, const Button &button) {
    return x >= button.x && x < button.x + button.width &&
        y >= button.y && y < button.y + button.height;
}

void drawCharacterSelectionScreen() {
    tft.fillScreen(SCREEN_BACKGROUND);
    drawCenteredText("CHOOSE A CHARACTER", 8, 2, ILI9341_WHITE);

    if (playableCount == 0) {
        drawCenteredText("NO PLAYABLE CHARACTERS", 100, 1, ILI9341_RED);
        return;
    }

    int cellWidth = SCREEN_WIDTH / playableCount;
    for (uint8_t slot = 0; slot < playableCount; ++slot) {
        int cellLeft = slot * cellWidth;
        int cellRight = (slot + 1) * cellWidth;
        int actualWidth = cellRight - cellLeft;
        int cardLeft = cellLeft + 4;
        int cardWidth = actualWidth - 8;
        tft.drawRect(cardLeft, 30, cardWidth, 171, ACCENT_CYAN);
        drawCharacterInArea(playableIndices[slot], cardLeft + 3, 36,
            cardWidth - 6, 116);

        char name[NAME_BUFFER_SIZE];
        copyCharacterName(playableIndices[slot], name, sizeof(name));
        drawTextCenteredInCell(name, cardLeft, cardWidth, 157, 1,
            ILI9341_WHITE);

        CharacterMetadata metadata =
            readCharacterMetadata(playableIndices[slot]);
        char hpText[12];
        snprintf(hpText, sizeof(hpText), "%u HP",
            static_cast<unsigned>(metadata.startingHp));
        drawTextCenteredInCell(hpText, cardLeft, cardWidth, 174, 1,
            ILI9341_WHITE);
    }
    drawCenteredText("Tap a character to begin", 218, 1, ILI9341_WHITE);
}

void drawExploreScreen() {
    tft.fillScreen(SCREEN_BACKGROUND);
    drawCenteredText("EXPLORE / SHOP", 10, 2, ILI9341_WHITE);
    tft.drawRect(76, 35, 168, 150, ACCENT_CYAN);
    drawCharacterInArea(game.playerIndex, 84, 40, 152, 132);
    drawTextCenteredInCell(game.playerName, 76, 168, 153, 2,
        ILI9341_WHITE);

    char hpText[20];
    snprintf(hpText, sizeof(hpText), "HP %u / %u",
        static_cast<unsigned>(game.playerHp),
        static_cast<unsigned>(game.playerStartingHp));
    drawTextCenteredInCell(hpText, 76, 168, 176, 1, ILI9341_WHITE);

    drawButton(exploreButton(), "EXPLORE", PLAYER_GREEN, ILI9341_BLACK);
    drawButton(shopButton(), "SHOP", DISABLED_GREY, ILI9341_DARKGREY);
}

uint16_t hpColor(uint8_t currentHp, uint8_t startingHp) {
    if (startingHp == 0 ||
        static_cast<uint16_t>(currentHp) * 100 <=
            static_cast<uint16_t>(startingHp) * 40) {
        return ILI9341_RED;
    }
    return ILI9341_WHITE;
}

void drawHpIndicator(bool isPlayer, uint8_t currentHp, uint8_t startingHp) {
    uint16_t color = hpColor(currentHp, startingHp);
    char hpText[12];
    snprintf(hpText, sizeof(hpText), "%u HP",
        static_cast<unsigned>(currentHp));
    tft.setTextSize(2);
    tft.setTextColor(color);
    int textWidth = static_cast<int>(strlen(hpText)) * 12;
    int textX = isPlayer ? 8 : SCREEN_WIDTH - textWidth - 8;
    tft.setCursor(textX, 5);
    tft.print(hpText);

    if (currentHp == 0 || startingHp == 0) {
        return;
    }

    int lineLength = static_cast<int>(currentHp) * HP_BAR_HEIGHT /
        startingHp;
    int lineX = isPlayer ? 2 : SCREEN_WIDTH - 5;
    tft.fillRect(lineX, HP_BAR_BOTTOM - lineLength, 3, lineLength, color);
}

void drawCombatScreen() {
    tft.fillScreen(SCREEN_BACKGROUND);
    drawHpIndicator(true, game.playerHp, game.playerStartingHp);
    drawHpIndicator(false, game.monsterHp, game.monsterStartingHp);

    drawCharacterInArea(game.playerIndex, 20, 30, 130, 122);
    drawCharacterInArea(game.monsterIndex, 170, 30, 130, 122);
    drawTextCenteredInCell(game.playerName, 18, 134, 154, 1,
        ILI9341_WHITE);
    drawTextCenteredInCell(game.monsterName, 168, 134, 154, 1,
        ILI9341_WHITE);

    tft.fillRect(8, COMBAT_MESSAGE_Y, SCREEN_WIDTH - 16,
        COMBAT_MESSAGE_HEIGHT, PANEL_BACKGROUND);
    if (game.combatMessageActive) {
        drawTextCenteredInCell(game.combatMessage, 8, SCREEN_WIDTH - 16,
            COMBAT_MESSAGE_Y + 6, 1, ILI9341_WHITE);
        return;
    }

    drawButton(runButton(), "RUN", 0xA145, ILI9341_WHITE);
    drawButton(attackButton(), "ATTACK", 0xA145, ILI9341_WHITE);
}

void drawGameOverScreen() {
    tft.fillScreen(SCREEN_BACKGROUND);
    drawCenteredText("GAME OVER", 24, 3, ILI9341_RED);
    drawTextCenteredInCell(game.playerName, 16, SCREEN_WIDTH - 32, 82, 2,
        ILI9341_WHITE);
    drawCenteredText("was defeated by", 112, 1, ILI9341_WHITE);
    drawTextCenteredInCell(game.monsterName, 16, SCREEN_WIDTH - 32, 136, 2,
        ILI9341_WHITE);
    drawButton(playAgainButton(), "PLAY AGAIN", PLAYER_GREEN,
        ILI9341_BLACK);
}

void drawCurrentScreen() {
    switch (game.screen) {
        case Screen::CharacterSelection:
            drawCharacterSelectionScreen();
            break;
        case Screen::Explore:
            drawExploreScreen();
            break;
        case Screen::Combat:
            drawCombatScreen();
            break;
        case Screen::GameOver:
            drawGameOverScreen();
            break;
    }
}

uint32_t randomBelow(uint32_t upperExclusive) {
    uint32_t threshold = (0U - upperExclusive) % upperExclusive;
    uint32_t value;
    do {
        value = esp_random();
    } while (value < threshold);
    return value % upperExclusive;
}

float attackProbability(uint8_t attackerStartingHp,
    uint8_t defenderStartingHp) {
    float probability = 0.5f + 0.05f *
        (static_cast<int>(attackerStartingHp) -
            static_cast<int>(defenderStartingHp));
    return std::max(0.05f, std::min(0.95f, probability));
}

bool attackHits(uint8_t attackerStartingHp, uint8_t defenderStartingHp) {
    double roll = static_cast<double>(esp_random()) / 4294967296.0;
    return roll < attackProbability(attackerStartingHp, defenderStartingHp);
}

uint8_t rollDamage() {
    return static_cast<uint8_t>(1 + randomBelow(3));
}

uint8_t applyDamage(uint8_t currentHp, uint8_t damage) {
    return damage >= currentHp ? 0 : currentHp - damage;
}

void showCombatMessage(const char *message, bool counterattackPending,
    bool transitionPending, Screen transitionTarget) {
    snprintf(game.combatMessage, sizeof(game.combatMessage), "%s", message);
    game.combatMessageStartedAt = millis();
    game.combatMessageActive = true;
    game.counterattackPending = counterattackPending;
    game.transitionPending = transitionPending;
    game.transitionTarget = transitionTarget;
    drawCombatScreen();
}

void performMonsterCounterattack();
void dismissCombatMessage();

void performPlayerAttack() {
    char message[MESSAGE_BUFFER_SIZE];
    if (attackHits(game.playerStartingHp, game.monsterStartingHp)) {
        uint8_t damage = rollDamage();
        game.monsterHp = applyDamage(game.monsterHp, damage);
        if (game.monsterHp == 0) {
            snprintf(message, sizeof(message), "%s defeated!",
                game.monsterName);
            showCombatMessage(message, false, true, Screen::Explore);
            return;
        }
        snprintf(message, sizeof(message), "%s hits %s for %u HP!",
            game.playerName, game.monsterName,
            static_cast<unsigned>(damage));
    } else {
        snprintf(message, sizeof(message), "%s misses!", game.playerName);
    }
    showCombatMessage(message, true, false, Screen::Combat);
}

void performMonsterCounterattack() {
    char message[MESSAGE_BUFFER_SIZE];
    if (attackHits(game.monsterStartingHp, game.playerStartingHp)) {
        uint8_t damage = rollDamage();
        game.playerHp = applyDamage(game.playerHp, damage);
        if (game.playerHp == 0) {
            snprintf(message, sizeof(message), "%s was defeated by %s!",
                game.playerName, game.monsterName);
            showCombatMessage(message, false, true, Screen::GameOver);
            return;
        }
        snprintf(message, sizeof(message), "%s hits %s for %u HP!",
            game.monsterName, game.playerName,
            static_cast<unsigned>(damage));
    } else {
        snprintf(message, sizeof(message), "%s misses!", game.monsterName);
    }
    showCombatMessage(message, false, false, Screen::Combat);
}

void dismissCombatMessage() {
    game.combatMessageActive = false;
    if (game.counterattackPending) {
        game.counterattackPending = false;
        performMonsterCounterattack();
        return;
    }
    if (game.transitionPending) {
        game.screen = game.transitionTarget;
        game.transitionPending = false;
        drawCurrentScreen();
        return;
    }
    drawCombatScreen();
}

void startEncounter() {
    if (monsterCount == 0) {
        return;
    }
    game.monsterIndex = monsterIndices[randomBelow(monsterCount)];
    CharacterMetadata monster = readCharacterMetadata(game.monsterIndex);
    game.monsterStartingHp = monster.startingHp;
    game.monsterHp = monster.startingHp;
    copyCharacterName(game.monsterIndex, game.monsterName,
        sizeof(game.monsterName));
    game.combatMessageActive = false;
    game.counterattackPending = false;
    game.transitionPending = false;
    game.screen = Screen::Combat;
    drawCurrentScreen();
}

void selectPlayerCharacter(uint8_t slot) {
    if (slot >= playableCount) {
        return;
    }
    game.playerIndex = playableIndices[slot];
    CharacterMetadata player = readCharacterMetadata(game.playerIndex);
    game.playerStartingHp = player.startingHp;
    game.playerHp = player.startingHp;
    copyCharacterName(game.playerIndex, game.playerName,
        sizeof(game.playerName));
    game.screen = Screen::Explore;
    drawCurrentScreen();
}

void handleCharacterSelectionTouch(int x, int y) {
    if (playableCount == 0 || y < CHARACTER_CELL_TOP ||
        y >= CHARACTER_CELL_BOTTOM) {
        return;
    }
    uint8_t slot = static_cast<uint8_t>(
        x * playableCount / SCREEN_WIDTH);
    if (slot < playableCount) {
        selectPlayerCharacter(slot);
    }
}

void handleExploreTouch(int x, int y) {
    if (pointInButton(x, y, exploreButton())) {
        startEncounter();
    }
}

void handleCombatTouch(int x, int y) {
    if (game.combatMessageActive) {
        dismissCombatMessage();
        return;
    }

    if (pointInButton(x, y, runButton())) {
        game.screen = Screen::Explore;
        game.combatMessageActive = false;
        drawCurrentScreen();
    } else if (pointInButton(x, y, attackButton())) {
        performPlayerAttack();
    }
}

void handleGameOverTouch(int x, int y) {
    if (!pointInButton(x, y, playAgainButton())) {
        return;
    }
    game.playerIndex = NO_CHARACTER;
    game.monsterIndex = NO_CHARACTER;
    game.playerHp = 0;
    game.playerStartingHp = 0;
    game.monsterHp = 0;
    game.monsterStartingHp = 0;
    game.combatMessageActive = false;
    game.counterattackPending = false;
    game.transitionPending = false;
    game.screen = Screen::CharacterSelection;
    drawCurrentScreen();
}

void readTouchPosition(int &x, int &y) {
    TS_Point point = ts.getPoint();
    x = constrain(map(point.x, 200, 3800, 0, SCREEN_WIDTH - 1),
        0, SCREEN_WIDTH - 1);
    y = constrain(map(point.y, 200, 3800, 0, SCREEN_HEIGHT - 1),
        0, SCREEN_HEIGHT - 1);
}

void handleTouch() {
    if (!ts.touched()) {
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
    switch (game.screen) {
        case Screen::CharacterSelection:
            handleCharacterSelectionTouch(x, y);
            break;
        case Screen::Explore:
            handleExploreTouch(x, y);
            break;
        case Screen::Combat:
            handleCombatTouch(x, y);
            break;
        case Screen::GameOver:
            handleGameOverTouch(x, y);
            break;
    }
}

void updateCombatMessage() {
    if (game.screen == Screen::Combat && game.combatMessageActive &&
        millis() - game.combatMessageStartedAt >=
            COMBAT_MESSAGE_DURATION_MS) {
        dismissCombatMessage();
    }
}

} // namespace

void setup() {
    setup_display();
    setup_touch();
    game.screen = Screen::CharacterSelection;
    game.playerIndex = NO_CHARACTER;
    game.monsterIndex = NO_CHARACTER;
    collectCharacterIndices();
    drawCurrentScreen();
}

void loop() {
    updateCombatMessage();
    handleTouch();
    delay(10);
}

