#pragma once

#include <stdint.h>

#ifndef CNFONT_CFG_TARGET_NAME
#define CNFONT_CFG_TARGET_NAME "qspi://chinese_font.bin"
#endif

#ifndef CNFONT_CFG_EXT_ADDR
#define CNFONT_CFG_EXT_ADDR 0x00300000U
#endif

#ifndef CNFONT_CFG_MAX_BYTES
#define CNFONT_CFG_MAX_BYTES 0x00080000U
#endif

#ifndef CNFONT_CFG_MAGIC
#define CNFONT_CFG_MAGIC 0x43484631U
#endif

#ifndef CNFONT_CFG_VERSION
#define CNFONT_CFG_VERSION 1U
#endif

#ifndef CNFONT_CFG_KEY_SIZE
#define CNFONT_CFG_KEY_SIZE 4U
#endif

#ifndef CNFONT_CFG_BITMAP_SIZE
#define CNFONT_CFG_BITMAP_SIZE 32U
#endif

#ifndef CNFONT_CFG_GLYPH_WIDTH
#define CNFONT_CFG_GLYPH_WIDTH 16U
#endif

#ifndef CNFONT_CFG_GLYPH_HEIGHT
#define CNFONT_CFG_GLYPH_HEIGHT 16U
#endif

#ifndef CNFONT_CFG_LINE_HEIGHT
#define CNFONT_CFG_LINE_HEIGHT (CNFONT_CFG_GLYPH_HEIGHT + 4U)
#endif

#ifndef CNFONT_CFG_Y_OFFSET
#define CNFONT_CFG_Y_OFFSET (-2)
#endif

namespace nodara
{

void ExtFlashSelfTest();

bool ExtFlashRawReady();
bool ExtFlashRawRead(uint32_t addr, void *buf, uint32_t len);
bool ExtFlashRawWrite(uint32_t addr, const void *buf, uint32_t len);
bool ExtFlashRawErase(uint32_t addr, uint32_t len);

bool ExtFlashBeginChineseFontUpload();
bool ExtFlashWriteChineseFontUploadChunk(uint32_t offset, const void *buf, uint32_t len);
bool ExtFlashFinishChineseFontUpload(uint32_t totalBytes);
void ExtFlashAbortChineseFontUpload();

} // namespace nodara
