#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "gba_system.hpp"

namespace {

using LOTUSGBA::GbaSystem;

constexpr uint32_t GPIO_DATA = 0x080000C4;
constexpr uint32_t GPIO_DIRECTION = 0x080000C6;
constexpr uint32_t GPIO_CONTROL = 0x080000C8;

std::vector<uint8_t> makeRtcRom(bool withFlash = false) {
    std::vector<uint8_t> rom(0x200, 0);
    constexpr uint32_t branchToSelf = 0xEAFFFFFE;
    for (unsigned byte = 0; byte < 4; ++byte)
        rom[0xC0 + byte] = static_cast<uint8_t>(branchToSelf >> (byte * 8));
    rom[0xC4] = 0xA4;
    rom[0xC6] = 0xA6;
    rom[0xC8] = 0xA8;
    if (withFlash) {
        constexpr char flashMarker[] = "FLASH1M_V103";
        rom.insert(rom.end(), flashMarker, flashMarker + sizeof(flashMarker) - 1);
    }
    constexpr char rtcMarker[] = "SIIRTC_V001";
    rom.insert(rom.end(), rtcMarker, rtcMarker + sizeof(rtcMarker) - 1);
    return rom;
}

void beginTransaction(GbaSystem &system) {
    system.write16(GPIO_DATA, 1); // SCK high, CS low.
    system.write16(GPIO_DATA, 5); // Assert CS.
    system.write16(GPIO_DIRECTION, 7);
}

void sendByte(GbaSystem &system, uint8_t value, bool msbFirst) {
    for (unsigned index = 0; index < 8; ++index) {
        const unsigned bit = msbFirst ? 7 - index : index;
        const uint16_t data = static_cast<uint16_t>(
            4 | (((value >> bit) & 1) << 1));
        system.write16(GPIO_DATA, data); // SCK low; place SIO.
        system.write16(GPIO_DATA, static_cast<uint16_t>(data | 1)); // SCK high.
    }
}

void sendCommand(GbaSystem &system, uint8_t command) {
    sendByte(system, command, true);
}

void finishTransaction(GbaSystem &system) {
    system.write16(GPIO_DATA, 1); // Deassert CS.
}

void writeCommand(GbaSystem &system, uint8_t command,
                  const uint8_t *bytes, size_t length) {
    beginTransaction(system);
    sendCommand(system, command);
    for (size_t index = 0; index < length; ++index)
        sendByte(system, bytes[index], false);
    finishTransaction(system);
}

std::vector<uint8_t> readCommand(GbaSystem &system, uint8_t command,
                                 size_t length) {
    beginTransaction(system);
    sendCommand(system, command);
    system.write16(GPIO_DIRECTION, 5); // SIO becomes an input.
    std::vector<uint8_t> bytes(length, 0);
    for (size_t byte = 0; byte < length; ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            system.write16(GPIO_DATA, 4);
            system.write16(GPIO_DATA, 5);
            if ((system.read8(GPIO_DATA) & 2) != 0)
                bytes[byte] |= static_cast<uint8_t>(1u << bit);
        }
    }
    finishTransaction(system);
    return bytes;
}

void testRtcMarkerAndReadEnable() {
    const auto markedRom = makeRtcRom();
    GbaSystem plain(std::vector<uint8_t>(markedRom.begin(),
                                         markedRom.begin() + 0x200));
    assert(plain.getRtcSize() == 0);
    assert(plain.getRtcData() == nullptr);
    assert(plain.read8(GPIO_DATA) == 0xA4);
    plain.write16(GPIO_CONTROL, 1);
    assert(plain.read8(GPIO_DATA) == 0xA4);

    GbaSystem rtc(makeRtcRom());
    assert(rtc.getRtcSize() == 24);
    assert(rtc.getRtcData() != nullptr);
    assert(rtc.read8(GPIO_DATA) == 0xA4); // Reads are disabled after power-up.
    rtc.write16(GPIO_CONTROL, 1);
    assert(rtc.read8(GPIO_CONTROL) == 1);
    assert((rtc.read8(GPIO_DATA) & 2) == 0);
    const auto status = readCommand(rtc, 0x63, 1);
    assert(status[0] == 0x40); // Healthy RTC, 24-hour mode.
}

void testRtcSetReadAdvanceAndRestore() {
    auto rom = makeRtcRom(true);
    GbaSystem system(rom);
    assert(system.getSaveRamSize() == GbaSystem::FLASH1M_SIZE);
    assert(system.getRtcSize() == 24);
    auto *flash = system.getSaveRamData();
    auto *rtcBytes = system.getRtcData();
    assert(flash && rtcBytes && flash != rtcBytes);
    flash[0x1234] = 0x5A;

    system.write16(GPIO_CONTROL, 1);
    const std::array<uint8_t, 7> dateTime = {
        0x00, 0x01, 0x01, 0x06, 0x12, 0x34, 0x20
    }; // 2000-01-01 12:34:20 in BCD.
    writeCommand(system, 0x64, dateTime.data(), dateTime.size());
    const auto firstRead = readCommand(system, 0x65, 7);
    assert(firstRead[0] == dateTime[0]);
    assert(firstRead[1] == dateTime[1]);
    assert(firstRead[2] == dateTime[2]);
    assert(firstRead[4] == dateTime[4]);
    assert(firstRead[5] == dateTime[5]);
    assert(firstRead[6] >= 0x20 && firstRead[6] <= 0x22);

    std::this_thread::sleep_for(std::chrono::milliseconds(1150));
    const auto advanced = readCommand(system, 0x67, 3);
    assert(advanced[0] == 0x12);
    assert(advanced[1] == 0x34);
    assert(advanced[2] >= 0x21 && advanced[2] <= 0x24);

    auto persisted = std::make_shared<std::vector<uint8_t>>(
        rtcBytes, rtcBytes + system.getRtcSize());
    GbaSystem restored(rom, {}, nullptr, persisted);
    restored.write16(GPIO_CONTROL, 1);
    const auto afterReload = readCommand(restored, 0x65, 7);
    assert(afterReload[0] == 0x00 && afterReload[1] == 0x01 &&
           afterReload[2] == 0x01);
    assert(afterReload[4] == 0x12 && afterReload[5] == 0x34);
    assert(afterReload[6] >= advanced[2]);
    assert(restored.getSaveRamSize() == GbaSystem::FLASH1M_SIZE);
    assert(flash[0x1234] == 0x5A);
}

void testLinkedRtcUsesEmulatedTime() {
    constexpr int64_t sessionEpoch = 946684799; // 1999-12-31 23:59:59 UTC.
    GbaSystem parent(makeRtcRom(), {}, nullptr, nullptr, sessionEpoch);
    GbaSystem child(makeRtcRom(), {}, nullptr, nullptr, sessionEpoch);
    parent.connectLinkChild(child);
    parent.write16(GPIO_CONTROL, 1);
    child.write16(GPIO_CONTROL, 1);
    const std::vector<uint8_t> beforeMidnight = {0x99, 0x12, 0x31, 5, 0x23, 0x59, 0x59};
    assert(readCommand(parent, 0x65, 7) == beforeMidnight);
    assert(readCommand(child, 0x65, 7) == beforeMidnight);

    for (unsigned frame = 0; frame < 60; ++frame)
        assert(parent.runLinkedFrame().success);
    const std::vector<uint8_t> midnight = {0, 1, 1, 6, 0, 0, 0};
    assert(readCommand(parent, 0x65, 7) == midnight);
    assert(readCommand(child, 0x65, 7) == midnight);

    const std::array<uint8_t, 7> changedDate = {0, 1, 2, 0, 0x13, 0, 0};
    writeCommand(parent, 0x64, changedDate.data(), changedDate.size());
    writeCommand(child, 0x64, changedDate.data(), changedDate.size());
    const std::vector<uint8_t> expected(changedDate.begin(), changedDate.end());
    assert(readCommand(parent, 0x65, 7) == expected);
    assert(readCommand(child, 0x65, 7) == expected);
    assert(*parent.getRtcStorage() == *child.getRtcStorage());
}

} // namespace

int main() {
    testRtcMarkerAndReadEnable();
    testRtcSetReadAdvanceAndRestore();
    testLinkedRtcUsesEmulatedTime();
}
