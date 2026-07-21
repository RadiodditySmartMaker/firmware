#include "configuration.h"

#if defined(ARCH_NRF52)
#include "ExternalFlash.h"
#include <Arduino.h>
#include <cstring>

#if defined(EXTERNAL_FLASH_USE_QSPI) && __has_include(<Adafruit_SPIFlash.h>)
#ifndef SS
#define SS PIN_QSPI_CS
#endif
#include "SPILock.h"
#include <Adafruit_SPIFlash.h>
#include <flash_devices.h>
#define NODARA_EXT_FLASH_QSPI 1
#endif

namespace nodara
{

static constexpr uint32_t CNFONT_EXT_ADDR = CNFONT_CFG_EXT_ADDR;
static constexpr uint32_t CNFONT_EXT_MAX_BYTES = CNFONT_CFG_MAX_BYTES;
static constexpr uint32_t CNFONT_MAGIC = CNFONT_CFG_MAGIC;
static constexpr uint32_t CNFONT_VERSION = CNFONT_CFG_VERSION;
static constexpr uint32_t CNFONT_KEY_SIZE = CNFONT_CFG_KEY_SIZE;
static constexpr uint32_t CNFONT_BITMAP_SIZE = CNFONT_CFG_BITMAP_SIZE;

#pragma pack(push, 1)
struct ChineseFontImageHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t reserved;
};
#pragma pack(pop)

#if defined(NODARA_EXT_FLASH_QSPI)
static Adafruit_FlashTransport_QSPI extFlashTransport;
static Adafruit_SPIFlash extFlash(&extFlashTransport);
static bool extFlashInitDone = false;
static bool extFlashReady = false;
static constexpr uint32_t kExtFlashSectorSize = 4096;
static constexpr uint32_t kExtFlashPageSize = 256;
static constexpr uint32_t kExtFlashWaitTimeoutMs = 2000;

static void probeRawExternalFlash()
{
    uint8_t jedec[4] = {0};
    uint8_t sr1 = 0;
    uint8_t sr2 = 0;

    extFlashTransport.begin();
    (void)extFlashTransport.readCommand(SFLASH_CMD_READ_JEDEC_ID, jedec, sizeof(jedec));
    (void)extFlashTransport.readCommand(SFLASH_CMD_READ_STATUS, &sr1, 1);
    (void)extFlashTransport.readCommand(SFLASH_CMD_READ_STATUS2, &sr2, 1);
    extFlashTransport.end();
}

static bool beginExternalFlashWithConfiguredDevice()
{
    static SPIFlash_Device_t puyaP25q32 = []() {
        SPIFlash_Device_t d = {};
        d.manufacturer_id = 0x85;
        d.memory_type = 0x20;
        d.capacity = 0x16;
        d.supports_qspi = true;
        d.supports_qspi_writes = true;
        return d;
    }();
    return extFlash.begin(&puyaP25q32, 1);
}

static void clearExternalFlashProtectionBits()
{
    uint8_t sr1Before = extFlash.readStatus();
    uint8_t sr2Before = extFlash.readStatus2();
    uint8_t sr1Unlocked = (uint8_t)(sr1Before & 0xC3U);
    uint8_t sr2Wanted = (uint8_t)(sr2Before | 0x02U);

    uint8_t srPair[2] = {sr1Unlocked, sr2Wanted};
    extFlash.writeEnable();
    (void)extFlashTransport.writeCommand(SFLASH_CMD_WRITE_STATUS, srPair, 2);

    const uint32_t t1 = millis();
    while ((extFlash.readStatus() & 0x01U) != 0U && (uint32_t)(millis() - t1) < kExtFlashWaitTimeoutMs) {
        delay(1);
    }

    extFlash.writeEnable();
    (void)extFlashTransport.writeCommand(SFLASH_CMD_WRITE_STATUS2, &sr2Wanted, 1);

    const uint32_t t2 = millis();
    while ((extFlash.readStatus() & 0x01U) != 0U && (uint32_t)(millis() - t2) < kExtFlashWaitTimeoutMs) {
        delay(1);
    }

    extFlash.writeDisable();
    const uint32_t twel = millis();
    while ((extFlash.readStatus() & 0x02U) != 0U && (uint32_t)(millis() - twel) < kExtFlashWaitTimeoutMs) {
        delay(1);
    }
}

static bool ensureExtFlashReady()
{
    if (extFlashInitDone)
        return extFlashReady;
    extFlashInitDone = true;

    if (!beginExternalFlashWithConfiguredDevice()) {
        LOG_WARN("[EXTFLASH] begin failed");
        return false;
    }

    clearExternalFlashProtectionBits();
    extFlashReady = true;
    return true;
}

static bool waitExtFlashWipClearWithTimeout(const char *reason, uint32_t timeoutMs)
{
    const uint32_t start = millis();
    while ((extFlash.readStatus() & 0x01U) != 0U) {
        if ((uint32_t)(millis() - start) >= timeoutMs) {
            LOG_WARN("[EXTFLASH] wait ready timeout (%s), SR1=0x%02x SR2=0x%02x", reason ? reason : "unknown",
                     extFlash.readStatus(), extFlash.readStatus2());
            return false;
        }
        delay(1);
    }
    return true;
}

bool ExtFlashRawReady()
{
    return ensureExtFlashReady();
}

bool ExtFlashRawRead(uint32_t addr, void *buf, uint32_t len)
{
    if (!buf || len == 0 || !ensureExtFlashReady())
        return false;
    concurrency::LockGuard g(spiLock);
    return extFlash.readBuffer(addr, static_cast<uint8_t *>(buf), len) == len;
}

bool ExtFlashRawWrite(uint32_t addr, const void *buf, uint32_t len)
{
    if (!buf || len == 0 || !ensureExtFlashReady())
        return false;
    concurrency::LockGuard g(spiLock);

    const uint8_t *src = static_cast<const uint8_t *>(buf);
    uint32_t remain = len;
    while (remain > 0) {
        extFlash.writeDisable();
        if (!waitExtFlashWipClearWithTimeout("pre-write-page", kExtFlashWaitTimeoutMs)) {
            LOG_WARN("[EXTFLASH] write pre-wait timeout addr=0x%08lx", (unsigned long)addr);
            return false;
        }

        const uint32_t leftOnPage = kExtFlashPageSize - (addr & (kExtFlashPageSize - 1U));
        const uint32_t toWrite = (remain < leftOnPage) ? remain : leftOnPage;
        const uint32_t wrote = extFlash.writeBuffer(addr, src, toWrite);
        if (wrote != toWrite) {
            LOG_WARN("[EXTFLASH] write page failed addr=0x%08lx req=%lu got=%lu", (unsigned long)addr, (unsigned long)toWrite,
                     (unsigned long)wrote);
            return false;
        }

        if (!waitExtFlashWipClearWithTimeout("post-write-page", kExtFlashWaitTimeoutMs)) {
            LOG_WARN("[EXTFLASH] write post-wait timeout addr=0x%08lx", (unsigned long)addr);
            return false;
        }
        extFlash.writeDisable();

        addr += toWrite;
        src += toWrite;
        remain -= toWrite;
    }

    return true;
}

bool ExtFlashRawErase(uint32_t addr, uint32_t len)
{
    if (len == 0 || !ensureExtFlashReady())
        return false;
    concurrency::LockGuard g(spiLock);

    const uint32_t startSector = addr / kExtFlashSectorSize;
    const uint32_t endAddr = addr + len - 1U;
    const uint32_t endSector = endAddr / kExtFlashSectorSize;
    for (uint32_t s = startSector; s <= endSector; ++s) {
        if (!extFlash.eraseSector(s)) {
            return false;
        }
        if (!waitExtFlashWipClearWithTimeout("erase-sector", kExtFlashWaitTimeoutMs)) {
            return false;
        }
    }
    return true;
}

bool ExtFlashBeginChineseFontUpload()
{
    return ExtFlashRawErase(CNFONT_EXT_ADDR, CNFONT_EXT_MAX_BYTES);
}

bool ExtFlashWriteChineseFontUploadChunk(uint32_t offset, const void *buf, uint32_t len)
{
    if (!buf || len == 0)
        return false;
    if (offset > CNFONT_EXT_MAX_BYTES || len > (CNFONT_EXT_MAX_BYTES - offset))
        return false;

    return ExtFlashRawWrite(CNFONT_EXT_ADDR + offset, buf, len);
}

bool ExtFlashFinishChineseFontUpload(uint32_t totalBytes)
{
    if (totalBytes < sizeof(ChineseFontImageHeader) || totalBytes > CNFONT_EXT_MAX_BYTES)
        return false;

    ChineseFontImageHeader header = {};
    if (!ExtFlashRawRead(CNFONT_EXT_ADDR, &header, sizeof(header)))
        return false;

    if (header.magic != CNFONT_MAGIC || header.version != CNFONT_VERSION || header.count == 0)
        return false;

    const uint32_t keyBytes = header.count * CNFONT_KEY_SIZE;
    const uint32_t bitmapBytes = header.count * CNFONT_BITMAP_SIZE;
    const uint32_t expectedBytes = sizeof(header) + keyBytes + bitmapBytes;
    if (expectedBytes != totalBytes || expectedBytes > CNFONT_EXT_MAX_BYTES)
        return false;

    return true;
}

void ExtFlashAbortChineseFontUpload()
{
    (void)ExtFlashRawErase(CNFONT_EXT_ADDR, kExtFlashSectorSize);
}

void ExtFlashSelfTest()
{
    probeRawExternalFlash();

    if (!ensureExtFlashReady()) {
        LOG_WARN("[EXTFLASH] self-test failed");
    }
}
#else
void ExtFlashSelfTest() {}
bool ExtFlashRawReady()
{
    return false;
}
bool ExtFlashRawRead(uint32_t, void *, uint32_t)
{
    return false;
}
bool ExtFlashRawWrite(uint32_t, const void *, uint32_t)
{
    return false;
}
bool ExtFlashRawErase(uint32_t, uint32_t)
{
    return false;
}
bool ExtFlashBeginChineseFontUpload()
{
    return false;
}
bool ExtFlashWriteChineseFontUploadChunk(uint32_t, const void *, uint32_t)
{
    return false;
}
bool ExtFlashFinishChineseFontUpload(uint32_t)
{
    return false;
}
void ExtFlashAbortChineseFontUpload() {}
#endif

} // namespace nodara

#else // !ARCH_NRF52

#include "ExternalFlash.h"

namespace nodara
{

void ExtFlashSelfTest() {}
bool ExtFlashRawReady()
{
    return false;
}
bool ExtFlashRawRead(uint32_t, void *, uint32_t)
{
    return false;
}
bool ExtFlashRawWrite(uint32_t, const void *, uint32_t)
{
    return false;
}
bool ExtFlashRawErase(uint32_t, uint32_t)
{
    return false;
}
bool ExtFlashBeginChineseFontUpload()
{
    return false;
}
bool ExtFlashWriteChineseFontUploadChunk(uint32_t, const void *, uint32_t)
{
    return false;
}
bool ExtFlashFinishChineseFontUpload(uint32_t)
{
    return false;
}
void ExtFlashAbortChineseFontUpload() {}

} // namespace nodara

#endif // ARCH_NRF52
