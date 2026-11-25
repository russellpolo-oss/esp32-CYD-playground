// ==========================================
// Common CYD configuration and setup
// ==========================================

#include "common.cpp"
#include "common_touch.cpp"

static const uint16_t palette[] = {
    ILI9341_WHITE,
    ILI9341_RED,
    ILI9341_GREEN,
    ILI9341_BLUE,
    ILI9341_YELLOW,
    ILI9341_CYAN,
    ILI9341_MAGENTA,
    ILI9341_ORANGE
};

static const int PALETTE_WIDTH = 40;
static const int PALETTE_BOX_HEIGHT = 24;
static const int PALETTE_BOX_TOP = 12;
static const int PALETTE_BOX_GAP = 6;
static const int PALETTE_COUNT = sizeof(palette) / sizeof(palette[0]);

uint16_t penColor = ILI9341_WHITE;
int selectedColor = 0;
bool wasTouched = false;
bool hasPreviousPoint = false;
int previousX = 0;
int previousY = 0;

void drawPalette() {
    tft.fillRect(0, 0, PALETTE_WIDTH, SCREEN_HEIGHT, ILI9341_BLACK);
    tft.drawFastVLine(PALETTE_WIDTH - 1, 0, SCREEN_HEIGHT, ILI9341_DARKGREY);

    for (int i = 0; i < PALETTE_COUNT; i++) {
        int boxTop = PALETTE_BOX_TOP + i * (PALETTE_BOX_HEIGHT + PALETTE_BOX_GAP);
        tft.fillRect(8, boxTop, 24, PALETTE_BOX_HEIGHT, palette[i]);

        if (i == selectedColor) {
            tft.drawRect(5, boxTop - 3, 30, PALETTE_BOX_HEIGHT + 6, ILI9341_WHITE);
            tft.drawRect(6, boxTop - 2, 28, PALETTE_BOX_HEIGHT + 4, ILI9341_WHITE);
        }
    }
}

void setup() {
    setup_display();
    setup_touch();

    tft.fillScreen(ILI9341_BLACK);
    drawPalette();
}

void loop() {
    bool touched = ts.touched();

    if (touched) {
        TS_Point p = ts.getPoint();
        int x = map(p.x, 200, 3800, 0, SCREEN_WIDTH - 1);
        int y = map(p.y, 200, 3800, 0, SCREEN_HEIGHT - 1);

        x = constrain(x, 0, SCREEN_WIDTH - 1);
        y = constrain(y, 0, SCREEN_HEIGHT - 1);

        if (!wasTouched) {
            if (x < PALETTE_WIDTH) {
                for (int i = 0; i < PALETTE_COUNT; i++) {
                    int boxTop = PALETTE_BOX_TOP + i * (PALETTE_BOX_HEIGHT + PALETTE_BOX_GAP);
                    if (y >= boxTop && y < boxTop + PALETTE_BOX_HEIGHT) {
                        selectedColor = i;
                        penColor = palette[i];
                        drawPalette();
                        break;
                    }
                }
            }
            else {
                tft.drawPixel(x, y, penColor);
                previousX = x;
                previousY = y;
                hasPreviousPoint = true;
            }

            Serial.printf(
                "Touch raw=(%d,%d) pressure=%d screen=(%d,%d) pen=%04X\n",
                p.x, p.y, p.z, x, y, penColor
            );
        }
        else if (x >= PALETTE_WIDTH) {
            if (hasPreviousPoint) {
                tft.drawLine(previousX, previousY, x, y, penColor);
            }
            else {
                tft.drawPixel(x, y, penColor);
            }
            previousX = x;
            previousY = y;
            hasPreviousPoint = true;
        }
        else {
            hasPreviousPoint = false;
        }
    }
    else {
        hasPreviousPoint = false;
    }

    wasTouched = touched;
    delay(10);
}
