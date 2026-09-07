#include "ChannelMessageRenderer.h"

#include "NodeDB.h"
#include "OLEDDisplay.h"
#include "PowerStatus.h"
#include "graphics/Screen.h"
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/TimeFormatters.h"
#include "graphics/draw/DrawCjkFont.h"
#include "graphics/draw/MessageRenderer.h"
#include "graphics/draw/UIRenderer.h"
#include "graphics/fonts/OLEDDisplayFontsDL.h"
#include "main.h"
#include "meshUtils.h"
#define MAX_VALID_CHANNELS 8
int validChannelIndices[MAX_VALID_CHANNELS];
int validChannelCount = 0;
size_t channelIndex = 0;
uint16_t channelFrameBeginIndex = 0;
uint16_t channelPacketBrowseIndex = -1;
namespace graphics
{

namespace ChannelMessageRenderer
{

// 辅助函数：根据屏幕旋转角度选择合适的字体
struct FontSelection {
    const uint8_t *font;
    int height;
};

FontSelection selectFontForRotation(uint8_t rotation)
{
    FontSelection selection;
    if (rotation == 0) {
        // 竖屏模式使用最小字体 (Arimo_Regular_14, 高度14像素)
        selection.font = FONT_SMALL;
        selection.height = _fontHeight(FONT_SMALL);
    } else {
        // 横屏模式使用标准小字体 (Arimo_Regular_16, 高度16像素)
        selection.font = FONT_MEDIUM;
        selection.height = FONT_HEIGHT_MEDIUM;
        // selection.font = FONT_LARGE;
        // selection.height = _fontHeight(FONT_LARGE);
    }
    return selection;
}

bool getFrameIndexByChannelIndex(uint8_t channelIndex, uint8_t *frameIndex)
{
    uint8_t i = 0;
    uint8_t channelCnt = sizeof(validChannelIndices) / sizeof(validChannelIndices[0]);

    for (i = 0; i < channelCnt; ++i) {
        if (validChannelIndices[i] == channelIndex) {
            break;
        }
    }

    if (i >= channelCnt) {
        return (false);
    }

    *frameIndex = i;
    return (true);
}

bool isBrowsingChannelPacketFrame(uint8_t currentFrame)
{
    // 只有单独的“频道消息”页面，currentFrame 等于该帧索引时才认为在浏览频道消息
    if (validChannelCount == 0) {
        return false;
    }

    return (currentFrame == channelFrameBeginIndex);
}

uint8_t getBrowsingChannelIndex(uint8_t /*currentFrame*/)
{
    // 当前浏览的频道索引直接由全局的 channelIndex 决定
    return static_cast<uint8_t>(channelIndex);
}

/// Draw the last text message we received
void drawChannelTextMessageFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    // the max length of this buffer is much longer than we can possibly print
    static char tempBuf[237];
    int width = display->getWidth();
    int height = display->getHeight();
#ifdef Nodara

#if HAS_SCREEN
    uint8_t currentRotation = (width < height) ? 0 : 3;
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    FontSelection fontSel = selectFontForRotation(currentRotation);
#else
    uint8_t currentRotation = 3;
    FontSelection fontSel = selectFontForRotation(3);
#endif
    const uint8_t *selectedFont = fontSel.font;
    int selectedFontHeight = fontSel.height;

    display->setFont(selectedFont);

    int localActualChannelIndex = getBrowsingChannelIndex(state->currentFrame);
    if (localActualChannelIndex >= 8) {
        LOG_INFO("Error localActualChannelIndex = %d >=  validChannelCount = %d\n", localActualChannelIndex, validChannelCount);
        return;
    }

    char title[23];
    if (currentRotation == 0)
        snprintf(title, sizeof(title), "CH Msg");
    else
        snprintf(title, sizeof(title), "Channel Message");

    // 使用 drawCommonHeader 绘制标题栏
    drawCommonHeader(display, x, y, title, false);

    const int headerHeight = FONT_HEIGHT_SMALL + 1;
    // y += headerHeight;

    // display channel name
    const char *name = channelFile.channels[localActualChannelIndex].settings.name;
    char displayName[25];

    if (strlen(name) > 0) {
        if (channelFile.channels[localActualChannelIndex].role == meshtastic_Channel_Role_PRIMARY)
            snprintf(displayName, sizeof(displayName), "Pri Ch: %s", name);
        else
            snprintf(displayName, sizeof(displayName), "Sec Ch: %s", name);
    } else {
        char preset = config.lora.modem_preset;
        switch (preset) {
        case meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST:
            name = "Long_fast";
            break;
        case meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW:
            name = "Long_slow";
            break;
        case meshtastic_Config_LoRaConfig_ModemPreset_VERY_LONG_SLOW:
            name = "Very_long_slow";
            break;
        case meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_SLOW:
            name = "Medium_slow";
            break;
        case meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST:
            name = "Medium_fast";
            break;
        case meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW:
            name = "Short_slow";
            break;
        case meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST:
            name = "Short_fast";
            break;
        default:
            name = "Long_fast";
            break;
        }
        if (channelFile.channels[localActualChannelIndex].role == meshtastic_Channel_Role_PRIMARY)
            snprintf(displayName, sizeof(displayName), "Pri Ch: %s", name);
        else
            snprintf(displayName, sizeof(displayName), "Sec Ch: %s", name);
    }

    display->setTextAlignment(TEXT_ALIGN_LEFT);

    display->drawString(x + 5, y + selectedFontHeight, displayName);

    display->setTextAlignment(TEXT_ALIGN_RIGHT);

    display->drawLine(x, y + selectedFontHeight * 2, x + width, y + selectedFontHeight * 2);
    y += selectedFontHeight * 2;

    display->setTextAlignment(TEXT_ALIGN_LEFT);

    // display packet info
    // uint8_t direction = 0;
    if (!chatHistoryStore) {
        return;
    }

    uint16_t packetListSize = chatHistoryStore->getMeshPacketListSize(localActualChannelIndex);

    if (packetListSize == 0) {
        return;
    }

    if (channelPacketBrowseIndex >= packetListSize) {
        channelPacketBrowseIndex = packetListSize - 1;
    }

    const meshtastic_MeshPacket &mp = chatHistoryStore->getRecentMeshPacket(localActualChannelIndex, channelPacketBrowseIndex);
    // added by QCB end
    meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(getFrom(&mp));

    // For time delta
    uint32_t seconds = sinceReceived(&mp);
    uint32_t minutes = seconds / 60;
    uint32_t hours = minutes / 60;
    uint32_t days = hours / 24;

    // For timestamp
    uint8_t timestampHours, timestampMinutes;
    int32_t daysAgo;
    bool useTimestamp = deltaToTimestamp(seconds, &timestampHours, &timestampMinutes, &daysAgo);

    const bool mine = isFromUs(&mp);
    const AckStatus ack =
        chatHistoryStore->getRecentMeshPacketAck(localActualChannelIndex, static_cast<uint8_t>(channelPacketBrowseIndex));
    constexpr int kAckMarkSize = 8;
    const int ackPad = (mine && ack != AckStatus::NONE) ? (kAckMarkSize + 4) : 0;
    if (ackPad) {
        MessageRenderer::drawAckStatusMark(display, x, y, ack, kAckMarkSize);
    }

    // If bold, draw twice, shifting right by one pixel
    for (uint8_t xOff = 0; xOff <= (config.display.heading_bold ? 1 : 0); xOff++) {
        // Show a timestamp if received today, but longer than 15 minutes ago
        if (useTimestamp && minutes >= 15 && daysAgo == 0) {
            display->drawStringf(xOff + x + ackPad, 0 + y, tempBuf, "At %02hu:%02hu from %s", timestampHours, timestampMinutes,
                                 (node && node->has_user) ? node->user.short_name : "???");
        }
        // Timestamp yesterday (if display is wide enough)
        else if (useTimestamp && daysAgo == 1 && display->width() >= 200) {
            display->drawStringf(xOff + x + ackPad, 0 + y, tempBuf, "Yesterday %02hu:%02hu from %s", timestampHours,
                                 timestampMinutes, (node && node->has_user) ? node->user.short_name : "???");
        }
        // Otherwise, show a time delta
        else {
            display->drawStringf(xOff + x + ackPad, 0 + y, tempBuf, "%s ago from %s",
                                 UIRenderer::drawTimeDelta(days, hours, minutes, seconds).c_str(),
                                 (node && node->has_user) ? node->user.short_name : "???");
        }
    }

    snprintf(tempBuf, sizeof(tempBuf), "%s", mp.decoded.payload.bytes);
    // display->drawStringMaxWidth(0 + x, 0 + y + selectedFontHeight, x + display->getWidth(), tempBuf);
    drawCjkStringWithLineBreak(display, 0 + x, 0 + y + selectedFontHeight, tempBuf);

#endif
}
} // namespace ChannelMessageRenderer
} // namespace graphics
