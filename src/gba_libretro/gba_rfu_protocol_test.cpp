#include "gba_rfu_protocol.hpp"

#include <cassert>
#include <stdexcept>

using LOTUSGBA::GbaRfuRequestDecoder;
using LOTUSGBA::GbaRfuLogin;
using LOTUSGBA::GbaRfuDataChannel;

int main() {
    GbaRfuRequestDecoder decoder;
    GbaRfuRequestDecoder::Request request;

    assert(decoder.pushWord(LOTUSGBA::makeGbaRfuCommandHeader(0x10, 0),
                            request) == GbaRfuRequestDecoder::Result::Complete);
    assert(request.command == 0x10);
    assert(request.payload.empty());
    assert(!decoder.awaitingPayload());

    assert(decoder.pushWord(LOTUSGBA::makeGbaRfuCommandHeader(0x16, 2),
                            request) == GbaRfuRequestDecoder::Result::Incomplete);
    assert(decoder.awaitingPayload());
    assert(decoder.pushWord(0x12345678, request) ==
           GbaRfuRequestDecoder::Result::Incomplete);
    assert(decoder.pushWord(0x90ABCDEF, request) ==
           GbaRfuRequestDecoder::Result::Complete);
    assert(request.command == 0x16);
    assert(request.payload.size() == 2);
    assert(request.payload[0] == 0x12345678);
    assert(request.payload[1] == 0x90ABCDEF);
    assert(!decoder.awaitingPayload());

    assert(LOTUSGBA::makeGbaRfuResponseHeader(0x16, 1) == 0x99660196);
    assert(decoder.pushWord(0x12340010, request) ==
           GbaRfuRequestDecoder::Result::Invalid);
    assert(decoder.pushWord(LOTUSGBA::GbaRfuRequestDecoder::HEADER_MAGIC |
                            (0x1Du << 8) | 0x10, request) ==
           GbaRfuRequestDecoder::Result::Invalid);

    bool commandHeaderRejected = false;
    try {
        (void)LOTUSGBA::makeGbaRfuCommandHeader(0x10, 0x1D);
    } catch (const std::invalid_argument &) {
        commandHeaderRejected = true;
    }
    assert(commandHeaderRejected);

    bool responseHeaderRejected = false;
    try {
        (void)LOTUSGBA::makeGbaRfuResponseHeader(0x80, 0);
    } catch (const std::invalid_argument &) {
        responseHeaderRejected = true;
    }
    assert(responseHeaderRejected);

    GbaRfuLogin login;
    constexpr uint32_t guestWords[] = {
        0x7FFF494E, 0xFFFF494E, 0xB6B1494E, 0xB6B1544E,
        0xABB1544E, 0xABB14E45, 0xB1BA4E45, 0xB1BA4F44,
        0xB0BB4F44, 0xB0BB8001};
    constexpr uint32_t adapterWords[] = {
        0x00000000, 0x494EB6B1, 0x494EB6B1, 0x544EB6B1,
        0x544EABB1, 0x4E45ABB1, 0x4E45B1BA, 0x4F44B1BA,
        0x4F44B0BB, 0x8001B0BB};
    for (unsigned index = 0; index < GbaRfuLogin::TRANSFER_COUNT; ++index) {
        assert(login.exchangeWord(guestWords[index]) == adapterWords[index]);
        assert(login.transferCount() == index + 1);
    }
    assert(login.complete());
    login.reset();
    assert(!login.complete());
    assert(login.exchangeWord(guestWords[0]) == adapterWords[0]);

    LOTUSGBA::GbaRfuCommandSession session;
    auto word = session.exchangeWord(
        LOTUSGBA::makeGbaRfuCommandHeader(0x16, 2));
    assert(word.adapterWord == 0x80000000 && !word.requestComplete);
    word = session.exchangeWord(0x11223344);
    assert(word.adapterWord == 0x80000000 && !word.requestComplete);
    word = session.exchangeWord(0x55667788);
    assert(word.adapterWord == 0x80000000 && word.requestComplete);
    assert(word.request.command == 0x16);
    assert(word.request.payload.size() == 2);
    assert(session.awaitingResponse());
    session.respond(true);
    word = session.exchangeWord(0x80000000);
    assert(word.adapterWord == 0x99660096);
    assert(!session.awaitingResponse());

    word = session.exchangeWord(
        LOTUSGBA::makeGbaRfuCommandHeader(0x55, 0));
    assert(word.requestComplete && word.request.command == 0x55);
    session.respond(false, {}, 1);
    assert(session.exchangeWord(0x80000000).adapterWord == 0x996601EE);
    assert(session.exchangeWord(0x80000000).adapterWord == 1);
    assert(!session.awaitingResponse());

    word = session.exchangeWord(
        LOTUSGBA::makeGbaRfuCommandHeader(0x26, 0));
    assert(word.requestComplete);
    session.respond(true, {0x00000500, 0x04030201});
    word = session.exchangeWord(0x80000000);
    assert(word.adapterWord == 0x996602A6);
    assert(!word.responseComplete);
    word = session.exchangeWord(0x80000000);
    assert(word.adapterWord == 0x00000500);
    assert(!word.responseComplete);
    word = session.exchangeWord(0x80000000);
    assert(word.adapterWord == 0x04030201);
    assert(word.responseComplete);
    assert(!session.awaitingResponse());

    // The RFU LAN data channel is tested without GbaSystem, a BIOS, a ROM, or
    // serial transfers. A parent has one child-TX slot, advanced by 0x25;
    // a child has a bounded receive queue drained by 0x26.
    GbaRfuDataChannel channel;
    assert(channel.receive(1, 1, {0xAA}) ==
           GbaRfuDataChannel::ReceiveResult::LinkDisabled);
    channel.reset(true);
    channel.setConnection(true, true);
    assert(channel.receive(2, 1, {0xAA}) ==
           GbaRfuDataChannel::ReceiveResult::UnexpectedSequence);
    assert(channel.receive(1, 1, {}) ==
           GbaRfuDataChannel::ReceiveResult::PayloadTooLarge);
    assert(channel.receive(1, 1, {0xAA}) ==
           GbaRfuDataChannel::ReceiveResult::Accepted);
    assert(channel.hasScheduledChildMessage());
    assert(channel.receive(2, 1, {0xBB}) ==
           GbaRfuDataChannel::ReceiveResult::QueueFull);
    assert(channel.expectedSequence() == 2);
    assert(channel.advanceParentRadioCycle());
    assert(!channel.hasScheduledChildMessage());
    assert(channel.queuedMessages() == 1);
    assert(channel.receive(2, 1, {0xBB}) ==
           GbaRfuDataChannel::ReceiveResult::Accepted);
    assert(channel.advanceParentRadioCycle());
    auto parentReceived = channel.takeReceived();
    assert(parentReceived && parentReceived->sequence == 1);
    assert(parentReceived->frameWords == std::vector<uint32_t>{0xAA});
    parentReceived = channel.takeReceived();
    assert(parentReceived && parentReceived->sequence == 2);
    assert(parentReceived->frameWords == std::vector<uint32_t>{0xBB});

    // An empty child reply completes a radio exchange and its transport
    // sequence without manufacturing a guest-visible application packet.
    channel.reset(true);
    channel.setConnection(true, true);
    assert(channel.receive(1, 0, {}) == GbaRfuDataChannel::ReceiveResult::Accepted);
    assert(channel.expectedSequence() == 2);
    assert(!channel.hasScheduledChildMessage() && !channel.hasReceived());

    channel.reset(true);
    channel.setConnection(true, false);
    for (uint32_t sequence = 1;
         sequence <= GbaRfuDataChannel::MAX_QUEUED_MESSAGES; ++sequence) {
        assert(channel.receive(sequence, 1, {sequence}) ==
               GbaRfuDataChannel::ReceiveResult::Accepted);
    }
    assert(channel.receive(33, 1, {33}) ==
           GbaRfuDataChannel::ReceiveResult::QueueFull);
    for (uint32_t sequence = 1;
         sequence <= GbaRfuDataChannel::MAX_QUEUED_MESSAGES; ++sequence) {
        auto received = channel.takeReceived();
        assert(received && received->sequence == sequence);
    }
    assert(channel.receive(33, 1, {33}) ==
           GbaRfuDataChannel::ReceiveResult::Accepted);
    channel.setConnection(false, false);
    assert(channel.receive(34, 1, {34}) ==
           GbaRfuDataChannel::ReceiveResult::DroppedDisconnected);
    assert(channel.expectedSequence() == 35);
    const auto drainedOnDisconnect = channel.disconnect();
    assert(drainedOnDisconnect == std::vector<uint32_t>{33});
    assert(channel.queuedMessages() == 0);
    assert(channel.receive(35, 1, {35}) ==
           GbaRfuDataChannel::ReceiveResult::DroppedDisconnected);
}
