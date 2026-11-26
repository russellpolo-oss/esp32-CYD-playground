#include "defaults.h"
#include "common.cpp"
#include "common_touch.cpp"

#include <math.h>
#include <stdint.h>

namespace {

const int GROUND_Y = 184;
const int HUD_BOTTOM = 30;
const int TERRAIN_HEIGHT = GROUND_Y - HUD_BOTTOM;
const int BUTTON_TOP = 198;
const int CANNON_X = 25;
const int CANNON_Y = GROUND_Y - 5;
const int CANNON_LENGTH = 25;
const int TARGET_RADIUS = 10;
const int TARGET_GROUND_Y = GROUND_Y - TARGET_RADIUS;
const int BALL_RADIUS = 3;
const float PROJECTILE_SPEED = 240.0f;
const float GRAVITY = 180.0f;
const float PI_F = 3.14159265f;
const unsigned long IMPACT_DURATION_MS = 500;
const unsigned long IMPACT_FRAME_MS = 30;
const int ANGLE_STEP = 3;
const int MIN_ANGLE = 10;
const int MAX_ANGLE = 75;
const int BUTTON_NONE = 0;
const int BUTTON_MINUS = 1;
const int BUTTON_PLUS = 2;
const int BUTTON_FIRE = 3;
const unsigned long BUTTON_FLASH_DURATION_MS = 120;

enum GameState {
    READY,
    IN_FLIGHT,
    IMPACTING
};

GameState gameState = READY;
int hits = 0;
int misses = 0;
int cannonAngle = 65;
uint8_t mountainPixels[SCREEN_WIDTH][TERRAIN_HEIGHT];
int mountainLeft = 0;
int mountainRight = 0;
int targetX = 280;
int targetY = TARGET_GROUND_Y;
int craterX = 0;
int craterY = 0;
bool craterActive = false;
bool touchWasDown = false;
bool touchTapped = false;
int touchX = 0;
int touchY = 0;

float ballX = 0;
float ballY = 0;
float ballVelocityX = 0;
float ballVelocityY = 0;
unsigned long lastFlightUpdate = 0;
unsigned long impactStarted = 0;
unsigned long lastImpactFrame = 0;
int impactX = 0;
int impactY = 0;
bool impactIsHit = false;
bool impactVisible = false;
int flashingButton = BUTTON_NONE;
unsigned long buttonFlashStarted = 0;

float degreesToRadians(float degrees) {
    return degrees * (PI_F / 180.0f);
}

bool projectileVisible() {
    return ballX - BALL_RADIUS >= 0 &&
        ballX + BALL_RADIUS < SCREEN_WIDTH &&
        ballY - BALL_RADIUS >= HUD_BOTTOM &&
        ballY + BALL_RADIUS < GROUND_Y;
}

float projectileYAt(float x, float angleDegrees) {
    float angle = degreesToRadians(angleDegrees);
    float startX = CANNON_X + CANNON_LENGTH * cosf(angle);
    float startY = CANNON_Y - CANNON_LENGTH * sinf(angle);
    float velocityX = PROJECTILE_SPEED * cosf(angle);
    float elapsed = (x - startX) / velocityX;

    return startY - PROJECTILE_SPEED * sinf(angle) * elapsed
        + 0.5f * GRAVITY * elapsed * elapsed;
}

bool mountainPixelAt(int x, int y) {
    return x >= 0 && x < SCREEN_WIDTH &&
        y >= HUD_BOTTOM && y < GROUND_Y &&
        mountainPixels[x][y - HUD_BOTTOM] != 0;
}

bool circleIntersectsMountain(int centerX, int centerY) {
    const int radiusSquared = BALL_RADIUS * BALL_RADIUS;
    for (int dx = -BALL_RADIUS; dx <= BALL_RADIUS; dx++) {
        for (int dy = -BALL_RADIUS; dy <= BALL_RADIUS; dy++) {
            if (dx * dx + dy * dy <= radiusSquared &&
                mountainPixelAt(centerX + dx, centerY + dy)) {
                return true;
            }
        }
    }
    return false;
}

bool removeMountainPixels(int centerX, int centerY) {
    const int craterRadius = BALL_RADIUS + 3;
    const int radiusSquared = craterRadius * craterRadius;
    bool removedPixels = false;

    for (int dx = -craterRadius; dx <= craterRadius; dx++) {
        for (int dy = -craterRadius; dy <= craterRadius; dy++) {
            if (dx * dx + dy * dy <= radiusSquared &&
                mountainPixelAt(centerX + dx, centerY + dy)) {
                mountainPixels[centerX + dx][centerY + dy - HUD_BOTTOM] = 0;
                removedPixels = true;
            }
        }
    }
    return removedPixels;
}

bool trajectoryClearsMountain(float angleDegrees, int destinationX) {
    float angle = degreesToRadians(angleDegrees);
    float startX = CANNON_X + CANNON_LENGTH * cosf(angle);
    float startY = CANNON_Y - CANNON_LENGTH * sinf(angle);
    float velocityX = PROJECTILE_SPEED * cosf(angle);
    float velocityY = -PROJECTILE_SPEED * sinf(angle);
    const int hitRadius = TARGET_RADIUS + BALL_RADIUS;

    float destinationY = projectileYAt(destinationX, angleDegrees);
    if (fabsf(destinationY - TARGET_GROUND_Y) > hitRadius) {
        return false;
    }

    for (int x = (int)startX; x <= destinationX; x++) {
        float elapsed = (x - startX) / velocityX;
        float y = startY + velocityY * elapsed
            + 0.5f * GRAVITY * elapsed * elapsed;

        int targetDX = x - destinationX;
        int targetDY = (int)(y + 0.5f) - TARGET_GROUND_Y;
        bool reachesTarget = targetDX * targetDX + targetDY * targetDY
            <= hitRadius * hitRadius;

        if (circleIntersectsMountain(x, (int)(y + 0.5f)) && !reachesTarget) {
            return false;
        }
    }

    return true;
}

void generateMountain() {
    mountainLeft = random(65, 88);
    int peakX = random(145, 176);
    mountainRight = random(235, 260);
    int peakY = random(104, 129);
    int roughness = 0;

    for (int x = 0; x < SCREEN_WIDTH; x++) {
        if (x % 7 == 0) {
            roughness = random(-5, 6);
        }

        int surfaceY = GROUND_Y;
        if (x > mountainLeft && x < mountainRight) {
            float progress;
            if (x <= peakX) {
                progress = (float)(x - mountainLeft) / (peakX - mountainLeft);
            } else {
                progress = (float)(mountainRight - x) / (mountainRight - peakX);
            }
            progress = constrain(progress, 0.0f, 1.0f);
            float rise = sinf(progress * PI_F * 0.5f);
            surfaceY = GROUND_Y - (int)(rise * (GROUND_Y - peakY))
                + (int)(roughness * rise);
            surfaceY = constrain(surfaceY, peakY - 4, GROUND_Y);
        }

        for (int y = HUD_BOTTOM; y < GROUND_Y; y++) {
            mountainPixels[x][y - HUD_BOTTOM] = y >= surfaceY ? 1 : 0;
        }
    }
}

void generateLayout() {
    bool hasClearShot = false;
    for (int attempt = 0; attempt < 20 && !hasClearShot; attempt++) {
        generateMountain();
        int minTargetX = max(mountainRight + TARGET_RADIUS + 6, 260);
        int maxTargetX = SCREEN_WIDTH - TARGET_RADIUS - 4;
        targetX = random(minTargetX, maxTargetX + 1);

        for (int angle = MIN_ANGLE; angle <= MAX_ANGLE; angle++) {
            if (trajectoryClearsMountain(angle, targetX)) {
                hasClearShot = true;
                break;
            }
        }
    }
    targetY = TARGET_GROUND_Y;
}

void drawMountain() {
    tft.fillRect(mountainLeft, HUD_BOTTOM, mountainRight - mountainLeft + 1,
        GROUND_Y - HUD_BOTTOM, ILI9341_BLACK);
    tft.fillRect(0, GROUND_Y, SCREEN_WIDTH, SCREEN_HEIGHT - GROUND_Y, ILI9341_DARKGREEN);
    for (int x = mountainLeft; x <= mountainRight; x++) {
        int runStart = -1;
        for (int y = HUD_BOTTOM; y < GROUND_Y; y++) {
            bool isMountain = mountainPixelAt(x, y);
            if (isMountain && runStart < 0) {
                runStart = y;
            } else if (!isMountain && runStart >= 0) {
                tft.drawFastVLine(x, runStart, y - runStart, ILI9341_GREEN);
                runStart = -1;
            }
        }
        if (runStart >= 0) {
            tft.drawFastVLine(x, runStart, GROUND_Y - runStart, ILI9341_GREEN);
        }
    }
    tft.drawFastHLine(0, GROUND_Y, SCREEN_WIDTH, ILI9341_DARKGREEN);
}

void drawCrater() {
    if (!craterActive) {
        return;
    }
    tft.drawCircle(craterX, craterY, 5, ILI9341_LIGHTGREY);
    tft.fillCircle(craterX, craterY, 2, ILI9341_BLACK);
}

void drawTarget() {
    tft.fillCircle(targetX, targetY, TARGET_RADIUS, ILI9341_RED);
    tft.drawCircle(targetX, targetY, TARGET_RADIUS, ILI9341_WHITE);
    tft.fillCircle(targetX, targetY, 4, ILI9341_YELLOW);
    tft.drawFastHLine(targetX - 7, GROUND_Y - 1, 14, ILI9341_WHITE);
}

void drawCannon() {
    float angle = degreesToRadians(cannonAngle);
    int endX = CANNON_X + (int)(CANNON_LENGTH * cosf(angle));
    int endY = CANNON_Y - (int)(CANNON_LENGTH * sinf(angle));

    tft.drawLine(CANNON_X, CANNON_Y, endX, endY, ILI9341_WHITE);
    tft.drawLine(CANNON_X, CANNON_Y + 1, endX, endY + 1, ILI9341_LIGHTGREY);
    tft.fillCircle(CANNON_X, CANNON_Y, 8, ILI9341_LIGHTGREY);
    tft.drawCircle(CANNON_X, CANNON_Y, 8, ILI9341_WHITE);
}

void redrawCannon() {
    const int top = CANNON_Y - CANNON_LENGTH - 5;
    tft.fillRect(0, top, 60, GROUND_Y + 9 - top, ILI9341_BLACK);
    tft.fillRect(0, GROUND_Y, 60, BUTTON_TOP - GROUND_Y, ILI9341_DARKGREEN);
    drawCannon();
}

void drawButton(int button, uint16_t color) {
    int x;
    int width;
    const int y = 204;
    const int height = 30;
    const char *label;

    switch (button) {
        case BUTTON_MINUS:
            x = 78;
            width = 60;
            label = "-";
            break;
        case BUTTON_PLUS:
            x = 8;
            width = 60;
            label = "+";
            break;
        default:
            x = 174;
            width = 138;
            label = "FIRE";
            color = color == ILI9341_YELLOW ? ILI9341_YELLOW : ILI9341_RED;
            break;
    }

    tft.fillRoundRect(x, y, width, height, 5, color);
    tft.setTextColor(color == ILI9341_YELLOW ? ILI9341_BLACK : ILI9341_WHITE);
    if (button == BUTTON_MINUS || button == BUTTON_PLUS) {
        tft.setTextSize(3);
        tft.setCursor(x + 23, y + 3);
    } else {
        tft.setTextSize(2);
        tft.setCursor(x + 37, y + 7);
    }
    tft.print(label);
}

void drawControls() {
    tft.fillRect(0, BUTTON_TOP, SCREEN_WIDTH, SCREEN_HEIGHT - BUTTON_TOP, ILI9341_DARKGREY);
    tft.drawFastHLine(0, BUTTON_TOP, SCREEN_WIDTH, ILI9341_WHITE);

    flashingButton = BUTTON_NONE;
    drawButton(BUTTON_MINUS, ILI9341_BLUE);
    drawButton(BUTTON_PLUS, ILI9341_BLUE);
    drawButton(BUTTON_FIRE, ILI9341_RED);
}

void flashButton(int button) {
    flashingButton = button;
    buttonFlashStarted = millis();
    drawButton(button, ILI9341_YELLOW);
}

void updateButtonFlash() {
    if (flashingButton == BUTTON_NONE ||
        millis() - buttonFlashStarted < BUTTON_FLASH_DURATION_MS) {
        return;
    }

    int button = flashingButton;
    flashingButton = BUTTON_NONE;
    drawButton(button, button == BUTTON_FIRE ? ILI9341_RED : ILI9341_BLUE);
}

void drawAngle() {
    tft.fillRect(230, 0, SCREEN_WIDTH - 230, HUD_BOTTOM, ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(2);
    tft.setCursor(238, 6);
    tft.print(cannonAngle);
    tft.print("d");
}

void drawHud() {
    tft.fillRect(0, 0, 230, HUD_BOTTOM, ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(2);
    tft.setCursor(8, 6);
    tft.print("Hits:");
    tft.print(hits);
    tft.print(" Miss:");
    tft.print(misses);
    drawAngle();
}

void drawScene() {
    tft.fillScreen(ILI9341_BLACK);
    drawMountain();
    drawCrater();
    drawTarget();
    drawCannon();
    drawHud();
    drawControls();
}

void drawImpactEffect(unsigned long elapsed) {
    if (!impactVisible) {
        return;
    }

    int radius = 3 + (int)(elapsed / IMPACT_FRAME_MS);
    if (radius > 20) {
        radius = 20;
    }

    if (impactIsHit) {
        tft.drawCircle(impactX, impactY, radius, ILI9341_YELLOW);
        if (radius > 4) {
            tft.drawCircle(impactX, impactY, radius - 4, ILI9341_ORANGE);
        }
        if (radius > 8) {
            tft.drawCircle(impactX, impactY, radius - 8, ILI9341_RED);
        }
    } else {
        tft.drawCircle(impactX, impactY, radius, ILI9341_ORANGE);
        tft.drawFastHLine(impactX - radius, impactY, radius * 2, ILI9341_YELLOW);
        tft.drawFastVLine(impactX, impactY - radius, radius * 2, ILI9341_YELLOW);
    }
}

void beginImpact(bool hit, int x, int y, bool makeCrater) {
    impactIsHit = hit;
    impactVisible = x >= 0 && x < SCREEN_WIDTH &&
        y >= HUD_BOTTOM && y < SCREEN_HEIGHT;
    impactX = constrain(x, 0, SCREEN_WIDTH - 1);
    impactY = constrain(y, HUD_BOTTOM, GROUND_Y);
    impactStarted = millis();
    lastImpactFrame = 0;
    gameState = IMPACTING;

    if (hit) {
        hits++;
    } else {
        misses++;
        if (makeCrater) {
            craterActive = true;
            craterX = impactX;
            craterY = impactY;
            if (removeMountainPixels(impactX, impactY)) {
                drawMountain();
            }
            drawCrater();
        }
    }

    drawHud();
    drawImpactEffect(0);
}

void launchProjectile() {
    float angle = degreesToRadians(cannonAngle);
    ballX = CANNON_X + CANNON_LENGTH * cosf(angle);
    ballY = CANNON_Y - CANNON_LENGTH * sinf(angle);
    ballVelocityX = PROJECTILE_SPEED * cosf(angle);
    ballVelocityY = -PROJECTILE_SPEED * sinf(angle);
    lastFlightUpdate = millis();
    gameState = IN_FLIGHT;
    if (projectileVisible()) {
        tft.fillCircle((int)(ballX + 0.5f), (int)(ballY + 0.5f),
            BALL_RADIUS, ILI9341_YELLOW);
    }
}

void updateProjectile() {
    unsigned long now = millis();
    float elapsed = (now - lastFlightUpdate) / 1000.0f;
    if (elapsed <= 0.0f) {
        return;
    }
    if (elapsed > 0.05f) {
        elapsed = 0.05f;
    }

    if (projectileVisible()) {
        tft.fillCircle((int)(ballX + 0.5f), (int)(ballY + 0.5f), BALL_RADIUS, ILI9341_BLACK);
    }
    ballX += ballVelocityX * elapsed;
    ballY += ballVelocityY * elapsed + 0.5f * GRAVITY * elapsed * elapsed;
    ballVelocityY += GRAVITY * elapsed;
    lastFlightUpdate = now;

    int x = (int)(ballX + 0.5f);
    int y = (int)(ballY + 0.5f);
    int targetDX = x - targetX;
    int targetDY = y - targetY;
    int hitRadius = TARGET_RADIUS + BALL_RADIUS;

    if (targetDX * targetDX + targetDY * targetDY <= hitRadius * hitRadius) {
        beginImpact(true, x, y, false);
        return;
    }

    if (circleIntersectsMountain(x, y)) {
        beginImpact(false, x, y, true);
        return;
    }

    if (x >= SCREEN_WIDTH - BALL_RADIUS) {
        beginImpact(false, x, y, false);
        return;
    }
    if (y + BALL_RADIUS >= GROUND_Y) {
        beginImpact(false, x, GROUND_Y, true);
        return;
    }

    if (projectileVisible()) {
        tft.fillCircle(x, y, BALL_RADIUS, ILI9341_YELLOW);
    }
}

void updateImpact() {
    unsigned long elapsed = millis() - impactStarted;
    unsigned long frame = elapsed / IMPACT_FRAME_MS;
    if (frame != lastImpactFrame) {
        lastImpactFrame = frame;
        drawImpactEffect(elapsed);
    }

    if (elapsed >= IMPACT_DURATION_MS) {
        if (impactIsHit) {
            craterActive = false;
            generateLayout();
        }
        gameState = READY;
        drawScene();
    }
}

void updateTouch() {
    bool down = ts.touched();
    touchTapped = down && !touchWasDown;

    if (down) {
        TS_Point point = ts.getPoint();
        touchX = constrain(map(point.x, 200, 3800, 0, SCREEN_WIDTH - 1), 0, SCREEN_WIDTH - 1);
        touchY = constrain(map(point.y, 200, 3800, 0, SCREEN_HEIGHT - 1), 0, SCREEN_HEIGHT - 1);
    }
    touchWasDown = down;
}

void handleTap() {
    if (!touchTapped || touchY < BUTTON_TOP) {
        return;
    }

    if (touchX >= 78 && touchX < 138) {
        cannonAngle = max(MIN_ANGLE, cannonAngle - ANGLE_STEP);
        flashButton(BUTTON_MINUS);
        redrawCannon();
        drawAngle();
    } else if (touchX >= 8 && touchX < 68) {
        cannonAngle = min(MAX_ANGLE, cannonAngle + ANGLE_STEP);
        flashButton(BUTTON_PLUS);
        redrawCannon();
        drawAngle();
    } else if (touchX >= 174 && touchX < 312) {
        flashButton(BUTTON_FIRE);
        launchProjectile();
    }
}

} // namespace

void setup() {
    setup_display();
    setup_touch();
    randomSeed(micros());
    generateLayout();
    drawScene();
}

void loop() {
    updateTouch();
    updateButtonFlash();

    switch (gameState) {
        case READY:
            handleTap();
            break;
        case IN_FLIGHT:
            updateProjectile();
            break;
        case IMPACTING:
            updateImpact();
            break;
    }

    delay(10);
}

