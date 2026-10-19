#include <cstdlib>
#include <iostream>

#include "lotus_gbit_c_wrapper.h"

namespace {

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void testIndependentAdapterLifetimes() {
    pILotusGBIT first = lotusGBIT_Create();
    pILotusGBIT second = lotusGBIT_Create();
    expect(first && second, "create two independent GBIT adapters");

    uint8_t program[] = {0x00, 0x00};
    lotusGBIT_Init(first, sizeof(program), program);
    lotusGBIT_Init(second, sizeof(program), program);

    LotusGBState state{};
    state.sp = 0xFFFE;
    state.pc = 0;
    lotusGBIT_SetStatus(first, &state);
    lotusGBIT_SetStatus(second, &state);

    lotusGBIT_Destroy(first);

    // Destroying one adapter must not invalidate the other adapter's decoder.
    lotusGBIT_SetStatus(second, &state);
    LotusGBState actual{};
    lotusGBIT_GetStatus(second, &actual);
    expect(actual.pc == state.pc, "remaining adapter can restore and report CPU state");

    lotusGBIT_Destroy(second);
}

} // namespace

int main() {
    testIndependentAdapterLifetimes();
    std::cout << "Adapter lifetime tests passed\n";
    return EXIT_SUCCESS;
}
