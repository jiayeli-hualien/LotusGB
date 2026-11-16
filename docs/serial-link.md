# Serial bit exchange

The core supports logical serial bit exchange between DMG/CGB ports through
host callbacks and externally scheduled sampling edges. Original component and
two-system CPU tests cover byte exchange, serial IRQ/HALT wake and paired
partial-transfer snapshot replay. The libretro core provides a local two-ROM
subsystem. On 2026-11-16, the user reported one successful, crash-free
stream-v1 LAN trade between Pokémon Red (USA) on Mac and Pokémon Blue (USA) on
Steam Deck. The user also tried a link battle. Both games displayed “You Win,”
then the user observed that Mac's Charmander had fainted while the Deck game
still treated it as alive. Captured session logs show both endpoints paired
through transfer 3202 with no sequence errors or duplicate peer bytes; the
Deck had started transfer 3203 and was still waiting for the host byte after a
five-second diagnostic timeout. This shows the next byte had not arrived at
the client when sampled, but does not establish whether game protocol,
different local progress, or another timing factor caused the battle-state
divergence. Save persistence after a normal frontend exit and reload remains
to be verified. Physical cables and accessories are also pending. CGB infrared
peer behavior has a separate
[level-based interface and evidence](cgb-infrared.md).

## Network-mode fidelity contract

The network link mode prioritizes ordered game data and a responsive frontend;
it does not claim bit-accurate cable timing. Each RetroArch instance advances
its own CPU, PPU and APU clocks. The peers synchronize serial data by transfer
sequence, not by locking whole-frame or CPU-cycle progress. The physical cable
also has its own master-driven serial clock domain; the two consoles' CPU clock
phases do not need to match ([hardware references](references.md#hardware-behavior)).

For stream-v1, each peer's outgoing byte is paired only with the same transfer
sequence from the other peer. While the session remains connected, a missing
byte leaves the serial transfer pending as guest emulation and frontend
callbacks continue; the core does not silently substitute data. On a detected
peer disconnect it has an explicit open-line `$FF` fallback. Once the matching
byte arrives, all eight shifts and the serial interrupt are applied atomically.
This preserves byte values and order when both games issue matching transfer
sequences, while changing observable partial-SB and SC/interrupt timing. A
game's own link wait can therefore remain visible even while video and audio
callbacks continue.

The unit and delayed-process tests establish this contract for synthetic ROMs.
They do not establish cycle-level cable accuracy, RetroArch GUI smoothness,
Pokémon trade compatibility, or save persistence for stream-v1.

## RetroArch input Netplay and link-cable transport

RetroArch's standard input Netplay is replay-based: each player sends controller
input by frame, the frontend predicts missing remote input, and rewinds/replays
the core when the real input arrives. The server also supplies the canonical
frame count and an initial serialized state to a joining client. This requires
a deterministic core and identical core/content on every peer. Configuring
input latency can make corrections less frequent and cheaper; it is not the
same as turning rollback off ([Netplay design](https://docs.libretro.com/development/retroarch/netplay/)).

RetroArch's FAQ says standard input Netplay is for local multiplayer that runs
on one emulated system, not link-cable games ([Netplay FAQ](https://docs.libretro.com/guides/netplay-faq/)).
The Nintendo Switch Online description supplied during this work is useful as
a conceptual comparison, but its claims about Nintendo's internal instances,
pointer wiring and exact synchronization behavior are not treated as verified
implementation facts.

LotusGB's current stream-v1 path sends serial `ByteReady` data through the
libretro Netpacket interface. Changing RetroArch's input rollback behavior
would not remove the per-transfer byte pairing or its game-visible completion
barrier. A future deterministic twin-emulation design could have every peer run
the same pair of linked Game Boy instances and synchronize controller input by
frame, but it would also need identical initial state for both machines,
consistent RNG/RTC and careful ownership of each player's persistent SRAM.
That is a substantial alternative architecture, not a RetroArch setting; the
current decision is to retain stream-v1 until real-game evidence shows it is
insufficient.

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
[DoubleCherryGB](https://github.com/TimOelrichs/doublecherryGB-libretro)).
DoubleCherryGB's README documents Gen 1/2 online trading through Netplay,
states that it also works over LAN, and lists Pokémon R/B/Y; it notes that
battling can be unstable. LotusGB therefore does not claim to be the first
RetroArch core for LAN Red/Blue trading. Its stream-v1 result below is a
separate, directly tested implementation. The observed session latency is
recorded below rather than generalized from the core's README.

RetroArch's [libretro Netpacket API](https://github.com/libretro/RetroArch/blob/master/libretro-common/include/libretro.h)
defines custom packets up to 64 KiB. Reliable packets preserve delivery order,
and the flush hint sends queued packets immediately without blocking. The API
does not prescribe a bit-level Game Boy protocol. A core can also poll for
incoming packets while running, though that poll may block and must have a
short timeout. This makes byte packets and asynchronous send possible; deciding
when the emulated serial transfer can finish remains LotusGB's responsibility.

In the tested RetroArch 1.22.2 source, differing ROM CRCs in core Netpacket
mode produce a warning rather than rejecting the connection. The handshake
also omits SRAM synchronization in this mode, and the host does not send its
emulation state to the client. Each endpoint therefore keeps its own game and
save while Netpacket carries the cable data ([CRC handling](https://github.com/libretro/RetroArch/blob/v1.22.2/network/netplay/netplay_frontend.c#L1552-L1563),
[SRAM sync](https://github.com/libretro/RetroArch/blob/v1.22.2/network/netplay/netplay_frontend.c#L1212-L1214),
[state sync](https://github.com/libretro/RetroArch/blob/v1.22.2/network/netplay/netplay_frontend.c#L8709-L8717)).

The single-ROM core registers RetroArch's Netpacket interface. Earlier
diagnostic builds exchanged each internal-clock sampling edge as a reliable
packet (`LotusGB-serial-bit-v1`). The first byte-oriented prototype used a
request/reply pair for each complete byte (`LotusGB-serial-byte-v1`). Its
request waited for the peer reply at a byte boundary and retried at the
application layer. These versions are historical; the current network prototype is
`LotusGB-serial-stream-v1`.

In stream-v1, each endpoint sends one reliable, ordered `ByteReady` packet
when its ROM starts an SC transfer. The packet contains the full outgoing SB
value and a 32-bit monotonically increasing transfer sequence, encoded
explicitly in big-endian order. There is no per-byte reply or application retry:
both endpoints publish their own byte, queue the peer's byte if it arrives
early, and pair only equal sequence numbers. An active transfer whose byte has
not arrived remains pending while the CPU, video and audio continue to advance.
When the matching peer byte arrives, the core completes all eight local shifts
at once through the normal serial completion/interrupt path. A timeout logs a
warning but keeps the transfer pending so a late byte can still resume it. A
peer disconnect supplies `0xFF` to an active transfer, matching the open-line
value used by this core.

The packet parser checks packet length, magic, type and reserved fields. It
rejects duplicates and sequence discontinuities, and the queue is bounded.
Netpacket's reliable flag provides ordered delivery within a session. LotusGB
does not currently add an application-level checksum; packet integrity is left
to RetroArch's transport and underlying network stack. This relies on both games starting the same number of serial-byte
transfers in the same order. If the games diverge, the sequence mismatch is
reported and the transfer remains pending instead of pairing unrelated bytes.
The host accepts one peer.

This stream design removes the application-level round trip per byte and avoids
blocking `retro_run` for peer data. It deliberately relaxes physical cable
timing: clock edges are not networked, byte completion and the serial interrupt
occur atomically on peer-byte arrival, and the two CPUs are not kept in
frame-level lockstep. Games that inspect partial SB values, exact SC/interrupt
timing, or make different numbers of transfers may behave differently. One
stream-v1 Red/Blue LAN trade has now been reported successful without a crash.
In a later battle session, the user reported that one game registered a faint
while the other still treated that Pokémon as alive. Captured logs showed both
endpoints had paired through transfer 3202 without sequence errors or duplicate
peer bytes; the client had sent transfer 3203 but timed out after five seconds
without receiving the host's matching byte. This is evidence of a peer byte
not yet being available at that sample, not proof of the underlying cause.
These observations demonstrate one ROM pair and session, not broad game
compatibility. Both RetroArch instances were still open when the isolated SRAM
files were sampled. After the later normal shutdown, both save files were
updated on disk, confirming frontend flush; loading those files in the games
and checking the traded party/Pokédex entries remains unverified.

### Gen I Pokémon link-battle synchronization

The Pokémon Red disassembly shows that link battles reduce repeated cable data:
the Cable Club setup exchanges a shared ten-byte random-number list, and the
Game Boy providing the serial clock's list is used by both consoles. The other
console copies that list from received data. During each turn, the game
exchanges a compact action code (move selection, switching, or running); each
ROM then applies the battle locally rather than receiving authoritative hit,
critical-hit, damage, or HP results. `BattleRandom` advances an index into the
shared list, including for random turn order when the Pokémon have equal speed.
See the upstream [`pokered` link setup](https://github.com/pret/pokered/blob/master/engine/link/cable_club.asm),
[`pokered` battle engine](https://github.com/pret/pokered/blob/master/engine/battle/core.asm),
and [`pokeyellow` action exchange](https://github.com/pret/pokeyellow/blob/master/engine/battle/core.asm).

This design requires matching logical battle state and matching consumption of
the shared random sequence. If one side takes a different branch or consumes an
extra random value, later accuracy, critical-hit, or damage rolls can diverge.
It does not require the two emulated CPU clocks to have the same phase: the
physical link's secondary serial port is clocked by the primary serial signal,
while the consoles' system clocks remain independent ([Game Boy: Complete
Technical Reference](https://gekkio.fi/files/gb-docs/gbctr.pdf)). LotusGB's
stream-v1 pairs ordered serial bytes but does not synchronize Pokémon's battle
state or its random-list index, so successful byte pairing alone cannot prove
that both battle simulations stayed in agreement.

For the 2026-11-16 battle observation, the Deck produced serial transfer 3203
while the Mac had not produced the matching transfer before shutdown. This is
consistent with the ROMs reaching different communication progress, but does
not identify whether that was the cause or a consequence of the earlier HP
disagreement. The `pokered` disassembly also documents a specific link-battle
fainting desync involving Bide's accumulated-damage cleanup; that is a
conditional game bug, not an explanation for this session unless Bide was
involved. No battle fix is made yet. A later diagnosis should trace the action
code, random-list index, and relevant HP on both endpoints around each turn,
then distinguish RNG-position drift from a ROM-specific battle bug or serial
progress mismatch.

A network relay can provide a route when direct connections or port forwarding
are unavailable, but it does not change serial-byte scheduling or remove
latency. RetroArch already provides a relay-server option for that connection
path ([setup guide](https://docs.libretro.com/guides/netplay-getting-started/));
the LotusGB Netpacket path through that relay has not yet been tested. Adding a
second transparent proxy would duplicate transport responsibilities and can
add another network hop. A proxy also cannot know a future byte before the ROM
produces it; predicting or completing bytes early risks changing game-visible
SB/SC/interrupt timing. Stream-v1 has passed same-Mac and cross-device LAN
synthetic probes. These prove one-byte Netpacket exchange and SRAM persistence,
not a real Pokémon trade. Keep the reliable ordered Netpacket path unless a
real-game trace identifies a transport defect. Pending transfers do not block
the core's frame/audio callbacks, so the frontend can keep presenting while a
game waits for the serial interrupt; this does not make the exchange itself
independent of ping.

The libretro unit tests cover a local peer byte, pre-arrival queueing, sequence
validation, duplicate rejection, disconnection and a late byte after timeout.
They check that video and audio callbacks continue while a byte is pending. A
two-process test relays 16 byte exchanges per endpoint. The first 16 packets
use a fixed 100 ms delay per direction for eight byte pairs (200 ms
round-trip equivalent); the remaining packets use 50/150/250 ms per-packet
delays, reaching 400 ms round-trip latency under
the variable-delay profile; both games store every peer byte in order. This
exercises longer ordered streams and nonblocking progress under synthetic latency.
The endpoint processes also wait 17 ms and 33 ms, respectively, after each
`retro_run()` to model roughly 60 FPS versus 30 FPS frontend cadence; the byte
stream still completes in sequence. This does not run RetroArch frontends or
verify that a real background window uses either cadence. It does not cover real
jitter or establish broad game compatibility. `make test` passed after the
stream-v1 changes.

Set the `Netpacket byte trace` core option to `on` and reload content to add
per-transfer diagnostics. Logs include the emulated T-cycle count and CPU PC,
SB/SC, IF/IE, IME, HALT and STOP state at transfer start, after byte pairing,
and when a peer-byte wait times out. This makes an absent peer byte,
sequence mismatch, or guest-side wait visible without changing transfer
behavior. After collecting both endpoint logs, run
`python3 tools/analyze_serial_link_timing.py HOST_LOG CLIENT_LOG` to compare
guest T-cycles advanced during each endpoint's local transfer wait with its
wall-clock wait. This can reveal whether a frontend advances guest time more
slowly while waiting. It does not measure absolute phase skew between machines;
the per-endpoint session clocks have independent origins, and different game
code may spend different time between serial transfers.

Prior manual evidence remains useful for the older byte-v1 implementation. On
macOS, the user ran standalone RetroArch 1.22.2 Host and Client in separate
windows with synthetic ROMs. Both sides exchanged bytes and retained separate
SRAM; `pause_nonactive = "false"` allowed the host to continue when the client
window had focus. A later Mac ↔ Steam Deck GUI LAN probe also passed its
synthetic SRAM checks. Deck SteamOS/RetroArch versions and the client log were
not captured. Internet relay, real-network jitter and reconnect behavior
remain unverified. The user deferred Internet testing on 2026-11-14 because
their network has no port forwarding. An optional relay-lobby path is prepared
in ignored probe launchers, but no external relay was contacted. Probe
launchers support private ROM paths, per-player data directories and GUI
host/client setup; fake frontend dry-runs verified those paths and the guard
against repository ROMs or saves. No commercial ROM or save was opened by the
launcher in that work.

On 2026-11-15, the user joined the Mac host by scanning the LAN from the Steam
Deck. The Deck client reported SRAM prefix `A5 3C A5`, and the Mac host
verifier returned `3C A5 A5`. The Mac log recorded the peer connection, first
remote serial edge, peer disconnect and successful SRAM save. This confirms
one GUI-driven LAN byte exchange and SRAM persistence on both ends with the
prior implementation. At that point Deck background execution, Internet
routing and higher-latency link behavior remained unverified; the later Deck
focus result is recorded below.

On 2026-11-15, the user completed a Red/Blue trade over the Mac ↔ Steam Deck
LAN session. Both sides received the other player's Pokémon, both Pokédexes
showed the traded Pokémon, and both retained the result after choosing the
game's restart option and reloading. The game saves the trade automatically;
a separate Save command is unnecessary. This verifies one real cross-device
trade and persistence by user report on the prior implementation. The exact
ROM/core hashes, Deck SteamOS/RetroArch versions and client log were not
captured.

On 2026-11-16, stream-v1 passed `make build`, the complete `make test` suite,
a subsequent Libretro test run, and the five macOS installer checks. The
standalone RetroArch core hash is `7d753cf554b9c7a88806886d135e6745ac75c72cd52874dcc9e09ee2c2beb60a`;
it matches the macOS package and the installed core. The Steam Deck diagnostic
package contains Linux core hash
`211d057d6e8ed874260ab9893654403f282c7464d2b18c6e2efa5f2100016f63`. The first
package build had SHA-256
`b37fb7e2bc73fbff4fdf65301cd92192a2e3aff6bb5047623cbc5952062ddca9`; the
repaired package was installed on the Deck and its current archive hash is
recorded below.

On 2026-11-16, two standalone RetroArch 1.22.2 GUI processes on the same Mac
completed a stream-v1 synthetic loopback exchange. The logs are in the ignored
`build/serial-stream-v1-live/` directory; `verify.sh both` passed with SRAM
prefixes Host `3ca5a5` and Client `a53ca5`. Final counters on both sides were
1 byte sent/received/paired, with zero sequence errors, duplicates or pending
transfers. RetroArch reported ping 21 ms on the host and 14 ms on the client.
The client transfer waited 10.76 s, logged the 5 s warning, then paired late;
the host paired in 49 μs. These wait values include the synthetic ROMs'
different start conditions and are not pure network latency. The trace analyzer
could not compare guest-clock progress because the logs lacked host transfer
state snapshots. RetroArch's existing per-core `LotusGB.opt` overrode the
probe's `core_options_path`; the isolated Mac and Deck configs now set
`global_core_options = true` to make their bundled trace settings take effect
without editing the user-wide core options. See
[RetroArch issue #12901](https://github.com/libretro/RetroArch/issues/12901)
for the documented precedence behavior.

On 2026-11-16, the Steam Deck Steam build connected over LAN to the Ubuntu
RetroArch host using the same LotusGB core binary on both ends
(`211d057d6e8ed874260ab9893654403f282c7464d2b18c6e2efa5f2100016f63`). The
Deck ran RetroArch 1.22.2; Ubuntu ran 1.18.0. RetroArch warned about different
frontend versions and different synthetic games, but accepted the Netplay
connection. Both verifiers passed: host SRAM `3ca5a5`, client SRAM `a53ca5`,
each with one byte sent/received/paired and zero sequence errors, duplicates,
or pending transfers. SRAM checkpoints `11 22 33 44` confirmed each probe ROM
entered, started, received and completed its transfer. The client paired
`0x3C` with host `0xA5`; the host paired `0xA5` with client `0x3C`. Reported
pair wait was 95 μs on the Deck and 161 ms on the Ubuntu VM; these are local
transfer wait measurements, not pure network RTT. The TCP connection to port
55437 was established without adding a firewall rule. This verifies one
cross-device synthetic exchange; it does not verify a Pokémon trade. The
installed Deck package is
`build/packages/LotusGB-Steam-Deck-Netpacket-Diagnostics.zip` (SHA-256
`eb346e69576443898042ef2f750593d7c7f47543111c4b1e48c277b44e4543e4`), built
from original probe ROMs generated by
`tools/generate_netpacket_probe_roms.py`.

On 2026-11-16, the Steam Deck Steam RetroArch 1.22.2 focus-marker test passed.
The test used the bundled original `focus.gb`, an isolated save directory,
`pause_nonactive = "false"` and `autosave_interval = "1"`. An X11 terminal
window was confirmed active for the full 26-second measured interval while
RetroArch remained unfocused. The SRAM frame marker changed from 240 to 32;
the verifier reported `Focus test: PASS (marker advanced while unfocused)`.
The test ROM, SRAM and RetroArch log remain outside the repository under
`/home/deck/lotusgb-netpacket/focus-check-stream-v1-20261116/`. This verifies
foreground loss does not stop this Deck RetroArch session; it does not test
background controller input or a Pokémon trade.

On 2026-11-16, Ubuntu 24.04's RetroArch 1.18.0 produced an Apport report for a
synthetic-host launch (`SIGSEGV`, 22:39:57; command line used the generated
`host.gb`, not a commercial ROM). The available backtrace reaches libc's
`exit()` handlers with an unresolved instruction address, so the exact crash
cause is not established. A later host launch remained alive and bound TCP
55437; a second simultaneous launch failed to bind the same port. Its log shows
Wayland connection failure followed by X11 initialization, so the Wayland
message alone does not explain the earlier crash. After the user closed all
RetroArch windows, one isolated host was launched through Ubuntu's active X11
session (`DISPLAY=:0`) with a separate synthetic save/log directory. Its
RetroArch window became the active X11 window within six seconds without a key
press, remained alive for 36 seconds, and shut down cleanly on SIGINT with SRAM
saved and port 55437 released. The log had no usable audio driver but continued
normally. This did not reproduce the crash. It suggests the key press was
dismissing an earlier Apport crash notice rather than being required for a
normal launch; that is an inference, and the original SIGSEGV cause remains
unknown. This Ubuntu frontend issue is separate from the successful Deck focus
test and the passing cross-device synthetic byte exchange.

On 2026-11-16, the user tested the stream-v1 link with Pokémon Red (USA) as
Mac host and Pokémon Blue (USA) as Steam Deck client. The user reported that a
trade completed without a crash. They then tested a link battle and observed a
state divergence: Mac's Charmander fainted while Blue still treated it as alive.
Both games had also displayed “You Win” during the battle. Captured logs show
both endpoints pairing transfers through sequence 3202 with no sequence errors
or duplicate bytes; the Deck had started sequence 3203 and was still waiting
for the host byte after the five-second diagnostic timeout. The logs do not
establish whether the missing-at-that-time byte resulted from game handshake
behavior, different guest progress, or another timing factor. The isolated SRAM
files were unchanged on disk while both RetroArch sessions remained open. They
were updated after the later normal shutdown, confirming frontend flush; the
in-game reload check is still pending. The user plans to test ordinary
RetroArch GUI launch and Host/Connect operation without the project launcher.
Log snapshots are kept outside the repository under `/private/tmp/` for local
analysis.
