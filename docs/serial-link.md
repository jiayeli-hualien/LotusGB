# Serial bit exchange

The core supports logical serial bit exchange between DMG/CGB ports through
host callbacks and externally scheduled sampling edges. Original component and
two-system CPU tests cover byte exchange, serial IRQ/HALT wake and paired
partial-transfer snapshot replay. RetroArch link transport, multiplayer game
acceptance, physical cables and accessories remain unsupported. CGB infrared
peer behavior has a separate [level-based interface and evidence](cgb-infrared.md).

## Host interface

`GameBoySystem` exposes three methods:

- `getSerialOutgoingBit()` returns SB bit 7, the next outgoing bit. This is a
  logical value, not a model of an idle electrical line.
- `clockSerialExternalBit(incoming)` supplies one sampling edge. It shifts only
  when SC has transfer enabled and external clock selected.
- `setSerialClockCallback(callback)` connects an internally clocked port. The
  callback receives its outgoing bit and returns the incoming bit. An empty
  callback restores incoming-high disconnected behavior.

`DmgSerial` provides the corresponding component methods. A host samples both
outgoing bits before shifting either endpoint. For example:

```cpp
master.setSerialClockCallback([&](bool outgoing) {
    const bool incoming = slave.getSerialOutgoingBit();
    slave.clockSerialExternalBit(outgoing);
    return incoming;
});
```

The host owns peer lifetimes and schedules both systems at the appropriate
emulated times. Callbacks run synchronously during internal clock advancement;
they must not recursively advance the master or mutate its transfer state.
Host failures propagate as exceptions, without substituting a successful
received bit. A failed host must decide how to recover or stop the session.

External mode does not shift merely because CPU time passes. Each supplied
edge shifts one MSB-first bit; eight edges clear transfer enable and request
serial IF. Abort retains partial SB data. DMG internal clock edges occur every
512 CPU T-cycles; CGB fast mode uses 16. CPU double speed halves those intervals
in base-clock time. See [Pan Docs serial transfer](https://github.com/gbdev/pandocs/blob/master/src/Serial_Data_Transfer_(Link_Cable).md).

## Snapshots and evidence

Callbacks and peer references are host wiring and are not serialized. Guest
SB, SC, partial bit count and clock phase use the existing snapshot fields;
the [snapshot format](save-states.md) carries guest progress without serializing host wiring. Loading guest state preserves an installed callback.
A link host must restore both endpoints and its own scheduling state at a
matching boundary; one endpoint's snapshot does not capture the other system.

`make test` covers:

- Component byte exchange in both directions across DMG/CGB combinations,
  intermediate SB values and eighth-edge IRQs; irregular external edges,
  abort/restart, inactive/internal-mode gating and post-completion behavior.
- Callback failure propagation and explicitly removing the callback to restore
  disconnected incoming-high transfers.
- Original CPU programs on two systems, exchanging `A5`/`3C`, waiting in HALT
  and exposing the received byte and completed SC state in cartridge SRAM.
  Tests include DMG and all four CGB clock-rate combinations.
- Paired snapshots after three edges, exact five remaining callbacks, stable
  SRAM pointers, retained host wiring, CPU-observed results and exact PCM/full
  state continuation for each endpoint.

The test host advances the slave before the master at M-cycle boundaries.
This validates the portable logical contract, not arbitrary asynchronous clock
phases, wire-level transitions, peer-device timing races or a frontend session.
