#include "gba_rfu_protocol.hpp"

#include <stdexcept>
#include <utility>

namespace LOTUSGBA {

GbaRfuRequestDecoder::Result GbaRfuRequestDecoder::pushWord(
    uint32_t word, Request &request) {
    if (!active_) {
        if ((word & 0xFFFF0000u) != HEADER_MAGIC)
            return Result::Invalid;

        const auto wordCount = static_cast<uint8_t>((word >> 8) & 0xFF);
        if (wordCount > MAX_PAYLOAD_WORDS)
            return Result::Invalid;

        command_ = static_cast<uint8_t>(word);
        expectedWords_ = wordCount;
        payload_.clear();
        if (expectedWords_ != 0) {
            active_ = true;
            return Result::Incomplete;
        }

        request.command = command_;
        request.payload.clear();
        return Result::Complete;
    }

    payload_.push_back(word);
    if (payload_.size() < expectedWords_)
        return Result::Incomplete;

    request.command = command_;
    request.payload = std::move(payload_);
    reset();
    return Result::Complete;
}

void GbaRfuRequestDecoder::reset() {
    active_ = false;
    command_ = 0;
    expectedWords_ = 0;
    payload_.clear();
}

void GbaRfuDataChannel::reset(bool enabled) {
    enabled_ = enabled;
    connected_ = false;
    parent_ = false;
    expectedSequence_ = 1;
    received_.clear();
    scheduledChild_.reset();
}

void GbaRfuDataChannel::setConnection(bool connected, bool parent) {
    connected_ = connected;
    parent_ = connected && parent;
}

GbaRfuDataChannel::ReceiveResult GbaRfuDataChannel::receive(
    uint32_t sequence, uint8_t payloadBytes,
    const std::vector<uint32_t> &frameWords) {
    if (!enabled_)
        return ReceiveResult::LinkDisabled;
    if (sequence != expectedSequence_)
        return ReceiveResult::UnexpectedSequence;
    if (payloadBytes > 127 ||
        frameWords.size() != (payloadBytes + 3) / 4 ||
        frameWords.size() + 1 > GbaRfuRequestDecoder::MAX_PAYLOAD_WORDS)
        return ReceiveResult::PayloadTooLarge;
    if (!connected_) {
        // In-flight packets after disconnect still advance the reliable
        // transport sequence so the sender can release its send credit.
        ++expectedSequence_;
        return ReceiveResult::DroppedDisconnected;
    }

    if (parent_ && payloadBytes == 0) {
        ++expectedSequence_;
        return ReceiveResult::Accepted;
    }
    Message message{sequence, payloadBytes, frameWords};
    if (parent_) {
        // Keep one admitted child response until GbaSystem completes the
        // outstanding parent exchange. The staging module itself has no
        // clock or knowledge of the caller's exchange state.
        if (scheduledChild_ || received_.size() >= MAX_QUEUED_MESSAGES)
            return ReceiveResult::QueueFull;
        scheduledChild_ = std::move(message);
    } else {
        if (received_.size() >= MAX_QUEUED_MESSAGES)
            return ReceiveResult::QueueFull;
        received_.push_back(std::move(message));
    }
    ++expectedSequence_;
    return ReceiveResult::Accepted;
}

bool GbaRfuDataChannel::advanceParentRadioCycle() {
    if (!connected_ || !parent_ || !scheduledChild_ ||
        received_.size() >= MAX_QUEUED_MESSAGES)
        return false;
    received_.push_back(std::move(*scheduledChild_));
    scheduledChild_.reset();
    return true;
}

std::optional<GbaRfuDataChannel::Message>
GbaRfuDataChannel::takeReceived() {
    if (received_.empty())
        return std::nullopt;
    Message message = std::move(received_.front());
    received_.pop_front();
    return message;
}

std::vector<uint32_t> GbaRfuDataChannel::disconnect() {
    std::vector<uint32_t> sequences;
    sequences.reserve(received_.size() + (scheduledChild_ ? 1 : 0));
    for (const auto &message : received_)
        sequences.push_back(message.sequence);
    if (scheduledChild_)
        sequences.push_back(scheduledChild_->sequence);
    received_.clear();
    scheduledChild_.reset();
    connected_ = false;
    parent_ = false;
    return sequences;
}

GbaRfuCommandGroup::Decision GbaRfuCommandGroup::append(
    uint8_t payloadBytes, const std::vector<uint32_t> &words) {
    const auto wait = [this] {
        active_ = false; fence_ = true;
        return Decision::WaitForPeer;
    };
    if (fence_) return Decision::WaitForPeer;
    if (words.size() != (payloadBytes + 3u) / 4u) return wait();
    const auto byte = [&words](size_t at) {
        return static_cast<uint8_t>(words[at / 4] >> ((at % 4) * 8));
    };
    if (active_ && ++commands_ >= MAX_COMMANDS) return wait();
    if (payloadBytes == 0 || payloadBytes == 1)
        return active_ ? Decision::BufferedPrefix : wait();
    if (payloadBytes != 73) return wait();
    const uint32_t header = uint32_t{byte(0)} | (uint32_t{byte(1)} << 8) |
                            (uint32_t{byte(2)} << 16);
    if ((header & 127) != 70 || ((header >> 14) & 15) != 4)
        return wait(); // UNI only; NI keeps its guest-generated ACK contract.
    uint16_t command = 0, argument = 0, owner = 0;
    for (size_t row = 0; row < 5; ++row) {
        const size_t at = 3 + row * 14;
        const uint16_t cmd = static_cast<uint16_t>(byte(at) | (uint16_t{byte(at + 1)} << 8));
        if (row == 0) {
            command = cmd;
            argument = static_cast<uint16_t>(byte(at + 2) | (uint16_t{byte(at + 3)} << 8));
            owner = static_cast<uint16_t>(byte(at + 4) | (uint16_t{byte(at + 5)} << 8));
        } else if (cmd && cmd != 0xBE00 && (cmd & 0xFF00) != 0x8800 &&
                   (cmd & 0xFF00) != 0x8900)
            return wait(); // Readiness/disconnect/unknown peer commands fence the stream.
    }
    if (command == 0x8800) {
        if (!argument || argument > MAX_BLOCKS || owner != 0x80) return wait();
        if (!active_) { active_ = true; blocks_ = argument; next_ = 0; commands_ = 1; }
        else if (next_ != 0 || argument != blocks_) return wait();
        return Decision::BufferedPrefix;
    }
    if ((command & 0xFF00) == 0x8900) {
        const unsigned index = command & 255;
        if (!active_ || index != next_) return wait();
        ++next_;
        return next_ == blocks_ ? wait() : Decision::BufferedPrefix;
    }
    // Idle/key traffic outside an explicit block group must complete remotely.
    // An unbounded stream of speculative successes can fill RX while the guest
    // is waiting for the next radio completion before issuing another 0x26.
    if (command == 0 || command == 0xBE00)
        return active_ ? Decision::BufferedPrefix : wait();
    return wait();
}
void GbaRfuCommandGroup::completeFence() { reset(); }
void GbaRfuCommandGroup::reset() {
    active_ = fence_ = false;
    blocks_ = next_ = 0;
    commands_ = 0;
}

uint32_t makeGbaRfuCommandHeader(uint8_t command, uint8_t payloadWords) {
    if (payloadWords > GbaRfuRequestDecoder::MAX_PAYLOAD_WORDS)
        throw std::invalid_argument("RFU payload exceeds packet capacity");
    return GbaRfuRequestDecoder::HEADER_MAGIC |
        (static_cast<uint32_t>(payloadWords) << 8) | command;
}

uint32_t makeGbaRfuResponseHeader(uint8_t command, uint8_t payloadWords) {
    if (command > 0x7F)
        throw std::invalid_argument("RFU command cannot be acknowledged");
    return makeGbaRfuCommandHeader(
        static_cast<uint8_t>(command | 0x80), payloadWords);
}

GbaRfuCommandSession::Exchange GbaRfuCommandSession::exchangeWord(
    uint32_t guestWord) {
    Exchange exchange;
    switch (phase_) {
    case Phase::Request: {
        const auto result = decoder_.pushWord(guestWord, exchange.request);
        if (result == GbaRfuRequestDecoder::Result::Invalid) {
            exchange.protocolError = true;
            return exchange;
        }
        if (result == GbaRfuRequestDecoder::Result::Complete) {
            activeCommand_ = exchange.request.command;
            phase_ = Phase::Acknowledge;
            exchange.requestComplete = true;
        }
        return exchange;
    }
    case Phase::Acknowledge:
        if (acknowledgement_ == 0)
            return exchange;
        exchange.adapterWord = acknowledgement_;
        acknowledgement_ = 0;
        if (responseWords_.empty()) {
            phase_ = Phase::Request;
            activeCommand_ = 0;
        } else {
            phase_ = Phase::Response;
            responseIndex_ = 0;
        }
        return exchange;
    case Phase::Response:
        exchange.adapterWord = responseWords_[responseIndex_++];
        if (responseIndex_ == responseWords_.size()) {
            responseWords_.clear();
            responseIndex_ = 0;
            activeCommand_ = 0;
            phase_ = Phase::Request;
            exchange.responseComplete = true;
        }
        return exchange;
    }
    return exchange;
}

void GbaRfuCommandSession::respond(bool accepted,
                                   std::vector<uint32_t> responseWords,
                                   uint32_t errorCode) {
    if (phase_ != Phase::Acknowledge || acknowledgement_ != 0)
        throw std::logic_error("RFU command has no pending acknowledgement");
    if (responseWords.size() > GbaRfuRequestDecoder::MAX_PAYLOAD_WORDS)
        throw std::invalid_argument("RFU response exceeds packet capacity");

    if (accepted) {
        acknowledgement_ = makeGbaRfuResponseHeader(
            activeCommand_, static_cast<uint8_t>(responseWords.size()));
        responseWords_ = std::move(responseWords);
    } else {
        acknowledgement_ = makeGbaRfuCommandHeader(0xEE, 1);
        responseWords_ = {errorCode};
    }
}

void GbaRfuCommandSession::reset() {
    phase_ = Phase::Request;
    decoder_.reset();
    activeCommand_ = 0;
    acknowledgement_ = 0;
    responseWords_.clear();
    responseIndex_ = 0;
}

bool GbaRfuCommandSession::awaitingResponse() const {
    return phase_ != Phase::Request;
}

void GbaRfuLogin::reset() {
    previousGuestHalfword_ = 0;
    transferCount_ = 0;
}

uint32_t GbaRfuLogin::exchangeWord(uint32_t guestWord) {
    if (complete())
        throw std::logic_error("RFU login exchange already completed");

    const uint16_t guestHalfword = static_cast<uint16_t>(guestWord);
    uint32_t adapterWord = 0;
    if (transferCount_ != 0) {
        adapterWord = (static_cast<uint32_t>(guestHalfword) << 16) |
            static_cast<uint16_t>(~previousGuestHalfword_);
    }
    previousGuestHalfword_ = guestHalfword;
    ++transferCount_;
    return adapterWord;
}

} // namespace LOTUSGBA
