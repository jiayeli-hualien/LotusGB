#include <cstdlib>
#include <iostream>

#include "interrupt_registers.hpp"
#include "joypad.hpp"

namespace {

using namespace LOTUSGB;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void clearJoypadRequest(InterruptRegisters &interrupts) {
    interrupts.write(0xFF0F, 0);
}

void testSelectionAndActiveLowInputs() {
    InterruptRegisters interrupts;
    Joypad joypad(interrupts);

    expect(joypad.read(0xFF00) == 0xFF, "no selected group reads all input lines high");
    joypad.write(0xFF00, 0x20); // Select directions (P14 low).
    expect(joypad.read(0xFF00) == 0xEF,
           "direction selection is retained and unpressed active-low lines read high");

    joypad.setButtonState(JoypadButton::Right, true);
    expect(joypad.read(0xFF00) == 0xEE,
           "pressing Right pulls only the selected Right line low");
    expect(interrupts.read(0xFF0F) == 0xF0,
           "a selected input high-to-low transition requests the joypad interrupt");

    clearJoypadRequest(interrupts);
    joypad.setButtonState(JoypadButton::Right, false);
    expect(joypad.read(0xFF00) == 0xEF, "releasing Right returns its line high");
    expect(interrupts.read(0xFF0F) == 0xE0,
           "an input low-to-high transition does not request the joypad interrupt");
}

void testSelectionWritesAndUnselectedButtons() {
    InterruptRegisters interrupts;
    Joypad joypad(interrupts);
    joypad.write(0xFF00, 0x20); // Directions selected, buttons not selected.
    joypad.setButtonState(JoypadButton::A, true);

    expect(joypad.read(0xFF00) == 0xEF,
           "a pressed button in the unselected group does not affect P1 input lines");
    expect(interrupts.read(0xFF0F) == 0xE0,
           "an unselected button does not request a joypad interrupt");

    joypad.write(0xFF00, 0x10); // Select action buttons while A is held.
    expect(joypad.read(0xFF00) == 0xDE,
           "selecting a held button group exposes its active-low input line");
    expect(interrupts.read(0xFF0F) == 0xF0,
           "selecting a group can cause a joypad interrupt on a falling input line");

    clearJoypadRequest(interrupts);
    joypad.write(0xFF00, 0x00); // Both groups selected.
    joypad.setButtonState(JoypadButton::Right, true);
    expect(joypad.read(0xFF00) == 0xCE,
           "selecting both groups combines their active-low input lines");
    expect(interrupts.read(0xFF0F) == 0xE0,
           "a second button on an already-low line creates no new falling edge");

    joypad.setButtonState(JoypadButton::A, false);
    expect(joypad.read(0xFF00) == 0xCE,
           "releasing one button keeps a shared line low while another button holds it");
    joypad.setButtonState(JoypadButton::Right, false);
    expect(joypad.read(0xFF00) == 0xCF,
           "the shared line rises only after all buttons on it are released");
}

}

int main() {
    testSelectionAndActiveLowInputs();
    testSelectionWritesAndUnselectedButtons();
    std::cout << "Joypad tests passed\n";
    return EXIT_SUCCESS;
}
