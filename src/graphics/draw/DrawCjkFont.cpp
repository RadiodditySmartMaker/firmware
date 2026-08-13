#include "DrawCjkFont.h"
#include "DebugConfiguration.h"
#include "nodara/ExternalFlash.h"
#include <string.h>

#ifndef CJKFONT_EMBED_INTERNAL_TABLE
#define CJKFONT_EMBED_INTERNAL_TABLE 0
#endif

// Runtime must never rewrite QSPI unless explicitly enabled.
// External font images are written only by the host upload path
// (custom_upload_external_cjk_font in platformio.ini).
#ifndef CJKFONT_ALLOW_RUNTIME_EXT_REBUILD
#define CJKFONT_ALLOW_RUNTIME_EXT_REBUILD 0
#endif

namespace
{
static constexpr uint32_t kCjkFontMagic = CJKFONT_CFG_MAGIC;
static constexpr uint32_t kCjkFontBaseAddr = CJKFONT_CFG_EXT_ADDR;
static constexpr uint32_t kCjkFontMaxBytes = CJKFONT_CFG_MAX_BYTES;
static constexpr uint32_t kUtf8KeySize = CJKFONT_CFG_KEY_SIZE;
static constexpr uint32_t kBitmapSize = CJKFONT_CFG_BITMAP_SIZE;
static constexpr uint32_t kGlyphWidth = CJKFONT_CFG_GLYPH_WIDTH;
static constexpr uint32_t kGlyphHeight = CJKFONT_CFG_GLYPH_HEIGHT;
static constexpr uint32_t kBytesPerRow = (kGlyphWidth + 7U) / 8U;
static constexpr int16_t kLineHeight = CJKFONT_CFG_LINE_HEIGHT;
static constexpr int16_t kGlyphYOffset = CJKFONT_CFG_Y_OFFSET;

struct CjkFontFileHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t reserved;
};

static constexpr uint32_t kCjkFontVersionUnsorted = 1;
static constexpr uint32_t kCjkFontVersionSorted = 2;
static constexpr uint32_t kKeyChunkCount = 64; // 256 bytes on the stack

static bool gExternalFontTriedInit = false;
static bool gExternalFontReady = false;
static bool gExternalFontSorted = false;
static uint32_t gExternalFontCount = 0;
static bool gExternalFontLoggedReady = false;
static bool gExternalFontLoggedHit = false;
static bool gExternalFontLoggedMiss = false;

static uint8_t utf8CharLength(uint8_t c)
{
    if (c < 0x80) {
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

static void makeUtf8Key(const char *utf8, uint8_t out[kUtf8KeySize])
{
    memset(out, 0, kUtf8KeySize);
    if (!utf8)
        return;
    for (uint32_t i = 0; i < (kUtf8KeySize - 1U) && utf8[i] != '\0'; ++i) {
        out[i] = static_cast<uint8_t>(utf8[i]);
    }
}

static bool isSupportedExternalVersion(uint32_t version)
{
    return version == kCjkFontVersionUnsorted || version == kCjkFontVersionSorted;
}

#if CJKFONT_ALLOW_RUNTIME_EXT_REBUILD && CJKFONT_EMBED_INTERNAL_TABLE
static bool exportCjkFontToExternal()
{
    // Unsorted v1 image; write keys one-by-one so runtime never allocates count*key_size.
    CjkFontFileHeader header = {kCjkFontMagic, kCjkFontVersionUnsorted, cjkFontCount, 0};
    const uint32_t keyBytes = cjkFontCount * kUtf8KeySize;
    const uint32_t bitmapBytes = cjkFontCount * kBitmapSize;
    const uint32_t totalBytes = sizeof(header) + keyBytes + bitmapBytes;
    if (totalBytes > kCjkFontMaxBytes) {
        LOG_WARN("[CJKFONT][EXT] layout too large: need=%lu limit=%lu", (unsigned long)totalBytes,
                 (unsigned long)kCjkFontMaxBytes);
        return false;
    }

    if (!nodara::ExtFlashRawErase(kCjkFontBaseAddr, totalBytes)) {
        LOG_WARN("[CJKFONT][EXT] erase failed base=0x%08lx len=%lu", (unsigned long)kCjkFontBaseAddr,
                 (unsigned long)totalBytes);
        return false;
    }

    if (!nodara::ExtFlashRawWrite(kCjkFontBaseAddr, &header, sizeof(header))) {
        LOG_WARN("[CJKFONT][EXT] write header failed");
        return false;
    }

    uint8_t key[kUtf8KeySize];
    uint32_t keyAddr = kCjkFontBaseAddr + sizeof(header);
    for (uint32_t i = 0; i < cjkFontCount; ++i) {
        makeUtf8Key(cjkFont[i].utf8, key);
        if (!nodara::ExtFlashRawWrite(keyAddr, key, kUtf8KeySize)) {
            LOG_WARN("[CJKFONT][EXT] write key failed at index=%lu", (unsigned long)i);
            return false;
        }
        keyAddr += kUtf8KeySize;
    }

    uint32_t bitmapAddr = kCjkFontBaseAddr + sizeof(header) + keyBytes;
    for (uint32_t i = 0; i < cjkFontCount; ++i) {
        if (!nodara::ExtFlashRawWrite(bitmapAddr, cjkFont[i].bitmap, kBitmapSize)) {
            LOG_WARN("[CJKFONT][EXT] write bitmap failed at index=%lu", (unsigned long)i);
            return false;
        }
        bitmapAddr += kBitmapSize;
    }

    LOG_INFO("[CJKFONT][EXT] exported count=%lu bytes=%lu base=0x%08lx", (unsigned long)cjkFontCount,
             (unsigned long)totalBytes, (unsigned long)kCjkFontBaseAddr);
    return true;
}
#endif

static bool ensureExternalCjkFont()
{
    if (gExternalFontTriedInit) {
        return gExternalFontReady;
    }
    gExternalFontTriedInit = true;

    if (!nodara::ExtFlashRawReady()) {
        LOG_WARN("[CJKFONT][EXT] raw flash not ready");
        return false;
    }

    CjkFontFileHeader header;
    if (!nodara::ExtFlashRawRead(kCjkFontBaseAddr, &header, sizeof(header))) {
        LOG_WARN("[CJKFONT][EXT] read header failed");
        return false;
    }

    const bool headerBasicInvalid =
        (header.magic != kCjkFontMagic || !isSupportedExternalVersion(header.version) || header.count == 0);

    if (headerBasicInvalid) {
#if CJKFONT_ALLOW_RUNTIME_EXT_REBUILD && CJKFONT_EMBED_INTERNAL_TABLE
        LOG_INFO("[CJKFONT][EXT] invalid header, try rebuild (magic=0x%08lx ver=%lu count=%lu)", (unsigned long)header.magic,
                 (unsigned long)header.version, (unsigned long)header.count);
        if (!exportCjkFontToExternal()) {
            LOG_WARN("[CJKFONT][EXT] rebuild failed");
            return false;
        }
        if (!nodara::ExtFlashRawRead(kCjkFontBaseAddr, &header, sizeof(header))) {
            LOG_WARN("[CJKFONT][EXT] read header after rebuild failed");
            return false;
        }
#else
        LOG_WARN("[CJKFONT][EXT] invalid external header (magic=0x%08lx ver=%lu count=%lu); not rewriting QSPI",
                 (unsigned long)header.magic, (unsigned long)header.version, (unsigned long)header.count);
        return false;
#endif
    }

    if (header.magic != kCjkFontMagic || !isSupportedExternalVersion(header.version) || header.count == 0) {
        LOG_WARN("[CJKFONT][EXT] invalid header after init");
        return false;
    }

    const uint32_t keyBytes = header.count * kUtf8KeySize;
    if ((sizeof(header) + keyBytes + header.count * kBitmapSize) > kCjkFontMaxBytes) {
        LOG_WARN("[CJKFONT][EXT] invalid size after header check");
        return false;
    }

    gExternalFontCount = header.count;
    gExternalFontSorted = (header.version >= kCjkFontVersionSorted);
    gExternalFontReady = true;
    if (!gExternalFontLoggedReady) {
        LOG_INFO("[CJKFONT][EXT] ready count=%lu ver=%lu sorted=%d base=0x%08lx", (unsigned long)gExternalFontCount,
                 (unsigned long)header.version, gExternalFontSorted ? 1 : 0, (unsigned long)kCjkFontBaseAddr);
        gExternalFontLoggedReady = true;
    }
    return true;
}

static uint32_t externalKeyTableAddr()
{
    return kCjkFontBaseAddr + sizeof(CjkFontFileHeader);
}

static uint32_t externalBitmapTableAddr()
{
    return externalKeyTableAddr() + gExternalFontCount * kUtf8KeySize;
}

static bool readExternalBitmapAt(uint32_t index, uint8_t outBitmap[kBitmapSize])
{
    return nodara::ExtFlashRawRead(externalBitmapTableAddr() + index * kBitmapSize, outBitmap, kBitmapSize);
}

static bool lookupExternalIndexSorted(const uint8_t key[kUtf8KeySize], uint32_t &outIndex)
{
    uint32_t lo = 0;
    uint32_t hi = gExternalFontCount;
    const uint32_t keyBase = externalKeyTableAddr();

    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        uint8_t midKey[kUtf8KeySize];
        if (!nodara::ExtFlashRawRead(keyBase + mid * kUtf8KeySize, midKey, kUtf8KeySize)) {
            return false;
        }
        const int cmp = memcmp(midKey, key, kUtf8KeySize);
        if (cmp == 0) {
            outIndex = mid;
            return true;
        }
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

static bool lookupExternalIndexChunked(const uint8_t key[kUtf8KeySize], uint32_t &outIndex)
{
    uint8_t chunk[kKeyChunkCount * kUtf8KeySize];
    const uint32_t keyBase = externalKeyTableAddr();

    for (uint32_t i = 0; i < gExternalFontCount;) {
        const uint32_t n = (gExternalFontCount - i < kKeyChunkCount) ? (gExternalFontCount - i) : kKeyChunkCount;
        if (!nodara::ExtFlashRawRead(keyBase + i * kUtf8KeySize, chunk, n * kUtf8KeySize)) {
            return false;
        }
        for (uint32_t j = 0; j < n; ++j) {
            if (memcmp(chunk + j * kUtf8KeySize, key, kUtf8KeySize) == 0) {
                outIndex = i + j;
                return true;
            }
        }
        i += n;
    }
    return false;
}

static bool lookupExternalCjkBitmap(const char *utf8, uint8_t outBitmap[kBitmapSize])
{
    if (!ensureExternalCjkFont()) {
        return false;
    }

    uint8_t key[kUtf8KeySize];
    makeUtf8Key(utf8, key);

    uint32_t foundIndex = 0;
    const bool found =
        gExternalFontSorted ? lookupExternalIndexSorted(key, foundIndex) : lookupExternalIndexChunked(key, foundIndex);
    if (!found) {
        return false;
    }
    return readExternalBitmapAt(foundIndex, outBitmap);
}

#if CJKFONT_EMBED_INTERNAL_TABLE
static bool lookupInternalCjkBitmap(const char *utf8, const uint8_t *&outBitmap)
{
    for (unsigned int i = 0; i < cjkFontCount; i++) {
        if (strcmp(cjkFont[i].utf8, utf8) == 0) {
            outBitmap = cjkFont[i].bitmap;
            return true;
        }
    }
    return false;
}
#endif

static void drawGlyphBitmap(OLEDDisplay *display, int16_t x, int16_t y, const uint8_t *bitmap)
{
    LOG_INFO("kGlyphYOffset = %d", kGlyphYOffset);
    for (uint32_t row = 0; row < kGlyphHeight; ++row) {
        for (uint32_t col = 0; col < kGlyphWidth; ++col) {
            const uint32_t byteIndex = row * kBytesPerRow + (col / 8U);
            const uint32_t bitIndex = 7U - (col % 8U);
            if (bitmap[byteIndex] & (1U << bitIndex)) {
                display->setPixel(x + static_cast<int16_t>(col), y + static_cast<int16_t>(row) + kGlyphYOffset);
            }
        }
    }
}
} // namespace

// Lookup order: internal table first, then external QSPI (v2 binary search, v1 chunked).
// Never rewrite QSPI at runtime unless CJKFONT_ALLOW_RUNTIME_EXT_REBUILD=1 (default off).
bool drawCjkChar(OLEDDisplay *display, int16_t x, int16_t y, const char *utf8)
{
#if CJKFONT_EMBED_INTERNAL_TABLE
    const uint8_t *internalBitmap = nullptr;
    if (lookupInternalCjkBitmap(utf8, internalBitmap)) {
        drawGlyphBitmap(display, x, y, internalBitmap);
        return true;
    }
#endif

    uint8_t externalBitmap[kBitmapSize];
    if (lookupExternalCjkBitmap(utf8, externalBitmap)) {
        if (!gExternalFontLoggedHit) {
            LOG_INFO("[CJKFONT][EXT] first external glyph hit");
            gExternalFontLoggedHit = true;
        }
        drawGlyphBitmap(display, x, y, externalBitmap);
        return true;
    }

    if (!gExternalFontLoggedMiss) {
        LOG_WARN("[CJKFONT] glyph not found in internal/external tables");
        gExternalFontLoggedMiss = true;
    }
    return false;
}

void drawCjkStringWithLineBreak(OLEDDisplay *display, int16_t x, int16_t y, const char *str)
{
    int offset = 0;
    int16_t currentX = x;
    int16_t currentY = y;
    const int16_t lineHeight = kLineHeight;
    int16_t screenWidth = display->getWidth();

    while (str[offset]) {
        if (str[offset] == '\n') {
            currentX = x;
            currentY += lineHeight;
            offset++;
            continue;
        }

        if (currentX >= screenWidth) {
            currentX = x;
            currentY += lineHeight;
        }

        unsigned char c = static_cast<unsigned char>(str[offset]);

        // 1-byte ASCII uses the OLED Latin font.
        if (c < 0x80) {
            char buf[2] = {static_cast<char>(c), 0};

            int16_t w = display->getStringWidth(buf);
            if (currentX + w > screenWidth) {
                currentX = x;
                currentY += lineHeight;
            }

            display->drawString(currentX, currentY, buf);
            currentX += w;
            offset += 1;
        }
        // Multi-byte UTF-8: internal glyph table, then external QSPI.
        else {
            const uint8_t charLen = utf8CharLength(c);
            char buf[5] = {0};
            for (uint8_t i = 0; i < charLen; ++i) {
                if (!str[offset + i]) {
                    return;
                }
                buf[i] = str[offset + i];
            }

            if (currentX + static_cast<int16_t>(kGlyphWidth) > screenWidth) {
                currentX = x;
                currentY += lineHeight;
            }

            bool drawn = drawCjkChar(display, currentX, currentY, buf);
            if (drawn) {
                currentX += static_cast<int16_t>(kGlyphWidth);
            } else {
                int16_t w = (charLen == 2) ? display->getStringWidth("A") : static_cast<int16_t>(kGlyphWidth);
                if (currentX + w > screenWidth) {
                    currentX = x;
                    currentY += lineHeight;
                }
                if (charLen == 2) {
                    display->drawString(currentX, currentY, buf);
                }
                currentX += w;
            }

            offset += charLen;
        }
    }
}
