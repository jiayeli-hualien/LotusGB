#include "include/address_bus.hpp"

#include <stdexcept>
#include <utility>

namespace LOTUSGB {

void AddressBus::map(uint16_t start, uint16_t end, IAddressRegion &region) {
    mapAlias(start, end, region, start);
}

void AddressBus::mapAlias(uint16_t start, uint16_t end, IAddressRegion &region,
                          uint16_t regionStart) {
    if (start > end)
        throw std::invalid_argument("address region start exceeds end");

    const uint32_t regionEnd = static_cast<uint32_t>(regionStart) + (end - start);
    if (regionEnd > 0xFFFF)
        throw std::invalid_argument("aliased address region exceeds address space");

    for (const Mapping &mapping : mappings) {
        if (start <= mapping.end && mapping.start <= end)
            throw std::invalid_argument("address regions cannot overlap");
    }

    mappings.push_back({start, end, &region, regionStart});
}

void AddressBus::setCpuBusAccessCallback(std::function<void()> callback) {
    cpuBusAccessCallback = std::move(callback);
}

void AddressBus::setCpuBusEventCallback(std::function<void(uint16_t, CpuBusEvent)> callback) {
    cpuBusEventCallback = std::move(callback);
}

uint8_t AddressBus::readWithIdu(uint16_t address) { return readCpu(address, CpuBusEvent::ReadIdu); }
void AddressBus::writeWithIdu(uint16_t address, uint8_t value) { writeCpu(address, value, CpuBusEvent::WriteIdu); }
void AddressBus::notifyIdu(uint16_t address) {
    onCpuBusAccess();
    if (cpuBusEventCallback) cpuBusEventCallback(address, CpuBusEvent::Idu);
}

void AddressBus::setCpuBusArbiter(ICpuBusArbiter &arbiter) {
    cpuBusArbiter = &arbiter;
}

void AddressBus::onCpuBusAccess() {
    if (cpuBusAccessCallback)
        cpuBusAccessCallback();
}

uint8_t AddressBus::read(uint16_t address) { return readCpu(address, CpuBusEvent::Read); }
uint8_t AddressBus::readCpu(uint16_t address, CpuBusEvent event) {
    onCpuBusAccess();
    if (cpuBusEventCallback) cpuBusEventCallback(address, event);
    if (cpuBusArbiter && cpuBusArbiter->ownsCpuAddress(address))
        return cpuBusArbiter->readConflictedCpuAddress(address);
    return readForDma(address);
}

uint8_t AddressBus::readForDma(uint16_t address) {
    for (const Mapping &mapping : mappings) {
        if (address >= mapping.start && address <= mapping.end)
            return mapping.region->read(static_cast<uint16_t>(
                mapping.regionStart + (address - mapping.start)));
    }
    return 0xFF;
}

void AddressBus::write(uint16_t address, uint8_t value) { writeCpu(address, value, CpuBusEvent::Write); }
void AddressBus::writeCpu(uint16_t address, uint8_t value, CpuBusEvent event) {
    onCpuBusAccess();
    if (cpuBusEventCallback) cpuBusEventCallback(address, event);
    if (cpuBusArbiter && cpuBusArbiter->ownsCpuAddress(address))
        return;
    for (const Mapping &mapping : mappings) {
        if (address >= mapping.start && address <= mapping.end) {
            mapping.region->write(static_cast<uint16_t>(
                mapping.regionStart + (address - mapping.start)), value);
            return;
        }
    }
}

}
