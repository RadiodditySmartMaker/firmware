#pragma once

#include <stdint.h>

#ifndef CJKFONT_CFG_TARGET_NAME
#define CJKFONT_CFG_TARGET_NAME "qspi://cjk_font.bin"
#endif

#ifndef CJKFONT_CFG_EXT_ADDR
#define CJKFONT_CFG_EXT_ADDR 0x00300000U
#endif

#ifndef CJKFONT_CFG_MAX_BYTES
#define CJKFONT_CFG_MAX_BYTES 0x00080000U
#endif

#ifndef CJKFONT_CFG_MAGIC
#define CJKFONT_CFG_MAGIC 0x434A4B31U
#endif

#ifndef CJKFONT_CFG_VERSION
#define CJKFONT_CFG_VERSION 2U
#endif

#ifndef CJKFONT_CFG_KEY_SIZE
#define CJKFONT_CFG_KEY_SIZE 4U
#endif

#ifndef CJKFONT_CFG_BITMAP_SIZE
#define CJKFONT_CFG_BITMAP_SIZE 32U
#endif

#ifndef CJKFONT_CFG_GLYPH_WIDTH
#define CJKFONT_CFG_GLYPH_WIDTH 16U
#endif

#ifndef CJKFONT_CFG_GLYPH_HEIGHT
#define CJKFONT_CFG_GLYPH_HEIGHT 16U
#endif

#ifndef CJKFONT_CFG_LINE_HEIGHT
#define CJKFONT_CFG_LINE_HEIGHT (CJKFONT_CFG_GLYPH_HEIGHT + 4U)
#endif

#ifndef CJKFONT_CFG_Y_OFFSET
#define CJKFONT_CFG_Y_OFFSET (-2)
#endif

namespace nodara
{

void ExtFlashSelfTest();

bool ExtFlashRawReady();
bool ExtFlashRawRead(uint32_t addr, void *buf, uint32_t len);
bool ExtFlashRawWrite(uint32_t addr, const void *buf, uint32_t len);
bool ExtFlashRawErase(uint32_t addr, uint32_t len);

bool ExtFlashBeginCjkFontUpload();
bool ExtFlashWriteCjkFontUploadChunk(uint32_t offset, const void *buf, uint32_t len);
bool ExtFlashFinishCjkFontUpload(uint32_t totalBytes);
void ExtFlashAbortCjkFontUpload();

} // namespace nodara
