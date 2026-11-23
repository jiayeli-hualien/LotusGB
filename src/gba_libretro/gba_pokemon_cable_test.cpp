// Original synthetic cable guest. No commercial ROM or captured game data.
// Oracle: the checksum/data phases in pokeemerald's SerialCB/DoRecv/DoSend.
#include "gba_system.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using LOTUSGBA::GbaSystem;
using Words = GbaSystem::MultiplayerDataWords;
constexpr uint32_t CONTROL = 0x04000128, SEND = 0x0400012A, DATA = 0x04000120;
void require(bool yes, const char *reason) { if (!yes) throw std::runtime_error(reason); }
uint16_t sum(const Words &words) {
    uint16_t value = 0;
    for (auto w : words) value = static_cast<uint16_t>(value + w);
    return value;
}
std::vector<uint8_t> probe() {
    std::vector<uint8_t> rom(0xC4, 0);
    rom[0] = 0x2E; rom[3] = 0xEA;
    rom[0xC0] = 0xFE; rom[0xC1] = rom[0xC2] = 0xFF; rom[0xC3] = 0xEA;
    return rom;
}
struct Endpoint {
    GbaSystem system{probe()};
    std::vector<Words> sent;
    unsigned raw = 0;
    bool allowDataBlockSend = true;
    Endpoint() {
        system.configureRemoteMultiplayerLink(false, true,
            [&](uint32_t seq, uint16_t word, uint8_t, uint64_t) {
                ++raw;
                require(word == 0x8FFF, "unexpected handshake word");
                require(system.receiveRemoteMultiplayerWord(seq, 0xB9A0), "handshake response rejected");
            }, {}, {}, {}, {}, [&](uint32_t seq, const Words &words, uint16_t checksum) {
                if (!allowDataBlockSend) return false;
                require(seq == sent.size() + 1, "block transport sequence skipped");
                require(checksum == sum(words), "wire integrity checksum is not own payload sum");
                sent.push_back(words);
                return true;
            });
        system.setRemoteMultiplayerBufferedProtocol(true);
        system.setRemoteMultiplayerEventPump([] {});
        system.write16(CONTROL, 0x2000); system.setRemoteMultiplayerReady(true);
        system.write16(SEND, 0x8FFF); system.write16(CONTROL, 0x6083);
        require(system.runFrame().success && system.remoteMultiplayerBufferedProtocolActive(),
                "handshake did not select buffered cable protocol");
    }
    uint16_t word(uint16_t local) {
        system.write16(SEND, local); system.write16(CONTROL, 0x6083);
        require(system.runFrame().success, "synthetic cable guest failed");
        require(!(system.read16(CONTROL) & 0xC0), "cable transfer stalled or reported checksum error");
        require(system.read16(DATA) == local, "local serial word was overwritten");
        return system.read16(DATA + 2);
    }
};
void phases() {
    Endpoint e;
    const Words local{1,2,3,4,5,6,7,8}, remote{11,22,33,44,55,66,77,88};
    require(e.system.receiveRemoteMultiplayerDataBlock(1, remote, sum(remote)), "peer block rejected");
    require(e.word(0) == 0, "first phase is not previous-group checksum");
    require(e.sent.empty(), "checksum phase was treated as payload");
    for (size_t i = 0; i < 8; ++i) require(e.word(local[i]) == remote[i], "data word phase shifted");
    require(e.sent.size() == 1 && e.sent[0] == local, "group not sent as eight intact words");
    const uint16_t combined = static_cast<uint16_t>(sum(local) + sum(remote));
    require(e.word(combined) == combined, "guest checksum did not include both participants");
    require(e.raw == 1, "data group fell back to per-word network handshake");
}
void delayed() {
    Endpoint e;
    const Words local{0xFFFF,2,3,4,5,6,7,8}, remote{91,92,93,94,95,96,97,98};
    require(e.word(0) == 0, "idle checksum wrong");
    for (size_t i = 0; i < 8; ++i) {
        if (i == 4) require(e.system.receiveRemoteMultiplayerDataBlock(1, remote, sum(remote)), "late block rejected");
        require(e.word(local[i]) == 0, "late block spliced into an in-progress group");
    }
    require(e.word(sum(local)) == sum(local), "idle-peer checksum did not include own words");
    for (size_t i = 0; i < 8; ++i) require(e.word(0) == remote[i], "late group was lost or reordered");
    require(e.word(sum(remote)) == sum(remote), "checksum for delayed peer group incorrect");
}
void idle() {
    Endpoint e;
    for (unsigned group = 0; group < 40; ++group)
        for (unsigned phase = 0; phase < 9; ++phase)
            require(e.word(0) == 0, "empty group invented data");
    require(e.sent.empty(), "idle groups consume network sequence/storage");
    require(e.raw == 1, "idle polling generated per-word WAN traffic");
}
void invalid() {
    Endpoint e;
    const Words payload{1,2,3,4,5,6,7,8};
    const auto badChecksum = e.system.receiveRemoteMultiplayerDataBlockDetailed(
        1, payload, 35);
    require(!badChecksum.accepted(), "bad wire checksum accepted");
    require(badChecksum.reason == GbaSystem::RemoteMultiplayerDataBlockRejectReason::ChecksumMismatch &&
            badChecksum.expectedSequence == 1 && badChecksum.queuedBlocks == 0 &&
            badChecksum.calculatedChecksum == 36,
            "bad checksum diagnostic omitted the rejection state");
    require(e.system.receiveRemoteMultiplayerDataBlock(1, payload, 36), "bad packet consumed sequence");
    const auto duplicate = e.system.receiveRemoteMultiplayerDataBlockDetailed(
        1, payload, 36);
    require(!duplicate.accepted(), "duplicate wire block accepted");
    require(duplicate.reason == GbaSystem::RemoteMultiplayerDataBlockRejectReason::SequenceMismatch &&
            duplicate.expectedSequence == 2,
            "sequence rejection diagnostic omitted the expected sequence");
}
void wireChecksumWraparound() {
    Endpoint e;
    const Words payload{0xFFFF, 2, 3, 4, 5, 6, 7, 8};
    require(e.system.receiveRemoteMultiplayerDataBlock(1, payload, sum(payload)),
            "valid wrapped wire checksum rejected");
    require(e.word(0) == 0, "wrapped packet changed initial game checksum");
    for (size_t i = 0; i < payload.size(); ++i)
        require(e.word(0) == payload[i], "wrapped packet payload changed in transit");
}
void gameChecksumMismatch() {
    Endpoint e;
    const Words local{1,2,3,4,5,6,7,8};
    const Words remote{11,22,33,44,55,66,77,88};
    require(e.system.receiveRemoteMultiplayerDataBlock(1, remote, sum(remote)),
            "peer block rejected");
    require(e.word(0) == 0, "initial checksum phase was not zero");
    for (uint16_t word : local)
        e.word(word);

    const uint16_t expected = static_cast<uint16_t>(sum(local) + sum(remote));
    e.system.write16(SEND, static_cast<uint16_t>(expected + 1));
    e.system.write16(CONTROL, 0x6083);
    require(e.system.runFrame().success, "checksum mismatch transfer failed");
    require(e.system.read16(DATA + 2) == expected,
            "peer checksum phase did not expose the accumulated group checksum");
    require((e.system.read16(CONTROL) & 0x40) != 0,
            "incorrect guest checksum did not set multiplayer error");
}
void queueBackpressureAndRecovery() {
    Endpoint e;
    const Words payload{1,2,3,4,5,6,7,8};
    for (uint32_t sequence = 1; sequence <= 64; ++sequence)
        require(e.system.receiveRemoteMultiplayerDataBlock(sequence, payload, sum(payload)),
                "block rejected before the queue reached its limit");
    const auto full = e.system.receiveRemoteMultiplayerDataBlockDetailed(
        65, payload, sum(payload));
    require(!full.accepted(), "block beyond the bounded queue was admitted");
    require(full.reason == GbaSystem::RemoteMultiplayerDataBlockRejectReason::QueueFull &&
            full.expectedSequence == 65 && full.queuedBlocks == 64 &&
            full.calculatedChecksum == sum(payload),
            "queue rejection diagnostic omitted depth or checksum state");
    require((e.system.read16(CONTROL) & 0x40) == 0,
            "temporary queue pressure became a guest-visible link error");

    require(e.word(0) == 0, "first buffered group checksum changed under pressure");
    for (uint16_t value : payload)
        require(e.word(0) == value, "queued payload changed while draining pressure");
    require(e.system.receiveRemoteMultiplayerDataBlock(65, payload, sum(payload)),
            "backpressured block was not accepted after a queue slot opened");
    require(e.word(sum(payload)) == sum(payload), "drain did not preserve the next group checksum");
    for (uint16_t value : payload)
        require(e.word(0) == value, "deferred block was lost or reordered after retry");
}
void senderBackpressureRetainsBlock() {
    Endpoint e;
    const Words first{1,2,3,4,5,6,7,8};
    const Words second{11,12,13,14,15,16,17,18};
    require(e.word(0) == 0, "first group checksum changed");
    for (uint16_t value : first) e.word(value);
    require(e.sent.size() == 1 && e.sent.front() == first,
            "initial block was not transmitted");

    e.allowDataBlockSend = false;
    require(e.word(sum(first)) == sum(first), "second group checksum changed");
    for (uint16_t value : second) e.word(value);
    const auto blocked = e.system.runFrame();
    require(blocked.success && blocked.cycles == 0,
            "guest kept advancing after the sender exhausted its credits");
    require(e.sent.size() == 1, "blocked block was sent without a credit");

    e.allowDataBlockSend = true;
    const auto resumed = e.system.runFrame();
    require(resumed.success && resumed.cycles > 0,
            "guest did not resume after send credit returned");
    require(e.sent.size() == 2 && e.sent.back() == second,
            "blocked block was lost or changed when send credit returned");
}
void device() {
    auto rom = probe(); rom[0xAC] = 'B'; rom[0xAD] = 'P'; rom[0xAE] = 'E'; rom[0xAF] = 'E';
    GbaSystem wireless(rom), cable(rom);
    cable.setRfuAdapterEnabled(false);
    const auto login = [](GbaSystem &s) {
        s.write16(0x04000134, 0x8000); s.write16(0x04000134, 0x80A0);
        s.write16(0x04000134, 0x80A2); s.write16(0x04000134, 0x80A0);
        s.write16(CONTROL, 0x5003); s.write16(0x04000134, 0);
        s.write32(DATA, 0x7FFF494E); s.write16(CONTROL, 0x5083);
        require(s.runFrame().success, "device probe failed"); return s.read32(DATA);
    };
    require(login(wireless) == 0, "wireless adapter bootstrap unavailable");
    require(login(cable) == 0xFFFFFFFF, "cable mode still exposes a wireless adapter");
    cable.reset();
    require(login(cable) == 0xFFFFFFFF, "reset lost the selected cable device");
}
void serialRestart() {
    GbaSystem parent(probe()), child(probe());
    unsigned starts = 0, replies = 0;
    parent.configureRemoteMultiplayerLink(false, true,
        [&](uint32_t seq, uint16_t word, uint8_t baud, uint64_t cycle) {
            ++starts;
            require(child.receiveRemoteMultiplayerStart(seq, word, baud, cycle), "restart start rejected");
        }, {}, {}, {}, {}, [](uint32_t, const Words &, uint16_t) { return true; });
    child.configureRemoteMultiplayerLink(true, true, {},
        [&](uint32_t seq, uint16_t word) {
            ++replies;
            require(parent.receiveRemoteMultiplayerWord(seq, word), "restart reply rejected");
        }, {}, {}, {}, [](uint32_t, const Words &, uint16_t) { return true; });
    for (auto *s : {&parent, &child}) {
        s->setRemoteMultiplayerBufferedProtocol(true);
        s->setRemoteMultiplayerEventPump([] {});
        s->write16(CONTROL, 0x6003);
        s->setRemoteMultiplayerReady(true);
    }
    const auto handshake = [&] {
        parent.write16(SEND, 0x8FFF); child.write16(SEND, 0xB9A0);
        parent.write16(CONTROL, 0x6083);
        require(child.runFrame().success && parent.runFrame().success, "restart handshake failed");
        require(parent.remoteMultiplayerBufferedProtocolActive() &&
            child.remoteMultiplayerBufferedProtocolActive(), "restart did not establish buffered protocol");
    };
    handshake();
    // Only the child runs autonomous pulses: local transfer counts diverge.
    child.write16(SEND, 0);
    for (unsigned i = 0; i < 3; ++i) require(child.runFrame().success, "child cadence failed");
    require(child.remoteSioTransferCount() > parent.remoteSioTransferCount(), "fixture did not diverge clocks");
    const Words old{0xBBBB, 1, 2, 3, 4, 5, 6, 7};
    require(parent.receiveRemoteMultiplayerDataBlock(1, old, sum(old)), "old queued block rejected");
    for (uint16_t word : {uint16_t{0}, uint16_t{0x7777}}) {
        parent.write16(SEND, word); parent.write16(CONTROL, 0x6083);
        require(parent.runFrame().success, "partial old group failed");
    }
    // The leader can restart first. An old-attempt child must answer an open
    // slot, rather than fail the network session or reset its own ROM parser.
    parent.write16(CONTROL, 0x2000);
    parent.write16(SEND, 0x8FFF);
    parent.write16(CONTROL, 0x6083);
    require(parent.runFrame().success && !(parent.read16(CONTROL) & 0x80), "staggered restart stalled");
    require(parent.read16(DATA + 2) == 0xFFFF && child.remoteMultiplayerBufferedProtocolActive(),
        "old-attempt child did not expose an open slot");
    // DisableSerial keeps multiplayer mode selected but resets the ROM's parser.
    for (auto *s : {&parent, &child}) {
        s->write16(CONTROL, 0x2000);
        s->write16(SEND, 0);
        require(!s->remoteMultiplayerBufferedProtocolActive(), "DisableSerial retained buffered parser");
    }
    const auto count = child.remoteSioTransferCount();
    require(child.runFrame().success && child.remoteSioTransferCount() == count,
        "disabled child continued autonomous pulses");
    require(parent.receiveRemoteMultiplayerDataBlock(2, old, sum(old)), "late old block broke wire sequence");
    for (auto *s : {&parent, &child}) s->write16(CONTROL, 0x6003);
    handshake();
    require(starts == 3 && replies == 3, "restart reused virtual transfer counts as wire sequence");
    parent.write16(SEND, 0); parent.write16(CONTROL, 0x6083);
    require(parent.runFrame().success && parent.read16(DATA + 2) == 0, "restart checksum was not reset");
    parent.write16(SEND, 0); parent.write16(CONTROL, 0x6083);
    require(parent.runFrame().success && parent.read16(DATA + 2) == 0, "old attempt leaked into new guest group");
    const Words fresh{0x2222, 9, 8, 7, 6, 5, 4, 3};
    require(parent.receiveRemoteMultiplayerDataBlock(3, fresh, sum(fresh)), "new attempt lost wire sequence");
    // Finish the empty group, then deliver the new command at its boundary.
    for (unsigned phase = 2; phase < 9; ++phase) {
        parent.write16(SEND, 0); parent.write16(CONTROL, 0x6083);
        require(parent.runFrame().success && parent.read16(DATA + 2) == 0, "new block spliced into empty group");
    }
    parent.write16(SEND, 0); parent.write16(CONTROL, 0x6083);
    require(parent.runFrame().success && parent.read16(DATA + 2) == 0, "new checksum boundary shifted");
    parent.write16(SEND, 0); parent.write16(CONTROL, 0x6083);
    require(parent.runFrame().success && parent.read16(DATA + 2) == fresh[0], "new command was discarded");
}
void childReceiveLifetime() {
    GbaSystem child(probe());
    std::vector<Words> sent;
    child.configureRemoteMultiplayerLink(true, true, {},
        [](uint32_t, uint16_t) {}, {}, {}, {},
        [&](uint32_t, const Words &words, uint16_t) { sent.push_back(words); return true; });
    child.setRemoteMultiplayerBufferedProtocol(true);
    child.setRemoteMultiplayerEventPump([] {});
    child.write16(CONTROL, 0x6003); child.setRemoteMultiplayerReady(true);
    child.write16(SEND, 0xB9A0);
    require(child.receiveRemoteMultiplayerStart(1, 0x8FFF, 3), "child handshake rejected");
    require(child.runFrame().success && child.remoteMultiplayerBufferedProtocolActive(), "child handshake failed");
    const Words peer{11,22,33,44,55,66,77,88}, local{1,2,3,4,5,6,7,8};
    require(child.receiveRemoteMultiplayerDataBlock(1, peer, sum(peer)), "peer command rejected");
    const auto next = [&](uint16_t outgoing) {
        const auto before = child.remoteSioTransferCount();
        // Model SerialCB/DoSend publishing the next word after reading the
        // previous result. The synthetic CPU deliberately has no IRQ handler.
        child.write16(SEND, outgoing);
        for (unsigned frame = 0; frame < 3 && child.remoteSioTransferCount() == before; ++frame)
            require(child.runFrame().success, "child pulse failed");
        require(child.remoteSioTransferCount() == before + 1,
            "child overwrote an unconsumed serial result");
        require(!(child.read16(CONTROL) & 0xC0), "child transfer failed");
        return child.read16(DATA);
    };
    require(next(0) == 0, "child initial checksum phase shifted");
    require(next(local[0]) == peer[0], "child first data word shifted");
    const auto before = child.remoteSioTransferCount();
    for (unsigned frame = 0; frame < 12; ++frame) require(child.runFrame().success, "paused guest failed");
    require(child.remoteSioTransferCount() == before && child.read16(DATA) == peer[0],
        "paused serial callback lost its receive registers");
    for (size_t i = 1; i < local.size(); ++i)
        require(next(local[i]) == peer[i], "resumed child data shifted");
    require(sent.size() == 1 && sent[0] == local, "paused child corrupted outgoing command");
    const auto checksum = static_cast<uint16_t>(sum(local) + sum(peer));
    require(next(checksum) == checksum, "resumed child checksum diverged");
}
void delayedHandshakeWithUninitializedPeer() {
    GbaSystem parent(probe()), child(probe());
    unsigned replies = 0;
    uint16_t reply = 0xFFFF;
    parent.configureRemoteMultiplayerLink(false, true,
        [&](uint32_t seq, uint16_t word, uint8_t baud, uint64_t elapsed) {
            require(child.receiveRemoteMultiplayerStart(seq, word, baud, elapsed),
                    "delayed handshake start rejected");
        }, {}, {});
    child.configureRemoteMultiplayerLink(true, true, {},
        [&](uint32_t seq, uint16_t word) {
            ++replies;
            reply = word;
            require(parent.receiveRemoteMultiplayerWord(seq, word),
                    "actual delayed handshake word rejected");
        }, {});
    parent.setRemoteMultiplayerBufferedProtocol(true);
    child.setRemoteMultiplayerBufferedProtocol(true);
    parent.setRemoteMultiplayerEventPump([] {});
    child.setRemoteMultiplayerEventPump([] {});
    parent.write16(CONTROL, 0x6003);
    child.write16(CONTROL, 0x6001);
    parent.setRemoteMultiplayerReady(true);
    child.setRemoteMultiplayerReady(true);
    child.write16(SEND, 0);
    parent.write16(SEND, 0xB9A0);
    parent.write16(CONTROL, 0x6083);
    for (unsigned i = 0; i < 4; ++i)
        require(parent.runFrame().success, "waiting for delayed peer failed");
    require((parent.read16(CONTROL) & 0x80) && replies == 0,
            "ready peer fabricated a handshake reply before its transfer");
    require(child.runFrame().success && parent.runFrame().success,
            "real zero reply failed");
    require(replies == 1 && reply == 0 && parent.read16(DATA + 2) == 0,
            "guest did not receive the peer's real uninitialized word");
    require(!(parent.read16(CONTROL) & 0xC0), "zero reply set serial error");
    child.write16(SEND, 0xB9A0);
    parent.write16(SEND, 0x8FFF);
    parent.write16(CONTROL, 0x6083);
    require(child.runFrame().success && parent.runFrame().success,
            "later handshake failed");
    require(replies == 2 && reply == 0xB9A0 &&
            parent.remoteMultiplayerBufferedProtocolActive() &&
            child.remoteMultiplayerBufferedProtocolActive(),
            "confirmed handshake did not activate buffered protocol");
}
void rawMasterDoesNotWaitOnPeerFrameWatermark() {
    GbaSystem parent(probe());
    parent.configureRemoteMultiplayerLink(false, true,
        [&](uint32_t seq, uint16_t, uint8_t, uint64_t) {
            require(parent.receiveRemoteMultiplayerWord(seq, 0), "raw reply rejected");
        }, {}, {});
    parent.setRemoteMultiplayerBufferedProtocol(true);
    parent.setRemoteMultiplayerEventPump([] {});
    parent.write16(CONTROL, 0x6003);
    parent.setRemoteMultiplayerReady(true, 0);
    parent.write16(SEND, 0xB9A0);
    parent.write16(CONTROL, 0x6083);
    require(parent.runFrame().success, "initial raw frame failed");
    const auto start = parent.remoteFrameCount();
    for (unsigned i = 0; i < 12; ++i)
        require(parent.runFrame().success, "idle master frame failed");
    require(parent.remoteFrameCount() >= start + 12,
            "idle master blocked on a peer frame watermark");
}
void rawChildRebasesAfterBufferedReset() {
    GbaSystem child(probe());
    unsigned replies = 0;
    child.configureRemoteMultiplayerLink(true, true, {},
        [&](uint32_t, uint16_t) { ++replies; }, {});
    child.setRemoteMultiplayerBufferedProtocol(true);
    child.setRemoteMultiplayerEventPump([] {});
    child.write16(CONTROL, 0x6001);
    child.setRemoteMultiplayerReady(true, 0);
    child.write16(SEND, 0xB9A0);
    require(child.receiveRemoteMultiplayerStart(1, 0x8FFF, 3),
            "initial handshake start rejected");
    require(child.runFrame().success && child.remoteMultiplayerBufferedProtocolActive(),
            "initial child handshake failed");
    for (unsigned i = 0; i < 30; ++i)
        require(child.runFrame().success, "buffered child frame failed");
    const auto beforeReset = child.remoteFrameCount();
    require(beforeReset >= 30, "buffered child did not run independently");
    // Emerald resets SIOCNT without leaving multiplayer mode. Peer frame
    // progress remains old until the next watermark arrives.
    child.write16(CONTROL, 0x2000);
    child.write16(CONTROL, 0x6001);
    require(!child.remoteMultiplayerBufferedProtocolActive(), "serial reset remained buffered");
    require(child.receiveRemoteMultiplayerStart(2, 0xB9A0, 3),
            "post-reset raw start rejected");
    require(child.runFrame().success && replies == 2,
            "post-reset raw transfer failed");
    require(child.remoteFrameCount() > beforeReset,
            "child reused the previous buffered epoch's pacing baseline");
}
void delayedHandshakeFrameLead(unsigned oneWayTicks) {
    GbaSystem parent(probe()), child(probe());
    enum class Kind { Start, Reply, ParentFrame, ChildFrame };
    struct Packet {
        Kind kind;
        unsigned due;
        uint32_t seq = 0;
        uint16_t word = 0;
        uint8_t baud = 0;
        uint64_t value = 0;
    };
    unsigned tick = 0;
    unsigned replies = 0;
    std::deque<Packet> packets;
    auto enqueue = [&](Packet packet) {
        packet.due = tick + oneWayTicks;
        packets.push_back(packet);
    };
    parent.configureRemoteMultiplayerLink(false, true,
        [&](uint32_t seq, uint16_t word, uint8_t baud, uint64_t elapsed) {
            enqueue({Kind::Start, 0, seq, word, baud, elapsed});
        }, {}, {}, [&](uint64_t frame) {
            enqueue({Kind::ParentFrame, 0, 0, 0, 0, frame});
        });
    child.configureRemoteMultiplayerLink(true, true, {},
        [&](uint32_t seq, uint16_t word) {
            enqueue({Kind::Reply, 0, seq, word, 0, 0});
        }, {}, [&](uint64_t frame) {
            enqueue({Kind::ChildFrame, 0, 0, 0, 0, frame});
        });
    parent.setRemoteMultiplayerBufferedProtocol(true);
    child.setRemoteMultiplayerBufferedProtocol(true);
    parent.setRemoteMultiplayerEventPump([] {});
    child.setRemoteMultiplayerEventPump([] {});
    parent.write16(CONTROL, 0x6003);
    child.write16(CONTROL, 0x6001);
    parent.setRemoteMultiplayerReady(true, 0);
    child.setRemoteMultiplayerReady(true, 0);
    child.write16(SEND, 0);
    parent.write16(SEND, 0xB9A0);
    parent.write16(CONTROL, 0x6083);

    uint64_t maxLead = 0;
    for (; tick < 500 && !parent.remoteMultiplayerBufferedProtocolActive(); ++tick) {
        while (!packets.empty() && packets.front().due <= tick) {
            const auto packet = packets.front();
            packets.pop_front();
            switch (packet.kind) {
            case Kind::Start:
                require(child.receiveRemoteMultiplayerStart(packet.seq, packet.word,
                    packet.baud, packet.value), "delayed start rejected");
                break;
            case Kind::Reply:
                require(parent.receiveRemoteMultiplayerWord(packet.seq, packet.word),
                    "delayed actual reply rejected");
                ++replies;
                if (replies == 1)
                    require(packet.word == 0, "initial peer zero was replaced");
                break;
            case Kind::ParentFrame:
                require(child.receiveRemoteMultiplayerFrame(packet.value), "parent frame rejected");
                break;
            case Kind::ChildFrame:
                require(parent.receiveRemoteMultiplayerFrame(packet.value), "child frame rejected");
                break;
            }
        }
        if (replies == 1)
            child.write16(SEND, 0xB9A0);
        if (replies && !(parent.read16(CONTROL) & 0x80)) {
            parent.write16(SEND, replies >= 12 ? 0x8FFF : 0xB9A0);
            parent.write16(CONTROL, 0x6083);
        }
        require(parent.runFrame().success && child.runFrame().success,
                "delayed handshake frame failed");
        if (!parent.remoteMultiplayerBufferedProtocolActive() &&
            !child.remoteMultiplayerBufferedProtocolActive() &&
            child.remoteFrameCount() > parent.remoteFrameCount())
            maxLead = std::max(maxLead, child.remoteFrameCount() - parent.remoteFrameCount());
    }
    std::cout << " delay=" << oneWayTicks << " ticks=" << tick
              << " replies=" << replies << " child_frame_lead=" << maxLead;
    require(replies >= 13 && parent.remoteMultiplayerBufferedProtocolActive() &&
            child.remoteMultiplayerBufferedProtocolActive(), "handshake did not complete");
    require(maxLead <= 24, "child guest ran too far ahead during delayed raw handshake");
    const uint64_t childBefore = child.remoteFrameCount();
    for (unsigned i = 0; i < 30; ++i)
        require(child.runFrame().success, "buffered child frame failed");
    require(child.remoteFrameCount() >= childBefore + 30,
            "handshake pacing incorrectly blocked buffered guest frames");
}
void bootstrapRace(bool cancel, bool peerOutsideMode) {
    GbaSystem parent(probe()), child(probe());
    unsigned replies = 0; uint16_t reply = 0;
    parent.configureRemoteMultiplayerLink(false, true,
        [&](uint32_t seq, uint16_t word, uint8_t baud, uint64_t) {
            require(child.receiveRemoteMultiplayerStart(seq, word, baud,
                500ull * GbaSystem::CYCLES_PER_FRAME), "bootstrap start rejected during mode probe");
        }, {}, {});
    child.configureRemoteMultiplayerLink(true, true, {},
        [&](uint32_t seq, uint16_t word) {
            ++replies; reply = word;
            require(parent.receiveRemoteMultiplayerWord(seq, word), "bootstrap reply rejected");
        }, {});
    parent.setRemoteMultiplayerBufferedProtocol(true); child.setRemoteMultiplayerBufferedProtocol(true);
    parent.setRemoteMultiplayerEventPump([] {}); child.setRemoteMultiplayerEventPump([] {});
    parent.write16(CONTROL, 0x2000); child.write16(CONTROL, peerOutsideMode ? 0x5003 : 0x6001);
    parent.setRemoteMultiplayerReady(true); child.setRemoteMultiplayerReady(true);
    parent.write16(SEND, 0); child.write16(SEND, 0);
    parent.write16(CONTROL, 0x6083);
    if (cancel) child.write16(CONTROL, 0x5003);
    require(child.runFrame().success && parent.runFrame().success, "bootstrap frame failed");
    require(replies == 1, "bootstrap start lost or scheduled against unrelated guest entry time");
    require(reply == (cancel || peerOutsideMode ? 0xFFFF : 0), "incorrect interrupted bootstrap response");
    require(!(parent.read16(CONTROL) & 0x80), "leader stayed blocked after bootstrap mode race");
    // The next discovery request must still work after the open-slot reply.
    child.write16(CONTROL, 0x6001);
    child.write16(SEND, 0xB9A0);
    parent.write16(SEND, 0);
    parent.write16(CONTROL, 0x6083);
    require(child.runFrame().success && parent.runFrame().success, "discovery retry failed");
    require(replies == 2 && reply == 0xB9A0, "retry lost the child's discovery word");
    require(!parent.remoteMultiplayerBufferedProtocolActive() &&
        !child.remoteMultiplayerBufferedProtocolActive(), "discovery alone established the link");
    parent.write16(SEND, 0x8FFF);
    parent.write16(CONTROL, 0x6083);
    for (unsigned polls = 0; polls < 8; ++polls)
        require(parent.runFrame().success, "waiting for delayed handshake failed");
    require(replies == 2 && (parent.read16(CONTROL) & 0x80) &&
        !parent.remoteMultiplayerBufferedProtocolActive(), "handshake finished before the peer answered");
    require(child.runFrame().success && parent.runFrame().success, "final handshake frame failed");
    require(replies == 3 && parent.remoteMultiplayerBufferedProtocolActive() &&
        child.remoteMultiplayerBufferedProtocolActive(), "retry did not reach buffered mode at both endpoints");
}
}
int main(int argc, char **argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    unsigned passed = 0, failed = 0;
    const auto run = [&](const char *name, const std::function<void()> &test) {
        if (!filter.empty() && filter != name) return;
        try { test(); ++passed; std::cout << "PASS " << name << std::endl; }
        catch (const std::exception &e) { ++failed; std::cout << "FAIL " << name << ": " << e.what() << std::endl; }
    };
    run("checksum_and_data_phases", phases); run("delayed_complete_group", delayed);
    run("idle_without_network_roundtrips", idle); run("wire_checksum_and_sequence", invalid);
    run("wire_checksum_wraparound", wireChecksumWraparound);
    run("game_checksum_mismatch", gameChecksumMismatch);
    run("data_block_queue_backpressure", queueBackpressureAndRecovery);
    run("data_block_sender_backpressure", senderBackpressureRetainsBlock);
    run("selected_device_and_reset", device);
    run("serial_restart_with_independent_clocks", serialRestart);
    run("child_receive_result_lifetime", childReceiveLifetime);
    run("independent_bootstrap_clocks", [] { bootstrapRace(false, false); });
    run("delayed_handshake_uninitialized_peer", delayedHandshakeWithUninitializedPeer);
    run("raw_master_no_peer_frame_gate", rawMasterDoesNotWaitOnPeerFrameWatermark);
    run("raw_child_rebases_after_reset", rawChildRebasesAfterBufferedReset);
    run("handshake_frame_lead_134ms", [] { delayedHandshakeFrameLead(4); });
    run("handshake_frame_lead_200ms", [] { delayedHandshakeFrameLead(6); });
    run("cancelled_bootstrap_start", [] { bootstrapRace(true, false); });
    run("bootstrap_during_peer_probe", [] { bootstrapRace(false, true); });
    std::cout << "Cable tests: " << passed << " passed, " << failed << " failed\n";
    return failed || !passed ? 1 : 0;
}
