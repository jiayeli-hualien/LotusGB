#ifndef LOTUSGB_CARTRIDGE_HPP
#define LOTUSGB_CARTRIDGE_HPP

#include <cstdint>
#include <vector>

#include "address_bus.hpp"

namespace LOTUSGB {

// Initial cartridge implementation for ROM-only cartridges without banking.
class RomOnlyCartridge final : public IAddressRegion {
public:
    explicit RomOnlyCartridge(std::vector<uint8_t> rom);

    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;

private:
    std::vector<uint8_t> rom;
};

}

#endif // LOTUSGB_CARTRIDGE_HPP
