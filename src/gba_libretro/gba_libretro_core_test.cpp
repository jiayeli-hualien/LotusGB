#include "libretro.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "gba_boot_rom.hpp"
#include "gba_system.hpp"

namespace {

bool videoReceived = false;
unsigned videoRefreshCount = 0;
unsigned receivedWidth = 0;
unsigned receivedHeight = 0;
std::vector<uint32_t> receivedPixels;
std::vector<int16_t> receivedAudio;
enum retro_pixel_format selectedFormat = RETRO_PIXEL_FORMAT_0RGB1555;
bool acceptXrgb8888 = true;
uint16_t pressedJoypad = 0;
uint16_t secondJoypad = 0;
const retro_subsystem_info *linkSubsystem = nullptr;
const retro_netpacket_callback *netpacket = nullptr;
std::vector<std::vector<uint8_t>> sentNetpackets;
std::vector<std::string> logMessages;
std::string systemDirectoryPath;
std::string biosStartupMode = "Automatic";
bool biosOptionRegistered = false;
std::string linkDeviceOption = "Wireless Adapter";
bool linkDeviceOptionRegistered = false;

void inputPoll() {}

void testLog(retro_log_level, const char *format, ...) {
    char message[1024];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    logMessages.emplace_back(message);
}

size_t audioBatch(const int16_t *samples, size_t frames) {
    if (frames != 0) {
        assert(samples != nullptr);
        receivedAudio.insert(receivedAudio.end(), samples, samples + frames * 2);
    }
    return frames;
}

int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port > 1 || device != RETRO_DEVICE_JOYPAD || index != 0 || id >= 16)
        return 0;
    return ((port == 0 ? pressedJoypad : secondJoypad) & (1u << id)) != 0 ? 1 : 0;
}

std::vector<uint8_t> readFile(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    assert(input.good());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                 std::istreambuf_iterator<char>());
}

uint32_t expectedSolidRedPixel();
void runUntilSolidRed(unsigned additionalFrames = 16);

std::vector<uint8_t> makeFlash1mMode3Rom() {
    std::vector<uint8_t> rom = makeGbaMode3BootRom();
    constexpr char marker[] = "FLASH1M_V103";
    rom.insert(rom.end(), marker, marker + sizeof(marker) - 1);
    return rom;
}

std::vector<uint8_t> makeSaveMarkerMode3Rom(const char *marker) {
    std::vector<uint8_t> rom = makeGbaMode3BootRom();
    const size_t length = std::char_traits<char>::length(marker);
    rom.insert(rom.end(), marker, marker + length);
    return rom;
}

bool environment(unsigned command, void *data) {
    if (command == RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE) {
        netpacket = static_cast<const retro_netpacket_callback *>(data);
        return true;
    }
    if (command == RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO) {
        linkSubsystem = static_cast<const retro_subsystem_info *>(data);
        return true;
    }
    if (command == RETRO_ENVIRONMENT_SET_VARIABLES) {
        const auto *options = static_cast<const retro_variable *>(data);
        for (size_t index = 0; options[index].key; ++index) {
            biosOptionRegistered |=
                std::string(options[index].key) == "lotusgba_bios_mode";
            linkDeviceOptionRegistered |=
                std::string(options[index].key) == "lotusgba_link_device";
        }
        return true;
    }
    if (command == RETRO_ENVIRONMENT_GET_LOG_INTERFACE) {
        static_cast<retro_log_callback *>(data)->log = testLog;
        return true;
    }
    if (command == RETRO_ENVIRONMENT_GET_VARIABLE) {
        auto *option = static_cast<retro_variable *>(data);
        if (option && option->key && std::string(option->key) == "lotusgba_link_device") {
            option->value = linkDeviceOption.c_str(); return true;
        }
        if (option && option->key &&
            std::string(option->key) == "lotusgba_bios_mode") {
            option->value = biosStartupMode.c_str();
            return true;
        }
    }
    if (command == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY) {
        if (systemDirectoryPath.empty())
            return false;
        *static_cast<const char **>(data) = systemDirectoryPath.c_str();
        return true;
    }
    if (command == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) {
        selectedFormat = *static_cast<retro_pixel_format *>(data);
        return selectedFormat == RETRO_PIXEL_FORMAT_XRGB8888
                   ? acceptXrgb8888
                   : selectedFormat == RETRO_PIXEL_FORMAT_0RGB1555;
    }
    return false;
}

// Original ARM entry and Thumb payload. Each endpoint waits for its own
// controller, polls serial IF, then records both words and its role in SRAM.
std::vector<uint8_t> makeMultiplayerProbe(uint16_t color, uint16_t button,
                                          uint16_t outgoing, uint16_t control) {
    std::vector<uint8_t> rom(0xC0, 0);
    rom[0] = 0x2E;
    rom[3] = 0xEA; // B $080000C0
    const auto emit16 = [&rom](uint16_t word) {
        rom.push_back(static_cast<uint8_t>(word));
        rom.push_back(static_cast<uint8_t>(word >> 8));
    };
    const auto emit32 = [&emit16](uint32_t word) {
        emit16(static_cast<uint16_t>(word));
        emit16(static_cast<uint16_t>(word >> 16));
    };
    emit32(0xE59F0000); // LDR r0, [pc]
    emit32(0xE12FFF10); // BX r0
    emit32(0x080000CD); // Thumb entry
    const uint16_t code[] = {
        0x4818, 0x4919, 0x4A19, 0x800A, 0x2200, 0x8002, // Set backdrop color.
        0x4818, 0x4919, 0x4A19, 0x880B, 0x4213, 0xD1FC, // Wait for key.
        0x4A18, 0x8142, 0x2200, 0x8282, 0x4A17, 0x8102, // SEND, RCNT, SIOCNT.
        0x4D17, 0x4E18, 0x2702, // IF, save output and two rounds.
        0x8903, 0x2404, 0x4223, 0xD105, // Child skips START.
        0x2408, 0x4223, 0xD0F8, 0x2480, 0x4323, 0x8103, // Parent awaits SD.
        0x882B, 0x2480, 0x4223, 0xD0FB, 0x802C, // Poll and acknowledge IF.
        0x8803, 0x7033, 0x0A1B, 0x7073, // Store parent word as bytes.
        0x8843, 0x70B3, 0x0A1B, 0x70F3, // Store child word as bytes.
        0x8903, 0x7133, 0x3605, 0x3F01, 0xD1E3, 0xE7FE, // Role, next round.
    };
    for (uint16_t word : code)
        emit16(word);
    const uint32_t literals[] = {0x04000000, 0x05000000, color, 0x04000120,
        0x04000130, button, outgoing, control, 0x04000202, 0x0E000000};
    for (uint32_t word : literals)
        emit32(word);
    constexpr char saveMarker[] = "SRAM_V";
    rom.insert(rom.end(), saveMarker, saveMarker + sizeof(saveMarker));
    return rom;
}

void runMultiplayerBusCheck() {
    using LOTUSGBA::GbaSystem;
    constexpr uint32_t multi0 = 0x04000120;
    constexpr uint32_t control = 0x04000128;
    constexpr uint32_t send = 0x0400012A;
    constexpr uint32_t rcnt = 0x04000134;
    constexpr uint32_t irq = 0x04000202;
    GbaSystem parent(makeGbaMode3BootRom());
    {
        GbaSystem child(makeGbaMode3BootRom());
        parent.connectLinkChild(child);
        parent.write16(control, 0x6000);
        assert((parent.read16(control) & 8) == 0);
        parent.write8(control, 0x80); // Unready peer.
        assert((parent.read16(control) & 0xC0) == 0x40);
        assert((parent.read16(irq) & 0x80) == 0);

        child.write16(rcnt, 0x8000);
        child.write16(control, 0x2000);
        assert((parent.read16(control) & 8) == 0);
        child.write16(rcnt, 0);
        assert((parent.read16(control) & 0x3C) == 0x08);
        assert((child.read16(control) & 0x3C) == 0x1C);
        child.write8(control, 0xFC); // Status bits and child START are read-only.
        assert((child.read16(control) & 0xFC) == 0x1C);

        parent.write16(send, 0x4321);
        child.write16(send, 0xABCD);
        parent.write32(control, 0x43216080);
        assert((parent.read16(control) & 0xC0) == 0x80);
        assert((child.read16(control) & 0x80) == 0x80);
        assert((parent.read16(rcnt) & 1) == 0);
        assert(parent.read32(multi0) == 0xFFFFFFFF);
        assert(child.read32(multi0) == 0xFFFFFFFF);
        assert((parent.read16(irq) & 0x80) == 0);
        child.write16(send, 0x9876); // Next round's word.
        assert(parent.runLinkedFrame().success);
        for (GbaSystem *endpoint : {&parent, &child}) {
            assert(endpoint->read32(multi0) == 0xABCD4321);
            assert(endpoint->read32(multi0 + 4) == 0xFFFFFFFF);
            assert((endpoint->read16(control) & 0xC0) == 0);
            assert((endpoint->read16(rcnt) & 1) == 1);
        }
        assert((parent.read16(irq) & 0x80) != 0);
        assert((child.read16(irq) & 0x80) == 0);
        parent.write16(irq, 0x80);

        parent.write8(control, 0x80);
        child.write16(rcnt, 0x8000);
        assert((parent.read16(control) & 0xC8) == 0x40);
        assert(parent.runLinkedFrame().success);
        assert((parent.read16(irq) & 0x80) == 0);

        child.write16(rcnt, 0);
        parent.write8(control, 0x80);
        child.reset();
        assert((parent.read16(control) & 0xC8) == 0x40);
        child.write16(control, 0x2000);
        parent.write8(control, 0x80);
        assert(parent.runLinkedFrame().success);
        assert((parent.read16(control) & 0xC8) == 8);
        parent.write8(control, 0x80); // Destruction cancels an in-flight round.
    }
    assert((parent.read16(control) & 0xC8) == 0x40);
    assert(parent.runFrame().success);
}

void runMultiplayerTimingCheck() {
    using LOTUSGBA::GbaSystem;
    constexpr uint32_t expectedCycles[] = {62915, 15729, 10486, 5243};
    for (unsigned baud = 0; baud < 4; ++baud) {
        std::vector<uint8_t> rom(0xC0, 0);
        rom[0] = 0x2E;
        rom[3] = 0xEA; // ARM cartridge entry.
        const uint32_t code[] = {
            0xE59F0038, 0xE59F1030, 0xE1C010B0, // Select multiplayer.
            0xE59F2030, 0xE3A03502, 0xE5823000, // Run Timer 3 at CPU clock.
            0xE3811080, 0xE1C010B0, // Start the round.
            0xE1D010B0, 0xE3110080, 0x1AFFFFFC, // Poll SIOCNT busy.
            0xE1D230B0, 0xE3A00402, 0xE1C030B0, // Store Timer 3 in EWRAM.
            0xEAFFFFFE, 0x6000 | baud, 0x04000128, 0x0400010C,
        };
        for (uint32_t word : code)
            for (unsigned byte = 0; byte < 4; ++byte)
                rom.push_back(static_cast<uint8_t>(word >> (8 * byte)));
        GbaSystem parent(std::move(rom));
        GbaSystem child(makeGbaMode3BootRom());
        parent.connectLinkChild(child);
        child.write16(0x04000128, 0x2000);
        assert(parent.runLinkedFrame().success);
        const uint16_t measured = parent.read16(0x02000000);
        // Includes the instructions before START and after the last busy read.
        assert(measured >= expectedCycles[baud] && measured < expectedCycles[baud] + 64);
    }
}

void runRemoteMultiplayerLinkCheck() {
    using LOTUSGBA::GbaSystem;
    constexpr uint32_t multi0 = 0x04000120;
    constexpr uint32_t control = 0x04000128;
    constexpr uint32_t send = 0x0400012A;
    constexpr uint32_t irq = 0x04000202;
    GbaSystem parent(makeGbaMode3BootRom());
    GbaSystem child(makeGbaMode3BootRom());
    parent.configureRemoteMultiplayerLink(
        false, true,
        [&child](uint32_t sequence, uint16_t word, uint8_t baudIndex, uint64_t) {
            assert(baudIndex == 3);
            assert(child.receiveRemoteMultiplayerStart(sequence, word,
                                                       baudIndex));
        },
        {},
        [&child](bool ready) { child.setRemoteMultiplayerReady(ready); });
    child.configureRemoteMultiplayerLink(
        true, true, {},
        [&parent](uint32_t sequence, uint16_t word) {
            assert(parent.receiveRemoteMultiplayerWord(sequence, word));
        },
        [&parent](bool ready) { parent.setRemoteMultiplayerReady(ready); });

    parent.write16(control, 0x2000);
    child.write16(control, 0x2001);
    assert((parent.read16(control) & 0x08) != 0);
    assert((child.read16(control) & 0x08) != 0);
    assert((parent.read16(control) & 0x10) == 0);
    assert((child.read16(control) & 0x10) != 0);
    // Emerald's CheckMasterOrSlave tests SI/SD in SIOCNT and requires ID 0
    // with SD high to recognize the master.
    constexpr uint16_t sioMultiSiSd = 0x000C;
    constexpr uint16_t sioMultiSd = 0x0008;
    constexpr uint16_t sioMultiId = 0x0030;
    const auto isEmeraldMaster = [](uint16_t status) {
        return (status & sioMultiSiSd) == sioMultiSd &&
            (status & sioMultiId) == 0;
    };
    assert(isEmeraldMaster(parent.read16(control)));
    assert(!isEmeraldMaster(child.read16(control)));
    assert((child.read16(control) & 0x0004) != 0); // SI identifies the slave.

    parent.write16(send, 0x1234);
    child.write16(send, 0xABCD);
    parent.write16(control, 0x6083);
    assert((parent.read16(control) & 0x80) != 0);
    assert((child.read16(control) & 0x80) == 0);
    assert(parent.runFrame().success);
    assert(child.runFrame().success);
    assert(parent.read32(multi0) == 0xABCD1234);
    assert(child.read32(multi0) == 0xABCD1234);
    assert((parent.read16(control) & 0x80) == 0);
    assert((child.read16(control) & 0x80) == 0);
    assert((parent.read16(irq) & 0x80) != 0);
    assert((child.read16(irq) & 0x80) == 0);

    // The Pokémon link protocol repeatedly elects and checks the same parent.
    // Keep the simulated role/ready lines stable after one completed round,
    // then prove the next SIO start can complete on independently advanced
    // endpoint clocks too.
    const auto assertRolesReadyAndIdle = [&] {
        assert((parent.read16(control) & 0x3C) == 0x08);
        assert((child.read16(control) & 0x3C) == 0x1C);
        assert((parent.read16(0x04000134) & 0x0F) == 0x0B);
        assert((child.read16(0x04000134) & 0x0F) == 0x0F);
    };
    assertRolesReadyAndIdle();
    parent.write16(send, 0x5678);
    child.write16(send, 0xDCBA);
    parent.write16(control, 0x6083);
    assert(parent.runFrame().success);
    assert(child.runFrame().success);
    assert(parent.read32(multi0) == 0xDCBA5678);
    assert(child.read32(multi0) == 0xDCBA5678);
    assert((parent.read16(control) & 0x80) == 0);
    assert((child.read16(control) & 0x80) == 0);
    assertRolesReadyAndIdle();

    assert(!child.receiveRemoteMultiplayerStart(4, 0xFFFF, 3));
    assert((child.read16(control) & 0x40) != 0);
}

void runRemoteBufferedEmeraldLinkCheck() {
    using LOTUSGBA::GbaSystem;
    constexpr uint32_t control = 0x04000128;
    constexpr uint32_t send = 0x0400012A;
    constexpr uint32_t irqEnable = 0x04000200;
    constexpr uint32_t irqFlags = 0x04000202;

    GbaSystem parent(makeGbaMode3BootRom());
    GbaSystem child(makeGbaMode3BootRom());
    std::vector<GbaSystem::MultiplayerDataWords> parentBlocks;
    std::vector<GbaSystem::MultiplayerDataWords> childBlocks;
    std::vector<uint16_t> parentChecksums;
    std::vector<uint16_t> childChecksums;

    parent.configureRemoteMultiplayerLink(
        false, true,
        [&child](uint32_t sequence, uint16_t word, uint8_t baud,
                 uint64_t elapsed) {
            assert(child.receiveRemoteMultiplayerStart(sequence, word, baud,
                                                        elapsed));
        },
        {},
        [&child](bool ready) { child.setRemoteMultiplayerReady(ready); },
        [&child](uint64_t frame) {
            assert(child.receiveRemoteMultiplayerFrame(frame));
        }, {},
        [&child, &parentBlocks, &parentChecksums](
            uint32_t sequence, const GbaSystem::MultiplayerDataWords &words,
            uint16_t checksum) {
            parentBlocks.push_back(words);
            parentChecksums.push_back(checksum);
            assert(child.receiveRemoteMultiplayerDataBlock(sequence, words,
                                                           checksum));
            return true;
        });
    child.configureRemoteMultiplayerLink(
        true, true, {},
        [&parent](uint32_t sequence, uint16_t word) {
            assert(parent.receiveRemoteMultiplayerWord(sequence, word));
        },
        [&parent](bool ready) { parent.setRemoteMultiplayerReady(ready); },
        [&parent](uint64_t frame) {
            assert(parent.receiveRemoteMultiplayerFrame(frame));
        }, {},
        [&parent, &childBlocks, &childChecksums](
            uint32_t sequence, const GbaSystem::MultiplayerDataWords &words,
            uint16_t checksum) {
            childBlocks.push_back(words);
            childChecksums.push_back(checksum);
            assert(parent.receiveRemoteMultiplayerDataBlock(sequence, words,
                                                           checksum));
            return true;
        });
    parent.setRemoteMultiplayerBufferedProtocol(true);
    child.setRemoteMultiplayerBufferedProtocol(true);
    parent.setRemoteMultiplayerEventPump([] {});
    child.setRemoteMultiplayerEventPump([] {});

    parent.write16(control, 0x2000);
    child.write16(control, 0x6001);
    parent.write16(send, 0x8FFF);
    child.write16(send, 0xB9A0);
    parent.write16(irqEnable, 0x0080);
    child.write16(irqEnable, 0x0080);
    parent.write16(control, 0x6083);
    assert(parent.runFrame().success); // Stops at the raw handshake reply.
    assert(child.runFrame().success);  // Completes raw handshake and switches.
    assert(parent.runFrame().success); // Consumes the handshake reply/switches.
    assert(parent.remoteMultiplayerBufferedProtocolActive());
    assert(child.remoteMultiplayerBufferedProtocolActive());

    parent.write16(send, 0);
    child.write16(send, 0);
    const uint64_t childTransfersBefore = child.remoteSioTransferCount();
    for (unsigned phase = 0; phase < 9; ++phase) {
        // This fixture has no serial callback. Publish each next word just as
        // the guest's DoSend would, after consuming the previous result.
        child.write16(send, 0);
        const auto before = child.remoteSioTransferCount();
        for (unsigned frame = 0; frame < 3 && child.remoteSioTransferCount() == before; ++frame)
            assert(child.runFrame().success);
        assert(child.remoteSioTransferCount() == before + 1);
    }
    assert(child.remoteSioTransferCount() == childTransfersBefore + 9);
    assert((child.read16(irqFlags) & 0x0080) != 0);
    assert((child.read16(control) & 0x40) == 0);
    assert(childBlocks.empty()); // Local idle IRQ groups are not network messages.

    const GbaSystem::MultiplayerDataWords outgoing =
        {1, 2, 3, 4, 5, 6, 7, 8};
    const uint16_t expectedChecksum = 36;
    const std::array<uint16_t, 9> transferWords =
        {0, 1, 2, 3, 4, 5, 6, 7, 8};
    const uint64_t childFrameCountBeforeParentStream = child.remoteFrameCount();
    const uint64_t parentFrameCountBeforeStream = parent.remoteFrameCount();
    for (uint16_t word : transferWords) {
        parent.write16(send, word);
        parent.write16(control, 0x6083);
        const auto parentFrame = parent.runFrame();
        assert(parentFrame.success);
        assert(parentFrame.cycles >= GbaSystem::CYCLES_PER_FRAME);
        assert((parent.read16(control) & 0x80) == 0);
    }
    assert(child.remoteFrameCount() == childFrameCountBeforeParentStream);
    assert(parent.remoteFrameCount() >= parentFrameCountBeforeStream + 9);
    assert(!parentBlocks.empty());
    assert(parentBlocks.back() == outgoing);
    assert(parentChecksums.back() == expectedChecksum);
    assert((parent.read16(control) & 0x40) == 0);
    assert((child.read16(control) & 0x40) == 0);

    GbaSystem invalid(makeGbaMode3BootRom());
    invalid.configureRemoteMultiplayerLink(false, true, {}, {}, {}, {}, {});
    invalid.setRemoteMultiplayerBufferedProtocol(true);
    invalid.write16(control, 0x2000);
    GbaSystem::MultiplayerDataWords corrupt = outgoing;
    assert(!invalid.receiveRemoteMultiplayerDataBlock(1, corrupt, 35));
    assert((invalid.read16(control) & 0x40) != 0);
}

void runRemoteBufferedPredictedHandshakeCheck() {
    using LOTUSGBA::GbaSystem;
    constexpr uint32_t control = 0x04000128;
    constexpr uint32_t send = 0x0400012A;
    constexpr uint32_t multi0 = 0x04000120;

    GbaSystem parent(makeGbaMode3BootRom());
    GbaSystem child(makeGbaMode3BootRom());
    uint32_t parentSequence = 0;
    parent.configureRemoteMultiplayerLink(
        false, true,
        [&child, &parentSequence](uint32_t sequence, uint16_t word,
                                  uint8_t baud, uint64_t elapsed) {
            parentSequence = sequence;
            assert(child.receiveRemoteMultiplayerStart(sequence, word, baud,
                                                        elapsed));
        },
        {}, {}, {},
        [](uint32_t, uint16_t localWord) -> std::optional<uint16_t> {
            if (localWord == 0x8FFF)
                return 0xB9A0;
            return std::nullopt;
        });
    child.configureRemoteMultiplayerLink(
        true, true, {},
        [&parent](uint32_t sequence, uint16_t word) {
            assert(parent.receiveRemoteMultiplayerWord(sequence, word));
        }, {}, {});
    parent.setRemoteMultiplayerBufferedProtocol(true);
    child.setRemoteMultiplayerBufferedProtocol(true);
    parent.setRemoteMultiplayerReady(true);
    child.setRemoteMultiplayerReady(true);
    parent.setRemoteMultiplayerEventPump([] {});
    child.setRemoteMultiplayerEventPump([] {});

    parent.write16(control, 0x2000);
    child.write16(control, 0x6001);
    parent.write16(send, 0x8FFF);
    child.write16(send, 0xB9A0);
    parent.write16(control, 0x6083);

    // The parent finishes its local serial transaction before the child has
    // run the queued transfer or supplied its real word.
    assert(parent.runFrame().success);
    assert(parentSequence == 1);
    assert((parent.read16(control) & 0x80) == 0);
    assert(parent.read32(multi0) == 0xB9A08FFF);
    assert(parent.remoteMultiplayerBufferedProtocolActive());

    const GbaSystem::MultiplayerDataWords earlyBlock =
        {0x1234, 2, 3, 4, 5, 6, 7, 8};
    assert(child.receiveRemoteMultiplayerDataBlock(
        1, earlyBlock, 0x1257));

    // The child's real response arrives later, validates the prediction, and
    // activates the same buffered phase on the child. A block sent just after
    // the predicted handoff remains queued until that local transition.
    assert(child.runFrame().success);
    assert(child.remoteMultiplayerBufferedProtocolActive());
    assert(parent.remoteMultiplayerBufferedProtocolActive());
    assert((parent.read16(control) & 0x40) == 0);
    assert((child.read16(control) & 0x40) == 0);

    const uint64_t beforePrefetchedGroup = child.remoteSioTransferCount();
    child.write16(send, 0);
    for (unsigned frame = 0; frame < 3 &&
         child.remoteSioTransferCount() == beforePrefetchedGroup; ++frame)
        assert(child.runFrame().success);
    assert(child.remoteSioTransferCount() == beforePrefetchedGroup + 1);
    assert(child.read16(multi0) == 0); // Initial guest checksum phase.

    child.write16(send, 0);
    for (unsigned frame = 0; frame < 3 &&
         child.remoteSioTransferCount() == beforePrefetchedGroup + 1; ++frame)
        assert(child.runFrame().success);
    assert(child.remoteSioTransferCount() == beforePrefetchedGroup + 2);
    assert(child.read16(multi0) == earlyBlock[0]);
}

void runLocalMultiplayerProbeRomCheck() {
    using LOTUSGBA::GbaSystem;
    constexpr size_t saveSize = 32 * 1024;
    const auto parentSave = std::make_shared<std::vector<uint8_t>>(saveSize, 0);
    const auto childSave = std::make_shared<std::vector<uint8_t>>(saveSize, 0);
    GbaSystem parent(makeMultiplayerProbe(0x001F, 1, 0x1234, 0x6003), {},
                     parentSave);
    GbaSystem child(makeMultiplayerProbe(0x7C00, 2, 0xABCD, 0x6003), {},
                    childSave);
    parent.connectLinkChild(child);

    parent.setPressedButtons(1);
    child.setPressedButtons(2);
    for (unsigned frame = 0; frame < 120; ++frame) {
        if (frame != 0) {
            parent.setPressedButtons(0);
            child.setPressedButtons(0);
        }
        if (!parent.runLinkedFrame().success)
            throw std::runtime_error(parent.fault().empty() ? child.fault()
                                                            : parent.fault());
    }

    // The homebrew probe waits for each local controller, performs two
    // multiplayer transfers, and writes both received words plus its role to
    // SRAM. This exercises guest SIO writes, completion, and IRQ handling.
    const uint8_t parentResult[] = {0x34, 0x12, 0xCD, 0xAB, 0x0B};
    const uint8_t childResult[] = {0x34, 0x12, 0xCD, 0xAB, 0x1F};
    assert(std::equal(std::begin(parentResult), std::end(parentResult),
                      parentSave->begin()));
    assert(std::equal(std::begin(childResult), std::end(childResult),
                      childSave->begin()));
    assert((parent.read16(0x04000128) & 0x3C) == 0x08);
    assert((child.read16(0x04000128) & 0x3C) == 0x1C);
}

void runRemoteMultiplayerProbeRomLatencyCheck() {
    using LOTUSGBA::GbaSystem;
    constexpr uint32_t control = 0x04000128;
    constexpr size_t saveSize = 32 * 1024;
    constexpr uint8_t parentResult[] = {0x34, 0x12, 0xCD, 0xAB, 0x0B};
    constexpr uint8_t childResult[] = {0x34, 0x12, 0xCD, 0xAB, 0x1F};

    struct Event {
        enum class Type { Ready, Start, Word, Frame } type;
        unsigned dueFrame;
        bool ready = false;
        bool toParent = false;
        uint32_t sequence = 0;
        uint16_t word = 0;
        uint8_t baud = 0;
        uint64_t frame = 0;
        uint64_t startCycle = 0;
    };

    // Three frames each way is about 100 ms RTT; six is about 200 ms RTT.
    for (const unsigned oneWayDelayFrames : {0u, 3u, 6u}) {
        const auto parentSave =
            std::make_shared<std::vector<uint8_t>>(saveSize, 0);
        const auto childSave =
            std::make_shared<std::vector<uint8_t>>(saveSize, 0);
        GbaSystem parent(
            makeMultiplayerProbe(0x001F, 1, 0x1234, 0x6003), {}, parentSave);
        GbaSystem child(
            makeMultiplayerProbe(0x7C00, 2, 0xABCD, 0x6003), {}, childSave);

        unsigned frame = 0;
        unsigned starts = 0;
        unsigned replies = 0;
        std::deque<Event> packets;
        const auto queuePacket = [&](Event event) {
            event.dueFrame = frame + oneWayDelayFrames;
            packets.push_back(event);
        };
        parent.configureRemoteMultiplayerLink(
            false, true,
            [&](uint32_t sequence, uint16_t word, uint8_t baud,
                uint64_t elapsedCycles) {
                ++starts;
                Event event{Event::Type::Start, 0, false, false,
                            sequence, word, baud};
                event.startCycle = elapsedCycles;
                queuePacket(event);
            }, {},
            [&](bool ready) {
                Event event{Event::Type::Ready, 0, ready, false};
                event.frame = parent.remoteFrameCount();
                queuePacket(event);
            },
            [&](uint64_t progress) {
                Event event{Event::Type::Frame, 0, false, false};
                event.frame = progress;
                queuePacket(event);
            });
        child.configureRemoteMultiplayerLink(
            true, true, {},
            [&](uint32_t sequence, uint16_t word) {
                ++replies;
                queuePacket({Event::Type::Word, 0, false, false,
                             sequence, word});
            },
            [&](bool ready) {
                Event event{Event::Type::Ready, 0, ready, true};
                event.frame = child.remoteFrameCount();
                queuePacket(event);
            },
            [&](uint64_t progress) {
                Event event{Event::Type::Frame, 0, false, true};
                event.frame = progress;
                queuePacket(event);
            });

        const auto deliver = [&](bool toChild) {
            for (auto packet = packets.begin(); packet != packets.end();) {
                const bool packetToChild =
                    packet->type == Event::Type::Start ||
                    ((packet->type == Event::Type::Ready ||
                      packet->type == Event::Type::Frame) &&
                     !packet->toParent);
                if (packet->dueFrame > frame || packetToChild != toChild) {
                    ++packet;
                    continue;
                }
                const Event event = *packet;
                packet = packets.erase(packet);
                switch (event.type) {
                case Event::Type::Ready:
                    (event.toParent ? parent : child)
                        .setRemoteMultiplayerReady(event.ready, event.frame);
                    break;
                case Event::Type::Start:
                    assert(child.receiveRemoteMultiplayerStart(
                        event.sequence, event.word, event.baud,
                        event.startCycle));
                    break;
                case Event::Type::Word:
                    assert(parent.receiveRemoteMultiplayerWord(
                        event.sequence, event.word));
                    break;
                case Event::Type::Frame:
                    assert((toChild ? child : parent)
                               .receiveRemoteMultiplayerFrame(event.frame));
                    break;
                }
            }
        };

        bool completed = false;
        for (frame = 1; frame <= 1200 && !completed; ++frame) {
            parent.setPressedButtons(frame == 1 ? 1 : 0);
            child.setPressedButtons(frame == 1 ? 2 : 0);
            assert(parent.runFrame().success);
            deliver(true);
            assert(child.runFrame().success);
            deliver(false);
            completed = starts == 2 && replies == 2 &&
                std::equal(std::begin(parentResult), std::end(parentResult),
                           parentSave->begin()) &&
                std::equal(std::begin(childResult), std::end(childResult),
                           childSave->begin());
        }

        if (!completed) {
            std::cerr << "remote latency probe stalled: delay="
                      << oneWayDelayFrames << " frame=" << frame
                      << " starts=" << starts << " replies=" << replies
                      << " parentFrames=" << parent.remoteFrameCount()
                      << " childFrames=" << child.remoteFrameCount()
                      << " parentCycles=" << parent.emulatedCycles()
                      << " childCycles=" << child.emulatedCycles()
                      << " queued=" << packets.size() << '\n';
        }
        assert(completed);
        assert(starts == 2 && replies == 2);
        assert((parent.read16(control) & 0x40) == 0);
        assert((child.read16(control) & 0x40) == 0);
    }
}

void runRemoteMultiplayerLatencyCheck() {
    using LOTUSGBA::GbaSystem;
    constexpr uint32_t multi0 = 0x04000120;
    constexpr uint32_t control = 0x04000128;
    constexpr uint32_t send = 0x0400012A;
    for (const unsigned oneWayDelayFrames : {6u, 12u}) {
        GbaSystem parent(makeGbaMode3BootRom());
        GbaSystem child(makeGbaMode3BootRom());
        uint32_t starts = 0;
        uint32_t words = 0;
        uint32_t parentSequence = 0;
        uint16_t parentWord = 0;
        uint16_t childWord = 0;
        uint8_t baudIndex = 0;
        uint64_t parentStartCycle = 0;
        parent.configureRemoteMultiplayerLink(
            false, true,
            [&](uint32_t sequence, uint16_t word, uint8_t baud,
                uint64_t elapsedCycles) {
                ++starts;
                parentSequence = sequence;
                parentWord = word;
                baudIndex = baud;
                parentStartCycle = elapsedCycles;
            },
            {},
            [&child](bool ready) { child.setRemoteMultiplayerReady(ready); });
        child.configureRemoteMultiplayerLink(
            true, true, {},
            [&](uint32_t sequence, uint16_t word) {
                ++words;
                assert(sequence == words);
                childWord = word;
            },
            [&parent](bool ready) { parent.setRemoteMultiplayerReady(ready); });

        parent.write16(control, 0x2000);
        child.write16(control, 0x2001);
        parent.write16(send, 0x1234);
        child.write16(send, 0xABCD);
        parent.write16(control, 0x6083);
        assert(starts == 1 && parentSequence == 1 && parentWord == 0x1234);
        assert(baudIndex == 3);

        // The parent stops at the end of the local SIO duration while the
        // transfer packet is in transit. Let the child's independent clock
        // advance several frames before delivering the start event.
        for (unsigned frame = 0; frame < oneWayDelayFrames; ++frame)
            assert(parent.runFrame().success);
        assert((parent.read16(control) & 0x80) != 0);
        const uint64_t childBeforeStart = child.emulatedCycles();
        for (unsigned frame = 0; frame < oneWayDelayFrames; ++frame)
            assert(child.runFrame().success);
        assert(child.emulatedCycles() > childBeforeStart);
        assert(child.receiveRemoteMultiplayerStart(parentSequence, parentWord,
                                                  baudIndex, parentStartCycle));
        assert(words == 0);
        assert(child.runFrame().success);
        assert(words == 1 && childWord == 0xABCD);
        assert((child.read16(control) & 0x80) == 0);

        // The parent has reached its transfer boundary and waits for the
        // child's reply; receiving it lets the same frame continue.
        assert((parent.read16(control) & 0x80) != 0);
        assert(parent.receiveRemoteMultiplayerWord(parentSequence, childWord));
        assert((parent.read16(control) & 0x80) == 0);
        assert(parent.read32(multi0) == 0xABCD1234);
        assert(child.read32(multi0) == 0xABCD1234);
        assert(child.emulatedCycles() > parent.emulatedCycles());

    }
}

void runRemoteMultiplayerWaitCheck() {
    using LOTUSGBA::GbaSystem;
    constexpr uint32_t multi0 = 0x04000120;
    constexpr uint32_t control = 0x04000128;
    constexpr uint32_t send = 0x0400012A;
    constexpr uint32_t keyInput = 0x04000130;

    GbaSystem parent(makeGbaMode3BootRom());
    uint32_t sequence = 0;
    parent.configureRemoteMultiplayerLink(
        false, true,
        [&sequence](uint32_t nextSequence, uint16_t, uint8_t, uint64_t) {
            sequence = nextSequence;
        }, {}, {});
    parent.write16(control, 0x2000);
    parent.setRemoteMultiplayerReady(true);
    parent.write16(send, 0x1234);
    parent.write16(control, 0x6083);
    assert(sequence == 1);

    for (unsigned frame = 0; frame < 3; ++frame) {
        const auto pendingFrame = parent.runFrame();
        assert(pendingFrame.success &&
               pendingFrame.cycles >= GbaSystem::CYCLES_PER_FRAME);
    }
    const uint64_t stalledCycles = parent.emulatedCycles();
    const auto stalledFrame = parent.runFrame();
    assert(stalledFrame.success);
    assert(stalledFrame.cycles == 0);
    assert(parent.emulatedCycles() == stalledCycles);
    parent.setPressedButtons(1);
    assert((parent.read16(keyInput) & 1) == 0);
    parent.setPressedButtons(0);
    assert((parent.read16(keyInput) & 1) == 0);

    assert(parent.receiveRemoteMultiplayerWord(sequence, 0xABCD));
    assert(parent.receiveRemoteMultiplayerFrame(1));
    const auto resumedFrame = parent.runFrame();
    assert(resumedFrame.success);
    assert(resumedFrame.cycles >= GbaSystem::CYCLES_PER_FRAME);
    assert(parent.read32(multi0) == 0xABCD1234);
    assert((parent.read16(keyInput) & 1) == 1);
}

void runRemoteFramePacingCheck() {
    using LOTUSGBA::GbaSystem;
    constexpr uint32_t control = 0x04000128;
    constexpr uint32_t send = 0x0400012A;
    constexpr uint32_t multi0 = 0x04000120;
    GbaSystem parent(makeGbaMode3BootRom());
    GbaSystem child(makeGbaMode3BootRom());
    uint32_t sequence = 0;
    uint16_t parentWord = 0;
    uint8_t baud = 0;
    uint16_t childWord = 0;
    uint64_t parentStartCycle = 0;
    parent.configureRemoteMultiplayerLink(
        false, true,
        [&](uint32_t next, uint16_t word, uint8_t baudIndex,
            uint64_t elapsedCycles) {
            sequence = next;
            parentWord = word;
            baud = baudIndex;
            parentStartCycle = elapsedCycles;
        }, {},
        [&](bool ready) {
            child.setRemoteMultiplayerReady(ready, parent.remoteFrameCount());
        },
        [&](uint64_t frame) {
            assert(child.receiveRemoteMultiplayerFrame(frame));
        });
    child.configureRemoteMultiplayerLink(
        true, true, {},
        [&](uint32_t, uint16_t word) { childWord = word; },
        [&](bool ready) {
            parent.setRemoteMultiplayerReady(ready, child.remoteFrameCount());
        },
        [&](uint64_t frame) {
            assert(parent.receiveRemoteMultiplayerFrame(frame));
        });

    parent.write16(control, 0x2000);
    child.write16(control, 0x2001);
    for (unsigned frame = 0; frame < 6; ++frame)
        assert(parent.runFrame().cycles != 0);
    assert(parent.remoteFrameCount() == 6);
    assert(child.remoteFrameCount() == 0);

    // The remote master may be several frames ahead while no SIO transfer is
    // active. The virtual cable only paces the pair after link traffic starts.
    parent.write16(send, 0x1234);
    parent.write16(control, 0x6083);
    assert(sequence == 1 && parentWord == 0x1234 && baud == 3);
    for (unsigned frame = 0; frame < 3; ++frame) {
        const auto pendingFrame = parent.runFrame();
        assert(pendingFrame.success &&
               pendingFrame.cycles >= GbaSystem::CYCLES_PER_FRAME);
    }
    assert(parent.remoteFrameCount() == 9);
    const uint64_t parentCycles = parent.emulatedCycles();
    const auto pacedParent = parent.runFrame();
    assert(pacedParent.success && pacedParent.cycles == 0);
    assert(parent.emulatedCycles() == parentCycles);

    parent.setPressedButtons(1);
    assert((parent.read16(0x04000130) & 1) == 0);
    parent.setPressedButtons(0);
    assert((parent.read16(0x04000130) & 1) == 0);

    assert(child.receiveRemoteMultiplayerStart(sequence, parentWord, baud,
                                               parentStartCycle));
    child.write16(send, 0xABCD);
    for (unsigned frame = 0; frame < 8 && childWord == 0; ++frame)
        assert(child.runFrame().cycles != 0);
    assert(childWord == 0xABCD);
    assert(child.remoteFrameCount() >= 1);

    assert(parent.receiveRemoteMultiplayerWord(sequence, childWord));
    assert(parent.runFrame().success);
    assert(parent.remoteFrameCount() == 10);
    assert((parent.read16(0x04000130) & 1) == 1);
    // The child may have advanced its independent clock to the timestamped
    // start while the parent was waiting for the reply, so the parent is
    // allowed to resume here if it is now behind the bounded frame window.
    assert(parent.runFrame().success);
    assert(parent.read32(multi0) == 0xABCD1234);
    assert(child.read32(multi0) == 0xABCD1234);
    assert((parent.read16(control) & 0x40) == 0);
    assert((child.read16(control) & 0x40) == 0);
}

void runRemoteSioPredictionCheck() {
    using LOTUSGBA::GbaSystem;
    constexpr uint32_t multi0 = 0x04000120;
    constexpr uint32_t control = 0x04000128;
    constexpr uint32_t send = 0x0400012A;
    constexpr uint32_t irq = 0x04000202;

    GbaSystem system(makeGbaMode3BootRom());
    uint32_t sequence = 0;
    system.configureRemoteMultiplayerLink(
        false, true,
        [&](uint32_t next, uint16_t, uint8_t, uint64_t) {
            sequence = next;
        }, {}, {}, {},
        [](uint32_t, uint16_t localWord) -> std::optional<uint16_t> {
            if (localWord == 0xB9A0)
                return 0xB9A0;
            return std::nullopt;
        });
    system.write16(control, 0x2000);
    system.setRemoteMultiplayerReady(true, 0);
    assert(system.receiveRemoteMultiplayerFrame(0));
    system.write16(send, 0xB9A0);
    system.write16(control, 0x6083);
    assert(sequence == 1);
    assert(system.runFrame().success);
    assert((system.read16(control) & 0x80) == 0);
    assert(system.read32(multi0) == 0xB9A0B9A0);
    assert((system.read16(irq) & 0x80) != 0);
    for (unsigned frame = 0; frame < 4; ++frame) {
        const auto nextFrame = system.runFrame();
        assert(nextFrame.success && nextFrame.cycles != 0);
    }
    assert(system.remoteFrameCount() == 5);

    // The late network word is only discarded after it confirms the exact
    // predicted value. A different peer word is surfaced as an SIO error.
    assert(system.receiveRemoteMultiplayerWord(sequence, 0xB9A0));
    system.write16(send, 0xB9A0);
    system.write16(control, 0x6083);
    assert(system.runFrame().success);
    assert(!system.receiveRemoteMultiplayerWord(sequence, 0x2222));
    assert(system.remoteWordRejection().find("reason=prediction_mismatch") != std::string::npos);
    assert(system.remoteWordRejection().find("word=2222") != std::string::npos);
    assert((system.read16(control) & 0x40) != 0);
}

void runRemoteSioEventPumpCheck() {
    using LOTUSGBA::GbaSystem;
    constexpr uint32_t multi0 = 0x04000120;
    constexpr uint32_t control = 0x04000128;
    constexpr uint32_t send = 0x0400012A;

    GbaSystem system(makeGbaMode3BootRom());
    uint32_t sequence = 0;
    unsigned polls = 0;
    system.configureRemoteMultiplayerLink(
        false, true,
        [&sequence](uint32_t next, uint16_t, uint8_t, uint64_t) {
            sequence = next;
        }, {}, {}, {});
    system.setRemoteMultiplayerEventPump([&] {
        ++polls;
        assert((system.read16(control) & 0x80) != 0);
        assert(system.receiveRemoteMultiplayerWord(sequence, 0xABCD));
    });
    system.write16(control, 0x2000);
    system.setRemoteMultiplayerReady(true, 0);
    assert(system.receiveRemoteMultiplayerFrame(0));
    system.write16(send, 0x1234);
    system.write16(control, 0x6083);
    assert(sequence == 1);

    const auto completedFrame = system.runFrame();
    assert(completedFrame.success);
    assert(completedFrame.cycles >= GbaSystem::CYCLES_PER_FRAME);
    assert(polls == 1);
    assert(system.remoteFrameCount() == 1);
    assert((system.read16(control) & 0x80) == 0);
    assert(system.read32(multi0) == 0xABCD1234);

    GbaSystem waiting(makeGbaMode3BootRom());
    uint32_t waitingSequence = 0;
    unsigned waitingPolls = 0;
    waiting.configureRemoteMultiplayerLink(
        false, true,
        [&waitingSequence](uint32_t next, uint16_t, uint8_t, uint64_t) {
            waitingSequence = next;
        }, {}, {}, {});
    waiting.setRemoteMultiplayerEventPump([&] { ++waitingPolls; });
    waiting.write16(control, 0x2000);
    waiting.setRemoteMultiplayerReady(true, 0);
    assert(waiting.receiveRemoteMultiplayerFrame(0));
    waiting.write16(send, 0x1234);
    waiting.write16(control, 0x6083);

    const auto stalledFrame = waiting.runFrame();
    assert(stalledFrame.success);
    assert(stalledFrame.cycles > 0 &&
           stalledFrame.cycles < GbaSystem::CYCLES_PER_FRAME);
    assert(waitingPolls == 1);
    assert(waiting.remoteFrameCount() == 0);
    assert((waiting.read16(control) & 0x80) != 0);
    assert(waiting.receiveRemoteMultiplayerWord(waitingSequence, 0xABCD));
    assert(waiting.runFrame().success);
    assert(waiting.remoteFrameCount() == 1);
    assert((waiting.read16(control) & 0x80) == 0);
    assert(waiting.read32(multi0) == 0xABCD1234);
}

void runRemoteSioQueuedWordCheck() {
    using LOTUSGBA::GbaSystem;
    constexpr uint32_t multi0 = 0x04000120;
    constexpr uint32_t control = 0x04000128;
    constexpr uint32_t send = 0x0400012A;

    GbaSystem system(makeGbaMode3BootRom());
    uint32_t sequence = 0;
    unsigned polls = 0;
    system.configureRemoteMultiplayerLink(
        false, true,
        [&sequence](uint32_t next, uint16_t, uint8_t, uint64_t) {
            sequence = next;
        }, {}, {}, {});
    system.setRemoteMultiplayerEventPump([&] {
        ++polls;
        assert((system.read16(control) & 0x80) != 0);
        assert(system.receiveRemoteMultiplayerWord(sequence, 0xABCD));
        // A frontend packet poll may drain the next reliable reply before the
        // guest has started its matching SIO transfer.
        assert(system.receiveRemoteMultiplayerWord(sequence + 1, 0xDCBA));
    });
    system.write16(control, 0x2000);
    system.setRemoteMultiplayerReady(true, 0);
    assert(system.receiveRemoteMultiplayerFrame(0));

    system.write16(send, 0x1234);
    system.write16(control, 0x6083);
    assert(sequence == 1);
    assert(system.runFrame().success);
    assert(system.read32(multi0) == 0xABCD1234);
    assert((system.read16(control) & 0x80) == 0);

    system.write16(send, 0x5678);
    system.write16(control, 0x6083);
    assert(sequence == 2);
    assert(system.runFrame().success);
    assert(polls == 1);
    assert(system.read32(multi0) == 0xDCBA5678);
    assert((system.read16(control) & 0x40) == 0);
}

void runRemoteMultiplayerQueuedStartsCheck() {
    using LOTUSGBA::GbaSystem;
    constexpr uint32_t multi0 = 0x04000120;
    constexpr uint32_t control = 0x04000128;
    constexpr uint32_t send = 0x0400012A;
    constexpr uint64_t interTransferCycles = 18'528;

    GbaSystem child(makeGbaMode3BootRom());
    struct Reply {
        uint32_t sequence;
        uint16_t word;
        uint64_t cycle;
    };
    std::vector<Reply> replies;
    child.configureRemoteMultiplayerLink(
        true, true, {},
        [&](uint32_t sequence, uint16_t word) {
            replies.push_back({sequence, word, child.emulatedCycles()});
        }, {}, {});
    child.setRemoteMultiplayerReady(true, 0);
    child.write16(send, 0xABCD);
    child.write16(control, 0x2001);
    for (unsigned frame = 0; frame < 5; ++frame)
        assert(child.runFrame().success);

    // Both reliable start packets can arrive in one frontend poll. They must
    // remain ordered and keep their cycle spacing even if their original
    // timestamps are already behind the independently advancing client.
    assert(child.receiveRemoteMultiplayerStart(1, 0x1234, 3, 0));
    assert(child.receiveRemoteMultiplayerStart(2, 0x5678, 3,
                                                interTransferCycles));
    assert(child.runFrame().success);
    assert(replies.size() == 2);
    assert(replies[0].sequence == 1 && replies[0].word == 0xABCD);
    assert(replies[1].sequence == 2 && replies[1].word == 0xABCD);
    assert(replies[1].cycle > replies[0].cycle);
    const uint64_t observedGap = replies[1].cycle - replies[0].cycle;
    assert(observedGap >= interTransferCycles - 64 &&
           observedGap <= interTransferCycles + 64);
    assert(child.read32(multi0) == 0xABCD5678);
    assert((child.read16(control) & 0x40) == 0);
}

void runUnconnectedNormalSioCheck() {
    using LOTUSGBA::GbaSystem;
    constexpr uint32_t data = 0x04000120;
    constexpr uint32_t control = 0x04000128;
    constexpr uint32_t irq = 0x04000202;

    for (const uint16_t mode : {uint16_t{0x4081}, uint16_t{0x5081}}) {
        GbaSystem system(makeGbaMode3BootRom());
        system.write32(data, 0x12345678);
        system.write16(control, mode);
        assert((system.read16(control) & 0x80) != 0);
        assert(system.runFrame().success);
        assert((system.read16(control) & 0x80) == 0);
        if (mode & 0x1000)
            assert(system.read32(data) == 0xFFFFFFFF);
        else
            assert(system.read8(0x0400012A) == 0xFF);
        assert((system.read16(irq) & (1u << 7)) != 0);
    }

    GbaSystem externalClock(makeGbaMode3BootRom());
    externalClock.write32(data, 0x12345678);
    externalClock.write16(control, 0x5080);
    assert(externalClock.runFrame().success);
    assert((externalClock.read16(control) & 0x80) != 0);
    assert(externalClock.read32(data) == 0x12345678);
    assert((externalClock.read16(irq) & (1u << 7)) == 0);
    externalClock.write16(0x04000134, 0x8000);
    assert((externalClock.read16(control) & 0x80) == 0);
}

void runEmeraldRfuBootstrapCheck(char region) {
    using LOTUSGBA::GbaSystem;
    using LOTUSGBA::GbaRfuLogin;
    constexpr uint32_t serialData = 0x04000120;
    constexpr uint32_t serialControl = 0x04000128;
    constexpr uint32_t remoteControl = 0x04000134;
    constexpr uint32_t frameCycles = GbaSystem::CYCLES_PER_FRAME;

    auto emeraldRom = makeGbaMode3BootRom();
    emeraldRom.resize(0xC4, 0);
    emeraldRom[0xAC] = 'B';
    emeraldRom[0xAD] = 'P';
    emeraldRom[0xAE] = 'E';
    emeraldRom[0xAF] = static_cast<uint8_t>(region);
    GbaSystem emerald(std::move(emeraldRom));
    assert(emerald.hasEmeraldRfuProfile());

    emerald.write16(remoteControl, 0x8000);
    emerald.write16(remoteControl, 0x80A0);
    emerald.write16(remoteControl, 0x80A2); // RFU reset pulse.
    emerald.write16(remoteControl, 0x80A0);
    emerald.write16(serialControl, 0x5003);
    emerald.write16(remoteControl, 0);

    constexpr uint32_t guestWords[] = {
        0x7FFF494E, 0xFFFF494E, 0xB6B1494E, 0xB6B1544E,
        0xABB1544E, 0xABB14E45, 0xB1BA4E45, 0xB1BA4F44,
        0xB0BB4F44, 0xB0BB8001};
    constexpr uint32_t adapterWords[] = {
        0x00000000, 0x494EB6B1, 0x494EB6B1, 0x544EB6B1,
        0x544EABB1, 0x4E45ABB1, 0x4E45B1BA, 0x4F44B1BA,
        0x4F44B0BB, 0x8001B0BB};
    for (unsigned index = 0; index < GbaRfuLogin::TRANSFER_COUNT; ++index) {
        emerald.write32(serialData, guestWords[index]);
        emerald.write16(serialControl, 0x5083);
        const auto frame = emerald.runFrame();
        assert(frame.success && frame.cycles >= frameCycles);
        assert(emerald.read32(serialData) == adapterWords[index]);
    }
    assert(emerald.hasEmeraldRfuProfile());
    assert((emerald.read16(serialControl) & 0x04) != 0);

    emerald.write32(serialData,
                    LOTUSGBA::makeGbaRfuCommandHeader(0x10, 0));
    emerald.write16(serialControl, 0x5083);
    assert(emerald.runFrame().success);
    assert(emerald.read32(serialData) == 0x80000000);
    emerald.write32(serialData, 0x80000000);
    emerald.write16(serialControl, 0x5083);
    assert(emerald.runFrame().success);
    assert(emerald.read32(serialData) == 0x99660090);

    // SIOCNT bit 2 is the adapter's ready input and follows the guest's SO
    // output in the opposite phase between RFU word transfers.
    emerald.write16(serialControl, 0x500B);
    assert((emerald.read16(serialControl) & 0x04) == 0);

    GbaSystem otherGame(makeGbaMode3BootRom());
    assert(!otherGame.hasEmeraldRfuProfile());
    otherGame.write16(remoteControl, 0x80A2);
    otherGame.write16(remoteControl, 0);
    otherGame.write16(serialControl, 0x5003);
    otherGame.write32(serialData, guestWords[0]);
    otherGame.write16(serialControl, 0x5083);
    assert(otherGame.runFrame().success);
    assert(otherGame.read32(serialData) == 0xFFFFFFFF);
}

std::vector<uint8_t> makeJapaneseEmeraldProfileProbeRom() {
    auto rom = makeGbaMode3BootRom();
    // Preserve the complete test ROM's instruction loop and literal pool.
    rom[0xAC] = 'B';
    rom[0xAD] = 'P';
    rom[0xAE] = 'E';
    rom[0xAF] = 'J';
    return rom;
}

uint32_t runRfuTransfer(LOTUSGBA::GbaSystem &system, uint32_t word) {
    constexpr uint32_t SERIAL_DATA = 0x04000120;
    constexpr uint32_t SERIAL_CONTROL = 0x04000128;
    system.write32(SERIAL_DATA, word);
    system.write16(SERIAL_CONTROL, 0x5083);
    assert(system.runFrame().success);
    return system.read32(SERIAL_DATA);
}

void bootRfuAdapter(LOTUSGBA::GbaSystem &system) {
    constexpr uint32_t REMOTE_CONTROL = 0x04000134;
    constexpr uint32_t SERIAL_CONTROL = 0x04000128;
    system.write16(REMOTE_CONTROL, 0x8000);
    system.write16(REMOTE_CONTROL, 0x80A0);
    system.write16(REMOTE_CONTROL, 0x80A2);
    system.write16(REMOTE_CONTROL, 0x80A0);
    system.write16(SERIAL_CONTROL, 0x5003);
    system.write16(REMOTE_CONTROL, 0);
    constexpr uint32_t guestWords[] = {
        0x7FFF494E, 0xFFFF494E, 0xB6B1494E, 0xB6B1544E,
        0xABB1544E, 0xABB14E45, 0xB1BA4E45, 0xB1BA4F44,
        0xB0BB4F44, 0xB0BB8001};
    constexpr uint32_t adapterWords[] = {
        0x00000000, 0x494EB6B1, 0x494EB6B1, 0x544EB6B1,
        0x544EABB1, 0x4E45ABB1, 0x4E45B1BA, 0x4F44B1BA,
        0x4F44B0BB, 0x8001B0BB};
    for (size_t index = 0; index < std::size(guestWords); ++index)
        assert(runRfuTransfer(system, guestWords[index]) ==
               adapterWords[index]);
}

struct RfuCommandResult {
    uint32_t acknowledgement = 0;
    std::vector<uint32_t> response;
};

RfuCommandResult runRfuCommand(LOTUSGBA::GbaSystem &system,
                               uint8_t command,
                               const std::vector<uint32_t> &parameters = {}) {
    assert(parameters.size() <= LOTUSGBA::GbaRfuRequestDecoder::MAX_PAYLOAD_WORDS);
    assert(runRfuTransfer(system,
        LOTUSGBA::makeGbaRfuCommandHeader(
            command, static_cast<uint8_t>(parameters.size()))) == 0x80000000);
    for (uint32_t parameter : parameters)
        assert(runRfuTransfer(system, parameter) == 0x80000000);

    RfuCommandResult result;
    result.acknowledgement = runRfuTransfer(system, 0x80000000);
    const uint8_t responseWords = static_cast<uint8_t>(
        (result.acknowledgement >> 8) & 0xFF);
    if ((result.acknowledgement & 0xFFFF0000) == 0x99660000 &&
        static_cast<uint8_t>(result.acknowledgement) != 0xEE) {
        for (uint8_t index = 0; index < responseWords; ++index)
            result.response.push_back(runRfuTransfer(system, 0x80000000));
    }
    return result;
}

std::vector<uint32_t> makeExpectedRfuReceivePacket(
    const std::vector<uint32_t> &frame, uint8_t payloadBytes = 1,
    bool receivingAsParent = true) {
    std::vector<uint32_t> packet = {receivingAsParent
        ? static_cast<uint32_t>(payloadBytes) << 8 : payloadBytes};
    packet.insert(packet.end(), frame.begin(), frame.end());
    return packet;
}

void runEmeraldRfuAdapterClockAckCheck() {
    using LOTUSGBA::GbaSystem;
    constexpr uint32_t data = 0x04000120;
    constexpr uint32_t control = 0x04000128;
    constexpr uint32_t irq = 0x04000202;
    constexpr uint16_t serialIrq = 1u << 7;

    // Exercise the public SIO registers, including the guest's ready-line
    // handshake. A completed external transfer must consume an ACK already
    // present in that word, without requiring another dummy transfer.
    for (bool sameTransferAck : {true, false}) {
        GbaSystem system(makeJapaneseEmeraldProfileProbeRom());
        bootRfuAdapter(system);
        for (unsigned round = 0; round < 2; ++round) {
            assert(runRfuCommand(system, 0x27).acknowledgement == 0x996600A7);
            system.write16(control, 0x500B);
            assert((system.read16(control) & 4) == 0);
            system.write16(control, 0x5002);
            assert((system.read16(control) & 4) == 0);
            system.write16(irq, serialIrq);
            system.write32(data, sameTransferAck ? 0x996600A8 : 0x80000000);
            system.write16(control, 0x5082);
            assert((system.read16(control) & 0x80) != 0);
            assert(system.runFrame().success);
            assert((system.read16(control) & 0x80) == 0);
            assert((system.read16(irq) & serialIrq) != 0);
            assert(system.read32(data) == 0x99660028);
            assert((system.read16(control) & 4) == 0);
            system.write16(control, 0x500A);
            assert((system.read16(control) & 4) != 0);
            if (!sameTransferAck) {
                // Public RFU traces normally use a second reverse-clocked
                // word for A8, while the adapter shifts the idle word back.
                // A mismatched ACK must not hand clock ownership back.
                system.write16(control, 0x5002);
                system.write32(data, 0x996600A7);
                system.write16(irq, serialIrq);
                system.write16(control, 0x5082);
                assert(system.runFrame().success);
                assert(system.read32(data) == 0xFFFFFFFF);
                assert((system.read16(control) & 0x80) == 0);
                assert((system.read16(irq) & serialIrq) != 0);
                system.write16(control, 0x500A);
                assert((system.read16(control) & 4) != 0);
                system.write16(control, 0x5002);
                system.write32(data, 0x996600A8);
                system.write16(irq, serialIrq);
                system.write16(control, 0x5082);
                assert(system.runFrame().success);
                assert(system.read32(data) == 0x80000000);
                assert((system.read16(control) & 0x80) == 0);
                assert((system.read16(irq) & serialIrq) != 0);
                assert((system.read16(control) & 4) == 0);
                system.write16(control, 0x500A);
                assert((system.read16(control) & 4) != 0);
            }

            // After A8 there is no adapter clock for another external word.
            // It must stay busy and must not raise a serial IRQ.
            system.write16(control, 0x5002);
            system.write16(irq, serialIrq);
            system.write32(data, 0x11223344);
            system.write16(control, 0x5082);
            assert(system.runFrame().success);
            assert((system.read16(control) & 0x80) != 0);
            assert(system.read32(data) == 0x11223344);
            assert((system.read16(irq) & serialIrq) == 0);
            system.write16(control, 0x5003); // Cancel and resume guest clock.
            assert((system.read16(control) & 4) != 0);
            assert(runRfuCommand(system, 0x26).acknowledgement == 0x996600A6);
            assert(runRfuCommand(system, 0x13).response ==
                   std::vector<uint32_t>{0});
        }
    }

    // A network loss while the adapter owns SIO must reach Emerald through
    // the documented 0x29 request body and A9 ACK before guest clock resumes.
    GbaSystem disconnected(makeJapaneseEmeraldProfileProbeRom());
    bootRfuAdapter(disconnected);
    disconnected.configureRemoteRfuLink(
        1, 2,
        [](uint32_t, uint16_t, const GbaSystem::RfuAdvertisement &) {},
        [](uint32_t, uint16_t, uint16_t) {},
        [](uint32_t, uint8_t, const std::vector<uint32_t> &) { return true; });
    assert(runRfuCommand(disconnected, 0x19).acknowledgement == 0x99660099);
    assert(disconnected.receiveRemoteRfuConnectionRequest(1, 2, 1));
    assert(runRfuCommand(disconnected, 0x27).acknowledgement == 0x996600A7);
    disconnected.receiveRemoteRfuDisconnect();

    const auto adapterClockedTransfer = [&](uint32_t guestWord) {
        disconnected.write16(control, 0x5002);
        disconnected.write32(data, guestWord);
        disconnected.write16(control, 0x5082);
        assert(disconnected.runFrame().success);
        const uint32_t adapterWord = disconnected.read32(data);
        disconnected.write16(control, 0x500A);
        return adapterWord;
    };
    disconnected.write16(control, 0x500B);
    assert((disconnected.read16(control) & 4) == 0);
    disconnected.write16(control, 0x5002);
    assert(adapterClockedTransfer(0x80000000) == 0x99660129);
    assert(adapterClockedTransfer(0x80000000) == 1); // Slot 0, disconnected.
    assert(adapterClockedTransfer(0x996600A8) == 0xFFFFFFFF);
    assert((disconnected.read16(control) & 4) != 0);
    assert(adapterClockedTransfer(0x996600A9) == 0x80000000);
    assert((disconnected.read16(control) & 4) != 0);
    disconnected.write16(control, 0x5002);
    disconnected.write16(irq, serialIrq);
    disconnected.write32(data, 0x11223344);
    disconnected.write16(control, 0x5082);
    assert(disconnected.runFrame().success);
    assert((disconnected.read16(control) & 0x80) != 0);
    assert(disconnected.read32(data) == 0x11223344);
    assert((disconnected.read16(irq) & serialIrq) == 0);
    disconnected.write16(control, 0x5003);
}

void runEmeraldRfuRadioProgressCheck() {
    using LOTUSGBA::GbaSystem;
    GbaSystem system(makeJapaneseEmeraldProfileProbeRom());
    bootRfuAdapter(system);
    system.configureRemoteRfuLink(1, 2,
        [](uint32_t, uint16_t, const GbaSystem::RfuAdvertisement &) {},
        [](uint32_t, uint16_t, uint16_t) {},
        [](uint32_t, uint8_t, const std::vector<uint32_t> &) { return true; });
    assert(runRfuCommand(system, 0x19).acknowledgement == 0x99660099);
    assert(system.receiveRemoteRfuConnectionRequest(1, 2, 1));
    constexpr uint32_t data = 0x04000120, control = 0x04000128;
    constexpr uint32_t irq = 0x04000202;
    for (uint32_t sequence = 1; sequence <= 2; ++sequence) {
        // Starting a parent send is not the same as completing its radio
        // exchange: await a child reply while the guest CPU keeps progressing.
        assert(runRfuCommand(system, 0x26).response.empty());
        assert(runRfuCommand(system, 0x25, {1, 0xA0u + sequence})
                   .acknowledgement == 0x996600A5);
        system.write32(data, 0x80000000);
        system.write16(irq, 1u << 7);
        system.write16(control, 0x5082);
        assert(system.runFrame().success);
        assert((system.read16(control) & 0x80) != 0);
        assert((system.read16(irq) & (1u << 7)) == 0);
        assert(system.receiveRemoteRfuData(sequence, 1, {sequence}) ==
               GbaSystem::RfuDataReceiveResult::Accepted);
        assert(system.runFrame().success);
        assert((system.read16(control) & 0x80) == 0);
        assert((system.read16(irq) & (1u << 7)) != 0);
        assert(system.read32(data) == 0x99660028);
        system.write16(control, 0x500A);
        system.write32(data, 0x996600A8);
        system.write16(control, 0x5082);
        assert(system.runFrame().success);
        assert(system.read32(data) == 0x80000000);
        system.write16(control, 0x500A);
        system.write16(control, 0x5003);
        assert(runRfuCommand(system, 0x26).response ==
               makeExpectedRfuReceivePacket({sequence}));
    }
}

void runEmeraldRfuChildSnapshotCheck() {
    using LOTUSGBA::GbaSystem;
    GbaSystem child(makeJapaneseEmeraldProfileProbeRom());
    bootRfuAdapter(child);
    bool credit = false;
    std::vector<std::vector<uint32_t>> replies;
    std::vector<uint32_t> admissions;
    child.configureRemoteRfuLink(2, 1,
        [](uint32_t, uint16_t, const GbaSystem::RfuAdvertisement &) {},
        [&](uint32_t seq, uint16_t source, uint16_t) {
            assert(child.receiveRemoteRfuConnectionResult(seq, source, 0, true));
        }, [&](uint32_t seq, uint8_t count, const std::vector<uint32_t> &data) {
            assert(seq == replies.size() + 1);
            assert(count == data.size() * 4);
            if (!credit) return false;
            replies.push_back(data);
            return true;
        }, [&](uint32_t seq) { admissions.push_back(seq); });
    assert(child.receiveRemoteRfuAdvertisement(1, 1, {}));
    assert(runRfuCommand(child, 0x1C).acknowledgement == 0x9966009C);
    assert(runRfuCommand(child, 0x1F, {1}).acknowledgement == 0x9966009F);
    assert(runRfuCommand(child, 0x24, {4u << 8, 0x11111111}).acknowledgement == 0x996600A4);
    assert(replies.empty()); // No independent child radio transmission.
    assert(child.receiveRemoteRfuData(1, 1, {0}) == GbaSystem::RfuDataReceiveResult::Accepted);
    assert(admissions == std::vector<uint32_t>{1}); // Not waiting for ROM 0x26.
    assert(runRfuCommand(child, 0x24, {4u << 8, 0x22222222}).acknowledgement == 0x996600A4);
    assert(child.receiveRemoteRfuData(2, 1, {0}) == GbaSystem::RfuDataReceiveResult::QueueFull);
    assert(admissions.size() == 1); // Cannot falsely ACK an unbuffered exchange.
    credit = true;
    assert(child.runFrame().success);
    assert(replies == std::vector<std::vector<uint32_t>>{{0x11111111}});
    assert(child.receiveRemoteRfuData(2, 1, {0}) == GbaSystem::RfuDataReceiveResult::Accepted);
    assert(replies.size() == 2 && replies.back() == std::vector<uint32_t>{0x22222222});
    assert(child.receiveRemoteRfuData(3, 1, {0}) == GbaSystem::RfuDataReceiveResult::Accepted);
    assert(replies.size() == 3 && replies.back().empty()); // Consume scheduled TX once.
    assert(admissions == (std::vector<uint32_t>{1, 2, 3}));
    assert(runRfuCommand(child, 0x26).response == makeExpectedRfuReceivePacket({0}, 1, false));
    assert(admissions.size() == 3); // ROM consumption never emits another credit.
}

void runEmeraldRfuTransmitBackpressureCheck() {
    using LOTUSGBA::GbaSystem;
    GbaSystem system(makeJapaneseEmeraldProfileProbeRom());
    bootRfuAdapter(system);
    bool hasSendCredit = false;
    std::vector<std::vector<uint32_t>> snapshots;
    std::vector<uint32_t> admissions;
    system.configureRemoteRfuLink(1, 2,
        [](uint32_t, uint16_t, const GbaSystem::RfuAdvertisement &) {},
        [](uint32_t, uint16_t, uint16_t) {},
        [&](uint32_t sequence, uint8_t count, const std::vector<uint32_t> &data) {
            assert(sequence == snapshots.size() + 1);
            assert(count == 4 && data.size() == 1);
            if (!hasSendCredit) return false;
            snapshots.push_back(data);
            return true;
        }, [&](uint32_t sequence) { admissions.push_back(sequence); });
    assert(runRfuCommand(system, 0x19).acknowledgement == 0x99660099);
    assert(system.receiveRemoteRfuConnectionRequest(1, 2, 1));
    assert(runRfuCommand(system, 0x24, {4, 0x11111111}).acknowledgement == 0x996600A4);
    for (unsigned i = 0; i < 16; ++i) assert(system.runFrame().success);
    assert(snapshots.empty());
    // A not-yet-admitted parent register can be updated; it is not a FIFO.
    assert(runRfuCommand(system, 0x24, {4, 0x22222222}).acknowledgement == 0x996600A4);
    hasSendCredit = true;
    assert(system.runFrame().success);
    assert(snapshots == std::vector<std::vector<uint32_t>>{{0x22222222}});
    assert(system.runFrame().success);
    assert(snapshots.size() == 1);
    // The in-flight snapshot is immutable. Later writes wait for its reply.
    assert(runRfuCommand(system, 0x24, {4, 0x33333333}).acknowledgement == 0x996600A4);
    assert(system.runFrame().success);
    assert(snapshots.size() == 1);
    assert(system.receiveRemoteRfuData(1, 0, {}) == GbaSystem::RfuDataReceiveResult::Accepted);
    assert(admissions == std::vector<uint32_t>{1});
    assert(system.runFrame().success);
    assert(snapshots.size() == 2 && snapshots.back() == std::vector<uint32_t>{0x33333333});
    assert(system.receiveRemoteRfuData(2, 0, {}) == GbaSystem::RfuDataReceiveResult::Accepted);
    assert(runRfuCommand(system, 0x26).response.empty());
    assert(admissions == (std::vector<uint32_t>{1, 2}));

    // A5 is local, but its 0x28 must wait for the actual child response.
    hasSendCredit = false;
    assert(runRfuCommand(system, 0x25, {4, 0x44444444}).acknowledgement == 0x996600A5);
    constexpr uint32_t data = 0x04000120, control = 0x04000128;
    system.write16(control, 0x5002);
    system.write32(data, 0x80000000);
    system.write16(control, 0x5082);
    for (unsigned i = 0; i < 16; ++i) assert(system.runFrame().success);
    assert((system.read16(control) & 0x80) != 0);
    hasSendCredit = true;
    assert(system.runFrame().success);
    assert(snapshots.size() == 3);
    assert((system.read16(control) & 0x80) != 0); // Sent, not radio-complete.
    assert(system.receiveRemoteRfuData(3, 1, {0x5A}) == GbaSystem::RfuDataReceiveResult::Accepted);
    assert(admissions == (std::vector<uint32_t>{1, 2, 3})); // Before 0x26.
    assert(system.runFrame().success);
    assert(system.read32(data) == 0x99660028);
    system.write16(control, 0x500A);
    system.write16(control, 0x5002);
    system.write32(data, 0x996600A8);
    system.write16(control, 0x5082);
    assert(system.runFrame().success);
    assert(system.read32(data) == 0x80000000);
    system.write16(control, 0x500A);
    system.write16(control, 0x5003);
    assert(runRfuCommand(system, 0x26).response == makeExpectedRfuReceivePacket({0x5A}, 1, true));
    assert(admissions.size() == 3); // No second credit when ROM reads data.
    hasSendCredit = false;
    assert(runRfuCommand(system, 0x24, {4, 0x55555555}).acknowledgement == 0x996600A4);
    system.receiveRemoteRfuDisconnect();
    hasSendCredit = true;
    assert(system.runFrame().success);
    assert(snapshots.size() == 3); // No unsent data leaks across disconnect.
}

void runEmeraldRfuParentCandidateCheck() {
    using LOTUSGBA::GbaSystem;
    GbaSystem parent(makeJapaneseEmeraldProfileProbeRom());
    GbaSystem child(makeJapaneseEmeraldProfileProbeRom());
    bootRfuAdapter(parent);
    bootRfuAdapter(child);
    parent.configureRemoteRfuLink(
        1, 2,
        [&child](uint32_t sequence, uint16_t id,
                 const GbaSystem::RfuAdvertisement &data) {
            assert(child.receiveRemoteRfuAdvertisement(sequence, id, data));
        }, [](uint32_t, uint16_t, uint16_t) {},
        [](uint32_t, uint8_t, const std::vector<uint32_t> &) {
            return true;
        });
    child.configureRemoteRfuLink(
        2, 1,
        [&parent](uint32_t sequence, uint16_t id,
                  const GbaSystem::RfuAdvertisement &data) {
            assert(parent.receiveRemoteRfuAdvertisement(sequence, id, data));
        }, [](uint32_t, uint16_t, uint16_t) {},
        [](uint32_t, uint8_t, const std::vector<uint32_t> &) {
            return true;
        });

    std::array<uint8_t, 24> config{};
    config[0] = 0x78;
    config[1] = 0x56;
    constexpr char GAME_NAME[] = "EMERALD";
    constexpr char USER_NAME[] = "RIKU";
    for (size_t i = 0; i < sizeof(GAME_NAME) - 1; ++i)
        config[2 + i] = static_cast<uint8_t>(GAME_NAME[i]);
    for (size_t i = 0; i < sizeof(USER_NAME) - 1; ++i)
        config[16 + i] = static_cast<uint8_t>(USER_NAME[i]);
    uint8_t checksum = 0;
    for (size_t i = 0; i < 8; ++i)
        checksum = static_cast<uint8_t>(checksum + config[2 + i] +
                                        config[16 + i]);
    config[15] = static_cast<uint8_t>(~checksum);
    std::vector<uint32_t> advertisement(6);
    for (size_t i = 0; i < advertisement.size(); ++i)
        for (size_t byte = 0; byte < 4; ++byte)
            advertisement[i] |= static_cast<uint32_t>(config[i * 4 + byte])
                                << (byte * 8);

    assert(runRfuCommand(parent, 0x16, advertisement).acknowledgement ==
           0x99660096);
    assert(runRfuCommand(parent, 0x19).acknowledgement == 0x99660099);
    assert(runRfuCommand(child, 0x1C).acknowledgement == 0x9966009C);
    const auto found = runRfuCommand(child, 0x1D);
    assert(found.acknowledgement == 0x9966079D);
    assert(found.response.size() == 7);

    // Mirror the byte offsets used by Emerald's
    // rfu_STC_readParentCandidateList: the adapter's first word contains the
    // peer ID/slot, followed by the peer's 24-byte GameConfig payload.
    std::array<uint8_t, 28> candidate{};
    for (size_t i = 0; i < found.response.size(); ++i)
        for (size_t byte = 0; byte < 4; ++byte)
            candidate[i * 4 + byte] = static_cast<uint8_t>(
                found.response[i] >> (byte * 8));
    assert(candidate[0] == 1 && candidate[1] == 0);
    assert(candidate[2] == 0);
    assert(candidate[4] == 0x78 && candidate[5] == 0x56);
    checksum = 0;
    for (size_t i = 0; i < 8; ++i)
        checksum = static_cast<uint8_t>(checksum + candidate[6 + i] +
                                        candidate[20 + i]);
    assert(candidate[19] == static_cast<uint8_t>(~checksum));
    assert(std::string(candidate.begin() + 6, candidate.begin() + 13) ==
           "EMERALD");
    assert(std::string(candidate.begin() + 20, candidate.begin() + 24) ==
           "RIKU");
}

void runEmeraldRfuHostDrivenCycleCheck() {
    using LOTUSGBA::GbaSystem;
    GbaSystem parent(makeJapaneseEmeraldProfileProbeRom());
    GbaSystem child(makeJapaneseEmeraldProfileProbeRom());
    bootRfuAdapter(parent);
    bootRfuAdapter(child);

    parent.configureRemoteRfuLink(
        1, 2,
        [&child](uint32_t sequence, uint16_t id,
                 const GbaSystem::RfuAdvertisement &data) {
            assert(child.receiveRemoteRfuAdvertisement(sequence, id, data));
        },
        [&child, &parent](uint32_t sequence, uint16_t sourceId,
                          uint16_t targetId) {
            const bool accepted = child.receiveRemoteRfuConnectionRequest(
                sequence, sourceId, targetId);
            assert(parent.receiveRemoteRfuConnectionResult(
                sequence, sourceId, 0, accepted));
        },
        [&child](uint32_t sequence, uint8_t payloadBytes,
                 const std::vector<uint32_t> &words) {
            return child.receiveRemoteRfuData(sequence, payloadBytes, words) ==
                   GbaSystem::RfuDataReceiveResult::Accepted;
        });
    child.configureRemoteRfuLink(
        2, 1,
        [&parent](uint32_t sequence, uint16_t id,
                  const GbaSystem::RfuAdvertisement &data) {
            assert(parent.receiveRemoteRfuAdvertisement(sequence, id, data));
        },
        [&parent, &child](uint32_t sequence, uint16_t sourceId,
                          uint16_t targetId) {
            const bool accepted = parent.receiveRemoteRfuConnectionRequest(
                sequence, sourceId, targetId);
            assert(child.receiveRemoteRfuConnectionResult(
                sequence, sourceId, 0, accepted));
        },
        [&parent](uint32_t sequence, uint8_t payloadBytes,
                  const std::vector<uint32_t> &words) {
            return parent.receiveRemoteRfuData(sequence, payloadBytes, words) ==
                   GbaSystem::RfuDataReceiveResult::Accepted;
        });

    const std::vector<uint32_t> parentBroadcast(6, 0x11111111);
    const std::vector<uint32_t> childBroadcast(6, 0x22222222);
    assert(runRfuCommand(parent, 0x16, parentBroadcast).acknowledgement ==
           0x99660096);
    assert(runRfuCommand(child, 0x16, childBroadcast).acknowledgement ==
           0x99660096);
    assert(runRfuCommand(parent, 0x19).acknowledgement == 0x99660099);
    assert(runRfuCommand(child, 0x1C).acknowledgement == 0x9966009C);
    assert(runRfuCommand(child, 0x1F, {1}).acknowledgement == 0x9966009F);
    assert(runRfuCommand(child, 0x21).response == std::vector<uint32_t>{2});
    assert(runRfuCommand(parent, 0x21).response == std::vector<uint32_t>{1});

    constexpr uint32_t serialData = 0x04000120;
    constexpr uint32_t serialControl = 0x04000128;
    const auto finishAdapterEvent = [&](GbaSystem &system) {
        system.write16(serialControl, 0x5002);
        system.write32(serialData, 0x80000000);
        system.write16(serialControl, 0x5082);
        assert(system.runFrame().success);
        assert(system.read32(serialData) == 0x99660028);
        system.write16(serialControl, 0x500A);
        system.write16(serialControl, 0x5002);
        system.write32(serialData, 0x996600A8);
        system.write16(serialControl, 0x5082);
        assert(system.runFrame().success);
        assert(system.read32(serialData) == 0x80000000);
        system.write16(serialControl, 0x500A);
        system.write16(serialControl, 0x5003);
    };

    // Emerald's leader sends this idle request before any real TX frame.
    // An initialized zero prefix is still a one-byte radio payload.
    assert(runRfuCommand(parent, 0x24, {1}).acknowledgement == 0x996600A4);
    assert(runRfuCommand(child, 0x26).response ==
           makeExpectedRfuReceivePacket({0}, 1, false));
    assert(runRfuCommand(parent, 0x24, {1}).acknowledgement == 0x996600A4);
    assert(runRfuCommand(child, 0x26).response ==
           makeExpectedRfuReceivePacket({0}, 1, false));

    // Before finalization, Emerald's parent uses 0x24 without changing its
    // clock. That exchange must release the child's scheduled name/data too.
    assert(runRfuCommand(child, 0x25, {4u << 8, 0x00AA0001})
               .acknowledgement == 0x996600A5);
    assert(runRfuCommand(parent, 0x26).response.empty());
    assert(runRfuCommand(parent, 0x24, {1}).acknowledgement == 0x996600A4);
    finishAdapterEvent(child);
    assert(runRfuCommand(parent, 0x26).response ==
           makeExpectedRfuReceivePacket({0x00AA0001}, 4, true));
    assert(runRfuCommand(child, 0x26).response ==
           makeExpectedRfuReceivePacket({0}, 1, false));

    const std::vector<uint32_t> childData = {0x00AA0001};
    const std::vector<uint32_t> parentData = {0x00BB0001};
    assert(runRfuCommand(child, 0x25, {4u << 8, childData.front()})
               .acknowledgement == 0x996600A5);
    // Child has only scheduled its uplink. Parent cannot receive it yet.
    assert(runRfuCommand(parent, 0x26).response.empty());
    assert(runRfuCommand(parent, 0x25, {3, parentData.front()})
               .acknowledgement == 0x996600A5);
    finishAdapterEvent(parent);
    finishAdapterEvent(child);
    assert(runRfuCommand(parent, 0x26).response ==
           makeExpectedRfuReceivePacket(childData, 4, true));
    assert(runRfuCommand(child, 0x26).response ==
           makeExpectedRfuReceivePacket(parentData, 3, false));

    const std::vector<uint32_t> nextChildData = {0x00CC0001};
    assert(runRfuCommand(child, 0x25, {4u << 8, nextChildData.front()})
               .acknowledgement == 0x996600A5);
    // A one-byte ghost send reuses the first retained parent byte (0x01),
    // drives another exchange, and is not represented as an empty packet.
    assert(runRfuCommand(parent, 0x25, {1}).acknowledgement == 0x996600A5);
    finishAdapterEvent(parent);
    finishAdapterEvent(child);
    assert(runRfuCommand(parent, 0x26).response ==
           makeExpectedRfuReceivePacket(nextChildData, 4, true));
    assert(runRfuCommand(child, 0x26).response ==
           makeExpectedRfuReceivePacket({1}, 1, false));

    // Radio completion is an event in its own right. Parent must wake even
    // when the child did not schedule a new payload for this cycle.
    assert(runRfuCommand(child, 0x27).acknowledgement == 0x996600A7);
    assert(runRfuCommand(parent, 0x25, {1}).acknowledgement == 0x996600A5);
    finishAdapterEvent(parent);
    finishAdapterEvent(child);
    assert(runRfuCommand(parent, 0x26).response.empty());
    assert(runRfuCommand(child, 0x26).response ==
           makeExpectedRfuReceivePacket({1}, 1, false));
}

void runHleVBlankWaitDuringSerialInterruptCheck() {
    using LOTUSGBA::GbaSystem;
    // The serial IRQ occurs on the first few scanlines. The IRQ handler
    // records its source and acknowledges IF. SWI 05 must wait until line 160.
    std::vector<uint8_t> rom(0xC0, 0);
    rom[0] = 0x2E;
    rom[3] = 0xEA;
    constexpr uint32_t code[] = {
        0xE59F0058, 0xE59F1058, 0xE5801000, // Install IRQ handler.
        0xE59F0054, 0xE3A01081, 0xE1C010B0, 0xE321F01F, // Enable VBlank and serial IRQ.
        0xE59F0048, 0xE59F1048, 0xE1C010B0, // Start a cable round.
        0xEF000005, // HLE VBlankIntrWait.
        0xE59F0040, 0xE1D010B0, 0xE3A02402, 0xE1C210B0, // Record VCOUNT.
        0xEAFFFFFE,
        0xE59F0030, 0xE1D010B0, 0xE1C010B0, // IRQ handler: read and clear IF.
        0xE3A02402, 0xE1D200B2, 0xE1800001, 0xE1C200B2, // Record the interrupt sources.
        0xE12FFF1E, // BX LR.
        0x03007FFC, 0x08000100, 0x04000200, 0x04000128, 0x6083, 0x04000006, 0x04000202,
    };
    for (uint32_t word : code)
        for (unsigned byte = 0; byte < 4; ++byte)
            rom.push_back(static_cast<uint8_t>(word >> (8 * byte)));
    GbaSystem parent(std::move(rom));
    GbaSystem child(makeGbaMode3BootRom());
    parent.connectLinkChild(child);
    parent.write16(0x04000004, 8); // DISPSTAT VBlank IRQ enable.
    parent.write16(0x03007FF8, 0x81); // Stale VBlank and unrelated serial flags.
    child.write16(0x04000128, 0x2000);
    if (!parent.runLinkedFrame().success)
        throw std::runtime_error(parent.fault().empty() ? child.fault() : parent.fault());
    assert(parent.read16(0x02000000) == 160);
    assert(parent.read16(0x02000002) == 0x81);
    assert(parent.read16(0x03007FF8) == 0x80);
}

void runHleNestedSerialInterruptCheck() {
    using LOTUSGBA::GbaSystem;
    std::vector<uint8_t> rom(0xC0, 0);
    rom[0] = 0x2E;
    rom[3] = 0xEA;
    constexpr uint32_t code[] = {
        // Install the handler, enable IRQs, wait for VBlank and record r12.
        0xE59F001C, 0xE59F101C, 0xE5801000, 0xE321F01F, 0xE3A0C033, 0xEF000005,
        0xE3A02402, 0xE5C2C002, 0xEAFFFFFE, 0x03007FFC, 0x08000100,
        0, 0, 0, 0, 0,
        // Dispatch by IF; acknowledge VBlank, save SPSR/LR and enable IRQs in System mode.
        0xE3A00301, 0xE2801C02, 0xE1D120B2, 0xE3120080, 0x1A000015,
        0xE3A02001, 0xE1C120B2, 0xE14F1000, 0xE92D4002, 0xE321F01F,
        // Set phase 1, start a transfer, and wait until SIOCNT is no longer busy.
        0xE3A0C055, 0xE3A02402, 0xE3A03001, 0xE5C23000, 0xE2801F4A,
        0xE59F304C, 0xE1C130B0, 0xE1D130B0, 0xE3130080, 0x1AFFFFFC,
        // Record the restored r12, set phase 2, then return from the outer IRQ.
        0xE5C2C001, 0xE3A03002, 0xE5C23000, 0xE321F092,
        0xE8BD4002, 0xE16FF001, 0xE12FFF1E,
        // Serial handler: acknowledge IF, capture the current phase and both
        // received words, and clobber r12 before returning.
        0xE3A02080, 0xE1C120B2, 0xE3A02402, 0xE5D21000, 0xE5C21003,
        0xE5903120, 0xE5823004, 0xE3A0C099, 0xE12FFF1E, 0x00006083,
    };
    for (uint32_t word : code)
        for (unsigned byte = 0; byte < 4; ++byte)
            rom.push_back(static_cast<uint8_t>(word >> (8 * byte)));
    GbaSystem parent(std::move(rom));
    GbaSystem child(makeGbaMode3BootRom());
    parent.connectLinkChild(child);
    parent.write16(0x04000004, 8); // DISPSTAT VBlank IRQ enable.
    parent.write16(0x04000200, 0x81); // IE: VBlank and serial.
    child.write16(0x04000128, 0x2000);
    parent.write16(0x0400012A, 0x1234);
    child.write16(0x0400012A, 0xABCD);
    if (!parent.runLinkedFrame().success)
        throw std::runtime_error(parent.fault().empty() ? child.fault() : parent.fault());
    assert(parent.read8(0x02000003) == 1); // Serial ran inside VBlank.
    assert(parent.read8(0x02000000) == 2); // VBlank resumed.
    assert(parent.read8(0x02000001) == 0x55);
    assert(parent.read8(0x02000002) == 0x33); // The interrupted program resumed.
    assert(parent.read32(0x02000004) == 0xABCD1234);
}

void runLocalLinkSubsystemCheck() {
    assert(linkSubsystem && std::string(linkSubsystem->ident) == "gba_link_2p");
    const auto parent = makeMultiplayerProbe(0x001F, 1, 0x1234, 0x6003);
    auto child = makeMultiplayerProbe(0x7C00, 2, 0xABCD, 0x6003);
    constexpr char rtcMarker[] = "SIIRTC_V001";
    child.insert(child.end(), rtcMarker, rtcMarker + sizeof(rtcMarker));
    const retro_game_info games[] = {
        {"same.gba", parent.data(), parent.size(), nullptr},
        {"same.gba", child.data(), child.size(), nullptr},
    };
    biosStartupMode = "Skip BIOS";
    assert(!retro_load_game_special(linkSubsystem->id, games, 1));
    assert(retro_load_game_special(linkSubsystem->id, games, 2));
    assert(!retro_load_game_special(linkSubsystem->id, games, 1));
    retro_system_av_info info{};
    retro_get_system_av_info(&info);
    assert(info.geometry.base_width == 480 && info.geometry.base_height == 160);
    const auto &firstMemory = linkSubsystem->roms[0].memory[0];
    const auto &secondMemory = linkSubsystem->roms[1].memory[0];
    assert(std::string(firstMemory.extension) != secondMemory.extension);
    auto *first = static_cast<uint8_t *>(retro_get_memory_data(firstMemory.type));
    auto *second = static_cast<uint8_t *>(retro_get_memory_data(secondMemory.type));
    assert(first && second && first != second);
    assert(retro_get_memory_size(firstMemory.type) == 32768);
    assert(retro_get_memory_size(secondMemory.type) == 32768);
    assert(!retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    const auto &firstRtc = linkSubsystem->roms[0].memory[1];
    const auto &secondRtc = linkSubsystem->roms[1].memory[1];
    assert(!retro_get_memory_data(firstRtc.type));
    const auto *rtc = retro_get_memory_data(secondRtc.type);
    assert(rtc && retro_get_memory_size(secondRtc.type) != 0);

    pressedJoypad = 1u << RETRO_DEVICE_ID_JOYPAD_A;
    secondJoypad = 0;
    retro_run();
    assert(first[0] == 0xFF && second[0] == 0xFF);
    secondJoypad = 1u << RETRO_DEVICE_ID_JOYPAD_B;
    retro_run();
    assert(receivedWidth == 480 && receivedHeight == 160);
    const bool xrgb = selectedFormat == RETRO_PIXEL_FORMAT_XRGB8888;
    for (unsigned y = 0; y < 160; ++y) {
        for (unsigned x = 0; x < 240; ++x) {
            assert(receivedPixels[y * 480 + x] == (xrgb ? 0xFF0000u : 0x7C00u));
            assert(receivedPixels[y * 480 + x + 240] == (xrgb ? 0x0000FFu : 0x001Fu));
        }
    }
    const uint8_t parentResult[] = {0x34, 0x12, 0xCD, 0xAB, 0x0B};
    const uint8_t childResult[] = {0x34, 0x12, 0xCD, 0xAB, 0x1F};
    for (unsigned round = 0; round < 2; ++round) {
        assert(std::equal(std::begin(parentResult), std::end(parentResult), first + round * 5));
        assert(std::equal(std::begin(childResult), std::end(childResult), second + round * 5));
    }
    retro_reset();
    assert(retro_get_memory_data(firstMemory.type) == first && first[0] == 0x34);
    assert(retro_get_memory_data(secondMemory.type) == second && second[0] == 0x34);
    assert(retro_get_memory_data(secondRtc.type) == rtc);
    std::fill_n(first, 10, 0);
    std::fill_n(second, 10, 0);
    retro_run();
    assert(std::equal(std::begin(parentResult), std::end(parentResult), first));
    assert(std::equal(std::begin(childResult), std::end(childResult), second));
    retro_unload_game();
    assert(!retro_get_memory_data(firstMemory.type));
    assert(!retro_get_memory_data(secondMemory.type));
    assert(!retro_get_memory_data(secondRtc.type));
    pressedJoypad = secondJoypad = 0;
    biosStartupMode = "Automatic";
}

void runRemoteNetplaySioCheck() {
    assert(netpacket);
    const auto rom = makeMultiplayerProbe(0x001F, 1, 0x1234, 0x6003);
    const retro_game_info game{"netplay-link.gba", rom.data(), rom.size(), nullptr};
    biosStartupMode = "Skip BIOS";
    assert(retro_load_game(&game));
    auto *save = static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    assert(save && retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == 32768);
    sentNetpackets.clear();
    logMessages.clear();
    netpacket->start(0, [](int flags, const void *data, size_t len, uint16_t id) {
        assert(id == 1 && (flags & RETRO_NETPACKET_RELIABLE));
        const auto *bytes = static_cast<const uint8_t *>(data);
        sentNetpackets.emplace_back(bytes, bytes + len);
    }, nullptr);
    assert(netpacket->connected(1));
    retro_run();
    assert(sentNetpackets.size() == 1); // Startup shares metadata, not SRAM.
    assert(sentNetpackets.front().size() == 27);
    assert(sentNetpackets.front()[4] == 0);
    auto startup = sentNetpackets.front();
    netpacket->receive(startup.data(), startup.size(), 1);
    retro_run(); // Unknown games do not acquire a guessed compatibility profile.
    assert(std::any_of(logMessages.begin(), logMessages.end(), [](const auto &message) {
        return message.find("Wireless Adapter requires") != std::string::npos;
    }));
    netpacket->stop();
    retro_unload_game();
    biosStartupMode = "Automatic";
}

void runPokemonCableSelectionCheck() {
    assert(linkDeviceOptionRegistered);
    // Original ARM guest probes adapter presence through the public subsystem.
    // It stores the first RFU login reply byte in each cartridge's SRAM.
    std::vector<uint32_t> code;
    std::vector<std::pair<size_t, uint32_t>> literals;
    const auto load = [&](unsigned reg, uint32_t value) {
        literals.emplace_back(code.size(), value);
        code.push_back(0xE59F0000 | (reg << 12));
    };
    load(0, 0x04000134);
    for (uint16_t value : std::array<uint16_t, 4>{0x8000, 0x80A0, 0x80A2, 0x80A0}) {
        load(1, value); code.push_back(0xE1C010B0); // strh r1, [r0]
    }
    load(2, 0x04000128); load(1, 0x5003); code.push_back(0xE1C210B0);
    load(1, 0); code.push_back(0xE1C010B0);
    load(3, 0x04000120); load(1, 0x7FFF494E); code.push_back(0xE5831000);
    load(1, 0x5083); code.push_back(0xE1C210B0);
    code.push_back(0xE1D210B0); code.push_back(0xE3110080); code.push_back(0x1AFFFFFC);
    code.push_back(0xE5931000); load(4, 0x0E000000); code.push_back(0xE5C41000);
    code.push_back(0xEAFFFFFE);
    for (const auto &[index, value] : literals) {
        code[index] |= static_cast<uint32_t>((code.size() - index) * 4 - 8);
        code.push_back(value);
    }
    std::vector<uint8_t> adapterProbe(0xC0, 0);
    adapterProbe[0] = 0x2E; adapterProbe[3] = 0xEA;
    adapterProbe[0xAC] = 'B'; adapterProbe[0xAD] = 'P'; adapterProbe[0xAE] = 'E'; adapterProbe[0xAF] = 'E';
    for (uint32_t word : code)
        for (unsigned byte = 0; byte < 4; ++byte)
            adapterProbe.push_back(static_cast<uint8_t>(word >> (8 * byte)));
    constexpr char saveMarker[] = "SRAM_V113";
    adapterProbe.insert(adapterProbe.end(), std::begin(saveMarker), std::end(saveMarker));
    const retro_game_info pair[] = {
        {"adapter-presence.gba", adapterProbe.data(), adapterProbe.size(), nullptr},
        {"adapter-presence.gba", adapterProbe.data(), adapterProbe.size(), nullptr}};
    biosStartupMode = "Skip BIOS";
    for (const char *device : {"Link Cable", "Wireless Adapter"}) {
        linkDeviceOption = device;
        assert(retro_load_game_special(linkSubsystem->id, pair, 2));
        for (unsigned pass = 0; pass < 2; ++pass) {
            if (pass) retro_reset();
            for (unsigned player = 0; player < 2; ++player) {
                auto *save = static_cast<uint8_t *>(retro_get_memory_data(linkSubsystem->roms[player].memory[0].type));
                assert(save); save[0] = 0x5A;
            }
            for (unsigned frame = 0; frame < 3; ++frame) retro_run();
            for (unsigned player = 0; player < 2; ++player) {
                const auto *save = static_cast<uint8_t *>(retro_get_memory_data(linkSubsystem->roms[player].memory[0].type));
                assert(save[0] == (std::string(device) == "Link Cable" ? 0xFF : 0));
            }
        }
        retro_unload_game();
    }
    auto rom = makeMultiplayerProbe(0x001F, 1, 0x1234, 0x6003);
    rom[0xAC] = 'B'; rom[0xAD] = 'P'; rom[0xAE] = 'E'; rom[0xAF] = 'E';
    const retro_game_info game{"original-cable-profile-probe.gba", rom.data(), rom.size(), nullptr};
    for (bool mismatch : {false, true}) {
        linkDeviceOption = "Link Cable"; biosStartupMode = "Skip BIOS";
        assert(retro_load_game(&game));
        sentNetpackets.clear(); logMessages.clear();
        netpacket->start(0, [](int, const void *data, size_t len, uint16_t) {
            const auto *bytes = static_cast<const uint8_t *>(data);
            sentNetpackets.emplace_back(bytes, bytes + len);
        }, nullptr);
        assert(netpacket->connected(1)); retro_run();
        assert(sentNetpackets.size() == 1 && sentNetpackets.front().size() == 27);
        auto startup = sentNetpackets.front(); assert(startup.back() == 1);
        if (mismatch) startup.back() = 0;
        netpacket->receive(startup.data(), startup.size(), 1); retro_run();
        assert(std::any_of(logMessages.begin(), logMessages.end(), [mismatch](const auto &s) {
            return s.find(mismatch ? "link device mismatch" : "buffered Pokemon cable") != std::string::npos;
        }));
        netpacket->stop(); retro_unload_game();
    }
    linkDeviceOption = "Wireless Adapter"; biosStartupMode = "Automatic";
}

void runRemoteEmeraldRfuNetpacketCheck(char region) {
    assert(netpacket);
    auto rom = makeJapaneseEmeraldProfileProbeRom();
    rom[0xAF] = static_cast<uint8_t>(region);
    const retro_game_info game{"emerald-rfu-probe.gba", rom.data(), rom.size(), nullptr};
    biosStartupMode = "Skip BIOS";
    assert(retro_load_game(&game));

    sentNetpackets.clear();
    logMessages.clear();
    netpacket->start(0, [](int flags, const void *data, size_t len, uint16_t id) {
        assert(id == 1 && (flags & RETRO_NETPACKET_RELIABLE));
        const auto *bytes = static_cast<const uint8_t *>(data);
        sentNetpackets.emplace_back(bytes, bytes + len);
    }, nullptr);
    assert(netpacket->connected(1));
    retro_run();
    assert(sentNetpackets.size() == 1);
    const auto startup = sentNetpackets.front();
    assert(startup.size() == 27 && startup[4] == 0 && startup.back() == 0);
    netpacket->receive(startup.data(), startup.size(), 1);
    retro_run();
    assert(std::any_of(logMessages.begin(), logMessages.end(), [](const auto &message) {
        return message.find("independent-clock Emerald RFU link") != std::string::npos;
    }));

    const auto makeRfuPacket = [](uint8_t type, uint32_t sequence,
                                  uint16_t firstId, uint16_t secondId) {
        std::vector<uint8_t> packet{'L', 'G', 'B', 'A', type};
        const auto append = [&packet](uint64_t value, unsigned count) {
            for (unsigned index = 0; index < count; ++index)
                packet.push_back(static_cast<uint8_t>(value >> (index * 8)));
        };
        append(sequence, 4);
        append(firstId, 2);
        append(secondId, 2);
        return packet;
    };
    std::vector<uint8_t> advertisement{'L', 'G', 'B', 'A', 6};
    const auto appendAdvertisementInteger = [&advertisement](uint64_t value,
                                                               unsigned count) {
        for (unsigned index = 0; index < count; ++index)
            advertisement.push_back(static_cast<uint8_t>(value >> (index * 8)));
    };
    appendAdvertisementInteger(1, 4);
    appendAdvertisementInteger(2, 2);
    for (uint32_t word : {0x454D4552u, 0x414C4420u, 1u, 2u, 3u, 4u}) {
        for (unsigned index = 0; index < 4; ++index)
            advertisement.push_back(static_cast<uint8_t>(word >> (index * 8)));
    }
    netpacket->receive(advertisement.data(), advertisement.size(), 1);

    auto connectionRequest = makeRfuPacket(7, 1, 2, 1);
    sentNetpackets.clear();
    netpacket->receive(connectionRequest.data(), connectionRequest.size(), 1);
    const auto resultPacket = std::find_if(sentNetpackets.begin(), sentNetpackets.end(),
        [](const auto &packet) { return packet.size() >= 5 && packet[4] == 8; });
    assert(resultPacket != sentNetpackets.end());
    const auto &connectionResult = *resultPacket;
    assert(connectionResult.size() == 13 && connectionResult[4] == 8);
    assert(connectionResult[5] == 1 && connectionResult[6] == 0 &&
           connectionResult[7] == 0 && connectionResult[8] == 0);
    assert(connectionResult[9] == 2 && connectionResult[10] == 0);
    assert(connectionResult[11] == 0 && connectionResult[12] == 0);
    // A data message already in transit after RFU reset is dropped and ACKed;
    // the frontend session remains alive instead of entering a permanent wait.
    std::vector<uint8_t> staleData{'L', 'G', 'B', 'A', 9,
                                   1, 0, 0, 0, 1, 1,
                                   0xDD, 0xCC, 0xBB, 0xAA};
    sentNetpackets.clear();
    netpacket->receive(staleData.data(), staleData.size(), 1);
    assert(sentNetpackets.size() == 1);
    assert(sentNetpackets.front().size() == 9 &&
           sentNetpackets.front()[4] == 10);
    assert(std::none_of(logMessages.begin(), logMessages.end(), [](const auto &message) {
        return message.find("GBA rejected remote RFU data") != std::string::npos;
    }));

    const std::vector<uint8_t> resetPacket{'L', 'G', 'B', 'A', 11};
    netpacket->receive(resetPacket.data(), resetPacket.size(), 1);
    assert(std::any_of(logMessages.begin(), logMessages.end(), [](const auto &message) {
        return message.find("peer reset the Emerald RFU adapter link") !=
               std::string::npos;
    }));
    assert(std::none_of(logMessages.begin(), logMessages.end(), [](const auto &message) {
        return message.find("Invalid GBA RFU") != std::string::npos ||
               message.find("GBA rejected remote RFU") != std::string::npos;
    }));

    netpacket->stop();
    retro_unload_game();
    biosStartupMode = "Automatic";
}

void runOptionalBiosCheck() {
    const auto directory = std::filesystem::temp_directory_path() /
                           "lotusgba_core_test_system";
    std::filesystem::create_directories(directory);
    const auto biosPath = directory / "gba_bios.bin";
    std::vector<uint8_t> bios(0x4000, 0);
    constexpr uint32_t loadPcFromLiteral = 0xE51FF004; // ldr pc, [pc, #-4]
    constexpr uint32_t cartridgeEntry = 0x08000000;
    for (unsigned byte = 0; byte < 4; ++byte) {
        bios[byte] = static_cast<uint8_t>(loadPcFromLiteral >> (byte * 8));
        bios[4 + byte] = static_cast<uint8_t>(cartridgeEntry >> (byte * 8));
    }
    {
        std::ofstream file(biosPath, std::ios::binary | std::ios::trunc);
        assert(file.good());
        file.write(reinterpret_cast<const char *>(bios.data()),
                   static_cast<std::streamsize>(bios.size()));
        assert(file.good());
    }

    systemDirectoryPath = directory.string();
    biosStartupMode = "Use BIOS";
    auto rom = makeGbaMode3BootRom();
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    videoReceived = false;
    receivedPixels.clear();
    assert(retro_load_game(&game));
    retro_run();
    retro_run();
    runUntilSolidRed();
    assert(videoReceived);
    assert(receivedWidth == 240 && receivedHeight == 160);
    if (selectedFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
        for (uint32_t pixel : receivedPixels)
            assert(pixel == 0x00FF0000);
    } else {
        for (uint32_t pixel : receivedPixels)
            assert(pixel == 0x7C00);
    }
    retro_unload_game();

    // Netplay recreates the console after metadata negotiation. A synthetic
    // BIOS writes a battery marker so the test observes actual BIOS execution
    // both on content load and after the Netplay restart (not just a log).
    constexpr uint32_t markerBios[] = {
        0xE3A0040E, // mov r0, #0x0E000000
        0xE3A0105A, // mov r1, #0x5A
        0xE5C01010, // strb r1, [r0, #16]
        0xE51FF004, // ldr pc, [pc, #-4]
        0x08000000,
    };
    std::fill(bios.begin(), bios.end(), 0);
    for (size_t index = 0; index < std::size(markerBios); ++index)
        for (unsigned byte = 0; byte < 4; ++byte)
            bios[index * 4 + byte] = static_cast<uint8_t>(
                markerBios[index] >> (byte * 8));
    {
        std::ofstream file(biosPath, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(bios.data()),
                   static_cast<std::streamsize>(bios.size()));
        assert(file.good());
    }
    rom = makeJapaneseEmeraldProfileProbeRom();
    constexpr char saveMarker[] = "SRAM_V113";
    rom.insert(rom.end(), std::begin(saveMarker), std::end(saveMarker));
    game.data = rom.data();
    game.size = rom.size();
    assert(retro_load_game(&game));
    auto *save = static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    assert(save);
    retro_run();
    assert(save[16] == 0x5A);
    save[16] = 0;
    sentNetpackets.clear();
    netpacket->start(0, [](int, const void *data, size_t len, uint16_t) {
        const auto *bytes = static_cast<const uint8_t *>(data);
        sentNetpackets.emplace_back(bytes, bytes + len);
    }, nullptr);
    assert(netpacket->connected(1));
    retro_run();
    assert(sentNetpackets.size() == 1);
    const auto startup = sentNetpackets.front();
    netpacket->receive(startup.data(), startup.size(), 1);
    retro_run();
    assert(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) == save);
    assert(save[16] == 0x5A);
    retro_unload_game();
    biosStartupMode = "Automatic";
    systemDirectoryPath.clear();
    std::filesystem::remove_all(directory);
}

void runFlashBatteryMemoryCheck() {
    biosStartupMode = "Skip BIOS";
    auto rom = makeFlash1mMode3Rom();
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    assert(retro_load_game(&game));

    constexpr size_t flashSize = 128 * 1024;
    auto *saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    assert(saveRam != nullptr);
    assert(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == flashSize);
    assert(saveRam[0] == 0xFF && saveRam[0x1234] == 0xFF);
    saveRam[0x1234] = 0x5A; // Simulate a frontend restoring battery data.

    retro_reset();
    auto *afterReset = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    assert(afterReset == saveRam && afterReset[0x1234] == 0x5A);
    std::vector<uint8_t> savedBattery(afterReset, afterReset + flashSize);

    retro_unload_game();
    assert(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) == nullptr);
    assert(retro_load_game(&game));
    auto *reloadedSave = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    assert(reloadedSave != nullptr);
    assert(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == flashSize);
    std::copy(savedBattery.begin(), savedBattery.end(), reloadedSave);
    assert(reloadedSave[0x1234] == 0x5A);

    retro_unload_game();
    biosStartupMode = "Automatic";
}

void runSaveMemoryTypeCheck(const char *marker, size_t expectedSize) {
    biosStartupMode = "Skip BIOS";
    auto rom = makeSaveMarkerMode3Rom(marker);
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    assert(retro_load_game(&game));

    auto *saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    assert(saveRam != nullptr);
    assert(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == expectedSize);
    assert(saveRam[0] == 0xFF);
    saveRam[0] = 0x6D;
    retro_reset();
    assert(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) == saveRam);
    assert(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == expectedSize);
    assert(saveRam[0] == 0x6D);

    retro_unload_game();
    biosStartupMode = "Automatic";
}

void runRtcMemoryCheck() {
    biosStartupMode = "Skip BIOS";
    auto rom = makeSaveMarkerMode3Rom("FLASH1M_V103");
    constexpr char rtcMarker[] = "SIIRTC_V001";
    rom.insert(rom.end(), rtcMarker, rtcMarker + sizeof(rtcMarker) - 1);
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    assert(retro_load_game(&game));

    auto *saveRam = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    auto *rtc = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_RTC));
    assert(saveRam && rtc);
    assert(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) ==
           LOTUSGBA::GbaSystem::FLASH1M_SIZE);
    assert(retro_get_memory_size(RETRO_MEMORY_RTC) == 24);
    assert(saveRam != rtc);
    saveRam[0x1234] = 0x5A;
    rtc[0] = 'G';
    rtc[1] = 'R';
    rtc[2] = 'T';
    rtc[3] = 'C';

    retro_reset();
    assert(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) == saveRam);
    assert(retro_get_memory_data(RETRO_MEMORY_RTC) == rtc);
    assert(saveRam[0x1234] == 0x5A);
    assert(rtc[0] == 'G' && rtc[3] == 'C');

    assert(retro_load_game(&game)); // Reloading the same loaded content retains both blocks.
    assert(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) == saveRam);
    assert(retro_get_memory_data(RETRO_MEMORY_RTC) == rtc);
    assert(saveRam[0x1234] == 0x5A);
    assert(rtc[0] == 'G' && rtc[3] == 'C');

    const std::vector<uint8_t> savedFlash(saveRam,
        saveRam + retro_get_memory_size(RETRO_MEMORY_SAVE_RAM));
    const std::vector<uint8_t> savedRtc(rtc,
        rtc + retro_get_memory_size(RETRO_MEMORY_RTC));
    retro_unload_game();
    assert(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) == nullptr);
    assert(retro_get_memory_data(RETRO_MEMORY_RTC) == nullptr);

    // A frontend persists these independent blocks and restores them after
    // unloading and loading the same content in a fresh core instance.
    assert(retro_load_game(&game));
    auto *reloadedFlash = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    auto *reloadedRtc = static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_RTC));
    assert(reloadedFlash && reloadedRtc);
    assert(retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) == savedFlash.size());
    assert(retro_get_memory_size(RETRO_MEMORY_RTC) == savedRtc.size());
    std::copy(savedFlash.begin(), savedFlash.end(), reloadedFlash);
    std::copy(savedRtc.begin(), savedRtc.end(), reloadedRtc);
    assert(std::equal(savedFlash.begin(), savedFlash.end(), reloadedFlash));
    assert(std::equal(savedRtc.begin(), savedRtc.end(), reloadedRtc));

    retro_unload_game();
    biosStartupMode = "Automatic";
}

void runRequiredBiosMissingCheck() {
    systemDirectoryPath.clear();
    biosStartupMode = "Use BIOS";
    auto rom = makeGbaMode3BootRom();
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    assert(!retro_load_game(&game));
    biosStartupMode = "Automatic";
}

void runSkipBiosOptionCheck() {
    const auto directory = std::filesystem::temp_directory_path() /
                           "lotusgba_skip_bios_test_system";
    std::filesystem::create_directories(directory);
    const auto biosPath = directory / "gba_bios.bin";
    std::vector<uint8_t> bios(0x4000, 0);
    constexpr uint32_t branchToSelf = 0xEAFFFFFE;
    for (unsigned byte = 0; byte < 4; ++byte)
        bios[byte] = static_cast<uint8_t>(branchToSelf >> (byte * 8));
    {
        std::ofstream file(biosPath, std::ios::binary | std::ios::trunc);
        assert(file.good());
        file.write(reinterpret_cast<const char *>(bios.data()),
                   static_cast<std::streamsize>(bios.size()));
        assert(file.good());
    }

    // A looping BIOS fixture makes the selected startup path observable:
    // Skip BIOS must boot the cartridge even though a BIOS file is present.
    systemDirectoryPath = directory.string();
    biosStartupMode = "Skip BIOS";
    auto rom = makeGbaMode3BootRom();
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    videoReceived = false;
    receivedPixels.clear();
    assert(retro_load_game(&game));
    retro_run();
    retro_run();
    runUntilSolidRed();
    assert(videoReceived);
    assert(receivedWidth == 240 && receivedHeight == 160);
    if (selectedFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
        for (uint32_t pixel : receivedPixels)
            assert(pixel == 0x00FF0000);
    } else {
        for (uint32_t pixel : receivedPixels)
            assert(pixel == 0x7C00);
    }

    retro_unload_game();
    biosStartupMode = "Automatic";
    systemDirectoryPath.clear();
    std::filesystem::remove_all(directory);
}

void videoRefresh(const void *data, unsigned width, unsigned height, size_t pitch) {
    assert(data != nullptr);
    videoReceived = true;
    ++videoRefreshCount;
    receivedWidth = width;
    receivedHeight = height;
    receivedPixels.clear();
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            if (selectedFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
                uint32_t pixel = 0;
                const size_t offset = static_cast<size_t>(y) * pitch + x * 4;
                pixel = static_cast<uint32_t>(bytes[offset]) |
                        (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
                        (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
                        (static_cast<uint32_t>(bytes[offset + 3]) << 24);
                receivedPixels.push_back(pixel);
            } else {
                const size_t offset = static_cast<size_t>(y) * pitch + x * 2;
                receivedPixels.push_back(static_cast<uint32_t>(bytes[offset]) |
                    (static_cast<uint32_t>(bytes[offset + 1]) << 8));
            }
        }
    }
}

uint32_t expectedPixelForBgr555(uint16_t color) {
    const uint32_t red = color & 0x1F;
    const uint32_t green = (color >> 5) & 0x1F;
    const uint32_t blue = (color >> 10) & 0x1F;
    if (selectedFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
        const auto expand = [](uint32_t value) {
            return (value << 3) | (value >> 2);
        };
        return (expand(red) << 16) | (expand(green) << 8) | expand(blue);
    }
    return (red << 10) | (green << 5) | blue;
}

void runMode0TileSpriteHomebrewAndCheck() {
    auto rom = makeGbaMode0TileSpriteHomebrewRom();
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    videoReceived = false;
    receivedPixels.clear();
    assert(retro_load_game(&game));

    std::vector<uint32_t> expected(240 * 160);
    for (unsigned y = 0; y < 160; ++y) {
        for (unsigned x = 0; x < 240; ++x) {
            const bool inSprite =
                (x >= 40 && x < 56 && y >= 32 && y < 48) ||
                (x >= 64 && x < 80 && y >= 128 && y < 144) ||
                (x >= 88 && x < 104 && y == 159) ||
                (x >= 136 && x < 152 && y < 15);
            const uint16_t color = inSprite ? 0x03E0 : 0x001F;
            expected[y * 240 + x] = expectedPixelForBgr555(color);
        }
    }

    bool expectedFrameReceived = false;
    for (unsigned frame = 0; frame < 16; ++frame) {
        videoReceived = false;
        retro_run();
        assert(videoReceived);
        assert(receivedWidth == 240 && receivedHeight == 160);
        if (receivedPixels == expected) {
            expectedFrameReceived = true;
            break;
        }
    }
    assert(expectedFrameReceived);
    retro_unload_game();
}

uint32_t expectedSolidRedPixel() {
    return expectedPixelForBgr555(0x001F);
}

void runUntilSolidRed(unsigned additionalFrames) {
    const uint32_t expected = expectedSolidRedPixel();
    for (unsigned frame = 0; frame < additionalFrames; ++frame) {
        if (std::all_of(receivedPixels.begin(), receivedPixels.end(),
                [expected](uint32_t pixel) { return pixel == expected; }))
            return;
        retro_run();
    }
    assert(std::all_of(receivedPixels.begin(), receivedPixels.end(),
        [expected](uint32_t pixel) { return pixel == expected; }));
}

unsigned countColor(uint32_t color, unsigned x0 = 0, unsigned y0 = 0,
                    unsigned x1 = 240, unsigned y1 = 160) {
    unsigned count = 0;
    for (unsigned y = y0; y < y1; ++y)
        for (unsigned x = x0; x < x1; ++x)
            count += receivedPixels[y * 240 + x] == color;
    return count;
}

int leftPaddleTop() {
    for (unsigned y = 0; y < 160; ++y)
        for (unsigned x = 1; x < 9; ++x)
            if (receivedPixels[y * 240 + x] == 0x000000FF)
                return static_cast<int>(y);
    return -1;
}

std::pair<int, int> ballTopLeft() {
    int left = 240;
    int top = 160;
    for (unsigned y = 0; y < 160; ++y) {
        for (unsigned x = 0; x < 240; ++x) {
            if (receivedPixels[y * 240 + x] == 0x0000FF00) {
                left = std::min(left, static_cast<int>(x));
                top = std::min(top, static_cast<int>(y));
            }
        }
    }
    return {left, top};
}

unsigned scorePixels(unsigned x0) {
    return countColor(0x00FFFFFF, x0, 10, x0 + 16, 26);
}

void runPongGameplayAndCheck() {
    auto rom = readFile("../../tests/roms/gba-pong/pong.gba");
    retro_game_info game{};
    game.data = rom.data();
    game.size = rom.size();
    assert(retro_load_game(&game));

    // WAITCNT makes ROM execution take longer, so wait for the first complete
    // title/menu draw instead of assuming it fits in two frontend frames.
    bool menuVisible = false;
    for (unsigned frame = 0; frame < 12 && !menuVisible; ++frame) {
        pressedJoypad = 0;
        videoReceived = false;
        retro_run();
        assert(videoReceived);
        menuVisible = countColor(0x00FFFFFF, 80, 64, 160, 72) > 0 &&
                      countColor(0x00FFFFFF, 72, 88, 160, 112) > 0;
    }
    assert(menuVisible);
    pressedJoypad = 0;
    retro_run(); // Let the completed menu draw reach the following frame.

    std::pair<int, int> initialBall{};
    std::pair<int, int> beforePaddleReturn{};
    unsigned initialPlayerScore = 0;
    unsigned initialComputerScore = 0;
    for (unsigned frame = 0; frame <= 460; ++frame) {
        pressedJoypad = 0;
        if (frame == 2)
            pressedJoypad = 1u << RETRO_DEVICE_ID_JOYPAD_START;
        if (frame >= 130 && frame < 140)
            pressedJoypad = 1u << RETRO_DEVICE_ID_JOYPAD_UP;
        if (frame >= 141 && frame < 151)
            pressedJoypad = 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
        if (frame >= 185 && frame < 220)
            pressedJoypad = 1u << RETRO_DEVICE_ID_JOYPAD_UP;
        if (frame >= 400 && frame < 434)
            pressedJoypad = 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;

        videoReceived = false;
        retro_run();
        assert(videoReceived);
        assert(receivedWidth == 240 && receivedHeight == 160);

        if (frame == 1) {
            // The title and both menu choices occupy the middle of Mode 3.
            assert(countColor(0x00FFFFFF, 80, 64, 160, 72) > 0);
            assert(countColor(0x00FFFFFF, 72, 88, 160, 112) > 0);
            assert(countColor(0x000000FF) == 0);
        }
        if (frame == 4) {
            assert(countColor(0x000000FF) > 0);
            assert(countColor(0x0000FF00) == 64);
            assert(leftPaddleTop() == 68);
            initialBall = ballTopLeft();
            initialPlayerScore = scorePixels(52);
            initialComputerScore = scorePixels(172);
            assert(initialPlayerScore > 0 && initialComputerScore > 0);
        }
        if (frame == 125)
            assert(ballTopLeft() != initialBall);
        if (frame == 129)
            assert(leftPaddleTop() == 68);
        if (frame == 139)
            assert(leftPaddleTop() == 50); // Visible one frame after the VBlank draw.
        if (frame == 140)
            assert(leftPaddleTop() == 48);
        if (frame == 150)
            assert(leftPaddleTop() == 66);
        if (frame == 151)
            assert(leftPaddleTop() == 68);
        if (frame == 219)
            assert(leftPaddleTop() == 0);
        if (frame == 220)
            beforePaddleReturn = ballTopLeft();
        if (frame == 247) {
            const auto returnedBall = ballTopLeft();
            assert(returnedBall.first > beforePaddleReturn.first);
            assert(returnedBall.first < 80);
        }
        if (frame == 433)
            assert(leftPaddleTop() == 66);
        if (frame == 434)
            assert(leftPaddleTop() == 68);
        if (frame == 455 || frame == 460) {
            // Moving away for the next return lets the CPU score once.
            assert(scorePixels(52) == initialPlayerScore);
            assert(scorePixels(172) != initialComputerScore);
        }
    }

    retro_unload_game();
}

} // namespace

int main() {
    retro_set_environment(environment);
    assert(biosOptionRegistered);
    assert(!netpacket);
    retro_set_video_refresh(videoRefresh);
    retro_set_audio_sample_batch(audioBatch);
    retro_set_input_poll(inputPoll);
    retro_set_input_state(inputState);
    retro_init();
    assert(netpacket);

    retro_system_info systemInfo{};
    retro_get_system_info(&systemInfo);
    assert(systemInfo.library_name && std::string(systemInfo.library_name) == "LotusGBA");
    assert(systemInfo.valid_extensions && std::string(systemInfo.valid_extensions) == "gba");
    assert(!systemInfo.need_fullpath);

    retro_system_av_info avInfo{};
    retro_get_system_av_info(&avInfo);
    assert(avInfo.geometry.base_width == 240);
    assert(avInfo.geometry.base_height == 160);
    assert(std::fabs(avInfo.timing.fps - 59.7275) < 0.01);
    assert(std::fabs(avInfo.timing.sample_rate - 48000.0) < 0.01);

    const auto runAndCheck = [](std::vector<uint8_t> rom, uint16_t color) {
        retro_game_info game{};
        game.data = rom.data();
        game.size = rom.size();
        videoReceived = false;
        receivedPixels.clear();
        receivedAudio.clear();
        assert(retro_load_game(&game));
        retro_run();
        retro_run();
        assert(receivedAudio.size() >= 1604 * 2);
        assert(receivedAudio.size() <= 1608 * 2);

        const uint32_t expectedPixel = expectedPixelForBgr555(color);
        for (unsigned frame = 0; frame < 16; ++frame) {
            if (std::all_of(receivedPixels.begin(), receivedPixels.end(),
                    [expectedPixel](uint32_t pixel) {
                        return pixel == expectedPixel;
                    }))
                break;
            retro_run();
        }

        assert(videoReceived);
        assert(receivedWidth == 240 && receivedHeight == 160);
        assert(receivedPixels.size() == 240 * 160);
        assert(std::all_of(receivedPixels.begin(), receivedPixels.end(),
            [expectedPixel](uint32_t pixel) { return pixel == expectedPixel; }));
    };
    constexpr uint16_t testColor = 0x7607; // R=7, G=16, B=29.
    runAndCheck(makeGbaMode3BootRom(), 0x001F);
    retro_unload_game();
    runAndCheck(makeGbaThumbMode3BootRom(), 0x001F);
    retro_unload_game();
    runAndCheck(makeGbaMode3BootRom(testColor), testColor);

    constexpr const char *letterA[] = {
        "..####..",
        ".##..##.",
        ".##..##.",
        ".######.",
        ".##..##.",
        ".##..##.",
        ".##..##.",
        "........",
    };
    const auto runInstructionSuiteAndCheck = [&](std::vector<uint8_t> rom) {
        retro_game_info game{};
        game.data = rom.data();
        game.size = rom.size();
        assert(retro_load_game(&game));
        for (unsigned frame = 0; frame < 5; ++frame) {
            videoReceived = false;
            retro_run();
            assert(videoReceived);
            assert(receivedWidth == 240 && receivedHeight == 160);
        }

        // Both upstream suites print "All tests passed" at (56, 76), while
        // their numbered failure report starts at a different position.
        assert(selectedFormat == RETRO_PIXEL_FORMAT_XRGB8888);
        for (unsigned y = 0; y < 8; ++y) {
            for (unsigned x = 0; x < 8; ++x) {
                const uint32_t expected = letterA[y][x] == '#'
                    ? 0x00000000
                    : 0x00FFFFFF;
                assert(receivedPixels[(76 + y) * 240 + 56 + x] == expected);
            }
        }
        retro_unload_game();
    };
    retro_unload_game();
    runMode0TileSpriteHomebrewAndCheck();
    runInstructionSuiteAndCheck(
        readFile("../../tests/roms/gba-arm-tests/arm.gba"));
    runInstructionSuiteAndCheck(
        readFile("../../tests/roms/gba-thumb-tests/thumb.gba"));
    runPongGameplayAndCheck();
    runFlashBatteryMemoryCheck();
    runSaveMemoryTypeCheck("SRAM_V", 32 * 1024);
    runSaveMemoryTypeCheck("FLASH512_V", 64 * 1024);
    runSaveMemoryTypeCheck("EEPROM_V", 512);
    runRtcMemoryCheck();
    runOptionalBiosCheck();
    runSkipBiosOptionCheck();
    runRequiredBiosMissingCheck();
    runMultiplayerBusCheck();
    runMultiplayerTimingCheck();
    runLocalMultiplayerProbeRomCheck();
    runRemoteMultiplayerProbeRomLatencyCheck();
    runRemoteMultiplayerLinkCheck();
    runRemoteMultiplayerLatencyCheck();
    runRemoteMultiplayerWaitCheck();
    runRemoteBufferedEmeraldLinkCheck();
    runRemoteBufferedPredictedHandshakeCheck();
    runRemoteFramePacingCheck();
    runRemoteSioPredictionCheck();
    runRemoteSioEventPumpCheck();
    runRemoteSioQueuedWordCheck();
    runRemoteMultiplayerQueuedStartsCheck();
    runUnconnectedNormalSioCheck();
    runEmeraldRfuBootstrapCheck('J');
    runEmeraldRfuBootstrapCheck('E');
    runEmeraldRfuAdapterClockAckCheck();
    runEmeraldRfuRadioProgressCheck();
    runEmeraldRfuChildSnapshotCheck();
    runEmeraldRfuTransmitBackpressureCheck();
    runEmeraldRfuHostDrivenCycleCheck();
    runEmeraldRfuParentCandidateCheck();
    runHleVBlankWaitDuringSerialInterruptCheck();
    runHleNestedSerialInterruptCheck();
    runLocalLinkSubsystemCheck();
    runRemoteNetplaySioCheck();
    runPokemonCableSelectionCheck();
    runRemoteEmeraldRfuNetpacketCheck('J');
    runRemoteEmeraldRfuNetpacketCheck('E');

    retro_deinit();

    acceptXrgb8888 = false;
    retro_init();
    assert(selectedFormat == RETRO_PIXEL_FORMAT_0RGB1555);
    runAndCheck(makeGbaThumbMode3BootRom(testColor), testColor);
    retro_unload_game();
    runMode0TileSpriteHomebrewAndCheck();
    runLocalLinkSubsystemCheck();
    retro_deinit();
}
