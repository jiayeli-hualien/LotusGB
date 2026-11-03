#ifndef LOTUSGB_BOOT_ROM_HPP
#define LOTUSGB_BOOT_ROM_HPP

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "address_bus.hpp"
#include "cartridge.hpp"

namespace LOTUSGB {

enum class HardwareModel { Dmg, CgbE };

// Model-specific boot overlay and one-way $FF50 disable latch.
// CGB images use an address-indexed 0x900-byte layout; $0100-$01FF is ignored.
class BootRom final : public IAddressRegion {
    friend class StateCodec;
public:
    BootRom(Cartridge &cartridge, std::vector<uint8_t> bootRom,
            HardwareModel model = HardwareModel::Dmg);

    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    bool isMapped() const { return mapped; }
    void setUnmappedCallback(std::function<void()> callback) { unmappedCallback = std::move(callback); }

private:
    std::function<void()> unmappedCallback;
    Cartridge &cartridge;
    std::vector<uint8_t> bootRom;
    bool mapped;
    HardwareModel model;
};

}

#endif // LOTUSGB_BOOT_ROM_HPP
