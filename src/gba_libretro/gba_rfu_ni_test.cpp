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
    unsigned delay = 0, creditDelay = 0, jitter = 0, now = 0, peak = 0;
    bool occupied = false, creditPending = false;
    unsigned creditDue = 0;
    uint32_t inFlightSequence = 0;
    std::deque<Packet> packets;
    bool send(uint32_t seq, uint8_t count, const std::vector<uint32_t> &data) {
        if (occupied) return false;
        occupied = true;
        inFlightSequence = seq;
        const unsigned variation = jitter ? (seq * 7) % (jitter + 1) : 0;
        packets.push_back({now + delay + variation, seq, count, data});
        peak = std::max(peak, static_cast<unsigned>(packets.size()));
        return true;
    }
    void accepted(uint32_t sequence) {
        require(occupied && !creditPending && sequence == inFlightSequence,
                "duplicate or mismatched transport admission ACK");
        creditPending = true;
        creditDue = now + creditDelay;
    }
    void tick(GbaSystem &destination, unsigned at) {
        now = at;
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
    explicit Pair(Scenario s) {
        down.delay = s.down; up.delay = s.up;
        down.creditDelay = up.creditDelay = s.credit;
        down.jitter = up.jitter = s.jitter;
        boot(*parent); boot(*child);
        parent->configureRemoteRfuLink(1, 2,
            [&](uint32_t seq, uint16_t id, const GbaSystem::RfuAdvertisement &value) {
                require(child->receiveRemoteRfuAdvertisement(seq, id, value), "parent advertisement failed");
            }, [](uint32_t, uint16_t, uint16_t) {},
            [&](uint32_t seq, uint8_t n, const std::vector<uint32_t> &value) { return down.send(seq, n, value); },
            [&](uint32_t seq) { up.accepted(seq); });
        child->configureRemoteRfuLink(2, 1,
            [&](uint32_t seq, uint16_t id, const GbaSystem::RfuAdvertisement &value) {
                require(parent->receiveRemoteRfuAdvertisement(seq, id, value), "child advertisement failed");
            }, [&](uint32_t seq, uint16_t source, uint16_t target) {
                require(parent->receiveRemoteRfuConnectionRequest(seq, source, target), "parent rejected connection");
                require(child->receiveRemoteRfuConnectionResult(seq, source, 0, true), "child rejected result");
            }, [&](uint32_t seq, uint8_t n, const std::vector<uint32_t> &value) { return up.send(seq, n, value); },
            [&](uint32_t seq) { down.accepted(seq); });
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
