#ifndef LOTUSGB_JOYPAD_HPP
#define LOTUSGB_JOYPAD_HPP

#include <cstdint>

#include "IInterruptController.hpp"
#include "address_bus.hpp"

namespace LOTUSGB {

enum class JoypadButton : uint8_t {
    Right = 0,
    Left = 1,
    Up = 2,
    Down = 3,
    A = 4,
    B = 5,
    Select = 6,
    Start = 7,
};

class Joypad final : public IAddressRegion {
public:
    explicit Joypad(IInterruptController &interrupts);

    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    void setButtonState(JoypadButton button, bool pressed);

private:
    uint8_t getSelectedInputLines() const;
    void requestFallingEdge(uint8_t previousLines);

    IInterruptController &interrupts;
    uint8_t selectionBits = 0x30;
    uint8_t pressedButtons = 0;
};

}

#endif // LOTUSGB_JOYPAD_HPP
