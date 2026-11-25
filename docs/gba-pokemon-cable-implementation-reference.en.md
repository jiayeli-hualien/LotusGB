# Pokémon Emerald Link Cable implementation reference (English)

[繁中設計說明](gba-pokemon-cable.zh-TW.md) · [繁中細節](gba-pokemon-cable-details.zh-TW.md) · [English overview](gba-pokemon-cable.md) · [English details](gba-pokemon-cable-details.md) · [Terminology](gba-link-glossary.zh-TW.md)

## Summary

This is a supplementary English source-level reference for implementation
behavior, test case names and diagnostics. The Traditional Chinese detail
document is the primary explanation, and its matching English detail document
is the translation.

The current Link Cable profile covers English Pokémon Emerald (`BPEE`) only.
LAN is the practical path; relay remains experimental. Past relay observations
include a trade followed by successful save reload, as well as later
disconnection. The runs used different versions and delay conditions, so they
do not establish relay reliability or a usable latency threshold. Battle,
repeated sessions and general WAN support remain unverified.

Each endpoint runs its own game and keeps its own save. RetroArch Netplay
transports link data; it does not send saves.

```text
Game A ⇄ LotusGBA A ⇄ RetroArch Netplay (LAN or relay) ⇄ LotusGBA B ⇄ Game B
```

Link Cable and RFU Wireless Adapter use different in-game protocols and have
separate validation records; see the [RFU article](gba-rfu-protocol.md).

## Emulation trade-offs vs a physical cable

A physical cable directly exchanges serial data between the two GBAs.
LotusGBA instead models each endpoint's SIO registers, transfer timing and
interrupts, then sends game data over Netplay. It reconstructs the behavior
Emerald expects to see; it does not reproduce the cable's electrical signal.

| Stage | Game/physical cable behavior | LotusGBA behavior | Play impact |
|---|---|---|---|
| Discovery and handshake | The game exchanges serial data and waits for the peer's reply. | The core uses actual peer data; it does not predict player identity or handshake results. | Network delay still adds wait time, and game timeouts can still occur. |
| After handshake | The cable exchanges serial data using SIO timing. | The core sends complete eight-word commands over the network, then reconstructs local SIO transfers at each endpoint. | Avoids a network round trip for every data word, but guest-visible timing differs from a physical cable. |
| Receiving data | The game reads serial registers and checksums, then decides game state. | Bounded queues and backpressure retain data; network admission and game acknowledgements remain separate. | Data is not dropped or fabricated for smoothness; a full queue may make the game wait. |

These are compatibility choices for cross-device English Emerald play. They do
not reproduce the physical cable's electrical timing exactly or guarantee
operation for arbitrary games or network conditions.

The frontend can select Wireless Adapter or Link Cable before loading
content. Cable mode currently enables an English Emerald (`BPEE`) profile;
other cable games are rejected instead of receiving guessed protocol data.
Synthetic system and frontend tests are implemented. A private English Emerald
check reached the trade room on both guests through the direct local two-system
subsystem. After preserving child receive results until guest consumption, two
independently clocked RetroArch instances over localhost Netpacket completed one
trade and returned to both party-selection menus with the exchanged species.
This bounded checkpoint does not establish broad game, battle or repeated-session
compatibility. Cross-device and relay observations appear below; they do not
establish reliable WAN support. The RFU checkpoint is described separately
in [the RFU inventory](gba-rfu-protocol.md#scope-and-evidence).

## Session setup and data transfer

### Device choice and session setup

`lotusgba_link_device` selects `Wireless Adapter` (default) or `Link Cable`.
Restart content to apply a device change. Cable mode does not answer the RFU
adapter bootstrap, including after a system reset. ROM identification selects
the compatible protocol, not the user's device preference.
The local two-ROM subsystem applies this device choice to both guests as well;
frontend fixtures probe adapter presence on each cartridge before and after reset.

Netpacket startup exchanges ROM identity, session clock, cartridge storage
sizes and device choice. It does not exchange SRAM contents. Different device
choices are rejected explicitly. Each endpoint runs one guest and keeps its
own save. The current frontend still requires matching ROM images. Connecting
retains the existing guest-restart behavior; removing that restart is separate
frontend work.

### Complete commands cross the network

The game's cable handshake remains guest-owned. The experimental profile waits
for actual peer words during discovery and handshake. An earlier prediction of
the slave word `0xB9A0` from multiplayer readiness proved unsafe: a relay test
observed an initial `0x0000` reply and failed on the mismatch. A previous
no-prediction experiment avoided that mismatch but fell to about 7.6 guest FPS
during relay handshake and subsequently failed during trade. Those attempts
used different reported pings, and neither established reliable relay play.
While raw transfers are active, the current implementation bounds the child's
guest frame lead using peer progress; the master can initiate transfers without
waiting for peer frame watermarks. A guest serial reset clears the old pacing
baseline, and once buffered command delivery is active both guests run
independently. This intentionally changes child guest timing during network
handshake. Synthetic fixed-delay tests cover these boundaries. In one manual
two-host test with 65 ms added in each network direction, this experimental
profile entered the trade menu, completed an exchange, and returned both
players to the trade room. The two displayed parties reflected the exchange;
independent save reload and normal Cable Club exit were not checked in that
run. Host throughput briefly fell to about 5.4 guest FPS during a handshake.
This checkpoint does not establish general timeout or WAN reliability.
With the same experimental core and starting saves, one separate test adding
100 ms in each direction entered the lobby but hit the game's communication
error after the players sat down, before reaching the trade menu. RetroArch
reported 235–295 ms at connection; the initial handshake ran at about 3.8
guest FPS. The host and child traces showed different link states near the
error, but did not establish its cause. A repeat with read-only task tracing
showed both guests' `Task_WaitForLinkPlayerConnection` counters reach 300
while remote player identities remained unreceived, followed by the error
callback on the next update. This confirms that run reached the game's
300-update timeout; it does not explain why the identity exchange stalled.
Another run at the same injected delay sampled the master game in connected
state with buffered transport active, while the child game remained in
handshake with buffered transport active; on the next sampled guest frame
the child reported zero players. The game requires a stable player count
before accepting the handshake, while the emulator switches on the
`0x8FFF`/`0xB9A0` word pair. The experimental English-ROM profile now observes
the child's game link state and repeats the already observed parent handshake
locally while that guest remains in handshake. These local transfers neither
consume buffered groups nor acknowledge network delivery. Once the guest is
connected, buffered checksum and data delivery starts. This is an intentional
guest-visible compatibility behavior, not a hardware claim. A synthetic
callback test covers the rejected-first-handshake case. In a subsequent
manual two-host run with 100 ms added in each direction, the user reported a
successful trade test. Both guests received player identities during the
post-seat handshake and continued exchanging buffered blocks; neither log
reported a core error or peer disconnect through the observed checkpoint.
Normal Club exit and independent save reload were not checked in that run.
A later two-host English Emerald relay session with this profile completed a
trade and a normal Cable Club exit. The user reported save persistence after
restarting, and both frontends logged writes to their isolated SRAM files.
The host logged peer disconnection during shutdown. An earlier relay session
also completed a trade, but disconnected after closing the trade menu.
Broader disconnect behavior and repeated trials remain unverified. The tests
do not establish a latency threshold.
Another relay session with the host and client reversed failed at the initial
handshake because the real peer word differed from the prediction. These are
bounded observations, not a general relay compatibility claim.
Ordered data blocks that arrive after the child's queued `0x8FFF` transfer are
held in its bounded receive queue until the child completes the matching local
handshake. Earlier blocks from a prior parser attempt are still discarded.

A matching `0x8FFF`/`0xB9A0` exchange activates the buffered protocol. Emerald
checks that the player count matches the previous discovery poll before
accepting the master's handshake. Discovery requests use the receiving guest's
current cycle because each guest can enter discovery at a different time. If
the receiver is outside multiplayer mode or leaves it before answering, the
initiating guest gets an open slot (`0xFFFF`) and can retry. After the handshake,
the network carries complete eight-word commands with sequence and payload
checksum; it does not perform a remote request/reply for each SIO word. Local
serial register updates, transfer timing and interrupts reconstruct the guest's
wire interface. The child's local cadence follows Emerald's VBlank/Timer 3
schedule. This is a game-specific compatibility policy, not general cable
hardware fidelity. No ROM instructions or high-level game callbacks are
replaced.

Source contracts come from `SerialCB`, `DoRecv`, `DoSend` and `SendRecvDone` in
[the matching pokeemerald link implementation](https://github.com/pret/pokeemerald/blob/9103e529c2fbdd7add4a1ff3350c6c1f92112c42/src/link.c).
The important distinction is between two checksums:

| Check | Meaning |
|---|---|
| Network packet checksum | Sum of that sender's eight payload words; validates the immutable message before queue admission. |
| Guest serial checksum phase | Sum of both participants' words delivered in the preceding local virtual group. Both local receive registers expose that checksum. |

Each guest group starts with the preceding group's combined checksum, followed
by eight data words. The first checksum is not validated, matching the game's
initial unavailable-checksum state. A later mismatch sets the SIO error state.
The local ROM's outgoing checksum is never mistaken for a payload word.

An arriving packet is selected only at a group boundary. Data arriving halfway
through a group remains queued until the next group, without splicing words or
changing a checksum already exposed to the guest. When no peer command is
available, the virtual peer supplies an empty group. All-zero outgoing command
groups are not sent over the network and do not consume message sequence numbers.
Nonempty commands remain ordered. The frontend limits each direction to 32
unacknowledged data blocks. The receiver returns a transport admission ACK only
after a block enters the system queue; this ACK replenishes network credit and
is separate from every Emerald checksum or guest-level response. If the local
queue is temporarily full, the frontend retains the ordered block in a bounded
deferred queue and withholds its admission ACK. When the guest consumes a queued
group, polling retries admission and then returns credit. If the sender exhausts
its 32 credits, the core retains the completed immutable block and pauses guest
execution at that group boundary until an ACK arrives. This avoids dropping or
reordering commands while bounding memory and guest lead. Invalid checksums,
duplicate sequences and gaps remain errors with diagnostic state (expected
sequence, queue depth, and payload checksum values). Synthetic tests cover
queue pressure and sender recovery; delayed cross-device stress still needs
game-level validation.

The reconstructed child clock also waits for the local guest's next
`SIOMLT_SEND` write before starting another transfer. In established Emerald
connections, `SerialCB` calls `DoRecv` before `DoSend`, making that publication
an observable receive-consumption boundary. Acknowledging the serial IF bit is
insufficient: the game's interrupt dispatcher acknowledges it before the
callback reads the receive registers. Without this local backpressure, another
virtual pulse can clear those registers to `0xFFFF` while the callback is
delayed, corrupting the guest checksum even though network commands are intact.
This is an intentional game-profile compatibility behavior, not a hardware
cable readiness signal or a remote ACK. Delayed pulses are not replayed in a
catch-up burst; the intra-group schedule includes wire time plus Timer 3 time,
and the next group targets a future local VBlank.

Emerald's `DisableSerial` / `EnableSerial` write `SIOCNT=0x2000` without
leaving multiplayer mode. The cable profile treats that write as a guest parser
reset: autonomous child pulses stop, partial and queued command groups are
discarded, and raw discovery must complete again. Raw network transfer sequence
numbers are independent of locally generated serial pulses. Network command
sequence numbers continue across guest resets; valid late commands received
before the next handshake are consumed without guest delivery. This relies on
the frontend's reliable ordered channel: old commands precede that sender's
next handshake completion. A discovery poll reaching a still-buffered peer
gets an open-slot response until that peer's own guest resets. Protocol v16
separates raw wire sequence numbers from virtual transfer counts.

`LinkMain1` provides a possible interface for a higher-level implementation.
It admits up to one outgoing command and consumes up to one received row of
commands per call. That row includes the local player's command. When its
queue is empty, it returns zero commands and reports no new data.

Emerald's upper layers still own these transitions:

| Stage | Game requirement |
|---|---|
| Identity | `ProcessRecvCmds` handles link type and reassembles each player's identity block before marking it received. |
| Party exchange | Player 0 requests each block. `BufferTradeParties` waits for both receive flags before advancing. |
| Confirm and finish | Trade control messages use the game's block transport. Player 0 sends the finish confirmation after receiving both ready messages. |
| Cancel and close | Trade cancellation and the link close/standby commands remain game decisions. Peer data queued for a closed attempt must not enter the next attempt. |

Block transfer uses one `INIT_BLOCK` command followed by ordered
`CONT_BLOCK` commands carrying up to 14 bytes each. The transport can admit
commands independently in each direction. A complete command can be delivered
without completing an application block or advancing a game barrier.
The local serial watchdog and the game timers are separate. For example,
`LinkVSync` checks for a child serial interrupt within ten VBlanks, while
`TryLinkTimeout` checks for link confirmation within 600 task updates.

## Latency and deferred investigation

The measurements below are bounded observations from earlier experiments,
not a supported latency threshold or a current task schedule.

LAN trade is the current practical baseline; relay operation remains
experimental rather than excluded. One relay trade completed and both saves
were reopened successfully, but the connection later dropped. This single
checkpoint does not establish a supported ping threshold or reliable WAN
operation. The failed no-prediction experiment and the prediction mismatch
occurred at different reported pings, so neither can isolate latency as the
sole cause.

Entering the wired trade seat closes the field link. The trade-menu setup then
opens a new link and waits for player identity exchange; see
[`Task_StartWiredTrade` and `Task_WaitForLinkPlayerConnection`](https://github.com/pret/pokeemerald/blob/9103e529c2fbdd7add4a1ff3350c6c1f92112c42/src/cable_club.c)
and [`CB2_CreateTradeMenu`](https://github.com/pret/pokeemerald/blob/9103e529c2fbdd7add4a1ff3350c6c1f92112c42/src/trade.c).
Successful lobby entry therefore does not verify this second handshake. The
game also has a 300-update wait for link players; reaching its error screen
does not by itself identify which condition fired.

The emulator can reconstruct local serial progress and buffer complete commands
without waiting for a network round trip per word. It must still wait for real
peer data at identity, party, confirmation and close barriers, preserve the
guest-visible ordering and checksums, and never turn transport admission into
a fabricated game acknowledgement. Arbitrary network delay or disconnection
cannot be made instantaneous; no ROM change is required to investigate
bounded guest pacing or a higher-level compatibility interface.

### Raw handshake performance

Before buffered delivery is active, Emerald's master starts a serial transfer
on each handshake VBlank. The current cable path waits for the actual remote
word before completing each transfer, so repeated discovery polls cost a
network round trip apiece. In the fixed-delay two-endpoint fixture, 13 replies
take 105 simulated ticks at four ticks per direction and 157 at six ticks per
direction; this is a bounded transport measurement, not a game FPS claim.
In the 100 ms-per-direction game run, both guests reached roughly 3.6–4 FPS
during the initial handshake. The beginning of that run included many repeated
`0xB9A0` starts, including while the other game was still entering discovery.

A possible optimization is to negotiate a bounded discovery window for a
confirmed guest state and satisfy repeated stable polls locally, ending that
window at a verified guest-state transition, reset, disconnect or actual
master-handshake boundary. It must not infer `0xB9A0` merely from device
readiness: a valid first reply can be `0x0000`. Before implementation, a
two-guest test must show how state publication, revocation and ordered
transport prevent stale replies when either guest changes state or restarts;
it must cover late changes, asymmetric delays, disconnect and role reversal.
Without those guarantees, a locally replayed discovery word could advance the
wrong guest state. This is an investigation, not implemented behavior.

The next work is to:

1. Build deterministic two-endpoint tests with fixed round-trip delay, jitter,
   asymmetric guest progress and both initial and post-seat handshakes. Record
   guest frames, serial interrupts, actual words, queue state and game-visible
   timeout boundaries; include disconnect, restart and stale-packet cases.
2. Extend and validate the experimental handshake-only pacing against those
   traces, and assess an explicit readiness barrier. It must accept an initial
   peer `0x0000`, avoid unverified prediction,
   allow both peers to progress without deadlock, and stop gating once buffered
   command delivery is active. Treat the hypothesis that guest clock skew
   causes the observed trade error as unproven until a test demonstrates it.
3. Compare the candidate with the current profile at the same measured delays,
   then validate lobby movement, trade, normal exit and independent save reload
   with both host assignments. Record responsiveness separately from correctness.
   If this boundary cannot meet both, assess a `LinkMain1`-level compatibility
   implementation with explicit identity, party and confirmation barriers.

## Reproducible checks

Run `make -C src/gba_libretro test-pokemon-cable build_dir="$PWD/build"` from
the repository root. The ordinary GBA test target also runs these original
synthetic fixtures, without a commercial ROM:

- `checksum_and_data_phases`: both sides nonzero; combined guest checksum,
  independent packet integrity and no per-word network requests after handshake.
- `delayed_complete_group`: late arrival, stable in-progress group, next-group
  delivery and 16-bit checksum wraparound.
- `handshake_frame_lead_134ms` and `handshake_frame_lead_200ms`: delayed raw
  exchanges use real peer words and bound child guest frame lead; buffered
  child frames continue when the peer stops advancing. These are synthetic
  transport checks, not full game or timeout simulations.
- `raw_master_no_peer_frame_gate` and `raw_child_rebases_after_reset`: the
  master is not stalled by peer frame watermarks, and a child serial reset
  rebases pacing instead of inheriting a buffered-session offset.
- `idle_without_network_roundtrips`: repeated local idle groups generate no
  application packets or remote transfer requests.
- `wire_checksum_and_sequence`: reject corrupt or duplicate messages without
  consuming a valid replacement's sequence; the rejection reports expected
  sequence and calculated payload checksum.
- `wire_checksum_wraparound`: accept and preserve a payload whose 16-bit sum
  wraps modulo 65536.
- `game_checksum_mismatch`: a wrong guest group checksum sets the multiplayer
  SIO error bit while the peer still exposes its accumulated checksum.
- `data_block_queue_boundary`: accept 64 pending commands and explicitly
  reject the next command with the full queue depth.
- `selected_device_and_reset`: public serial/GPIO bootstrap distinguishes
  wireless presence from cable mode and preserves device choice across reset.
- Discovery fixtures cover independent guest clocks, leaving multiplayer with
  a queued request, and arrival while the peer is probing another mode. Each
  verifies a retry can reach buffered mode on both endpoints.
- `serial_restart_with_independent_clocks` covers serial reset without a mode
  change, divergent virtual pulse counts, staggered guest restart, stopped child
  pulses, queued and late old commands, and new-command delivery after handshake.
- `child_receive_result_lifetime` suspends the synthetic guest's serial
  consumption for twelve frames, checks stable receive registers and transfer
  count, then resumes and verifies the intact command and combined checksum.

Frontend fixtures check device registration, a cable-profile startup and
device mismatch rejection. These contracts are separate from actual game
trade, battle, WAN compatibility and input-response measurements.
