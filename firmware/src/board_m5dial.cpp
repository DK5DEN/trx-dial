// M5Stack Dial: ESP32-S3, GC9A01 240x240, encoder on GPIO40/41, button GPIO42.
#if defined(BOARD_M5DIAL)
#include "board.h"
#include <ESP32Encoder.h>

namespace board {

static ESP32Encoder enc;
static int64_t encLast = 0;
static const int COUNTS_PER_DETENT = 4;

static bool evClick = false;
static bool evLong = false;
static bool longFired = false;
static bool evTap = false;
static int tapX = 0, tapY = 0;

void begin() {
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(0);
    M5.Display.setBrightness(140);
    M5.Speaker.setVolume(90);
    ESP32Encoder::useInternalWeakPullResistors = puType::up;
    enc.attachFullQuad(41, 40);
    enc.clearCount();
}

void update() {
    M5.update();
    auto &b = M5.BtnA;
    if (b.pressedFor(600) && !longFired) {
        longFired = true;
        evLong = true;
    }
    if (b.wasReleased()) {
        if (!longFired) evClick = true;
        longFired = false;
    }
    if (M5.Touch.getCount()) {
        auto d = M5.Touch.getDetail();
        if (d.wasClicked()) { evTap = true; tapX = d.x; tapY = d.y; }
    }
}

Gfx &display() { return M5.Display; }
int width() { return M5.Display.width(); }
int height() { return M5.Display.height(); }

int encoderSteps() {
    int64_t c = enc.getCount();
    int64_t d = c - encLast;
    int steps = (int)(d / COUNTS_PER_DETENT);
    encLast += (int64_t)steps * COUNTS_PER_DETENT;
    return steps;    // clockwise = positive
}

bool clicked() { bool r = evClick; evClick = false; return r; }
bool longPressed() { bool r = evLong; evLong = false; return r; }
bool tapped(int *x, int *y) {
    bool r = evTap;
    evTap = false;
    if (r && x) *x = tapX;
    if (r && y) *y = tapY;
    return r;
}

void beep(uint16_t freq, uint16_t ms) { M5.Speaker.tone(freq, ms); }
void setBrightness(uint8_t b) { M5.Display.setBrightness(b); }

}  // namespace board
#endif
