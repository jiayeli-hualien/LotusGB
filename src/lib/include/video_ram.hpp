#ifndef LOTUSGB_VIDEO_RAM_HPP
#define LOTUSGB_VIDEO_RAM_HPP

#include "address_bus.hpp"

namespace LOTUSGB {

// CPU accesses use read/write and the selected bank. PPU fetches specify a
// bank independently and bypass CPU access windows. The PPU owns those windows;
// implementations own storage and bank selection, not PPU timing.
class IVideoRam : public IAddressRegion {
public:
    virtual uint8_t readBank(uint8_t bank, uint16_t address) const = 0;
};

}

#endif
