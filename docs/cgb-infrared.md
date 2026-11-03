# CGB infrared port

Native CGB now models the digital RP port at `$FF56` and lets a host connect
two CGB systems through level callbacks. The guest controls emission and
reception through RP; no frontend or operating-system link service is required
for the core contract.

## RP behavior

The [CGB register reference](https://gbdev.io/pandocs/CGB_Registers.html#ff56--rp-cgb-mode-only-infrared-communications-port)
defines RP bit 0 as LED output, bits 7–6 as receive enable, and bit 1 as the
read-only active-low sensor result. Bits 2–5 read high. With either receive
enable bit clear, bit 1 reads high. After reception is enabled, the
[Game Boy Programming Manual](https://thissideout.wordpress.com/wp-content/uploads/2014/02/gameboyprogrammingmanual.pdf)
requires at least 50 ms before sampling. LotusGB counts 209,716 base T-cycles
(the first whole clock at or after 50 ms); this remains 50 ms in CGB double
speed. Disabling reception and enabling it again restarts the wait. Toggling
the LED while reception stays enabled does not.

The modeled receiver is digital. Its input callback is sampled on CGB base-clock
advancement; the sensor result is latched until the next sample, matching the
clocked input described in the programming manual. When enabled and settled,
detected light clears bit 1; otherwise bit 1 stays set. The callback is host wiring,
not guest state, and survives snapshot restore. `getInfraredLedState()` exposes
the local LED level for the peer callback. A DMG instance has no LED and cannot
install an IR input callback; CGB DMG-compatibility mode gates RP as native
hardware does.

Example two-system wiring:

```cpp
left.setInfraredInputCallback([&] { return right.getInfraredLedState(); });
right.setInfraredInputCallback([&] { return left.getInfraredLedState(); });
```

The host owns both systems and their advancement. Callbacks are sampled at the
system's emulated clock boundaries, so a host may also supply ambient-light or
test signals.

## Snapshots and tests

CGB snapshot v8 introduced the remaining receiver-settling clocks and the latched
sensor level; current CGB v13 also carries MBC7 cartridge protocol and rumble
control state. CGB v2–v7 states migrate with no pending wait; their earlier RP
behavior did not model a sensor. Host callbacks are never serialized. The
candidate-validate-then-commit loader preserves callbacks already installed on
the live system.

`make test` runs `cgb_ir_test`, which covers RP masks and read/write behavior,
the 50 ms threshold, receive restart, a CPU guest communicating with a second
CPU-driven CGB, normal and double CPU speed against the same base-time delay,
and exact replay from a snapshot taken during warm-up.

This does not model sensor sensitivity, ambient-light adaptation/fade,
phototransistor rise time or an actual optical path. RetroArch transport and
real-device IR timing remain outside the core interface.
