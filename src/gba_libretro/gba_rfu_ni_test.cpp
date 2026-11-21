// Original synthetic NI guest driver; no game ROM, saves or captured game data.
// Protocol oracle: pokeemerald librfu_rfu.c's NI header/phase contracts, described
// in docs/gba-rfu-protocol.md. This is not execution of Nintendo's RFU library.
#include "gba_system.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using LOTUSGBA::GbaSystem;
using Bytes = std::vector<uint8_t>;
constexpr uint32_t DATA = 0x04000120, CONTROL = 0x04000128;
void require(bool value, const std::string &why) {
    if (!value) throw std::runtime_error(why);
}
std::vector<uint32_t> words(const Bytes &bytes) {
    std::vector<uint32_t> result((bytes.size() + 3) / 4, 0);
    for (size_t i = 0; i < bytes.size(); ++i)
        result[i / 4] |= uint32_t{bytes[i]} << ((i % 4) * 8);
    return result;
}
Bytes bytes(const std::vector<uint32_t> &input, size_t count) {
    require(count <= input.size() * 4, "receive metadata exceeds serial response");
    Bytes result;
    for (size_t i = 0; i < count; ++i)
        result.push_back(static_cast<uint8_t>(input[i / 4] >> ((i % 4) * 8)));
    return result;
}
struct Frame {
    unsigned state = 0, phase = 0, sequence = 0;
    bool ack = false;
    Bytes payload;
};
Bytes encode(const Frame &frame, bool parent) {
    require(frame.state <= 4 && frame.phase < 4 && frame.sequence < 4,
            "oracle header field outside range");
    const unsigned headerBytes = parent ? 3 : 2;
    require(frame.payload.size() <= (parent ? 127u : 31u), "oracle payload too big");
    uint32_t header = static_cast<uint32_t>(frame.payload.size());
    header |= frame.state << (parent ? 14 : 10);
    header |= unsigned(frame.ack) << (parent ? 13 : 9);
    header |= frame.phase << (parent ? 9 : 5);
    header |= frame.sequence << (parent ? 11 : 7);
    if (parent) header |= 1u << 18; // child slot zero
    Bytes result;
    for (unsigned i = 0; i < headerBytes; ++i)
        result.push_back(static_cast<uint8_t>(header >> (i * 8)));
    result.insert(result.end(), frame.payload.begin(), frame.payload.end());
    return result;
}
std::vector<Frame> decode(const Bytes &input, bool parent) {
    const size_t headerBytes = parent ? 3 : 2;
    std::vector<Frame> result;
    size_t offset = 0;
    while (offset + headerBytes <= input.size()) {
        uint32_t header = 0;
        for (size_t i = 0; i < headerBytes; ++i)
            header |= uint32_t{input[offset + i]} << (i * 8);
        const size_t count = header & (parent ? 127u : 31u);
        require(offset + headerBytes + count <= input.size(), "truncated NI frame");
        if (parent) require(((header >> 18) & 15) == 1, "wrong NI destination slot");
        Frame frame;
        frame.state = (header >> (parent ? 14 : 10)) & 15;
        frame.ack = ((header >> (parent ? 13 : 9)) & 1) != 0;
        frame.phase = (header >> (parent ? 9 : 5)) & 3;
        frame.sequence = (header >> (parent ? 11 : 7)) & 3;
        frame.payload.assign(input.begin() + static_cast<ptrdiff_t>(offset + headerBytes),
                             input.begin() + static_cast<ptrdiff_t>(offset + headerBytes + count));
        result.push_back(std::move(frame));
        offset += headerBytes + count;
    }
    // The one-byte parent idle prefix has no complete LLSF header.
    require(offset == input.size() || (offset == 0 && input.size() == 1),
            "unexpected trailing NI bytes");
    return result;
}
Bytes identity() {
    Bytes value(26);
    value[0] = 0x34; value[1] = 0x12; // synthetic permitted serial number
    for (size_t i = 2; i < value.size(); ++i)
        value[i] = static_cast<uint8_t>(0x40 + i);
    return value;
}
class Sender {
public:
    explicit Sender(Bytes value) : value_(std::move(value)) {}
    Frame next() {
        if (state_ == 1) return {1, 0, 1, false,
            {1, 12, 0, static_cast<uint8_t>(value_.size()), 0, 0, 0}};
        if (state_ == 3) return {3, 0, 0, false, {}};
        if (state_ == 0) return {0, 0, 1, false, {}};
        for (unsigned i = 0; i < 4; ++i) {
            const unsigned phase = nextPhase_++ % 4;
            if (offset_[phase] >= value_.size()) continue;
            const size_t end = std::min(offset_[phase] + 12, value_.size());
            return {2, phase, sequence_[phase], false,
                Bytes(value_.begin() + static_cast<ptrdiff_t>(offset_[phase]),
                      value_.begin() + static_cast<ptrdiff_t>(end))};
        }
        throw std::runtime_error("sender has no outstanding data but is not in END");
    }
    void acknowledge(const Frame &frame) {
        if (!frame.ack || !frame.payload.empty()) return;
        if (state_ == 1 && frame.state == 1 && frame.phase == 0 && frame.sequence == 1) {
            state_ = 2;
        } else if (state_ == 2 && frame.state == 2 &&
                   offset_[frame.phase] < value_.size() &&
                   frame.sequence == sequence_[frame.phase]) {
            offset_[frame.phase] += 48;
            sequence_[frame.phase] = (sequence_[frame.phase] + 1) & 3;
            if (std::all_of(offset_.begin(), offset_.end(),
                           [&](size_t at) { return at >= value_.size(); })) state_ = 3;
        } else if (state_ == 3 && frame.state == 3 && frame.phase == 0 && frame.sequence == 0) {
            state_ = 0;
        }
    }
    void accepted(const Frame &frame) { if (frame.state == 0) done = true; }
    unsigned state() const { return state_; }
    bool done = false;
private:
    Bytes value_;
    unsigned state_ = 1, nextPhase_ = 0;
    std::array<size_t, 4> offset_{{0, 12, 24, 36}};
    std::array<unsigned, 4> sequence_{{1, 1, 1, 1}};
};
class Receiver {
public:
    void receive(const Frame &frame) {
        require(!frame.ack, "child sent an unexpected NI ACK");
        if (frame.state == 1) {
            require(frame.phase == 0 && frame.sequence == 1 && frame.payload.size() == 7,
                    "invalid NI identity control frame");
            require(frame.payload[0] == 1 && frame.payload[1] == 12 && frame.payload[2] == 0,
                    "wrong NI identity data type/chunk size");
            const uint32_t length = uint32_t{frame.payload[3]} |
                (uint32_t{frame.payload[4]} << 8) | (uint32_t{frame.payload[5]} << 16) |
                (uint32_t{frame.payload[6]} << 24);
            require(length == 26, "wrong NI identity length");
            if (!started_) { content.assign(length, 0); copies.assign(length, 0); started_ = true; }
        } else if (frame.state == 2) {
            require(started_, "NI data arrived before control frame");
            const unsigned phase = frame.phase;
            if (frame.sequence == ((sequence_[phase] + 1) & 3)) {
                require(offset_[phase] + frame.payload.size() <= content.size(), "NI receive overflow");
                for (size_t i = 0; i < frame.payload.size(); ++i) {
                    content[offset_[phase] + i] = frame.payload[i];
                    ++copies[offset_[phase] + i];
                }
                offset_[phase] += 48;
                sequence_[phase] = frame.sequence;
            } else {
                require(frame.sequence == sequence_[phase], "out-of-order NI sequence");
                ++duplicates;
            }
        } else if (frame.state == 3) {
            require(std::all_of(copies.begin(), copies.end(), [](unsigned n) { return n == 1; }) &&
                    copies.size() == 26, "END before complete identity");
            end_ = true;
        } else if (frame.state == 0) {
            require(end_, "NULL before END");
            complete = true;
            accepted = content[0] == 0x34 && content[1] == 0x12;
            return;
        } else throw std::runtime_error("unexpected NI link state");
        reply = Frame{frame.state, frame.phase, frame.sequence, true, {}};
    }
    Bytes content;
    std::vector<unsigned> copies;
    std::optional<Frame> reply;
    unsigned duplicates = 0;
    bool complete = false, accepted = false;
private:
    bool started_ = false, end_ = false;
    std::array<size_t, 4> offset_{{0, 12, 24, 36}};
    std::array<unsigned, 4> sequence_{{0, 0, 0, 0}};
};
std::vector<uint8_t> probeRom() {
    std::vector<uint8_t> rom(0xC4, 0);
    const auto store = [&](size_t at, uint32_t word) {
        for (unsigned i = 0; i < 4; ++i) rom[at + i] = static_cast<uint8_t>(word >> (i * 8));
    };
    store(0, 0xEA00002E); store(0xC0, 0xEAFFFFFE);
    rom[0xAC] = 'B'; rom[0xAD] = 'P'; rom[0xAE] = 'E'; rom[0xAF] = 'E';
    return rom;
}
void step(GbaSystem &system) { require(system.runFrame().success, system.fault()); }
uint32_t transfer(GbaSystem &system, uint32_t word) {
    system.write32(DATA, word); system.write16(CONTROL, 0x5083); step(system);
    return system.read32(DATA);
}
void boot(GbaSystem &system) {
    system.write16(0x04000134, 0x8000); system.write16(0x04000134, 0x80A0);
    system.write16(0x04000134, 0x80A2); system.write16(0x04000134, 0x80A0);
    system.write16(CONTROL, 0x5003); system.write16(0x04000134, 0);
    const uint32_t input[] = {0x7FFF494E, 0xFFFF494E, 0xB6B1494E, 0xB6B1544E,
        0xABB1544E, 0xABB14E45, 0xB1BA4E45, 0xB1BA4F44, 0xB0BB4F44, 0xB0BB8001};
    const uint32_t output[] = {0, 0x494EB6B1, 0x494EB6B1, 0x544EB6B1,
        0x544EABB1, 0x4E45ABB1, 0x4E45B1BA, 0x4F44B1BA, 0x4F44B0BB, 0x8001B0BB};
    for (unsigned i = 0; i < 10; ++i) require(transfer(system, input[i]) == output[i], "adapter login failed");
}
struct Response { uint32_t ack; std::vector<uint32_t> payload; };
Response command(GbaSystem &system, uint8_t id, const std::vector<uint32_t> &payload = {}) {
    require(transfer(system, LOTUSGBA::makeGbaRfuCommandHeader(id,
        static_cast<uint8_t>(payload.size()))) == 0x80000000, "command header collided with event/response");
    for (uint32_t word : payload) require(transfer(system, word) == 0x80000000, "command payload collision");
    Response result{transfer(system, 0x80000000), {}};
    require((result.ack & 0xFFFF0000) == 0x99660000, "missing local command ACK");
    for (unsigned i = 0; i < ((result.ack >> 8) & 255); ++i)
        result.payload.push_back(transfer(system, 0x80000000));
    return result;
}
void arm(GbaSystem &system) {
    system.write16(CONTROL, 0x500B); system.write16(CONTROL, 0x5002);
    system.write32(DATA, 0x80000000); system.write16(CONTROL, 0x5082);
}
bool event(GbaSystem &system) {
    if (system.read16(CONTROL) & 0x80) return false;
    require(system.read32(DATA) == 0x99660028, "expected radio event 0x28");
    system.write16(CONTROL, 0x500A); system.write16(CONTROL, 0x5002);
    system.write32(DATA, 0x996600A8); system.write16(CONTROL, 0x5082); step(system);
    require(system.read32(DATA) == 0x80000000, "subsequent-transfer A8 failed");
    system.write16(CONTROL, 0x500A); system.write16(CONTROL, 0x5003);
    return true;
}
Bytes receive(GbaSystem &system, bool parent) {
    const auto result = command(system, 0x26);
    require((result.ack & 255) == 0xA6, "receive command rejected");
    if (result.payload.empty()) return {};
    const size_t count = parent ? ((result.payload[0] >> 8) & 31) : result.payload[0] & 127;
    return bytes(std::vector<uint32_t>(result.payload.begin() + 1, result.payload.end()), count);
}
struct Packet { unsigned due; uint32_t seq; uint8_t count; std::vector<uint32_t> data; };
struct Wire {
    unsigned delay = 0, creditDelay = 0, jitter = 0, now = 0, peak = 0, window = 1;
    std::deque<uint32_t> active;
    std::deque<std::pair<uint32_t, unsigned>> credits;
    bool occupied = false, creditPending = false;
    unsigned creditDue = 0;
    uint32_t inFlightSequence = 0;
    std::deque<Packet> packets;
    bool send(uint32_t seq, uint8_t count, const std::vector<uint32_t> &data) {
        if (window > 1) {
            if (active.size() >= window) return false;
            active.push_back(seq);
            const unsigned variation = jitter ? (seq * 7) % (jitter + 1) : 0;
            packets.push_back({now + delay + variation, seq, count, data});
            peak = std::max(peak, static_cast<unsigned>(active.size()));
            return true;
        }
        if (occupied) return false;
        occupied = true;
        inFlightSequence = seq;
        const unsigned variation = jitter ? (seq * 7) % (jitter + 1) : 0;
        packets.push_back({now + delay + variation, seq, count, data});
        peak = std::max(peak, static_cast<unsigned>(packets.size()));
        return true;
    }
    void accepted(uint32_t sequence) {
        if (window > 1) {
            const size_t pending = credits.size();
            require(pending < active.size() && active[pending] == sequence,
                    "duplicate or unordered window credit");
            credits.push_back({sequence, now + creditDelay});
            return;
        }
        require(occupied && !creditPending && sequence == inFlightSequence,
                "duplicate or mismatched transport admission ACK");
        creditPending = true;
        creditDue = now + creditDelay;
    }
    void tick(GbaSystem &destination, unsigned at) {
        now = at;
        while (!credits.empty() && credits.front().second <= now) {
            require(!active.empty() && active.front() == credits.front().first,
                    "window credit ownership changed");
            active.pop_front(); credits.pop_front();
        }
        if (creditPending && creditDue <= now) { occupied = false; creditPending = false; }
        if (packets.empty() || packets.front().due > now) return;
        const auto &p = packets.front();
        const auto result = destination.receiveRemoteRfuData(p.seq, p.count, p.data);
        if (result == GbaSystem::RfuDataReceiveResult::QueueFull) return;
        require(result == GbaSystem::RfuDataReceiveResult::Accepted, "wire sequence/admission failure");
        packets.pop_front();
    }
};
struct Scenario { unsigned down = 0, up = 0, credit = 0, jitter = 0, limit = 240;
    bool loseAck = false;
    unsigned parentPeriod = 1; };
class Pair {
public:
    std::unique_ptr<GbaSystem> parent{std::make_unique<GbaSystem>(probeRom())};
    std::unique_ptr<GbaSystem> child{std::make_unique<GbaSystem>(probeRom())};
    Wire down, up;
    unsigned events = 0, buffered = 0, rejected = 0;
    explicit Pair(Scenario s, GbaSystem::RfuCompletionMode mode = GbaSystem::RfuCompletionMode::RemoteReply) {
        down.delay = s.down; up.delay = s.up;
        down.creditDelay = up.creditDelay = s.credit;
        down.jitter = up.jitter = s.jitter;
        boot(*parent); boot(*child);
        parent->configureRemoteRfuLink(1, 2,
            [&](uint32_t seq, uint16_t id, const GbaSystem::RfuAdvertisement &value) {
                require(child->receiveRemoteRfuAdvertisement(seq, id, value), "parent advertisement failed");
            }, [](uint32_t, uint16_t, uint16_t) {},
            [&](uint32_t seq, uint8_t n, const std::vector<uint32_t> &value) { return down.send(seq, n, value); },
            [&](uint32_t seq) { up.accepted(seq); }, {}, mode);
        child->configureRemoteRfuLink(2, 1,
            [&](uint32_t seq, uint16_t id, const GbaSystem::RfuAdvertisement &value) {
                require(parent->receiveRemoteRfuAdvertisement(seq, id, value), "child advertisement failed");
            }, [&](uint32_t seq, uint16_t source, uint16_t target) {
                require(parent->receiveRemoteRfuConnectionRequest(seq, source, target), "parent rejected connection");
                require(child->receiveRemoteRfuConnectionResult(seq, source, 0, true), "child rejected result");
            }, [&](uint32_t seq, uint8_t n, const std::vector<uint32_t> &value) { return up.send(seq, n, value); },
            [&](uint32_t seq) { down.accepted(seq); }, {}, mode);
        for (auto *system : {parent.get(), child.get()})
            system->setRfuDiagnosticLogger([&](const std::string &line) {
                if (line.find("local TX buffered") != std::string::npos) ++buffered;
                if (line.find("result=rejected") != std::string::npos) ++rejected;
            });
        require(command(*parent, 0x19).ack == 0x99660099, "parent search failed");
        require(command(*child, 0x1C).ack == 0x9966009C, "child search failed");
        require(command(*child, 0x1D).payload.front() == 1, "wrong parent candidate ID");
        require(command(*child, 0x1F, {1}).ack == 0x9966009F, "connect command rejected");
        require(command(*child, 0x21).ack == 0x996601A1, "connect finalization failed");
    }
    void exchange(const Scenario &scenario, const Bytes &value, bool shouldAccept = true) {
        Sender sender(value); Receiver receiver;
        require(command(*child, 0x27).ack == 0x996600A7, "initial clock change failed");
        arm(*child);
        bool lost = false;
        const uint64_t parentCycles = parent->emulatedCycles(), childCycles = child->emulatedCycles();
        for (unsigned tick = 0; tick < scenario.limit; ++tick) {
            down.now = up.now = tick;
            down.tick(*child, tick); up.tick(*parent, tick);
            step(*parent); step(*child);
            for (const auto &frame : decode(receive(*parent, true), false)) receiver.receive(frame);
            const Bytes tx = receiver.reply ? encode(*receiver.reply, true) : Bytes{};
            std::vector<uint32_t> parameters{tx.empty() ? 1u : static_cast<uint32_t>(tx.size())};
            const auto body = words(tx); parameters.insert(parameters.end(), body.begin(), body.end());
            if (tick % scenario.parentPeriod == 0)
                require(command(*parent, 0x24, parameters).ack == 0x996600A4,
                        "parent NI retry/update rejected while local TX pending");
            if (event(*child)) {
                ++events;
                for (const auto &ack : decode(receive(*child, false), true)) {
                    if (scenario.loseAck && !lost && ack.ack && ack.state == 2) { lost = true; continue; }
                    sender.acknowledge(ack);
                }
                if (!sender.done) {
                    const Frame frame = sender.next();
                    const auto encoded = encode(frame, false);
                    auto payload = words(encoded);
                    payload.insert(payload.begin(), static_cast<uint32_t>(encoded.size()) << 8);
                    require(command(*child, 0x25, payload).ack == 0x996600A5,
                            "child NI command ACK delayed or rejected");
                    sender.accepted(frame);
                } else require(command(*child, 0x27).ack == 0x996600A7, "final slave rearm failed");
                arm(*child);
            }
            require(down.peak <= 1 && up.peak <= 1, "transport storage grew without bound");
            if (receiver.complete && sender.done) {
                require(receiver.content == value, "identity bytes corrupted");
                require(receiver.accepted == shouldAccept, "serial acceptance mismatch");
                require(parent->emulatedCycles() > parentCycles && child->emulatedCycles() > childCycles,
                        "one guest CPU did not progress");
                if (scenario.loseAck) require(lost, "lost-ACK case never injected loss");
                std::cout << " ticks=" << tick + 1 << " events=" << events
                          << " duplicates=" << receiver.duplicates << " buffered=" << buffered;
                return;
            }
        }
        throw std::runtime_error("NI identity timeout: events=" + std::to_string(events) +
            " sender_state=" + std::to_string(sender.state()) + " rejected=" + std::to_string(rejected));
    }
};
// Original guest-side model of the leader's completion/retry contract. This
// counts main-loop opportunities separately from real peer acknowledgements.
uint32_t finishParentEvent(GbaSystem &system, const std::function<void()> &afterHeader = {}) {
    require((system.read16(CONTROL) & 0x80) == 0, "parent event still pending");
    const uint32_t header = system.read32(DATA);
    require(header == 0x99660128 || header == 0x99660028, "invalid parent event header");
    const auto clock = [&](uint32_t word) {
        system.write16(CONTROL, 0x500A); system.write16(CONTROL, 0x5002);
        system.write32(DATA, word); system.write16(CONTROL, 0x5082); step(system);
        require((system.read16(CONTROL) & 0x80) == 0, "adapter body/ACK did not finish");
        return system.read32(DATA);
    };
    if (afterHeader) afterHeader();
    const uint32_t mask = header == 0x99660128 ? clock(0x80000000) : 1;
    require(clock(0x996600A8) == 0x80000000, "parent A8 did not restore clock");
    system.write16(CONTROL, 0x500A); system.write16(CONTROL, 0x5003);
    return mask;
}
uint32_t parentPoll(GbaSystem &system, const std::function<void()> &afterHeader = {}) {
    arm(system); step(system);
    require((system.read16(CONTROL) & 0x80) == 0,
            "leader is still held waiting for the network reply");
    require(system.read32(DATA) == 0x99660128,
            "parent event must carry an explicit ACK mask, not implicit success");
    return finishParentEvent(system, afterHeader);
}
// Synthetic UNI command rows, no captured game payloads.
Bytes parentUni(uint16_t command, uint16_t argument = 0) {
    Bytes payload(70, 0);
    payload[0] = static_cast<uint8_t>(command); payload[1] = static_cast<uint8_t>(command >> 8);
    payload[2] = static_cast<uint8_t>(argument); payload[3] = static_cast<uint8_t>(argument >> 8);
    return encode({4, 0, 0, false, payload}, true);
}
std::vector<uint32_t> parentParameters(const Bytes &payload) {
    if (payload.empty()) return {1};
    auto value = words(payload); value.insert(value.begin(), static_cast<uint32_t>(payload.size()));
    return value;
}
// Source-derived guest flow: a fixed local fade delay, a busy block-send
// callback, conditional standby installation, then unconditional UI advance.
// This reproduces the timing dependency, not commercial ROM execution.
Bytes blockInit(unsigned count);
void tradeHandoff(GbaSystem::RfuCompletionMode mode, bool idleGap) {
    Pair pair({4, 5, 2, 1}, mode);
    std::vector<Bytes> frames{blockInit(2), parentUni(0x8900, 0x1234),
                              parentUni(0x8901, 0x5678), Bytes{}};
    if (idleGap) frames.insert(frames.begin() + 1, Bytes{});
    if (mode == GbaSystem::RfuCompletionMode::GroupedCommands) {
        require(command(*pair.child, 0x27).ack == 0x996600A7, "handoff child clock failed");
        arm(*pair.child);
        pair.down.window = pair.up.window = 32;
    }
    unsigned delivered = 0, mainFrames = 0;
    bool waiting = false, needResume = false, senderBusy = true;
    bool standbyInstalled = false, attemptedHandoff = false, animation = false;
    for (unsigned tick = 0; tick < 240; ++tick) {
        pair.down.now = pair.up.now = tick;
        pair.down.tick(*pair.child, tick); pair.up.tick(*pair.parent, tick);
        step(*pair.child); step(*pair.parent);
        if (mode == GbaSystem::RfuCompletionMode::GroupedCommands && event(*pair.child)) {
            receive(*pair.child, false);
            require(command(*pair.child, 0x27).ack == 0x996600A7, "handoff child rearm failed");
            arm(*pair.child);
        }
        if (!waiting) {
            const auto reply = command(*pair.parent, needResume ? 0x37 : 0x25,
                needResume ? std::vector<uint32_t>{} : parentParameters(frames.at(delivered)));
            require((reply.ack & 255) == (needResume ? 0xB7 : 0xA5), "handoff send/retry rejected");
            arm(*pair.parent); waiting = true; step(*pair.parent);
        }
        if (pair.parent->read16(CONTROL) & 0x80) continue;
        const uint32_t ack = finishParentEvent(*pair.parent);
        require(receive(*pair.parent, true).empty(), "empty child fabricated data");
        waiting = false; needResume = ack == 0;
        if (ack) { ++delivered; senderBusy = delivered != frames.size(); }
        ++mainFrames;
        if (!attemptedHandoff && mainFrames >= frames.size()) {
            // Rfu_SetLinkStandbyCallback installs only if the old callback is NULL.
            standbyInstalled = !senderBusy; attemptedHandoff = true;
        } else if (attemptedHandoff && !senderBusy && !standbyInstalled) animation = true;
        if (!senderBusy) {
            require(standbyInstalled && !animation,
                    "standby callback skipped while block sender busy; UI can enter animation alone");
            require(pair.down.peak <= pair.down.window && pair.up.peak <= pair.up.window, "handoff queue grew");
            std::cout << " delivered=" << delivered << " main_frames=" << mainFrames;
            return;
        }
    }
    throw std::runtime_error("handoff fixture timed out");
}
Bytes guardedRound(Pair &pair, const Bytes &payload, bool mustWait, unsigned at) {
    pair.down.now = pair.up.now = at;
    pair.down.tick(*pair.child, at); pair.up.tick(*pair.parent, at);
    require(command(*pair.parent, 0x25, parentParameters(payload)).ack == 0x996600A5,
            "guarded phase send rejected");
    arm(*pair.parent); step(*pair.parent);
    if (mustWait) require((pair.parent->read16(CONTROL) & 0x80) != 0,
                          "critical phase advanced a local main-loop frame before peer reply");
    else require(finishParentEvent(*pair.parent) == 0, "key phase failed to allow a local attempt");
    pair.down.now = pair.up.now = at + 1;
    pair.down.tick(*pair.child, at + 1); pair.up.tick(*pair.parent, at + 1);
    if (!mustWait) {
        require(command(*pair.parent, 0x37).ack == 0x996600B7, "key phase resume failed");
        arm(*pair.parent);
    }
    step(*pair.parent);
    require(finishParentEvent(*pair.parent) == 1, "real peer reply did not finish guarded cycle");
    return receive(*pair.parent, true);
}
Bytes blockInit(unsigned count) {
    Bytes payload(70, 0); payload[1] = 0x88;
    payload[2] = static_cast<uint8_t>(count); payload[4] = 0x80;
    return encode({4, 0, 0, false, payload}, true);
}
void commandGroupPlan() {
    using Group = LOTUSGBA::GbaRfuCommandGroup;
    Group group;
    const auto append = [&](const Bytes &p) { return group.append(static_cast<uint8_t>(p.size()), words(p)); };
    require(append(blockInit(2)) == Group::Decision::BufferedPrefix,
            "block initializer still waits for a per-command peer round trip");
    require(append(blockInit(2)) == Group::Decision::BufferedPrefix, "repeated initializer not retained");
    require(append(parentUni(0x8900, 0x1234)) == Group::Decision::BufferedPrefix, "first block fragment waits too early");
    require(append(parentUni(0x8901, 0x5678)) == Group::Decision::WaitForPeer && group.awaitingFence(),
            "last fragment bypassed cumulative confirmation fence");
    require(append(parentUni(0xBE00)) == Group::Decision::WaitForPeer, "new phase bypassed an unfinished fence");
    group.completeFence();
    require(append(parentUni(0xBE00)) == Group::Decision::WaitForPeer, "key traffic outside a group completed speculatively");
    group.completeFence();
    require(append(parentUni(0x6600, 4)) == Group::Decision::WaitForPeer, "readiness was acknowledged speculatively");
    group.completeFence();
    require(append(blockInit(2)) == Group::Decision::BufferedPrefix, "new group failed");
    require(append(parentUni(0x8901)) == Group::Decision::WaitForPeer, "missing fragment was ignored");
    group.reset();
    require(append(blockInit(Group::MAX_BLOCKS + 1)) == Group::Decision::WaitForPeer, "oversized group entered pipeline");
    group.reset();
    require(append(blockInit(2)) == Group::Decision::BufferedPrefix, "bounded group init failed");
    unsigned buffered = 1;
    while (append(blockInit(2)) == Group::Decision::BufferedPrefix) {
        ++buffered; require(buffered <= Group::MAX_COMMANDS, "group storage/credit grew without bound");
    }
    require(group.awaitingFence(), "excess retries did not stop at a fence");
}
void groupedDelivery(Scenario scenario, unsigned window, unsigned blocks = 2, unsigned initRepeats = 1) {
    Pair pair(scenario, GbaSystem::RfuCompletionMode::GroupedCommands);
    pair.down.window = pair.up.window = window;
    const auto childToken = [](unsigned number) {
        Bytes data(14, 0); data[0] = static_cast<uint8_t>((number & 7) << 5); data[1] = 0xBE;
        data[2] = static_cast<uint8_t>(number); data[3] = 0x60;
        return encode({4, 0, 0, false, data}, false);
    };
    const auto scheduleChild = [&](const Bytes &frame, uint8_t id) {
        auto payload = words(frame);
        payload.insert(payload.begin(), static_cast<uint32_t>(frame.size()) << 8);
        require((command(*pair.child, id, payload).ack & 255) == (id | 0x80), "child group reply schedule failed");
    };
    scheduleChild(childToken(0), 0x24);
    require(command(*pair.child, 0x27).ack == 0x996600A7, "group child clock change failed");
    arm(*pair.child);
    std::vector<Bytes> payloads(initRepeats, blockInit(blocks));
    for (unsigned i = 0; i < blocks; ++i)
        payloads.push_back(parentUni(static_cast<uint16_t>(0x8900u + i), static_cast<uint16_t>(0x1234u + i)));
    // No delivery ticks yet: the entire prefix must be retained and complete
    // locally. The final fragment must wait for the actual ordered replies.
    for (unsigned i = 0; i < payloads.size(); ++i) {
        require(command(*pair.parent, 0x25, parentParameters(payloads[i])).ack == 0x996600A5,
                "group parent send rejected");
        arm(*pair.parent); step(*pair.parent);
        if (i + 1 != payloads.size()) {
            require((pair.parent->read16(CONTROL) & 0x80) == 0,
                    "group prefix blocked before all commands were produced");
            require(finishParentEvent(*pair.parent) == 1,
                    "buffered prefix did not advance the group producer");
            require(receive(*pair.parent, true).empty(), "prefix fabricated peer application data");
        } else require((pair.parent->read16(CONTROL) & 0x80) != 0,
                       "final group command completed without peer confirmation");
    }
    require(pair.parent->receiveRemoteRfuData(999, 0, {}) == GbaSystem::RfuDataReceiveResult::UnexpectedSequence,
            "unrelated reply completed a pending group fence");
    unsigned read = 0;
    bool parentDone = false;
    for (unsigned tick = 0; tick < 160; ++tick) {
        pair.down.now = pair.up.now = tick;
        pair.down.tick(*pair.child, tick); pair.up.tick(*pair.parent, tick);
        step(*pair.child); step(*pair.parent);
        if (event(*pair.child)) {
            require(read < payloads.size() && receive(*pair.child, false) == payloads[read],
                    "group command lost, duplicated or reordered at guest RX");
            ++read;
            scheduleChild(childToken(read), 0x25);
            arm(*pair.child);
        }
        if (!parentDone && !(pair.parent->read16(CONTROL) & 0x80)) {
            require(finishParentEvent(*pair.parent) == 1, "final group missing real ACK");
            parentDone = true;
        }
        require(pair.down.peak <= window && pair.up.peak <= window, "group exceeded transport window");
        if (read == payloads.size() && parentDone) {
            for (unsigned i = 0; i < read; ++i)
                require(receive(*pair.parent, true) == childToken(i), "child group replies lost, duplicated or reordered");
            require(receive(*pair.parent, true).empty(), "extra group reply fabricated");
            std::cout << " commands=" << read << " window=" << window << " ticks=" << tick + 1;
            return;
        }
    }
    throw std::runtime_error("group delivery/fence timeout");
}
void groupedPrefixBackpressure() {
    Pair pair({0, 0, 4}, GbaSystem::RfuCompletionMode::GroupedCommands);
    require(command(*pair.child, 0x27).ack == 0x996600A7, "pressure child arm failed");
    arm(*pair.child);
    require(command(*pair.parent, 0x25, parentParameters(blockInit(2))).ack == 0x996600A5,
            "pressure initializer failed");
    require(parentPoll(*pair.parent) == 1, "admitted initializer not completed locally");
    const Bytes first = parentUni(0x8900, 0x1234);
    require(command(*pair.parent, 0x25, parentParameters(first)).ack == 0x996600A5,
            "pressure command acceptance failed");
    arm(*pair.parent); step(*pair.parent);
    require(pair.parent->read16(CONTROL) & 0x80, "unadmitted prefix received buffered success");
    for (unsigned tick = 0; tick < 20; ++tick) {
        pair.down.now = pair.up.now = tick;
        pair.down.tick(*pair.child, tick); pair.up.tick(*pair.parent, tick);
        step(*pair.child); step(*pair.parent);
        if (event(*pair.child)) {
            receive(*pair.child, false);
            require(command(*pair.child, 0x27).ack == 0x996600A7, "pressure child rearm failed");
            arm(*pair.child);
        }
        if (!(pair.parent->read16(CONTROL) & 0x80)) {
            require(finishParentEvent(*pair.parent) == 1, "buffered prefix never resumed");
            require(pair.down.peak == 1 && pair.down.inFlightSequence == 2,
                    "pending prefix lost its original sequence");
            return;
        }
    }
    throw std::runtime_error("buffered prefix stayed blocked after credit returned");
}
void groupedDisconnect() {
    Pair pair({}, GbaSystem::RfuCompletionMode::GroupedCommands);
    pair.down.window = pair.up.window = 32;
    require(command(*pair.parent, 0x25, parentParameters(blockInit(2))).ack == 0x996600A5,
            "disconnect initializer failed");
    require(parentPoll(*pair.parent) == 1, "disconnect prefix did not complete");
    pair.parent->receiveRemoteRfuDisconnect();
    require((command(*pair.parent, 0x37).ack & 255) == 0xEE, "resume survived group disconnect");
    require(pair.parent->receiveRemoteRfuData(1, 0, {}) == GbaSystem::RfuDataReceiveResult::DroppedDisconnected,
            "late group reply was not drained after disconnect");
    require(receive(*pair.parent, true).empty(), "group data leaked after disconnect");
}
void groupedWindowBound() {
    Pair pair({}, GbaSystem::RfuCompletionMode::GroupedCommands);
    pair.down.window = pair.up.window = 32;
    const Bytes initializer = blockInit(20);
    const Bytes reply = encode({4, 0, 0, false, Bytes(14, 0)}, false);
    auto childTx = words(reply);
    childTx.insert(childTx.begin(), static_cast<uint32_t>(reply.size()) << 8);
    require(command(*pair.child, 0x24, childTx).ack == 0x996600A4, "window child TX failed");
    require(command(*pair.child, 0x27).ack == 0x996600A7, "bounded window child arm failed");
    arm(*pair.child);
    for (unsigned i = 0; i < 27; ++i) {
        require(command(*pair.parent, 0x25, parentParameters(initializer)).ack == 0x996600A5,
                "bounded window prefix rejected");
        require(parentPoll(*pair.parent) == 1, "owned prefix not completed");
    }
    require(pair.down.packets.size() == 27, "window did not preserve every immutable prefix");
    for (unsigned i = 0; i < 27; ++i)
        require(bytes(pair.down.packets[i].data, pair.down.packets[i].count) == initializer,
                "new guest write mutated an in-flight snapshot");
    require(command(*pair.parent, 0x25, parentParameters(initializer)).ack == 0x996600A5,
            "bounded group fence rejected");
    arm(*pair.parent); step(*pair.parent);
    require(pair.parent->read16(CONTROL) & 0x80, "group command bound fabricated success");
    unsigned tick = 0;
    const auto complete = [&] {
        for (unsigned limit = tick + 100; tick < limit; ++tick) {
            pair.down.now = pair.up.now = tick;
            pair.down.tick(*pair.child, tick); pair.up.tick(*pair.parent, tick);
            step(*pair.child); step(*pair.parent);
            if (event(*pair.child)) {
                require(receive(*pair.child, false) == initializer, "window child snapshot changed");
                require(command(*pair.child, 0x25, childTx).ack == 0x996600A5, "window child reply failed");
                arm(*pair.child);
            }
            if (!(pair.parent->read16(CONTROL) & 0x80)) {
                require(finishParentEvent(*pair.parent) == 1, "window fence ACK missing");
                ++tick; return;
            }
        }
        throw std::runtime_error("RX pressure prevented fence completion");
    };
    complete(); // Deliberately leave all 28 child replies unread.
    for (unsigned i = 0; i < 3; ++i) {
        require(command(*pair.parent, 0x25, parentParameters(initializer)).ack == 0x996600A5,
                "next group prefix rejected");
        require(parentPoll(*pair.parent) == 1, "next group prefix not owned");
    }
    require(command(*pair.parent, 0x25, parentParameters(initializer)).ack == 0x996600A5,
            "capacity fence rejected");
    arm(*pair.parent); step(*pair.parent);
    require(pair.parent->read16(CONTROL) & 0x80, "last RX slot was speculatively completed");
    complete();
    for (unsigned i = 0; i < 32; ++i)
        require(receive(*pair.parent, true) == reply, "bounded RX reply lost or duplicated");
    require(receive(*pair.parent, true).empty(), "bounded RX fabricated extra reply");
    require(pair.down.peak <= 32 && pair.up.peak <= 32, "bounded transport overflowed");
}
void groupedReceivePressure() {
    Pair pair({3, 40, 1}, GbaSystem::RfuCompletionMode::GroupedCommands);
    pair.down.window = pair.up.window = 32;
    const Bytes reply = encode({4, 0, 0, false, Bytes(14, 0)}, false);
    auto childTx = words(reply);
    childTx.insert(childTx.begin(), static_cast<uint32_t>(reply.size()) << 8);
    require(command(*pair.child, 0x24, childTx).ack == 0x996600A4, "pressure child write failed");
    require(command(*pair.child, 0x27).ack == 0x996600A7, "pressure child clock failed");
    arm(*pair.child);
    std::vector<Bytes> frames;
    for (unsigned i = 0; i < 40; ++i) frames.push_back(parentUni(0xBE00));
    for (unsigned group = 0; group < 4; ++group) {
        frames.push_back(blockInit(20));
        for (unsigned i = 0; i < 20; ++i) frames.push_back(parentUni(static_cast<uint16_t>(0x8900 + i)));
    }
    unsigned sent = 0, delivered = 0, received = 0;
    bool waiting = false;
    for (unsigned tick = 0; tick < 6000; ++tick) {
        pair.down.now = pair.up.now = tick;
        pair.down.tick(*pair.child, tick); pair.up.tick(*pair.parent, tick);
        step(*pair.child); step(*pair.parent);
        if (event(*pair.child)) {
            require(delivered < frames.size() && receive(*pair.child, false) == frames[delivered++],
                    "pressure stream changed at child");
            require(command(*pair.child, 0x25, childTx).ack == 0x996600A5, "pressure child response failed");
            arm(*pair.child);
        }
        if (!waiting && sent < frames.size()) {
            require(command(*pair.parent, 0x25, parentParameters(frames[sent])).ack == 0x996600A5,
                    "pressure parent command rejected");
            arm(*pair.parent); step(*pair.parent); waiting = true;
        }
        if (waiting && !(pair.parent->read16(CONTROL) & 0x80)) {
            require(finishParentEvent(*pair.parent) == 1, "pressure radio completion missing");
            const auto value = receive(*pair.parent, true); // One guest read per completed command.
            if (!value.empty()) { require(value == reply, "pressure response corrupted"); ++received; }
            ++sent; waiting = false;
        }
        if (sent == frames.size() && delivered == sent) {
            for (auto value = receive(*pair.parent, true); !value.empty(); value = receive(*pair.parent, true)) {
                require(value == reply, "pressure tail corrupted"); ++received;
            }
            require(received == sent, "pressure response lost or duplicated");
            return;
        }
    }
    throw std::runtime_error("parent waiting for ACK while RX queue needs guest reads");
}
unsigned groupWaitBudget(GbaSystem::RfuCompletionMode mode) {
    Pair pair({8, 8, 2}, mode); pair.down.window = pair.up.window = 32;
    require(command(*pair.child, 0x27).ack == 0x996600A7, "budget child arm failed"); arm(*pair.child);
    const std::vector<Bytes> packets{blockInit(2), parentUni(0x8900, 0x1234), parentUni(0x8901, 0x5678)};
    unsigned next = 0, read = 0; bool pending = false;
    for (unsigned tick = 0; tick < 200; ++tick) {
        pair.down.now = pair.up.now = tick;
        pair.down.tick(*pair.child, tick); pair.up.tick(*pair.parent, tick);
        step(*pair.child); step(*pair.parent);
        if (event(*pair.child)) {
            require(read < packets.size() && receive(*pair.child, false) == packets[read++], "budget stream changed");
            require(command(*pair.child, 0x27).ack == 0x996600A7, "budget child rearm failed"); arm(*pair.child);
        }
        if (!pending && next < packets.size()) {
            require(command(*pair.parent, 0x25, parentParameters(packets[next])).ack == 0x996600A5,
                    "budget send rejected");
            arm(*pair.parent); pending = true; step(*pair.parent);
        }
        if (pending && !(pair.parent->read16(CONTROL) & 0x80)) {
            require(finishParentEvent(*pair.parent) == 1, "budget false radio result");
            receive(*pair.parent, true); ++next; pending = false;
        }
        if (next == packets.size() && read == packets.size()) return tick + 1;
    }
    throw std::runtime_error("wait budget timed out");
}
void guardedPhaseBoundaries() {
    Pair pair({}, GbaSystem::RfuCompletionMode::GuardedLocalPoll);
    guardedRound(pair, {}, true, 0); // Unknown initial phase is conservative.
    guardedRound(pair, parentUni(0xBE00, 0x1234), false, 2);
    guardedRound(pair, {}, false, 4); // An idle key slot preserves the key phase.
    guardedRound(pair, parentUni(0x8800, 2), true, 6);
    guardedRound(pair, {}, true, 8); // Idle gap cannot end a data group.
    Bytes otherKey(70, 0); otherKey[15] = 0xBE;
    guardedRound(pair, encode({4, 0, 0, false, otherKey}, true), true, 10);
    Bytes mixed(70, 0); mixed[1] = 0xBE; mixed[15] = 0x88;
    guardedRound(pair, encode({4, 0, 0, false, mixed}, true), true, 12);
    guardedRound(pair, parentUni(0xBE00), false, 14);
    guardedRound(pair, Bytes{0x78, 0x56, 0x34, 0x12}, true, 16); // Opaque/truncated LLSF.
    guardedRound(pair, parentUni(0xBE00), false, 18);
}
void guardedPeerBoundary() {
    Pair pair({}, GbaSystem::RfuCompletionMode::GuardedLocalPoll);
    Bytes payload(14, 0); payload[1] = 0x88; payload[2] = 2;
    const Bytes frame = encode({4, 0, 0, false, payload}, false);
    auto parameters = words(frame); parameters.insert(parameters.begin(), static_cast<uint32_t>(frame.size()) << 8);
    require(command(*pair.child, 0x24, parameters).ack == 0x996600A4, "peer critical write failed");
    require(guardedRound(pair, parentUni(0xBE00), false, 0) == frame,
            "peer critical payload changed");
    guardedRound(pair, {}, true, 2); // Incoming data closes the local polling boundary.
    guardedRound(pair, parentUni(0xBE00), false, 4);
}
void localPollProgress() {
    Pair pair({}, GbaSystem::RfuCompletionMode::LocalPoll);
    require((command(*pair.parent, 0x37).ack & 255) == 0xEE,
            "resume without an exchange must be rejected");
    require(command(*pair.parent, 0x25, {4, 0x12345678}).ack == 0x996600A5,
            "initial parent send rejected");
    for (unsigned frame = 0; frame < 8; ++frame) {
        if (frame) require(command(*pair.parent, 0x37).ack == 0x996600B7,
                           "parent retry rejected");
        require(parentPoll(*pair.parent) == 0, "invented child radio ACK before delivery");
        require(receive(*pair.parent, true).empty(), "invented RX data before delivery");
    }
    require(pair.down.packets.size() == 1 && pair.up.packets.empty(),
            "local polls must not queue copies of an outstanding network exchange");
    // Deliver the original request after eight local main-loop opportunities.
    pair.down.now = pair.up.now = 8;
    pair.down.tick(*pair.child, 8); pair.up.tick(*pair.parent, 8);
    require(command(*pair.parent, 0x37).ack == 0x996600B7, "completion retry rejected");
    require(parentPoll(*pair.parent) == 1, "real child reply did not produce slot ACK");
    require(receive(*pair.parent, true).empty(), "empty reply fabricated application data");
    require((command(*pair.parent, 0x37).ack & 255) == 0xEE,
            "one reply was acknowledged twice");
    std::cout << " local_polls=8 remote_exchanges=1";
}
void localPollReplyRace(bool disconnect) {
    Pair pair({}, GbaSystem::RfuCompletionMode::LocalPoll);
    require(command(*pair.child, 0x24, {4u << 8, 0xA5B6C7D8}).ack == 0x996600A4,
            "child scheduling failed");
    require(command(*pair.parent, 0x25, {4, 0x12345678}).ack == 0x996600A5,
            "parent send failed");
    const uint32_t mask = parentPoll(*pair.parent, [&] {
        if (disconnect) pair.parent->receiveRemoteRfuDisconnect();
        else {
            pair.down.now = pair.up.now = 1;
            pair.down.tick(*pair.child, 1); pair.up.tick(*pair.parent, 1);
        }
    });
    require(mask == 0, "in-progress event ACK mask changed after its header");
    if (disconnect) {
        require((command(*pair.parent, 0x37).ack & 255) == 0xEE,
                "resume after disconnect accepted stale success");
        require(receive(*pair.parent, true).empty(), "RX leaked across disconnect");
    } else {
        // Emerald calls recvData even on an unsuccessful local attempt.
        require(receive(*pair.parent, true) == bytes({0xA5B6C7D8}, 4),
                "real reply lost when consumed before the successful poll");
        require(command(*pair.parent, 0x37).ack == 0x996600B7, "resume failed");
        require(parentPoll(*pair.parent) == 1, "real ACK lost between event header and body");
        require(receive(*pair.parent, true).empty(), "reply delivered twice");
    }
}
void localPollCredit() {
    Pair pair({}, GbaSystem::RfuCompletionMode::LocalPoll);
    pair.down.occupied = true; // Transport has not granted admission yet.
    require(command(*pair.parent, 0x25, {4, 0x12345678}).ack == 0x996600A5,
            "local command acceptance was coupled to transport credit");
    require(parentPoll(*pair.parent) == 0, "unadmitted send fabricated radio success");
    require((command(*pair.parent, 0x25, {4, 0xDEADBEEF}).ack & 255) == 0xEE,
            "fresh clock-change send overwrote an unfinished poll exchange");
    require(command(*pair.parent, 0x37).ack == 0x996600B7, "credit wait retry rejected");
    require(parentPoll(*pair.parent) == 0 && pair.down.packets.empty(),
            "credit wait emitted a network packet");
    pair.down.occupied = false; step(*pair.parent);
    require(pair.down.packets.size() == 1 && pair.down.packets.front().data ==
            std::vector<uint32_t>{0x12345678}, "buffered original snapshot changed");
    pair.down.now = pair.up.now = 1;
    pair.down.tick(*pair.child, 1); pair.up.tick(*pair.parent, 1);
    require(command(*pair.parent, 0x37).ack == 0x996600B7, "completion retry failed");
    require(parentPoll(*pair.parent) == 1, "admitted original send never completed");
}
void localPollClockTransition() {
    Pair pair({}, GbaSystem::RfuCompletionMode::LocalPoll);
    require(command(*pair.parent, 0x24, {1}).ack == 0x996600A4,
            "pre-finalization parent cycle failed");
    require(command(*pair.parent, 0x25, {4, 0x12345678}).ack == 0x996600A5,
            "clock-changing cycle cannot follow an outstanding ordinary cycle");
    require(parentPoll(*pair.parent) == 0, "pending clock-change cycle falsely completed");
    pair.down.now = pair.up.now = 1;
    pair.down.tick(*pair.child, 1); pair.up.tick(*pair.parent, 1);
    require(command(*pair.parent, 0x37).ack == 0x996600B7, "transition retry failed");
    require(parentPoll(*pair.parent) == 0, "ordinary-cycle reply acknowledged the newer clock-change cycle");
    pair.down.tick(*pair.child, 2); pair.up.tick(*pair.parent, 2);
    step(*pair.parent); // Release both credits, dispatch queued 0x25.
    require(pair.down.packets.size() == 1 && pair.down.packets.front().seq == 2,
            "pending clock-change snapshot was not dispatched exactly once");
    pair.down.now = pair.up.now = 3;
    pair.down.tick(*pair.child, 3); pair.up.tick(*pair.parent, 3);
    require(command(*pair.parent, 0x37).ack == 0x996600B7, "final transition retry failed");
    require(parentPoll(*pair.parent) == 1, "clock-change reply never acknowledged");
}
void groupedClockTransition() {
    Pair pair({}, GbaSystem::RfuCompletionMode::GroupedCommands);
    require(command(*pair.child, 0x27).ack == 0x996600A7, "child clock change failed");
    arm(*pair.child);
    require(command(*pair.parent, 0x24, {1}).ack == 0x996600A4,
            "ordinary grouped cycle failed");
    require(command(*pair.parent, 0x25, parentParameters(blockInit(2))).ack == 0x996600A5,
            "group initializer rejected behind outstanding ordinary cycle");
    arm(*pair.parent); step(*pair.parent);
    require((pair.parent->read16(CONTROL) & 0x80) != 0,
            "queued initializer completed before storage admission");
    pair.down.tick(*pair.child, 1); pair.up.tick(*pair.parent, 1);
    require((pair.parent->read16(CONTROL) & 0x80) != 0,
            "ordinary reply acknowledged newer clock-changing cycle");
    step(*pair.child);
    require(event(*pair.child), "ordinary child event missing");
    receive(*pair.child, false);
    require(command(*pair.child, 0x25, {0}).ack == 0x996600A5, "child reply scheduling failed");
    arm(*pair.child);
    pair.down.tick(*pair.child, 2); pair.up.tick(*pair.parent, 2);
    step(*pair.parent);
    require(pair.down.packets.size() == 1 && pair.down.packets.front().seq == 2,
            "queued initializer not dispatched exactly once");
    require(finishParentEvent(*pair.parent) == 1, "initializer prefix did not complete locally");
    require(command(*pair.parent, 0x25, parentParameters(parentUni(0x8900, 0x1234))).ack == 0x996600A5,
            "first block fragment rejected");
    arm(*pair.parent); step(*pair.parent);
    // The old ordinary reply must not clear the queued initializer's planner.
    // Free transport credit without consuming the initializer's peer reply.
    pair.down.tick(*pair.child, 3); pair.down.tick(*pair.child, 4);
    step(*pair.parent);
    require((pair.parent->read16(CONTROL) & 0x80) == 0,
            "ordinary reply erased the new block group");
    require(finishParentEvent(*pair.parent) == 1, "non-final fragment lost buffered completion");
}
void localPollStream(Scenario scenario) {
    Pair pair(scenario, GbaSystem::RfuCompletionMode::LocalPoll);
    require(command(*pair.child, 0x27).ack == 0x996600A7, "child clock change failed");
    arm(*pair.child);
    unsigned rounds = 0, attempts = 0, emptyPolls = 0, childReceives = 0;
    bool pending = false;
    for (unsigned tick = 0; tick < 240 && rounds < 12; ++tick) {
        pair.down.now = pair.up.now = tick;
        pair.down.tick(*pair.child, tick); pair.up.tick(*pair.parent, tick);
        step(*pair.parent); step(*pair.child);
        if (event(*pair.child)) {
            const auto payload = receive(*pair.child, false);
            require(payload == bytes({0x70000000u + childReceives}, 4),
                    "ordered parent stream was lost, duplicated or reordered");
            ++childReceives;
            require(command(*pair.child, 0x25, {4u << 8, 0x50000000u + childReceives}).ack == 0x996600A5,
                    "child stream scheduling failed");
            arm(*pair.child);
        }
        if (!pending) {
            require(command(*pair.parent, 0x25, {4, 0x70000000u + rounds}).ack == 0x996600A5,
                    "fresh parent exchange failed");
            pending = true;
        } else require(command(*pair.parent, 0x37).ack == 0x996600B7, "stream retry failed");
        const uint32_t mask = parentPoll(*pair.parent);
        const auto reply = receive(*pair.parent, true);
        ++attempts;
        if (!mask) {
            require(reply.empty(), "unexpected early reply in discrete scheduler");
            ++emptyPolls;
        } else {
            require(mask == 1, "ACK for an unconnected slot");
            require(reply == (rounds == 0 ? Bytes{} : bytes({0x50000000u + rounds}, 4)),
                    "ordered child stream was lost, duplicated or corrupted");
            ++rounds; pending = false;
        }
        require(pair.down.peak <= 1 && pair.up.peak <= 1, "retry polls grew transport queues");
    }
    require(rounds == 12 && childReceives == 12 && emptyPolls >= 12,
            "delayed stream did not complete through local polling");
    std::cout << " rounds=" << rounds << " local_attempts=" << attempts
              << " empty_polls=" << emptyPolls;
}
void diagnosticsObservational() {
    Pair traced({}, GbaSystem::RfuCompletionMode::LocalPoll);
    Pair reference({}, GbaSystem::RfuCompletionMode::LocalPoll);
    std::vector<std::string> messages;
    traced.parent->setRfuDiagnosticLogger([&](const std::string &line) {
        messages.push_back(line);
    }, true);
    const auto run = [](Pair &pair) {
        require(command(*pair.child, 0x24, {4u << 8, 0xA5B6C7D8}).ack == 0x996600A4,
                "diagnostic fixture child write failed");
        require(command(*pair.parent, 0x25, {4, 0x12345678}).ack == 0x996600A5,
                "diagnostic fixture parent write failed");
        require(parentPoll(*pair.parent) == 0, "diagnostic fixture false ACK");
        pair.down.now = pair.up.now = 1;
        pair.down.tick(*pair.child, 1); pair.up.tick(*pair.parent, 1);
        require(command(*pair.parent, 0x37).ack == 0x996600B7, "diagnostic fixture retry failed");
        require(parentPoll(*pair.parent) == 1, "diagnostic fixture missing ACK");
        require(receive(*pair.parent, true) == bytes({0xA5B6C7D8}, 4), "diagnostic changed payload");
    };
    run(traced); run(reference);
    require(traced.parent->emulatedCycles() == reference.parent->emulatedCycles() &&
            traced.parent->frameBuffer() == reference.parent->frameBuffer() &&
            traced.parent->read16(CONTROL) == reference.parent->read16(CONTROL),
            "detailed logging changed guest progress or output");
    const auto contains = [&](const std::string &a, const std::string &b) {
        return std::any_of(messages.begin(), messages.end(), [&](const std::string &line) {
            return line.find(a) != std::string::npos && line.find(b) != std::string::npos;
        });
    };
    require(contains("boundary=tx_dispatch", "hex=78563412"), "missing immutable transmitted bytes");
    require(contains("boundary=rx_admit", "hex=D8C7B6A5"), "missing admitted peer bytes");
    require(contains("boundary=rx_read", "hex=D8C7B6A5"), "missing guest-consumed bytes");
    traced.parent->write16(0x04000000, 0x80); // Explicit forced blank is white.
    for (unsigned i = 0; i < 60; ++i) step(*traced.parent);
    require(contains("RFU frame:", "black=0 "), "forced blank must be diagnosed as white");
    messages.clear();
    traced.parent->write16(0x04000000, 0); // All BG/OBJ off, zero backdrop.
    for (unsigned i = 0; i < 60; ++i) step(*traced.parent);
    require(contains("RFU frame:", "black=38400 "), "real black framebuffer not diagnosed");
}
void oracleCheck() {
    Sender sender(identity()); Receiver receiver;
    require(words(encode(sender.next(), false)).front() == 0x0C010487, "control frame golden word mismatch");
    receiver.receive(sender.next());
    require(words(encode(*receiver.reply, true)).front() == 0x00046800, "NI START ACK golden word mismatch");
    sender.acknowledge({1, 0, 2, true, {}});
    require(sender.state() == 1, "wrong NI ACK advanced sender");
    sender.acknowledge(*receiver.reply);
    Frame first = sender.next(); receiver.receive(first); receiver.receive(first);
    require(receiver.duplicates == 1, "duplicate data not recognized");
    sender.acknowledge(*receiver.reply);
    for (unsigned round = 0; round < 20 && !receiver.complete; ++round) {
        const Frame frame = sender.next(); receiver.receive(frame);
        if (receiver.reply) sender.acknowledge(*receiver.reply);
        sender.accepted(frame);
    }
    require(receiver.complete && receiver.accepted && sender.done, "oracle identity did not complete");
    require(receiver.content == identity(), "oracle bytes mismatch");
    require(std::all_of(receiver.copies.begin(), receiver.copies.end(), [](unsigned n) { return n == 1; }),
            "duplicate wrote identity bytes twice");
}
}
int main(int argc, char **argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    unsigned failed = 0, executed = 0;
    const auto run = [&](const std::string &name, const std::function<void()> &test) {
        if (!filter.empty() && name != filter) return;
        ++executed;
        try { test(); std::cout << " PASS " << name << '\n'; }
        catch (const std::exception &e) { ++failed; std::cout << " FAIL " << name << ": " << e.what() << '\n'; }
    };
    run("oracle", oracleCheck);
    run("command_group_plan", commandGroupPlan);
    run("grouped_handoff", [] { tradeHandoff(GbaSystem::RfuCompletionMode::GroupedCommands, false); });
    run("grouped_handoff_idle_gap", [] { tradeHandoff(GbaSystem::RfuCompletionMode::GroupedCommands, true); });
    run("grouped_prefix_backpressure", groupedPrefixBackpressure);
    run("grouped_window_bound", groupedWindowBound);
    run("grouped_receive_pressure", groupedReceivePressure);
    run("grouped_wait_budget", [] {
        const auto reference = groupWaitBudget(GbaSystem::RfuCompletionMode::RemoteReply);
        const auto grouped = groupWaitBudget(GbaSystem::RfuCompletionMode::GroupedCommands);
        require(grouped < reference, "grouping did not reduce per-command wait dependency");
        std::cout << " reference_ticks=" << reference << " grouped_ticks=" << grouped;
    });
    run("grouped_unreachable_peer", [] {
        bool timedOut = false;
        try { groupedDelivery({1000, 1000}, 32); }
        catch (const std::runtime_error &e) {
            if (std::string(e.what()) != "group delivery/fence timeout") throw;
            timedOut = true;
        }
        require(timedOut, "unreachable peer falsely confirmed final group");
    });
    run("grouped_disconnect", groupedDisconnect);
    run("grouped_ni_identity", [] {
        const Scenario s{1, 5, 2, 1};
        Pair pair(s, GbaSystem::RfuCompletionMode::GroupedCommands); pair.exchange(s, identity());
    });
    run("grouped_large_block", [] { groupedDelivery({5, 7, 3, 2}, 32, 17, 4); });
    run("grouped_delivery", [] { groupedDelivery({5, 7, 3, 2}, 32); });
    run("grouped_window_pressure", [] { groupedDelivery({5, 7, 8, 1}, 2); });
    run("guarded_phase_boundaries", guardedPhaseBoundaries);
    run("guarded_peer_boundary", guardedPeerBoundary);
    run("trade_handoff_reference", [] { tradeHandoff(GbaSystem::RfuCompletionMode::RemoteReply, false); });
    run("trade_handoff_guarded", [] { tradeHandoff(GbaSystem::RfuCompletionMode::GuardedLocalPoll, false); });
    run("trade_handoff_idle_gap", [] { tradeHandoff(GbaSystem::RfuCompletionMode::GuardedLocalPoll, true); });
    run("diagnostics_observational", diagnosticsObservational);
    run("local_poll_progress", localPollProgress);
    run("local_poll_reply_race", [] { localPollReplyRace(false); });
    run("local_poll_disconnect", [] { localPollReplyRace(true); });
    run("local_poll_credit", localPollCredit);
    run("local_poll_clock_transition", localPollClockTransition);
    run("grouped_clock_transition", groupedClockTransition);
    run("local_poll_stream", [] { localPollStream({2, 5, 0, 0}); });
    run("local_poll_stream_jitter", [] { localPollStream({1, 3, 4, 2}); });
    run("local_poll_ni_identity", [] {
        const Scenario s{1, 5, 2, 1};
        Pair pair(s, GbaSystem::RfuCompletionMode::LocalPoll); pair.exchange(s, identity());
    });
    run("connection_identity", [] {
        Pair pair({});
        const auto status = command(*pair.child, 0x20);
        require(status.payload == std::vector<uint32_t>{2}, "child own adapter ID must be 2, not parent ID");
    });
    const auto scenario = [&](const std::string &name, Scenario s) {
        run(name, [&] { Pair pair(s); pair.exchange(s, identity()); });
    };
    scenario("paced_reference", {0, 0, 0, 0, 240, false, 4});
    scenario("paced_symmetric_delay", {2, 2, 0, 0, 480, false, 16});
    scenario("paced_asymmetric_delay", {1, 5, 0, 0, 480, false, 24});
    scenario("paced_delayed_credit", {0, 0, 4, 0, 480, false, 16});
    scenario("paced_lost_ni_ack", {0, 0, 0, 0, 480, true, 4});
    scenario("zero_delay", {});
    scenario("symmetric_delay", {2, 2, 0, 0, 240, false});
    scenario("asymmetric_delay", {1, 5, 0, 0, 240, false});
    scenario("reverse_asymmetric_delay", {5, 1, 0, 0, 240, false});
    scenario("delayed_credit", {0, 0, 4, 0, 240, false});
    scenario("jitter", {1, 2, 1, 3, 240, false});
    scenario("lost_ni_ack", {0, 0, 0, 0, 240, true});
    run("wrong_serial", [] { Scenario s; s.parentPeriod = 4; Pair pair(s); auto value = identity(); value[0] = 0xFF; pair.exchange(s, value, false); });
    run("bounded_timeout", [] {
        const Scenario s{1000, 1000, 0, 0, 40, false, 4};
        Pair pair(s);
        bool timedOut = false;
        try { pair.exchange(s, identity()); }
        catch (const std::runtime_error &e) {
            const std::string reason = e.what();
            if (reason.find("NI identity timeout:") != 0) throw;
            timedOut = true;
        }
        require(timedOut, "unreachable peer falsely completed identity exchange");
    });
    if (executed == 0) { std::cerr << "unknown test filter\n"; return 2; }
    std::cout << "NI tests: " << executed - failed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
