# Serial bit exchange

The core supports logical serial bit exchange between DMG/CGB ports through
host callbacks and externally scheduled sampling edges. Original component and
two-system CPU tests cover byte exchange, serial IRQ/HALT wake and paired
partial-transfer snapshot replay. The libretro core provides a local two-ROM
subsystem. The user reports completing an in-game Pokémon trade and confirming
the exchanged result remained after saving and reopening both games; the exact
edition and ROM build were not recorded, so Red/Blue-specific acceptance is
still pending. Physical cables and accessories are also pending. CGB infrared
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

Standalone macOS RetroArch 1.22.2 (69a4f0ea) loaded two original test ROMs
with the same basename, reported 320×144 geometry, and saved each received
byte in its own 8 KiB file. On relaunch, test programs read the existing
bytes from both files and wrote copies back to SRAM. On 2026-11-13, the user
reported that a Pokémon trade completed in the standalone RetroArch
two-player subsystem. The exact ROM pair, region, and localization were not
recorded. The user later confirmed that after saving both games in-game and
reopening them, the traded Pokémon remained on the respective teams. The
second choice in the in-game Cable Club menu led to a white screen; that mode
remains unverified. An unmodified, same-language ROM pair is still needed as a
control to distinguish a localization issue from a core issue.

For a separate-window session, turn off **Settings → User Interface →
Pause Content When Not Active** (`pause_nonactive = "false"`). On the same
standalone macOS build, a test ROM incremented a battery RAM byte each
VBlank. With Finder verified as the foreground app, the autosaved byte
advanced over three seconds when the setting was false and stayed unchanged
when it was true. Background controller input and dual-process link remain
separate checks.


## Standalone RetroArch two-process setup

Use two independent configuration files so each process has its own save and
state directories. Set `pause_nonactive = "false"` in both. Open two Terminal
windows and start the host first:

```sh
retroarch --config /path/to/host.cfg -L /path/to/lotusgb_libretro.dylib \
  --host /path/to/red.gb
```

Then start the client in the other window:

```sh
retroarch --config /path/to/client.cfg -L /path/to/lotusgb_libretro.dylib \
  --connect=127.0.0.1 /path/to/blue.gb
```

For a second computer on the same LAN, replace `127.0.0.1` with the host's LAN
address and set the same Netplay port in both configs. A synthetic-ROM probe
between the standalone macOS build and Steam RetroArch on a Steam Deck passed
on 2026-11-14: the user reported client SRAM prefix `A5 3C A5`, and the host
verifier returned `3C A5 A5`. The Deck launched through Steam App ID 1118310.
This verifies a bounded cross-device LAN exchange; it does not verify gameplay
or a Pokémon trade. Internet hosting also depends on RetroArch network setup:
the host must be reachable on its Netplay TCP port, or both peers can use the
RetroArch relay option. Neither Internet path has been tested with LotusGB yet
([RetroArch Netplay setup](https://docs.libretro.com/guides/netplay-getting-started/)).


## Netpacket transport status

RetroArch's standard Netplay synchronizes input for the same core and content;
it does not itself emulate a Game Boy link cable. LotusGB instead uses its
Netpacket API as a custom cable-data channel, with one ROM running in each
process. RetroArch's documentation identifies DoubleCherryGB as an example of
Netpacket-based Pokémon trading ([Netplay FAQ](https://docs.libretro.com/guides/netplay-faq/),
[DoubleCherryGB](https://github.com/madcock/libretro-doublecherryGB#link-cable-over-network)).
DoubleCherryGB documents this mode as trade-focused and warns that it can be
too slow for general multiplayer; LotusGB's actual trade latency still needs
measurement.

In the tested RetroArch 1.22.2 source, differing ROM CRCs in core Netpacket
mode produce a warning rather than rejecting the connection. The handshake
also omits SRAM synchronization in this mode, and the host does not send its
emulation state to the client. Each endpoint therefore keeps its own game and
save while Netpacket carries the cable data ([CRC handling](https://github.com/libretro/RetroArch/blob/v1.22.2/network/netplay/netplay_frontend.c#L1552-L1563),
[SRAM sync](https://github.com/libretro/RetroArch/blob/v1.22.2/network/netplay/netplay_frontend.c#L1212-L1214),
[state sync](https://github.com/libretro/RetroArch/blob/v1.22.2/network/netplay/netplay_frontend.c#L8709-L8717)).

The single-ROM core registers RetroArch's Netpacket interface. The current
protocol exchanges each internal-clock sampling edge as a reliable packet.
The receiving core supplies that edge to its external-clock serial port, then
returns its outgoing bit. If there is no active Netplay connection, the
internal-clock callback preserves disconnected incoming-high behavior. The
session advertises protocol `LotusGB-serial-bit-v1`; packet payloads contain a
magic value, type, bit value and edge sequence. Fields are encoded byte by
byte, with the 32-bit sequence in big-endian order; the wire format does not
send native C++ structs. Netpacket's reliable flag guarantees delivery in send
order. LotusGB has no application checksum today; it validates packet length,
magic, type, bit range and reply sequence. The host accepts one peer, and the
sender waits up to two seconds for each reply. A timeout or peer disconnect
faults the running core instead of silently substituting data.

Automated libretro tests run host and client cores in separate processes and
relay reliable packets between them. A deterministic local relay adds
25/50/75 ms one-way delays for the first byte (75–125 ms round-trip per serial
edge), then 50/150/250 ms one-way delays for the second byte (200–400 ms
round-trip). Both CPU programs complete the consecutive exchanges and write
each received byte to their own SRAM. Separate tests verify the configured
two-second no-reply timeout and that a peer disconnect faults the active
transfer instead of substituting data. These cover the core transport with
synthetic timing;
actual network behavior remains unverified by those tests. On macOS, the user
ran the standalone RetroArch 1.22.2 Host and Client in separate windows with
the synthetic ROMs. Logs show the host accepted the client despite the
different-ROM warning, both sides processed the first remote serial edge, and
the isolated SRAM prefixes were `3C A5 A5` (host) and `A5 3C A5` (client).
Both instances used `pause_nonactive = "false"`; the host completed the
exchange after the client window was opened, validating this configured
background-run scenario. The subsequent Mac ↔ Steam Deck test also passed its
synthetic SRAM checks on both ends. The Deck SteamOS/RetroArch versions and
client log were not captured. Internet direct/relay, real-network jitter, and
reconnect behavior remain unverified. The user deferred Internet testing on 2026-11-14 because their
network has no port forwarding. RetroArch relay may avoid router port
forwarding. An optional relay-lobby path is now prepared in the ignored
Mac/Steam Deck probe launchers: the host advertises a password-protected room,
and the client joins through RetroArch's Netplay host list instead of using
`--connect`. Fake frontend dry-runs passed; no external relay was contacted.
Actual relay, real-network jitter, and reconnect behavior remain unverified.
The probe launchers now accept
private ROM paths plus per-player data directories outside the repository, and
an offline mode can reopen each side with the same SRAM directory. Fake
frontend dry-runs verified these paths and the guard against repository ROMs
or save directories; no commercial ROM or save was opened by the launcher in
this work. The launchers also offer a GUI mode that loads each side's content
and isolated data paths without auto-hosting or auto-connecting; the RetroArch
menu can then host or enter the peer IP, with GUI connect/listen ports aligned.
On 2026-11-15, the user joined the Mac host by scanning the LAN from the Steam
Deck. The Deck client reported SRAM prefix `A5 3C A5`, and the Mac host
verifier returned `3C A5 A5`. The Mac log recorded the peer connection, first
remote serial edge, peer disconnect, and successful SRAM save. This confirms
one GUI-driven LAN byte exchange and SRAM persistence on both ends. The local
ignored host probe ROM was changed to wait for a button press after the Deck
joins, removing the previous short startup delay. Deck background execution,
Internet routing, and higher-latency link behavior remain unverified.

On 2026-11-15, the user completed a Red/Blue trade over the Mac ↔ Steam Deck
LAN session. Both sides received the other player's Pokémon, both Pokédexes
showed the traded Pokémon, and both retained the result after choosing the
game's restart option and reloading. The game saves the trade automatically;
a separate Save command is unnecessary. This verifies one real cross-device
trade and persistence by user report. The exact ROM/core hashes, Deck
SteamOS/RetroArch versions and client log were not captured.

The captured Mac host log ended cleanly: 14,472 serial edge requests had
14,472 replies, with no timeout or sequence mismatch; mean reply time was
17.0 ms and maximum 116 ms. It recorded a normal peer disconnect, Netpacket
stop and successful SRAM save. `retro_run` still had calls lasting up to
1.79 seconds. The host also received 2,000 peer edge requests, of which 1,624
arrived while the host was active in external-clock mode and 376 were ignored
while it was inactive or using internal clock. Those ignored edges may be
relevant to the difficult in-game handshake, but the missing Deck log prevents
confirming whether they caused a failed attempt. The RTT and long calls
support synchronous per-edge waiting as the main observed source of trade
stalls; this run completed correctly despite the delay.

### Netpacket stall diagnostics

The core now records the local SC state on the first outgoing edge and the
remote SC state when it receives an edge. It marks whether the receiver had
transfer-start set in external-clock mode, tracks the serial bit position,
checks peer request sequence continuity, and measures request/reply latency.
Diagnostic event lines include monotonic microseconds elapsed since the local
Netpacket session started; these timestamps are per-machine and are not a
shared clock.
INFO/WARN summaries report progress every 64 edges, slow `retro_run` calls
(rate limited to one per second), slow replies, timeouts, disconnects and
frontend Netpacket stop. Detailed per-bit TX/RX tracing is opt-in through the
`Netpacket bit trace` core option, which defaults to off; RetroArch must also
permit DEBUG core logs (`libretro_log_level = 0` and verbose mode). These are
diagnostic-only changes; the wire protocol and emulation behavior are
unchanged. Tests verify that an active external-clock transfer accepts eight
remote edges and receives the expected byte, while a late edge after transfer
completion is counted as ignored. The serial unit test also checks that an
active internal-clock transfer ignores external edges. The full `make test`
suite passes with this instrumentation, including DMG serial, libretro normal
and fixed-step, GBA, homebrew and PPU runner checks. A real Pokémon trade has
now been manually reproduced with the Mac diagnostic host; the Steam Deck
client binary identity and log remain unconfirmed. Keep detailed traces
private: they contain game link data.

On 2026-11-15, matched-source diagnostic builds were prepared for a controlled
reproduction. The macOS installer is
`build/packages/LotusGB-RetroArch-macOS-universal.zip` (core SHA-256
`1b76c8b5efa1f8232bdc35c3c3bc10bc18a20331e9c9f60eb28f5ecf60e188c8`; package
SHA-256 `e62fd9cf48f0d60e2aeca15ee26f00db3b53c6d9e1e98122b6e41e111da2c365`);
its five installer/package tests passed. The Steam Deck diagnostic client
package is `build/packages/LotusGB-Steam-Deck-Netpacket-Diagnostics.zip`
(Linux core SHA-256
`811b2e13fd4c1d60549e79cd99fd9413532a1a63e3fa93174569013f838c420c`;
package SHA-256
`6a03820e7417c03be2a43abaca15b103ac26dc46ce9cc4e5c679194223809982`). The
Deck core cross-compiled and the package contains no commercial ROM or save;
the exact client binary used for the real trade is unconfirmed. Both packages use protocol
`LotusGB-serial-bit-v1` and the same source revision plus uncommitted
diagnostic changes. The opt-in `Netpacket bit trace` option defaults off;
the isolated Deck config captures initial INFO/WARN summaries at
`libretro_log_level = 1`.
The Mac diagnostic core has since been installed into the standalone
RetroArch user core directory; its installed SHA-256 matches the packaged
core (`1b76c8b5efa1f8232bdc35c3c3bc10bc18a20331e9c9f60eb28f5ecf60e188c8`).
The diagnostics include session-relative monotonic timestamps. RetroArch
must be fully restarted before loading the core. The Steam Deck
package still needs to be copied to and run on the device.

The standard standalone Mac config at
`~/Library/Application Support/RetroArch/config/retroarch.cfg` has
`log_to_file = false` and `log_verbosity = false`; the completed trade used
the isolated host launcher and wrote its log to the external data folder. The
isolated client launcher also writes logs to its external data folder, but the
Steam Deck copy has not been collected. Initial INFO/WARN diagnostics use the
default core log level. To capture per-bit DEBUG detail, enable the core
option and set `libretro_log_level = 0` with verbose launch on both sides
before reloading content.

On 2026-11-15, the user completed a Red/Blue trade over the Mac ↔ Steam Deck
LAN session. Both sides received the other player's Pokémon, both Pokédexes
showed the traded Pokémon, and both retained the result after choosing the
game's restart option and reloading. The game saves the trade automatically;
a separate Save command is unnecessary. This verifies one real cross-device
trade and persistence by user report. The exact ROM/core hashes, Deck
SteamOS/RetroArch versions and client log were not captured.

The captured Mac host log ended cleanly: 14,472 serial edge requests had
14,472 replies, with no timeout or sequence mismatch; mean reply time was
17.0 ms and maximum 116 ms. It recorded a normal peer disconnect, Netpacket
stop and successful SRAM save. `retro_run` still had calls lasting up to
1.79 seconds. The host also received 2,000 peer edge requests, of which 1,624
arrived while the host was active in external-clock mode and 376 were ignored
while it was inactive or using internal clock. Those ignored edges may be
relevant to the difficult in-game handshake, but the missing Deck log prevents
confirming whether they caused a failed attempt. The RTT and long calls
support synchronous per-edge waiting as the main observed source of trade
stalls; this run completed correctly despite the delay.
