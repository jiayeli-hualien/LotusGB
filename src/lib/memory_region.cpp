#include "include/memory_region.hpp"

#include <stdexcept>

namespace LOTUSGB {

RamRegion::RamRegion(uint16_t startAddress, size_t size)
    : startAddress(startAddress), bytes(size, 0) {
    const size_t addressSpaceSize = 0x10000;
    if (size == 0 || static_cast<size_t>(startAddress) + size > addressSpaceSize)
        throw std::invalid_argument("RAM region is outside the CPU address space");
}

uint8_t RamRegion::read(uint16_t address) {
    if (address < startAddress ||
        static_cast<size_t>(address - startAddress) >= bytes.size())
        return 0xFF;
    return bytes[address - startAddress];
}

void RamRegion::write(uint16_t address, uint8_t value) {
    if (address < startAddress ||
        static_cast<size_t>(address - startAddress) >= bytes.size())
        return;
    bytes[address - startAddress] = value;
}

}
