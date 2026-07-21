#include "NavCombos.h"

#if defined(Nodara)

#include "concurrency/OSThread.h"
#include "configuration.h"
#include "input/InputBroker.h"
#include "SixKeyNavInput.h"
#include "main.h"
#include "mesh/RadioLibInterface.h"
#include "meshUtils.h"
#include <utility> // std::pair
#include <vector>

#if 1

void navComboUp()
{

}

void navComboDown()
{

}

void navComboLeft()
{

}
void navComboRight()
{

}

void navComboEnter()
{

}

#else

// ---------------------------------------------------------------------------
// Fn + Up → toggle Bluetooth (persistent, triggers reboot)
// ---------------------------------------------------------------------------
void navComboUp()
{
    LOG_INFO("NavCombo: BLE toggle -> saveToDisk + reboot scheduled");
    if (sixKeyNavInput) {
        sixKeyNavInput->fireKbEvent(INPUT_BROKER_MSG_BLUETOOTH_TOGGLE);
    }
}

// ---------------------------------------------------------------------------
// Fn + Down → screen off / close overlay menu
//
// InkHUD mode (EINK_RA01S_GP02_INKHUD):
//   • Display already off    → wake it back up
//   • System applet active   → exitShort() (closes menu / keyboard / overlay)
//   • Main view, no overlay  → cut panel power (EN LOW, tristate SPI1 pins)
//
// Screen mode:
//   • fire INPUT_BROKER_CANCEL → Screen.cpp handles screen-off sequence
// ---------------------------------------------------------------------------
void navComboDown()
{
#if 1
    for (int i=0; ; i++) {
        LOG_INFO("NavCombo: delay %d s", i*5);
        delay(5000);
    }
#else
    LOG_INFO("NavCombo: screen off");
    if (sixKeyNavInput) {
        sixKeyNavInput->fireEvent(INPUT_BROKER_CANCEL);
    }
#endif
}

// ---------------------------------------------------------------------------
// Fn + Left → toggle LoRa radio on/off
// ---------------------------------------------------------------------------
static bool s_loraEnabled = true;

void navComboLeft()
{
    if (!RadioLibInterface::instance)
        return;
    s_loraEnabled = !s_loraEnabled;
    if (s_loraEnabled) {
        LOG_INFO("NavCombo: LoRa ON");
        RadioLibInterface::instance->enable();
        IF_SCREEN(screen->showSimpleBanner("LoRa ON", 3000));
    } else {
        LOG_INFO("NavCombo: LoRa OFF");
        static_cast<RadioInterface *>(RadioLibInterface::instance)->disable();
        IF_SCREEN(screen->showSimpleBanner("LoRa OFF", 3000));
    }
}

// ---------------------------------------------------------------------------
// Fn + Right → toggle GPS power (persistent)
// ---------------------------------------------------------------------------
void navComboRight()
{
    // Migration: previous firmware releases without GPS_RX_PIN defined
    // saved gps_mode=NOT_PRESENT, which would prevent gps creation forever.
    // If the hardware actually has a GPS RX pin, promote to DISABLED so the
    // user can enable GPS via menu or Fn+Right shortcut without a factory reset.
    LOG_INFO("NavCombo: GPS toggle");
    if (sixKeyNavInput) {
        sixKeyNavInput->fireEvent(INPUT_BROKER_GPS_TOGGLE);
    }
}

// ---------------------------------------------------------------------------
// Fn + Enter → toggle thread throttle
// ---------------------------------------------------------------------------
void navComboEnter()
{
    LOG_INFO("NavCombo: shutdown");
    if (sixKeyNavInput)
        sixKeyNavInput->fireEvent(INPUT_BROKER_SHUTDOWN);
}

#endif

#endif // Nodara
