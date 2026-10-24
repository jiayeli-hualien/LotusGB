#ifndef LOTUSGB_ADDRESS_BUS_HPP
#define LOTUSGB_ADDRESS_BUS_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "IMemoryAccess.hpp"

namespace LOTUSGB {

class IAddressRegion {
public:
    virtual ~IAddressRegion() = default;
    virtual uint8_t read(uint16_t address) = 0;
    virtual void write(uint16_t address, uint8_t value) = 0;
};

// A bus master may temporarily own CPU-visible addresses (for example DMA).
class ICpuBusArbiter {
public:
    virtual ~ICpuBusArbiter() = default;
    virtual bool ownsCpuAddress(uint16_t address) const = 0;
    virtual uint8_t readConflictedCpuAddress(uint16_t address) const = 0;
};

class AddressBus final : public IMemoryAccess {
public:
    // Mapped regions are borrowed and must outlive this bus.
    void map(uint16_t start, uint16_t end, IAddressRegion &region);
    // Map a CPU address range onto a region at a different address range.
    void mapAlias(uint16_t start, uint16_t end, IAddressRegion &region,
                  uint16_t regionStart);
    // Called at the CPU bus phase, before dispatching a read or write.
    void setCpuBusAccessCallback(std::function<void()> callback);
    // Borrowed, like mapped regions; must outlive bus accesses.
    void setCpuBusArbiter(ICpuBusArbiter &arbiter);
    // Device-originated reads do not advance CPU time or apply CPU arbitration.
    uint8_t readForDma(uint16_t address);
    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    void resize(size_t) override {}

private:
    struct Mapping {
        uint16_t start;
        uint16_t end;
        IAddressRegion *region;
        uint16_t regionStart;
    };

    void onCpuBusAccess();

    ICpuBusArbiter *cpuBusArbiter = nullptr;
    std::vector<Mapping> mappings;
    std::function<void()> cpuBusAccessCallback;
};

}

#endif // LOTUSGB_ADDRESS_BUS_HPP
