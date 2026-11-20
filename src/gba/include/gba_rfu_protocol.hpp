#ifndef LOTUSGBA_GBA_RFU_PROTOCOL_HPP
#define LOTUSGBA_GBA_RFU_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace LOTUSGBA {

// RFU commands travel as a 32-bit header followed by the number of 32-bit
// payload words declared in that header. This class only decodes that byte
// stream; command semantics and the radio/network service belong to a later
// layer.
class GbaRfuRequestDecoder final {
public:
    enum class Result { Incomplete, Complete, Invalid };

    struct Request {
        uint8_t command = 0;
        std::vector<uint32_t> payload;
    };

    static constexpr uint32_t HEADER_MAGIC = 0x99660000;
    static constexpr uint8_t MAX_PAYLOAD_WORDS = 0x1C;

    Result pushWord(uint32_t word, Request &request);
    void reset();
    bool awaitingPayload() const { return active_; }

private:
    bool active_ = false;
    uint8_t command_ = 0;
    uint8_t expectedWords_ = 0;
    std::vector<uint32_t> payload_;
};

// Role-aware RFU data staging between the reliable LAN transport and the
// game's radio-exchange/0x26 boundaries. This module has no CPU, ROM, SIO, or
// frontend dependency, so queue and sequence behavior can be tested alone.
class GbaRfuDataChannel final {
public:
    struct Message {
        uint32_t sequence = 0;
        uint8_t payloadBytes = 0;
        std::vector<uint32_t> frameWords;
    };

    enum class ReceiveResult {
        Accepted,
        LinkDisabled,
        NotConnected,
        DroppedDisconnected,
        UnexpectedSequence,
        PayloadTooLarge,
        QueueFull
    };

    static constexpr size_t MAX_QUEUED_MESSAGES = 32;

    void reset(bool enabled = false);
    void setConnection(bool connected, bool parent);
    ReceiveResult receive(uint32_t sequence, uint8_t payloadBytes,
                         const std::vector<uint32_t> &frameWords);
    // Move an admitted child reply into the completed parent RX queue.
    bool advanceParentRadioCycle();
    std::optional<Message> takeReceived();
    std::vector<uint32_t> disconnect();
    size_t queuedMessages() const { return received_.size(); }
    bool hasReceived() const { return !received_.empty(); }
    bool hasScheduledChildMessage() const {
        return scheduledChild_.has_value();
    }
    uint32_t expectedSequence() const { return expectedSequence_; }

private:
    bool enabled_ = false;
    bool connected_ = false;
    bool parent_ = false;
    uint32_t expectedSequence_ = 1;
    std::deque<Message> received_;
    std::optional<Message> scheduledChild_;
};

// Tracks the RFU's word-level command transaction: receive a framed request,
// emit the ACK on the next clocked word, then return any response words.
class GbaRfuCommandSession final {
public:
    struct Exchange {
        uint32_t adapterWord = 0x80000000;
        bool requestComplete = false;
        bool responseComplete = false;
        bool protocolError = false;
        GbaRfuRequestDecoder::Request request;
    };

    Exchange exchangeWord(uint32_t guestWord);
    void respond(bool accepted, std::vector<uint32_t> responseWords = {},
                 uint32_t errorCode = 1);
    void reset();
    bool awaitingResponse() const;

private:
    enum class Phase { Request, Acknowledge, Response };
    Phase phase_ = Phase::Request;
    GbaRfuRequestDecoder decoder_;
    uint8_t activeCommand_ = 0;
    uint32_t acknowledgement_ = 0;
    std::vector<uint32_t> responseWords_;
    size_t responseIndex_ = 0;
};

// The adapter login exchanges the "NINTENDO" challenge as 32-bit words. The
// responder keeps only the preceding guest halfword needed by the next reply.
class GbaRfuLogin final {
public:
    static constexpr uint8_t TRANSFER_COUNT = 10;

    void reset();
    uint32_t exchangeWord(uint32_t guestWord);
    uint8_t transferCount() const { return transferCount_; }
    bool complete() const { return transferCount_ == TRANSFER_COUNT; }

private:
    uint16_t previousGuestHalfword_ = 0;
    uint8_t transferCount_ = 0;
};

uint32_t makeGbaRfuCommandHeader(uint8_t command, uint8_t payloadWords);
uint32_t makeGbaRfuResponseHeader(uint8_t command, uint8_t payloadWords);

} // namespace LOTUSGBA

#endif
