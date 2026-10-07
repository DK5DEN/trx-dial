// Board abstraction: display, rotary encoder, button, beeper.
#pragma once
#include <Arduino.h>

#if defined(BOARD_M5DIAL)
#include <M5Unified.h>
using Gfx = M5GFX;
#else
#error "no board selected"
#endif

namespace board {

void begin();
void update();                 // call every loop
Gfx &display();
int  width();
int  height();

/// Detent steps since last call (+ = clockwise).
int  encoderSteps();

/// Button events since last call.
bool clicked();
bool longPressed();

/// Short tap on the touch screen since last call, with its position.
bool tapped(int *x = nullptr, int *y = nullptr);

void beep(uint16_t freq, uint16_t ms);
void setBrightness(uint8_t b);

}  // namespace board
