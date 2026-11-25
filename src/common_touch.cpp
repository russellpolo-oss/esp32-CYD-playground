#include <XPT2046_Touchscreen.h>

// CYD touchscreen pins
#define TOUCH_CS  33
#define TOUCH_IRQ 36
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25

XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);




void setup_touch() {

    // The XPT2046 library uses the global SPI bus.
    // Initialize it with the CYD touchscreen pins.
    SPI.begin(
        TOUCH_CLK,
        TOUCH_MISO,
        TOUCH_MOSI,
        TOUCH_CS
    );

    // Initialize touchscreen
    ts.begin();
    ts.setRotation(1);

    Serial.println("Touchscreen initialized");
 
}

