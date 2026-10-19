#include "include/joypad.hpp"

namespace LOTUSGB {

namespace {
constexpr uint16_t JOYPAD_ADDRESS = 0xFF00;
constexpr uint8_t DIRECTION_SELECT = 0x10;
constexpr uint8_t BUTTON_SELECT = 0x20;
constexpr uint8_t JOYPAD_INPUT_MASK = 0x0F;
}

Joypad::Joypad(IInterruptController &interrupts) : interrupts(interrupts) {}

uint8_t Joypad::read(uint16_t address) {
    if (address != JOYPAD_ADDRESS)
        return 0xFF;
    return static_cast<uint8_t>(0xC0 | selectionBits | getSelectedInputLines());
}

void Joypad::write(uint16_t address, uint8_t value) {
    if (address != JOYPAD_ADDRESS)
        return;

    const uint8_t previousLines = getSelectedInputLines();
    selectionBits = value & (DIRECTION_SELECT | BUTTON_SELECT);
    requestFallingEdge(previousLines);
}

void Joypad::setButtonState(JoypadButton button, bool pressed) {
    const uint8_t previousLines = getSelectedInputLines();
    const uint8_t buttonMask = static_cast<uint8_t>(1U << static_cast<uint8_t>(button));
    if (pressed)
        pressedButtons |= buttonMask;
    else
        pressedButtons &= static_cast<uint8_t>(~buttonMask);
    requestFallingEdge(previousLines);
}

uint8_t Joypad::getSelectedInputLines() const {
    uint8_t inputLines = JOYPAD_INPUT_MASK;

    if ((selectionBits & DIRECTION_SELECT) == 0)
        inputLines &= static_cast<uint8_t>(~pressedButtons) & JOYPAD_INPUT_MASK;
    if ((selectionBits & BUTTON_SELECT) == 0)
        inputLines &= static_cast<uint8_t>(~(pressedButtons >> 4)) & JOYPAD_INPUT_MASK;

    return inputLines;
}

void Joypad::requestFallingEdge(uint8_t previousLines) {
    const uint8_t currentLines = getSelectedInputLines();
    if ((previousLines & static_cast<uint8_t>(~currentLines) & JOYPAD_INPUT_MASK) != 0)
        interrupts.requestInterrupt(INTERRUPT_JOYPAD);
}

}
