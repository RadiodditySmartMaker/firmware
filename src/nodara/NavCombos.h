#pragma once

#if defined(Nodara)

/**
 * NavCombos - variant-specific combo-key behavior for eink-ra01s-gp02.
 *
 * Provides the 5 combo-key handler functions to be assigned to
 * SixKeyNavInput::onCombo* function pointers in InputBroker::Init().
 *
 * All state (e.g. throttle toggle) is managed here, so SixKeyNavInput
 * and main.cpp remain decoupled from board-specific behavior.
 *
 * Combo mapping:
 *   Fn + Up    → toggle Bluetooth (persistent, triggers reboot)
 *   Fn + Down  → screen off
 *   Fn + Left  → toggle LoRa radio on/off
 *   Fn + Right → toggle GPS power (persistent)
 *   Fn + Enter → toggle thread throttle (NAV_THROTTLE_MS)
 */

void navComboUp();
void navComboDown();
void navComboLeft();
void navComboRight();
void navComboEnter();

#endif // Nodara
