#include "Nodara.h"

#include "McuDataInput.h"

namespace nodara
{
namespace
{
McuDataInput mcuDataInput;
} // namespace

void setup()
{
    mcuDataInput.setup();
}

void loop()
{
    mcuDataInput.loop();
}

} // namespace nodara
