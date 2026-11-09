#include "gba_cartridge_rtc.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <stdexcept>

namespace LOTUSGBA {
namespace {

constexpr uint8_t STORAGE_MAGIC[] = {'G', 'R', 'T', 'C'};
constexpr uint8_t STORAGE_VERSION = 1;
constexpr size_t STORAGE_STATUS_OFFSET = 5;
constexpr size_t STORAGE_ALARM_HOUR_OFFSET = 6;
constexpr size_t STORAGE_ALARM_MINUTE_OFFSET = 7;
constexpr size_t STORAGE_RTC_TIME_OFFSET = 8;
constexpr size_t STORAGE_HOST_TIME_OFFSET = 16;

constexpr uint8_t RTC_24_HOUR = 1u << 6;
constexpr uint8_t RTC_POWER_FAILURE = 1u << 7;

int64_t hostTimeNow() {
    return static_cast<int64_t>(std::time(nullptr));
}

uint64_t readU64(const std::vector<uint8_t> &data, size_t offset) {
    uint64_t value = 0;
    for (unsigned byte = 0; byte < 8; ++byte)
        value |= static_cast<uint64_t>(data[offset + byte]) << (byte * 8);
    return value;
}

void writeU64(std::vector<uint8_t> &data, size_t offset, uint64_t value) {
    for (unsigned byte = 0; byte < 8; ++byte)
        data[offset + byte] = static_cast<uint8_t>(value >> (byte * 8));
}

uint8_t toBcd(unsigned value) {
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

bool fromBcd(uint8_t value, unsigned &decoded) {
    const unsigned tens = (value >> 4) & 0x0F;
    const unsigned units = value & 0x0F;
    if (tens > 9 || units > 9)
        return false;
    decoded = tens * 10 + units;
    return true;
}

bool isLeapYear(unsigned year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

unsigned daysInMonth(unsigned year, unsigned month) {
    constexpr unsigned days[] = {31, 28, 31, 30, 31, 30,
                                 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12)
        return 0;
    if (month == 2 && isLeapYear(year))
        return 29;
    return days[month - 1];
}

std::tm localTime(int64_t timestamp) {
    const std::time_t value = static_cast<std::time_t>(timestamp);
    const std::tm *local = std::localtime(&value);
    if (!local)
        throw std::runtime_error("GBA RTC could not convert host time");
    return *local;
}

bool validStorage(const std::vector<uint8_t> &data) {
    return data.size() == GbaCartridgeRtc::STORAGE_SIZE &&
        std::equal(std::begin(STORAGE_MAGIC), std::end(STORAGE_MAGIC),
                   data.begin()) &&
        data[4] == STORAGE_VERSION;
}

} // namespace

GbaCartridgeRtc::GbaCartridgeRtc(
    bool present, std::shared_ptr<std::vector<uint8_t>> storage)
    : present_(present), storage_(std::move(storage)) {
    if (!present_) {
        if (storage_)
            throw std::invalid_argument("RTC storage supplied for a cartridge without RTC hardware");
        return;
    }
    if (!storage_)
        storage_ = std::make_shared<std::vector<uint8_t>>(STORAGE_SIZE, 0);
    if (storage_->size() != STORAGE_SIZE)
        throw std::invalid_argument("GBA RTC save data must be 24 bytes");
}

size_t GbaCartridgeRtc::storageSize() const {
    return present_ && storage_ ? storage_->size() : 0;
}

uint8_t *GbaCartridgeRtc::storageData() {
    return present_ && storage_ ? storage_->data() : nullptr;
}

bool GbaCartridgeRtc::handlesAddress(uint32_t address) const {
    if (!present_ || address < 0x08000000 || address >= 0x0E000000)
        return false;
    const uint32_t offset = address & 0x01FFFFFF;
    return offset >= GPIO_DATA_OFFSET && offset <= GPIO_CONTROL_OFFSET + 1;
}

uint8_t GbaCartridgeRtc::readByte(uint32_t address, uint8_t romValue) const {
    if (!handlesAddress(address))
        return romValue;
    ensureInitialized();
    if ((gpioControl_ & 1) == 0)
        return romValue;
    return readGpioRegister(address & 0x01FFFFFF, romValue);
}

uint8_t GbaCartridgeRtc::readGpioRegister(uint32_t offset,
                                          uint8_t romValue) const {
    switch (offset) {
    case GPIO_DATA_OFFSET:
        return readGpioData();
    case GPIO_DATA_OFFSET + 1:
    case GPIO_DIRECTION_OFFSET + 1:
    case GPIO_CONTROL_OFFSET + 1:
        return 0;
    case GPIO_DIRECTION_OFFSET:
        return gpioDirection_ & 0x0F;
    case GPIO_CONTROL_OFFSET:
        return gpioControl_ & 1;
    default:
        return romValue;
    }
}

uint8_t GbaCartridgeRtc::readGpioData() const {
    const uint8_t outputPins = static_cast<uint8_t>(gpioData_ & gpioDirection_);
    const uint8_t inputPins = (gpioDirection_ & PIN_SIO) == 0 && rtcSioOutput_
        ? PIN_SIO
        : 0;
    return static_cast<uint8_t>((outputPins | inputPins) & 0x0F);
}

void GbaCartridgeRtc::writeRegister(uint32_t address, uint16_t value) {
    if (!handlesAddress(address))
        return;
    ensureInitialized();
    const uint32_t offset = address & 0x01FFFFFF;
    if (offset == GPIO_DATA_OFFSET) {
        const uint8_t oldData = gpioData_;
        gpioData_ = static_cast<uint8_t>(value & 0x0F);
        const bool wasSelected = (oldData & PIN_CS) != 0;
        const bool selected = (gpioData_ & PIN_CS) != 0;
        if (!wasSelected && selected)
            beginTransaction();
        if (wasSelected && !selected)
            endTransaction();
        if (selected && (oldData & PIN_SCK) == 0 &&
            (gpioData_ & PIN_SCK) != 0)
            clockRisingEdge();
        return;
    }
    if (offset == GPIO_DIRECTION_OFFSET) {
        gpioDirection_ = static_cast<uint8_t>(value & 0x0F);
        return;
    }
    if (offset == GPIO_CONTROL_OFFSET)
        gpioControl_ = static_cast<uint8_t>(value & 1);
}

void GbaCartridgeRtc::resetPort() {
    gpioData_ = 0;
    gpioDirection_ = 0;
    gpioControl_ = 0;
    rtcSioOutput_ = false;
    transactionActive_ = false;
    commandShift_ = 0;
    commandBitCount_ = 0;
    command_ = 0;
    commandComplete_ = false;
    commandIsRead_ = false;
    dataBitCount_ = 0;
    dataByteIndex_ = 0;
    dataShift_ = 0;
    writeLength_ = 0;
    readPayload_.fill(0xFF);
    writePayload_.fill(0);
    readLength_ = 0;
}

void GbaCartridgeRtc::beginTransaction() {
    transactionActive_ = true;
    commandShift_ = 0;
    commandBitCount_ = 0;
    command_ = 0;
    commandComplete_ = false;
    commandIsRead_ = false;
    dataBitCount_ = 0;
    dataByteIndex_ = 0;
    dataShift_ = 0;
    writeLength_ = 0;
    readPayload_.fill(0xFF);
    writePayload_.fill(0);
    readLength_ = 0;
    rtcSioOutput_ = false;
}

void GbaCartridgeRtc::endTransaction() {
    if (!transactionActive_)
        return;
    transactionActive_ = false;
    rtcSioOutput_ = false;
    if (commandComplete_ && !commandIsRead_ && writeLength_ != 0 &&
        dataByteIndex_ == writeLength_ && dataBitCount_ == 0)
        persistRtcTime();
}

void GbaCartridgeRtc::clockRisingEdge() {
    if (!transactionActive_)
        return;
    if (!commandComplete_) {
        if ((gpioDirection_ & PIN_SIO) == 0)
            return;
        commandShift_ = static_cast<uint8_t>((commandShift_ << 1) |
                                             ((gpioData_ & PIN_SIO) != 0));
        ++commandBitCount_;
        if (commandBitCount_ == 8)
            decodeCommand();
        return;
    }
    if (commandIsRead_) {
        if ((gpioDirection_ & PIN_SIO) != 0 || readLength_ == 0 ||
            dataByteIndex_ >= readLength_) {
            rtcSioOutput_ = false;
            return;
        }
        rtcSioOutput_ = ((readPayload_[dataByteIndex_] >> dataBitCount_) & 1) != 0;
        if (++dataBitCount_ == 8) {
            dataBitCount_ = 0;
            ++dataByteIndex_;
        }
        return;
    }
    if ((gpioDirection_ & PIN_SIO) == 0 || writeLength_ == 0 ||
        dataByteIndex_ >= writeLength_)
        return;
    dataShift_ = static_cast<uint8_t>(dataShift_ |
        (((gpioData_ & PIN_SIO) != 0 ? 1u : 0u) << dataBitCount_));
    if (++dataBitCount_ == 8) {
        acceptWriteByte(dataShift_);
        dataShift_ = 0;
        dataBitCount_ = 0;
        ++dataByteIndex_;
        if (dataByteIndex_ == writeLength_)
            applyWritePayload();
    }
}

void GbaCartridgeRtc::decodeCommand() {
    command_ = commandShift_;
    commandComplete_ = true;
    commandIsRead_ = (command_ & 1) != 0;
    if (command_ == 0x60) {
        resetRtc();
        return;
    }
    if (command_ == 0x6A)
        return; // Force the RTC interrupt output; the core has no cartridge IRQ line.
    if (commandIsRead_) {
        prepareReadPayload();
        return;
    }
    switch ((command_ >> 1) & 7) {
    case 1: writeLength_ = 1; break; // Status.
    case 2: writeLength_ = 7; break; // Date and time.
    case 3: writeLength_ = 3; break; // Time.
    case 4: writeLength_ = 2; break; // Alarm.
    default: writeLength_ = 0; break;
    }
}

void GbaCartridgeRtc::prepareReadPayload() {
    readPayload_.fill(0xFF);
    dataBitCount_ = 0;
    dataByteIndex_ = 0;
    const unsigned commandNumber = (command_ >> 1) & 7;
    if (commandNumber == 1) {
        readPayload_[0] = status_;
        status_ &= static_cast<uint8_t>(~RTC_POWER_FAILURE);
        readLength_ = 1;
        persistRtcTime();
        return;
    }

    const std::tm date = localTime(currentRtcTime());
    const unsigned year = static_cast<unsigned>(date.tm_year + 1900);
    const uint8_t hour24 = toBcd(static_cast<unsigned>(date.tm_hour));
    uint8_t hour12 = hour24;
    if ((status_ & RTC_24_HOUR) == 0) {
        unsigned hour = static_cast<unsigned>(date.tm_hour) % 12;
        if (hour == 0)
            hour = 12;
        hour12 = static_cast<uint8_t>(toBcd(hour) |
            (date.tm_hour >= 12 ? 0x80 : 0));
    }
    if (commandNumber == 2) {
        readPayload_[0] = toBcd((year + 100 - 2000) % 100);
        readPayload_[1] = toBcd(static_cast<unsigned>(date.tm_mon + 1));
        readPayload_[2] = toBcd(static_cast<unsigned>(date.tm_mday));
        readPayload_[3] = toBcd(static_cast<unsigned>(date.tm_wday));
        readPayload_[4] = hour12;
        readPayload_[5] = toBcd(static_cast<unsigned>(date.tm_min));
        readPayload_[6] = toBcd(static_cast<unsigned>(date.tm_sec));
        readLength_ = 7;
    } else if (commandNumber == 3) {
        readPayload_[0] = hour12;
        readPayload_[1] = toBcd(static_cast<unsigned>(date.tm_min));
        readPayload_[2] = toBcd(static_cast<unsigned>(date.tm_sec));
        readLength_ = 3;
    } else if (commandNumber == 4) {
        readPayload_[0] = alarmHour_;
        readPayload_[1] = alarmMinute_;
        readLength_ = 2;
    } else {
        readLength_ = 1;
    }
}

void GbaCartridgeRtc::acceptWriteByte(uint8_t value) {
    if (dataByteIndex_ < writePayload_.size())
        writePayload_[dataByteIndex_] = value;
}

void GbaCartridgeRtc::applyWritePayload() {
    if (writeLength_ == 1 && ((command_ >> 1) & 7) == 1) {
        status_ = static_cast<uint8_t>((status_ & RTC_POWER_FAILURE) |
            (writePayload_[0] & 0x6A));
    } else if (writeLength_ == 7 && ((command_ >> 1) & 7) == 2) {
        unsigned year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
        if (!fromBcd(writePayload_[0], year) ||
            !fromBcd(writePayload_[1], month) ||
            !fromBcd(writePayload_[2], day) ||
            !fromBcd(static_cast<uint8_t>(writePayload_[4] & 0x7F), hour) ||
            !fromBcd(writePayload_[5], minute) ||
            !fromBcd(writePayload_[6], second))
            return;
        year += 2000;
        if (day < 1 || day > daysInMonth(year, month) || hour > 23 ||
            minute > 59 || second > 59)
            return;
        if ((status_ & RTC_24_HOUR) == 0) {
            hour %= 12;
            if ((writePayload_[4] & 0x80) != 0)
                hour += 12;
        }
        std::tm date{};
        date.tm_year = static_cast<int>(year) - 1900;
        date.tm_mon = static_cast<int>(month) - 1;
        date.tm_mday = static_cast<int>(day);
        date.tm_hour = static_cast<int>(hour);
        date.tm_min = static_cast<int>(minute);
        date.tm_sec = static_cast<int>(second);
        date.tm_isdst = -1;
        const std::time_t timestamp = std::mktime(&date);
        if (timestamp == static_cast<std::time_t>(-1))
            return;
        rtcTimeBase_ = static_cast<int64_t>(timestamp);
        hostTimeBase_ = hostTimeNow();
    } else if (writeLength_ == 3 && ((command_ >> 1) & 7) == 3) {
        unsigned hour = 0, minute = 0, second = 0;
        if (!fromBcd(static_cast<uint8_t>(writePayload_[0] & 0x7F), hour) ||
            !fromBcd(writePayload_[1], minute) ||
            !fromBcd(writePayload_[2], second) || minute > 59 || second > 59)
            return;
        if ((status_ & RTC_24_HOUR) == 0) {
            if (hour < 1 || hour > 12)
                return;
            hour %= 12;
            if ((writePayload_[0] & 0x80) != 0)
                hour += 12;
        } else if (hour > 23) {
            return;
        }
        std::tm date = localTime(currentRtcTime());
        date.tm_hour = static_cast<int>(hour);
        date.tm_min = static_cast<int>(minute);
        date.tm_sec = static_cast<int>(second);
        date.tm_isdst = -1;
        const std::time_t timestamp = std::mktime(&date);
        if (timestamp == static_cast<std::time_t>(-1))
            return;
        rtcTimeBase_ = static_cast<int64_t>(timestamp);
        hostTimeBase_ = hostTimeNow();
    } else if (writeLength_ == 2 && ((command_ >> 1) & 7) == 4) {
        alarmHour_ = writePayload_[0];
        alarmMinute_ = writePayload_[1];
    }
    persistRtcTime();
}

void GbaCartridgeRtc::resetRtc() {
    status_ = 0;
    alarmHour_ = 0;
    alarmMinute_ = 0;
    std::tm resetDate{};
    resetDate.tm_year = 100; // S-3511 reset date is 2000-01-01.
    resetDate.tm_mon = 0;
    resetDate.tm_mday = 1;
    resetDate.tm_isdst = -1;
    const std::time_t timestamp = std::mktime(&resetDate);
    rtcTimeBase_ = timestamp == static_cast<std::time_t>(-1)
        ? hostTimeNow()
        : static_cast<int64_t>(timestamp);
    hostTimeBase_ = hostTimeNow();
    persistRtcTime();
}

int64_t GbaCartridgeRtc::currentRtcTime() const {
    const int64_t now = hostTimeNow();
    return rtcTimeBase_ + std::max<int64_t>(0, now - hostTimeBase_);
}

void GbaCartridgeRtc::persistRtcTime() const {
    if (!storage_)
        return;
    const int64_t now = hostTimeNow();
    rtcTimeBase_ = rtcTimeBase_ + std::max<int64_t>(0, now - hostTimeBase_);
    hostTimeBase_ = now;
    std::copy(std::begin(STORAGE_MAGIC), std::end(STORAGE_MAGIC), storage_->begin());
    (*storage_)[4] = STORAGE_VERSION;
    (*storage_)[STORAGE_STATUS_OFFSET] = status_;
    (*storage_)[STORAGE_ALARM_HOUR_OFFSET] = alarmHour_;
    (*storage_)[STORAGE_ALARM_MINUTE_OFFSET] = alarmMinute_;
    writeU64(*storage_, STORAGE_RTC_TIME_OFFSET,
             static_cast<uint64_t>(rtcTimeBase_));
    writeU64(*storage_, STORAGE_HOST_TIME_OFFSET,
             static_cast<uint64_t>(hostTimeBase_));
}

void GbaCartridgeRtc::ensureInitialized() const {
    if (!present_ || initialized_)
        return;
    if (!storage_ || storage_->size() != STORAGE_SIZE)
        throw std::runtime_error("GBA RTC persistence block is unavailable");
    const int64_t now = hostTimeNow();
    if (validStorage(*storage_)) {
        status_ = static_cast<uint8_t>((*storage_)[STORAGE_STATUS_OFFSET] & 0xEA);
        alarmHour_ = (*storage_)[STORAGE_ALARM_HOUR_OFFSET];
        alarmMinute_ = (*storage_)[STORAGE_ALARM_MINUTE_OFFSET];
        rtcTimeBase_ = static_cast<int64_t>(readU64(*storage_, STORAGE_RTC_TIME_OFFSET));
        hostTimeBase_ = static_cast<int64_t>(readU64(*storage_, STORAGE_HOST_TIME_OFFSET));
        rtcTimeBase_ += std::max<int64_t>(0, now - hostTimeBase_);
        hostTimeBase_ = now;
    } else {
        status_ = RTC_24_HOUR;
        alarmHour_ = 0;
        alarmMinute_ = 0;
        rtcTimeBase_ = now;
        hostTimeBase_ = now;
    }
    initialized_ = true;
    persistRtcTime();
}

} // namespace LOTUSGBA
