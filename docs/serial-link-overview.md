# Serial link overview

LotusGB models DMG/CGB serial transfers through a host interface that exchanges bits between systems. The libretro core also provides a two-content link subsystem and ordered network transport.

The network path prioritizes ordered data and frontend progress; it does not model cycle-accurate cable timing. Synthetic tests cover transfer order, interrupt behavior, paired-system state and selected delayed-event cases. These checks do not establish compatibility across games, hosts or network conditions. Physical cables and accessories are outside the current validation scope.

For implementation details, see the serial interface in [`src/lib/dmg_serial.cpp`](../src/lib/dmg_serial.cpp), the [architecture overview](architecture.md), and the [testing workflow](testing.md). Public game-level ROM checks use licensed homebrew and original fixtures.

## Network-mode fidelity contract

The network link mode prioritizes ordered game data and a responsive frontend;
it does not claim bit-accurate cable timing. Each RetroArch instance advances
its own CPU, PPU and APU clocks. The peers synchronize serial data by transfer
sequence, not by locking whole-frame or CPU-cycle progress. The physical cable
also has its own master-driven serial clock domain; the two consoles' CPU clock
phases do not need to match ([hardware references](references.md#hardware-behavior)).

In stream-v2, an internal serial clock edge sends the outgoing byte with a
transfer sequence. An armed external port replies to that request with its
current SB value. Two external ports wait until one supplies a clock. The
RetroArch host and client can each initiate a transfer when their ROM selects
internal clock. While the session remains connected, a missing
byte leaves the serial transfer pending as guest emulation and frontend
callbacks continue; the core does not silently substitute data. On a detected
peer disconnect it has an explicit open-line `$FF` fallback. Once the matching
byte arrives, all eight shifts and the serial interrupt are applied atomically.
This preserves byte values and order when both games issue matching transfer
sequences, while changing observable partial-SB and SC/interrupt timing. A
game's own link wait can therefore remain visible even while video and audio
callbacks continue.

After a peer disconnect or transport failure, this core session cannot safely
resume: the games may have advanced through different serial transfers. The
host rejects a replacement peer while the failed Netpacket session is active.
Both players must restart the content and multiplayer session before trying
again; the protocol does not restore or reconcile guest state across reconnects.

The unit and delayed-process tests establish this contract for synthetic ROMs.
They do not establish cycle-level cable accuracy, RetroArch GUI smoothness,
Pokémon trade compatibility, or save persistence for stream-v2.


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

The Game Boy serial port has no connector-present flag. A program can detect a
cooperating peer with a challenge byte and a known response byte; an
unconnected internal-clock transfer reads back `0xFF`. This only proves that a
peer speaks the probe protocol, not that an arbitrary game is ready to trade.

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
- A challenge/response probe that distinguishes the disconnected `0xFF`
  result from a peer returning the expected acknowledgement byte.
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


## RetroArch local link

Load the LotusGB core, choose **Game Boy Link (2 Players)** in **Subsystems**,
and select each player's ROM. The command-line equivalent is:

```sh
retroarch -L /path/to/lotusgb_libretro.so --subsystem=gb_link_2p first.gb second.gb
```

RetroPad ports 1 and 2 control the left and right screens. Audio comes from
player 1. The first slot uses `.srm` and `.rtc`. The second slot uses `.srm.2`
and `.rtc.2`, including when both ROMs have the same basename. Use the game's
own save command during a linked session. Link mode does not expose save
states, so there is no single-endpoint rewind or run-ahead.

The local scheduler advances both machines in order of M-cycle end time
on a shared base-clock timeline. Player 1 goes first when two M-cycles end
together. The libretro test runs original serial programs through the
subsystem, checks the exchanged bytes in separate SRAM regions, and verifies
that reset preserves the save pointers and reconnects the two ports.

## Separate RetroArch processes

Give each process its own configuration and save directory. Set
`pause_nonactive = "false"` so background guests continue running. Start the
host before the client, using each platform's native core artifact:

```sh
retroarch --config /path/to/host.cfg -L /path/to/lotusgb_libretro.so \
  --host /path/to/first.gb
retroarch --config /path/to/client.cfg -L /path/to/lotusgb_libretro.so \
  --connect=127.0.0.1 /path/to/second.gb
```

Replace `127.0.0.1` with the host's address for LAN play and use the same
Netplay port. Both peers require `LotusGB-serial-stream-v2`; this is distinct
from the LotusGBA RFU protocol. Each process reads its own local controller
and retains its own save. Session logs and game-derived artifacts remain
private.
