#ifndef LOTUSGB_MEMORY_REGION_HPP
#define LOTUSGB_MEMORY_REGION_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "address_bus.hpp"

namespace LOTUSGB {

class RamRegion final : public IAddressRegion {
    friend class StateCodec;
public:
    RamRegion(uint16_t startAddress, size_t size);

    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;

private:
    uint16_t startAddress;
    std::vector<uint8_t> bytes;
};

}

#endif // LOTUSGB_MEMORY_REGION_HPP
