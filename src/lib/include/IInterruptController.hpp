#ifndef LOTUSGB_IINTERRUPTCONTROLLER_HPP
#define LOTUSGB_IINTERRUPTCONTROLLER_HPP

#include <cstdint>

namespace LOTUSGB {

enum InterruptMask : uint8_t {
    INTERRUPT_VBLANK = 0x01,
    INTERRUPT_LCD_STAT = 0x02,
    INTERRUPT_TIMER = 0x04,
    INTERRUPT_SERIAL = 0x08,
    INTERRUPT_JOYPAD = 0x10,
};

class IInterruptController {
public:
    virtual ~IInterruptController() = default;
    virtual uint8_t getPendingInterrupts() const = 0;
    virtual void acknowledgeInterrupt(uint8_t interruptMask) = 0;
    virtual void requestInterrupt(uint8_t interruptMask) = 0;
};

}

#endif // LOTUSGB_IINTERRUPTCONTROLLER_HPP
