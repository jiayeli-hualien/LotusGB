#include "include/address_bus.hpp"

#include <stdexcept>
#include <utility>

namespace LOTUSGB {

void AddressBus::map(uint16_t start, uint16_t end, IAddressRegion &region) {
    if (start > end)
        throw std::invalid_argument("address region start exceeds end");

    for (const Mapping &mapping : mappings) {
        if (start <= mapping.end && mapping.start <= end)
            throw std::invalid_argument("address regions cannot overlap");
    }

    mappings.push_back({start, end, &region});
}

void AddressBus::setCpuBusAccessCallback(std::function<void()> callback) {
    cpuBusAccessCallback = std::move(callback);
}

void AddressBus::onCpuBusAccess() {
    if (cpuBusAccessCallback)
        cpuBusAccessCallback();
}

uint8_t AddressBus::read(uint16_t address) {
    onCpuBusAccess();
    for (const Mapping &mapping : mappings) {
        if (address >= mapping.start && address <= mapping.end)
            return mapping.region->read(address);
    }
    return 0xFF;
}

void AddressBus::write(uint16_t address, uint8_t value) {
    onCpuBusAccess();
    for (const Mapping &mapping : mappings) {
        if (address >= mapping.start && address <= mapping.end) {
            mapping.region->write(address, value);
            return;
        }
    }
}

}
