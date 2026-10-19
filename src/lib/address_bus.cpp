#include "include/address_bus.hpp"

#include <stdexcept>

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

uint8_t AddressBus::read(uint16_t address) {
    for (const Mapping &mapping : mappings) {
        if (address >= mapping.start && address <= mapping.end)
            return mapping.region->read(address);
    }
    return 0xFF;
}

void AddressBus::write(uint16_t address, uint8_t value) {
    for (const Mapping &mapping : mappings) {
        if (address >= mapping.start && address <= mapping.end) {
            mapping.region->write(address, value);
            return;
        }
    }
}

}
