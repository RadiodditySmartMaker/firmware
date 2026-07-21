#pragma once
#define MAX_VALID_CHANNELS 8

#include "OLEDDisplay.h"
#include "OLEDDisplayUi.h"
#include "graphics/emotes.h"
#include <string>
#include <vector>
extern int validChannelIndices[MAX_VALID_CHANNELS];
extern int validChannelCount;
extern size_t channelIndex;
extern uint16_t channelFrameBeginIndex;
extern uint16_t channelPacketBrowseIndex;
namespace graphics
{
namespace ChannelMessageRenderer
{

// 主渲染函数
void drawChannelTextMessageFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y);

// 辅助函数声明
bool getFrameIndexByChannelIndex(uint8_t channelIndex, uint8_t *frameIndex);
bool isBrowsingChannelPacketFrame(uint8_t currentFrame);
uint8_t getBrowsingChannelIndex(uint8_t currentFrame);
extern OLEDDisplay *display;
extern OLEDDisplayUi *ui;
extern std::vector<std::string> messages;
} // namespace ChannelMessageRenderer
} // namespace graphics