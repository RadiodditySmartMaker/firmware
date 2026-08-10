#pragma once
#include "OLEDDisplay.h"
#include <stdint.h>

typedef struct
{
    const char *utf8;
    const uint8_t bitmap[32];
} CjkFont;

#ifndef CJKFONT_EMBED_INTERNAL_TABLE
#define CJKFONT_EMBED_INTERNAL_TABLE 0
#endif

#ifndef CJKFONT_ALLOW_RUNTIME_EXT_REBUILD
#define CJKFONT_ALLOW_RUNTIME_EXT_REBUILD 0
#endif

#if CJKFONT_EMBED_INTERNAL_TABLE
extern const CjkFont cjkFont[];
#endif
extern const unsigned int cjkFontCount;

bool drawCjkChar(OLEDDisplay *display, int16_t x, int16_t y, const char *utf8);
void drawCjkStringWithLineBreak(OLEDDisplay *display, int16_t x, int16_t y, const char *str);
