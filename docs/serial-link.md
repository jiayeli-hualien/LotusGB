# Serial bit exchange

The core supports logical serial bit exchange between DMG/CGB ports through
host callbacks and externally scheduled sampling edges. Original component and
two-system CPU tests cover byte exchange, serial IRQ/HALT wake and paired
partial-transfer snapshot replay. The libretro core provides a local two-ROM
subsystem. On 2026-11-16, the user reported one successful, crash-free
stream-v1 LAN trade between Pokémon Red (USA) on Mac and Pokémon Blue (USA) on
Steam Deck. On 2026-11-17, both isolated save files were reopened through Steam
RetroArch 1.22.2. The Deck log confirms the Blue ROM and isolated SRAM path;
the Mac log confirms the Red ROM, selected LotusGB core and isolated SRAM path.
The user reports the last return trade persisted on both sides: Red's
Charmander and Blue's Squirtle returned to their respective games and remain
in the party or Pokédex. Both games exited normally; each frontend log records
a successful SRAM write to its isolated file, and both file timestamps
advanced after exit. This verifies both in-game readbacks for this ROM pair,
not broad compatibility. On 2026-11-17, SHA-1 checks of the private Mac Red
and Steam Deck Blue files matched the clean USA/Europe release hashes. The
“SGB Enhanced” suffix is used for these clean base ROMs in patch-source
checksum tables; it does not by itself mean the tested files are modified
games ([example clean-base table](https://github.com/ScoopDunk/Super-Vanilla-Red-and-Blue#how-to-patch)).
No ROM file or hash was added to the repository. The user also tried a link
battle. Both games displayed “You
Win,” then the user observed that Mac's Charmander had fainted while the Deck
game still treated it as alive. Captured session logs show both endpoints
paired through transfer 3202 with no sequence errors or duplicate peer bytes;
the Deck had started transfer 3203 and was still waiting for the host byte
after a five-second diagnostic timeout. This shows the next byte had not
arrived at the client when sampled, but does not establish whether game
protocol, different local progress, or another timing factor caused the
battle-state divergence. Physical cables and accessories are also pending.
CGB infrared peer behavior has a separate
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

After a peer disconnect or transport failure, this core session cannot safely
resume: the games may have advanced through different serial transfers. The
host rejects a replacement peer while the failed Netpacket session is active.
Both players must restart the content and multiplayer session before trying
again; stream-v1 does not restore or reconcile guest state across reconnects.

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
RetroArch relay option. At the time of this earlier LAN test, neither path had
been tested with LotusGB; later relay evidence is recorded below
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
updated on disk, confirming frontend flush. On 2026-11-17 the user reopened
both isolated saves through Steam RetroArch 1.22.2. The logs confirm the
selected Red/Blue content and isolated SRAM redirects. The user reports the
last return trade loaded correctly: Charmander was back in Red and Squirtle
was back in Blue, with the current party/Pokédex state retained. Both
frontends exited normally; their logs show successful SRAM writes to their
isolated files, and both modification times advanced after exit. This is one
game-level readback on each side, by user report, not a broad compatibility
claim.

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
involved. It also labels a missing Mirror Move check as a possible source of
link desync when combined with multi-turn moves such as Wrap. These are
specific move/state conditions, not a Charmander-versus-Squirtle matchup bug.
The species pairing alone does not show that either cartridge bug applies. If
the mismatch happened during ordinary moves without Bide, Mirror Move, or a
multi-turn trapping state, the documented cases do not explain it; an emulator,
ROM hack, or link-state divergence remains possible. No battle fix is made yet.
A later diagnosis should trace the action code, random-list index, and relevant
HP on both endpoints around each turn, then distinguish RNG-position drift
from a ROM-specific battle bug or serial progress mismatch. See the upstream
[`pokered` battle engine](https://github.com/pret/pokered/blob/master/engine/battle/core.asm)
for the commented Bide fainting and Mirror Move edge cases.

#### Synthetic shared-RNG and long-payload probe (2026-11-17)

`game_boy_system_test` now builds two original MBC1 battery-backed test ROMs
and runs them as linked `GameBoySystem` instances. The fixture uses a fixed
host seed table rather than reading either console's DIV, so it isolates the
link-battle rule: the host sends ten seed bytes once, both sides use the same
table, and the battle consumes the first nine values. After the ninth value,
the test ROM updates those nine values with `(5 * value + 1) mod 256` and resets
the index. The tenth transmitted byte remains outside that nine-value battle
table. This matches the behavior in the upstream
[`pokered` Cable Club setup](https://github.com/pret/pokered/blob/master/engine/link/cable_club.asm)
and [`BattleRandom`](https://github.com/pret/pokered/blob/master/engine/battle/core.asm).

After seed exchange, the client begins with a different table so the test
checks that it adopts the host's values. Both ROMs save an immediate copy of
the ten exchanged bytes. Each then sends a distinct patterned 512-byte payload
and saves every byte received from its peer to a separate SRAM range. They also
exchange one action byte per synthetic battle turn. Their small battle model
records random draws, hit/miss and critical counts, damage, turns and both HP
values in SRAM. The automated test compares every byte in both payload
directions and the SRAM battle records on both peers against an independent
C++ reference. Negative controls deliberately corrupt one received seed and
consume one extra random value; the checks detect the table mismatch and later
battle divergence.

Run the focused check with:

```sh
make -C src/unit_test build/game_boy_system_test -j8
src/unit_test/build/game_boy_system_test
```

To export the same ROM pair for a later RetroArch/Netpacket experiment:

```sh
src/unit_test/build/game_boy_system_test \
  --generate-pokemon-link-probes build/test-roms/pokemon-link-battle
```

The ROMs and their notes are build artifacts, not committed game files. This
fixture validates the shared-table model and ordered bidirectional long serial
streams in a local two-system test. It does not implement the full Pokémon
battle engine, model seed generation from DIV, add network latency, or establish
real-game battle compatibility.

A network relay can provide a route when direct connections or port forwarding
are unavailable, but it does not change serial-byte scheduling or remove
latency. RetroArch already provides a relay-server option for that connection
path ([setup guide](https://docs.libretro.com/guides/netplay-getting-started/));
a later synthetic probe confirmed LotusGB Netpacket bytes through that relay,
as recorded below. Adding a
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
two-process 64-byte test stores every received byte in SRAM and runs the peers
at asymmetric 17 ms / 33 ms `retro_run()` cadence. Its first eight byte pairs
use a fixed 100 ms delay in each direction (200 ms round-trip equivalent); the
remaining 56 pairs repeat 50/150/250 ms per-packet delays (up to 400 ms
round-trip equivalent). A separate two-process stress test stores and compares all 4,096
received bytes per endpoint, beyond the 3,202 paired transfers seen before the
real battle divergence; it applies bounded 100/200/300 μs packet delay jitter
and runs the frontend loop without artificial frame pacing. Both use a local
socket relay and the Netpacket core interface, not RetroArch frontends or
physical networks. They do not establish broad game compatibility.

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
remain unverified. The user deferred direct Internet testing on 2026-11-14
because their network has no port forwarding. A later test used RetroArch's
public relay with both devices still on the same home LAN; it verified that
relay route, but not peers on separate Internet connections. Probe
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

On 2026-11-17, an original synthetic host ROM on Ubuntu RetroArch 1.18.0 and
the Deck Steam RetroArch 1.22.2 client exchanged one stream-v1 byte through a
RetroArch public relay. Both devices were still on the same home LAN, so this
proves the relay route and Netpacket payload, not cross-ISP/NAT operation.
RetroArch reported roughly 406/412 ms ping. The first attempt exposed a probe
startup race: with `menu_pause_libretro = false`, the client ROM had already
started before it joined and the host's first byte remained pending. Restarting
the client after joining paired sequence 1. At clean shutdown both endpoints
reported one byte sent/received/paired and zero sequence errors, duplicates or
pending transfers; SRAM markers were host `3c a5 a5` and client `a5 3c a5`.
The host's reported wait includes the minutes before the client restart and is
not a measure of network latency. This single-byte probe does not establish
multi-byte reliability, reconnect behavior, or Pokémon gameplay over WAN.

After the Deck was shut down, an Ubuntu RetroArch 1.18.0 host and Mac Steam
RetroArch 1.22.2 client also exchanged one synthetic stream-v1 byte directly
over LAN. The host verifier passed with one sent/received/paired byte, no
sequence errors, duplicates or pending transfers, and SRAM marker `3c a5 a5`.
The Mac log recorded the pair and matching counters, but its SRAM file was not
present after signal shutdown, so this run does not verify Mac-side SRAM
persistence. This validates a command-line Mac/Ubuntu LAN path without the
Deck; it is not a second-network Internet test.

The separate real-game save readback was also completed on 2026-11-17: the
user confirmed the returned Charmander remained in Red and Squirtle remained
in Blue after reopening their isolated saves. Both RetroArch logs reported
successful SRAM writes on normal exit and both save-file timestamps advanced.
Together with the earlier manual Red/Blue trade, this verifies trade
persistence for those saves; it does not establish battle-state synchronization.

On 2026-11-17, the original probe-ROM generator gained independent host and
client start gates (`--host-start button`, `--client-start button`) and a
bounded transfer count (`--transfers 1..16`). Either side can wait for the
mapped Game Boy A input so both peers can join the RetroArch room before
starting serial traffic; automatic mode remains the default for direct-IP
launches. The full `make test` suite passed with ROM-runner checks: neither
gated role starts before input, the client then completes and stores 16
standalone serial bytes in order, and the host starts an external-clock
transfer but remains pending without a peer. Automatic client mode still
completes without input. These checks cover generated ROMs and local serial
behavior, not a 16-byte Netpacket frontend session.

On 2026-11-17, two Ubuntu RetroArch 1.18.0 processes using the same LotusGB core
then completed the bounded 16-byte probe over a direct local Netplay connection.
Both final logs reported 16 bytes sent, received and paired, with zero sequence
errors, duplicates and pending bytes. The host and client SRAM verifiers passed
all four checkpoints and the full 16-byte receive streams (`3c a5 a5` host,
`a5 3c a5` client). This confirms ordered multi-byte Netpacket delivery through
the actual RetroArch frontend and save flush on both ends. RetroArch reported
about 337–342 ms ping and the host recorded two `retro_run()` calls over 100 ms;
because both processes ran on one Ubuntu VM, these are frontend/session
measurements, not a WAN RTT or a clean latency benchmark. The test used only
the original synthetic ROMs and isolated saves. Packet-level jitter/loss,
disconnects, separate-Internet/NAT behavior, relay reconnects under loss, and
Pokémon gameplay through the relay remain unverified. The bounded 16-byte ROM
pair supports repeating through RetroArch under controlled network conditions;
do not have one peer retry unilaterally, since both ROMs must arm transfers in
the same sequence.

A second Ubuntu RetroArch run used a temporary user-space TCP bridge that
delayed each forwarded read by 100 ms in each direction. All 16 transfers again
paired with zero sequence errors, duplicates or pending bytes, and both SRAM
verifiers passed. The reported average/max byte wait was 103.9/262.0 ms on the
host and 133.6/277.9 ms on the client. This bridge is a bounded delay probe,
not packet-level `netem` and not a precise 200 ms RTT simulation. After SIGINT
shutdown, both RetroArch processes printed a segmentation fault after their
final Netpacket summaries, successful SRAM writes and normal core-unload log
entries. No new Ubuntu crash report was created (the existing report is from
2026-11-16); treat this as an unresolved frontend shutdown issue, separate from
the passing transfer and SRAM evidence.

A third run configured the same user-space bridge for 200 ms per forwarded read
in each direction and used the join-gated client ROM. The client was started
only after its RetroArch log confirmed the Netplay join, then received the
mapped Game Boy A key. Both endpoints completed 16/16 sent, received and paired
bytes with zero sequence errors, duplicates or pending data; both SRAM
verifiers passed the complete receive streams and checkpoints. Excluding the
host's first byte, which waited 109.9 seconds for the deliberate join/key gate,
host byte wait was 198.1 ms average / 194.4 ms median / 385.2 ms max; client
wait was 268.5 ms average / 248.3 ms median / 480.7 ms max. The initial
auto-start attempt at this delay was invalid: the client completed its local
ROM sequence before joining, wrote `0xFF` receive bytes, and the host timed out
on its first transfer. This confirms the GUI probe must use its join gate at
longer delays. The successful 200 ms run is still a same-VM synthetic probe
through a user-space TCP bridge; it does not establish WAN gameplay or
packet-level jitter/loss behavior.

A 2026-11-17 direct cross-device 16-byte attempt used Ubuntu RetroArch 1.18.0
as host and standalone macOS RetroArch 1.22.2 as client. The client joined the
Ubuntu host at 18–21 ms Netplay ping. RetroArch warned about the different
frontend versions and different synthetic games; Netpacket still started on
both peers. The join-gated client was released after the host had already
timed out on its first byte. That byte paired late, but the test was stopped
before the 16-byte sequence finished. The last flushed counters showed 10
paired bytes on the Ubuntu host and 8 on the Mac client, with zero reported
sequence errors or duplicate bytes; observed maximum waits reached about 111
seconds, and the Ubuntu RetroArch process used about 95% CPU. The 16-byte SRAM
verification was not completed. The host had run ahead while waiting for the
gated client, the frontend versions differed, and the Ubuntu host process was
CPU-bound on its VM. This is an incomplete diagnostic run rather than a clean
LAN reliability or latency result. It does show that the Mac client continued
advancing through several transfers while RetroArch was unfocused with
`pause_nonactive = false`; it does not establish completion of the byte stream.

After adding the host start gate, a second direct Ubuntu 1.18.0 host / Mac
1.22.2 client run joined successfully at 52 ms reported Netplay ping. Both
peers were released after joining, but the run still ended early: the host
reported four paired bytes and timed out at sequence 5; the Mac log showed
sequence 3 paired and sequence 4 started. The 16-byte SRAM check was not
completed. A separate same-Mac loopback attempt with two RetroArch 1.22.2
processes also joined and the client paired its first byte, then started
sequence 2 without completing the full stream. The two sides reported
inconsistent initial Netplay ping values (5000 ms host-side and 234 ms
client-side), and no 16-byte SRAM verification was run. Both are incomplete
frontend probes; neither establishes multi-byte LAN stability. They leave
frontend pacing, unfocused execution during Netplay, and peer timing as open
diagnostic questions.

A subsequent attempt on 2026-11-17 tried to repeat the bounded 16-byte probe
through RetroArch's public relay, using two Ubuntu RetroArch 1.18.0 processes
and the same synthetic ROMs. Both TCP connections reached the relay and the
host logged a completed tunnel link, but RetroArch did not establish a Netplay
peer: the client log reported `Incorrect password` and the host reported
`Failed to receive netplay password`. The host, client and temporary password
file contained identical values (checked without exposing the password), so
the cause in the CLI relay/password handshake remains unresolved. Neither core
received a peer serial byte; the SRAM verifiers correctly failed their
completion checks. Both processes were stopped and the public room disappeared
from the lobby. This attempt provides no evidence about 16-byte transport over
the relay; it is separate from the passing same-VM direct and delayed tests and
the earlier one-byte relay probe.

The matching synthetic frontend probe artifacts are ready under the ignored
build directory: the Mac host bundle is
`build/serial-stream-v1-16byte/`, and the stable Steam Deck archive is
`build/packages/LotusGB-Steam-Deck-Netpacket-Diagnostics.zip` (SHA-256
`3be881ba8a0b76c59579f1671ed4871e46a707431be3fbab63e7c0c99064959b`). The
Deck archive contains the original generated client test ROM, current
stream-v1 core, launcher, verifier and focus ROM; it contains no commercial
ROM, save, log or capture. The Mac bundle uses the installed standalone core.
The verifier fixture passed for both roles and correctly rejected one
corrupted received byte. The 16-byte RetroArch frontend exchange has since
passed on two Ubuntu processes; the prepared Mac and Steam Deck bundles remain
available for cross-device GUI validation.

A further Mac-to-Ubuntu frontend attempt on 2026-11-17 did not reach Netplay.
The isolated Ubuntu RetroArch 1.18.0 host listened on TCP port 55494 and shut
down normally with zero serial bytes sent, received or paired. On the Mac,
directly invoking the standalone RetroArch executable reproduced `SIGABRT`
during AppKit/HIServices application registration. `open -a` returned
Launch Services error `kLSNoExecutableErr`; checking the installed app also
reported an invalid code signature. A temporary copy was ad-hoc signed and
verified, but Launch Services returned the same error for that copy. Thus no
Mac core or client ROM loaded in this attempt, and it provides no evidence
about cross-device serial transport. The blocker is opening RetroArch from the
Codex-launched process context; it must be started in the interactive desktop
session before this frontend test can continue. All test files and logs stayed
outside the repository.

A corrected Ubuntu relay attempt later on 2026-11-17 found the password-
protected host in RetroArch's public lobby and established the host's tunnel
link. The first harness lookup missed the room because lobby entries wrap
their values in a `fields` object; after correcting that, the client process
loaded the synthetic ROM but never logged a Netplay join or LotusGB client
startup. The host ended with `bytes_sent/peer_bytes_received/bytes_paired =
0/0/0`, and both SRAM verifiers correctly failed. Both the lobby-composite
`--connect=HOST|PORT|SESSION` form and the separate `--connect`, `--port`, and
`--mitm-session` form were tried. A localhost CLI control did enter Netplay
and logged the expected connection failure, so command-line Netplay itself is
available; the relay-session client path remains unresolved. This is not a
failed serial-payload test. All RetroArch processes and the temporary lobby
room were closed; test ROMs, saves and logs remain outside the repository at
`/home/ubuntu/lotusgb-netpacket-relay-uiargs-20261117/`.

A GUI-based public-relay probe on 2026-11-17 then completed the full synthetic
16-byte stream. Ubuntu RetroArch 1.18.0 host and client were separate GUI
instances on the same Ubuntu VM, using the same LotusGB core and isolated
probe ROMs. The client found the password-protected host in the public lobby,
joined it, and started the stream after the mapped Game Boy A input. RetroArch
reported 98 ms host-side and 126 ms client-side Netplay ping. On graceful
shutdown, both final summaries reported sent/received/paired `16/16/16`, zero
sequence errors, duplicates and pending bytes. `verify.sh both` passed: both
SRAMs had checkpoints `11223344` and the expected complete received streams
(`3ca5a5` host, `a53ca5` client). The host's first byte waited 57.6 seconds
while the lobby was being navigated and the client joined, inflating its
overall average wait to 3.64 seconds; the client average/max were 78.4/158.7
ms. This verifies the GUI lobby, relay join and multi-byte Netpacket path, but
the two emulators were still on one VM, so it does not validate different
networks, NAT traversal between players, or Pokémon gameplay. Both processes
were stopped and the temporary room was removed; artifacts remain outside the
repository under `/home/ubuntu/lotusgb-netpacket-relay-gui-20261117/`.

A 2026-11-17 direct LAN run then used the Steam-launched Mac RetroArch 1.22.2
client and Ubuntu RetroArch 1.18.0 host, both with the stream-v1 LotusGB core.
The Steam core was updated from the current installed universal Mac core; the
previous Steam core was preserved in `/private/tmp/`. The peer connected to
`192.168.1.191:55495`; RetroArch warned that frontend versions and game
contents differed, and reported 27 ms client-side / 35 ms host-side ping. Both
logs show all 16 byte pairs. A check of the Mac client's `transfer-start` and
`after-byte-pair` log records matched all 16 local bytes and all 16 received
bytes to the expected streams. The Ubuntu host exited normally and
`verify.sh host` passed with checkpoints `11223344`, SRAM `3ca5a5`, and the
complete received stream. The Mac client was stopped with SIGINT; its log
contains all pairs but lacks the final frontend-stopped summary, and no client
SRAM file was flushed, so `verify.sh client` failed. This validates the
cross-device serial byte exchange from both logs, but leaves client-side SRAM
persistence unverified for this run. The earlier user-confirmed Red/Blue save
readback remains separate evidence. All probe ROMs, logs and saves stayed
outside the repository under `/private/tmp/lotusgb-netpacket-mac-lan-run2-20261117/`
and `/home/ubuntu/lotusgb-netpacket-mac-lan-run2-20261117/`.


A follow-up direct-LAN run on 2026-11-17 repeated the 16-byte exchange with
Steam RetroArch 1.22.2 on Mac and RetroArch 1.18.0 on Ubuntu, using isolated
synthetic ROMs and port 55499. RetroArch reported a 36 ms host-side ping and
warned about frontend and content version differences. Both sides completed
sent/received/paired `16/16/16`, with zero sequence errors, duplicates or
pending bytes. The client was launched through Steam with
`--max-frames=300`, allowing RetroArch to exit normally and flush its SRAM;
`verify.sh client` passed checkpoints `11223344`, SRAM `a53ca5`, and all
received bytes. The Ubuntu host was then stopped cleanly and
`verify.sh host` passed checkpoints `11223344`, SRAM `3ca5a5`, and all
received bytes. This closes the prior client-save gap for a synthetic direct
LAN stream. It still does not test Pokémon gameplay, cross-ISP/NAT or WAN
latency. Artifacts remain outside the repository under
`/private/tmp/lotusgb-netpacket-mac-lan-max-20261117/` and
`/home/ubuntu/lotusgb-netpacket-mac-lan-max-20261117/`.


A 2026-11-17 cross-device high-latency probe routed the Mac Steam RetroArch
1.22.2 client to the Ubuntu RetroArch 1.18.0 host through a temporary
user-space TCP bridge that delayed each forwarded read by 200 ms in both
directions. RetroArch reported 416–417 ms ping. After adding startup delay to
the synthetic ROMs so the Netplay peer could join before serial transfers,
both logs reached sent/received/paired `16/16/16`, with zero sequence errors
or duplicate peer bytes. Client average/max byte wait was 223.9/446.6 ms; host
average/max was 309.5 ms/2.05 s, with the maximum including its slow first
byte. The host exited and `verify.sh host` passed SRAM `3ca5a5` and the
complete received stream. The Mac log records all 16 correct pairs, but the
RetroArch client produced no final shutdown summary or SRAM file, so its save
verifier failed. An earlier attempt without startup delay joined at the same
latency but raced the client ROM before pairing; it is invalid and not counted
as a transport failure. The bridge adds delay per forwarded TCP read, so this
is a bounded high-latency proxy test, not WAN packet jitter, loss or an
Internet route. Artifacts remain outside the repository under
`/private/tmp/lotusgb-netpacket-mac-lan-proxy200-retry-20261117/` and
`/home/ubuntu/lotusgb-netpacket-mac-lan-proxy200-retry-20261117/`.


A follow-up Steam RetroArch shutdown-control probe on 2026-11-17 enabled the
Network Control Interface on localhost UDP port 55356. The Steam RetroArch
1.22.2 log confirmed that it opened the command port. After the delayed 16-byte
exchange, both core logs again showed all 16 pairs and no sequence errors or
duplicates; the Ubuntu host SRAM verifier passed. The client did not write an
SRAM file or emit the frontend-stopped summary. `GET_STATUS` and `QUIT` UDP
commands received no response, both with content loaded and in a separate
empty-front-end check. A short macOS stack sample showed the main thread
waiting in AppKit's event loop; it did not identify a core deadlock. The test
client was force-stopped after the synthetic exchange, and the Ubuntu host was
closed cleanly. This leaves Steam frontend shutdown/control behavior unresolved;
it does not change the successful direct-LAN client SRAM verification above.

A repeat Mac Steam RetroArch 1.22.2 ↔ Ubuntu RetroArch 1.18.0 direct-LAN probe
on 2026-11-17 used isolated synthetic ROMs and TCP port 55506. The client
connected at 42 ms reported ping and the host reported 51 ms. Both endpoints
completed sent/received/paired `16/16/16`, with zero sequence errors,
duplicates, or pending bytes. The client average/max byte wait was 22.5/51.1
ms; the host was 11.3/124.5 ms. Both frontends exited normally after the
bounded probe, and both SRAM verifiers passed: client `a53ca5`, host `3ca5a5`,
with checkpoints `11223344` and all 16 received bytes intact. RetroArch warned
about the frontend version and synthetic content differences but accepted the
session. This repeats the cross-device LAN byte-stream and save-flush check;
it is not a WAN, cross-NAT, or Pokémon gameplay test. Test files and saves
remain outside the repository under
`/private/tmp/lotusgb-netpacket-mac-lan-rerun-20261117/` and
`/home/ubuntu/lotusgb-netpacket-mac-lan-rerun-20261117/`.


A later 2026-11-17 direct-LAN probe between Steam Deck Steam RetroArch 1.22.2
and Ubuntu RetroArch 1.18.0 completed the gated 16-byte synthetic exchange on
TCP port 55508. Both endpoints used LotusGB core SHA-256
`211d057d6e8ed874260ab9893654403f282c7464d2b18c6e2efa5f2100016f63`. Despite
RetroArch warnings about frontend-version and synthetic-content differences,
both sides passed their SRAM verifiers: sent/received/paired `16/16/16`, zero
sequence errors, duplicates, or pending bytes, and complete expected receive
streams (`3c a5 a5 ...` on the host and `a5 b0 bb ...` on the client). Reported
LAN ping was about 12–20 ms. The host's maximum byte wait included the long
manual join/start gate and should not be read as steady-state network latency.
The Deck test package's stale client ROM and missing isolated keyboard binding
were corrected outside the repository before the passing run. This is a
cross-device synthetic LAN result, not a Pokémon gameplay test. At the user's
request, the current phase stays on direct LAN; no cross-network relay
transfer was attempted in this run. Generated ROMs, saves, and logs remain
outside the repository.
