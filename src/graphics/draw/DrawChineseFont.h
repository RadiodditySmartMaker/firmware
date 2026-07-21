#pragma once
#include "OLEDDisplay.h"
#include <stdint.h>

typedef struct
{
    const char *utf8;
    const uint8_t bitmap[32];
} ChineseFont;

#ifndef CNFONT_EMBED_INTERNAL_TABLE
#define CNFONT_EMBED_INTERNAL_TABLE 0
#endif

#if CNFONT_EMBED_INTERNAL_TABLE
extern const ChineseFont chineseFont[];
#endif
extern const unsigned int chineseFontCount;

bool drawChineseChar(OLEDDisplay *display, int16_t x, int16_t y, const char *utf8);
void drawChineseStringWithLineBreak(OLEDDisplay *display, int16_t x, int16_t y, const char *str);
