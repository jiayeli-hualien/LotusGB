# Pokémon cable protocol experiment

The frontend can select Wireless Adapter or Link Cable before loading
content. Cable mode currently enables an English Emerald (`BPEE`) profile;
other cable games are rejected instead of receiving guessed protocol data.
Synthetic system and frontend tests are implemented. A private English Emerald
check reached the trade room on both guests through the direct local two-system
subsystem. After preserving child receive results until guest consumption, two
independently clocked RetroArch instances over localhost Netpacket completed one
trade and returned to both party-selection menus with the exchanged species.
This bounded checkpoint does not establish cross-device, delayed-network, battle
or repeated-session compatibility. The RFU checkpoint is described separately
in [the RFU inventory](gba-rfu-protocol.md#scope-and-evidence).

## Device choice and session setup

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

## Complete commands cross the network

The game's cable handshake remains guest-owned. A matching `0x8FFF`/`0xB9A0`
exchange activates the buffered protocol. Emerald checks that the player count
matches the previous discovery poll before accepting the master's handshake.
Discovery requests use the receiving guest's current cycle because each guest
can enter discovery at a different time. If the receiver is outside multiplayer
mode or leaves it before answering, the initiating guest gets an open slot
(`0xFFFF`) and can retry. After the handshake, the network
carries complete eight-word commands with sequence and payload checksum;
it does not perform a remote request/reply for each SIO word. Local serial
register updates, transfer timing and interrupts reconstruct the guest's wire
interface. The child's local cadence follows Emerald's VBlank/Timer 3 schedule.
This is a game-specific compatibility policy, not general cable hardware fidelity.
No ROM instructions or high-level game callbacks are replaced.

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

## Reproducible checks

Run `make -C src/gba_libretro test-pokemon-cable build_dir="$PWD/build"` from
the repository root. The ordinary GBA test target also runs these original
synthetic fixtures, without a commercial ROM:

- `checksum_and_data_phases`: both sides nonzero; combined guest checksum,
  independent packet integrity and no per-word network requests after handshake.
- `delayed_complete_group`: late arrival, stable in-progress group, next-group
  delivery and 16-bit checksum wraparound.
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
