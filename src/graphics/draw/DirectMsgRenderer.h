#pragma once
#define MAX_VALID_CHANNELS 8

#include "OLEDDisplay.h"
#include "OLEDDisplayUi.h"
#include "graphics/emotes.h"
#include <string>
#include <vector>

namespace graphics
{
namespace DirectMsgRenderer
{
// 主渲染函数
void drawDirectMessageFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y);

extern OLEDDisplay *display;
extern OLEDDisplayUi *ui;
extern std::vector<std::string> messages;
} // namespace DirectMsgRenderer
} // namespace graphics