#include <PNGdec.h>
#pragma once

#include <Arduino.h>

// Pass a PNG object from the main code to avoid duplicate allocation
bool displayImageOnTFT(PNG& png, const char* filename, int16_t x = 0, int16_t y = 0);
