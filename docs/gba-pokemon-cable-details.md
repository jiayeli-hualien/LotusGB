# Pokémon Emerald Link Cable Details

[Traditional Chinese main article](gba-pokemon-cable.zh-TW.md) · [English overview](gba-pokemon-cable.md) · [繁中細節](gba-pokemon-cable-details.zh-TW.md) · [Terminology](gba-link-glossary.zh-TW.md)

This document is the English translation of the Traditional Chinese protocol
details. For exhaustive test names, trace fields and source indexes, see the
separate English [implementation reference](gba-pokemon-cable-implementation-reference.en.md).

## Scope and session setup

The remote Link Cable profile currently covers English Pokémon Emerald
(`BPEE`) only. The core option `lotusgba_link_device` selects `Wireless
Adapter` or `Link Cable`; reload the content after changing it. ROM
identification selects a compatible profile but does not override the device
choice.

At Netplay startup, both ends exchange ROM identity, session clock, cartridge
storage size and device choice, but not SRAM contents. A device mismatch is
rejected. Each end runs its own game and save; the current frontend requires
matching ROMs. Starting a session retains the existing game-restart behavior.

## Handshake and data transfer

The game owns the English Emerald handshake. During discovery and handshake,
the core waits for actual peer data; it does not infer `0xB9A0` from device
readiness. A valid initial reply can be `0x0000`. After entering the trade
seats, the game opens a new link and exchanges player identity again. Reaching
the lobby does not prove that this second handshake completed.

After the handshake, Netplay carries complete eight-word game commands instead
of making a separate network request/reply for every SIO word. The core still
updates local serial registers, transfer timing and interrupts so that the
game sees the SIO interface it expects. This is a game-specific compatibility
implementation, not a complete model of the physical cable's electrical
timing.

### Data groups and checksums

Each game data group begins with the combined checksum from the previous group,
followed by eight data words. The game does not validate the first checksum in
its initial state; a later mismatch sets the SIO error state. The network
packet checksum validates one sender's eight words. The game serial checksum
validates the previous local group from both players. These checksums serve
different purposes.

Packets are delivered only at a data-group boundary. If a packet arrives in
the middle of a group, it stays queued so it cannot be inserted into a group
already in progress or change a checksum already exposed to the game. An
all-zero idle command is not sent over the network and does not use a message
sequence number.

### ACKs and queue credit

LotusGBA uses RetroArch Netpacket's reliable mode and delegates packet
delivery and ordering to the frontend; the core does not redo TCP
retransmission. A Netplay admission ACK is a separate application-level credit
reply: it is sent only after the receiving core places data in its system
queue, limiting data not yet handed to the game. It does not mean Emerald read
the data, validated its checksum, completed a game data block or accepted a
trade.

Each direction allows up to 32 unacknowledged data blocks. If the receiver
queue is full, the frontend keeps the ordered packet and withholds credit. If
the sender runs out of credit, the core retains an immutable block at the data
group boundary and pauses the game until credit returns. Bounded queues and
backpressure prevent drops and reordering; the game may still wait when the
two network directions make different progress.

## Game-level boundaries

The game still handles player identity, party exchange, confirmation,
cancellation and connection close:

| Stage | Game responsibility |
|---|---|
| Identity | Receive and assemble each player's identity data before marking that player as received. |
| Party exchange | Request both players' data and wait for both receive flags before proceeding. |
| Confirm and finish | Use in-game control messages and both players' ready state to decide when to complete. |
| Cancel and close | Decide when to cancel or close; data from a closed attempt must not enter the next one. |

The serial watchdog and game task timeout are separate timers. For example,
the source gives the child's serial-interrupt check and the player-identity
wait different frame/task limits. A communication error screen alone does not
identify which layer caused it.

## Synchronization and reset trade-offs

Before buffered delivery starts, initial discovery may wait for a real peer
word on each VBlank. Network round trips can therefore lengthen the handshake.
During raw transfers, the core limits the child's guest frame lead. Once
buffered delivery starts, both games can advance independently rather than
waiting for the network after every word.

The buffered handshake starts with the `0x8FFF`/`0xB9A0` exchange. The game
requires a stable player count before accepting the parent's handshake. While
the child game remains in its handshake state, the core may repeat the already
observed parent handshake locally. This is a known game-compatibility policy,
not a hardware claim; a synthetic callback test covers rejection of the first
handshake.

Emerald's `DisableSerial`/`EnableSerial` writes `SIOCNT=0x2000` while the game
remains in multiplayer mode. The profile treats this as a game parser reset:
it stops automatic child sends, discards partial or queued groups and starts
discovery again. Network command sequence numbers are separate from local
virtual serial-pulse counts; this behavior relies on Netplay's reliable,
ordered channel.

These choices avoid a network round trip for every word during stable data
transfer, but do not make initial handshakes, game barriers, arbitrary delay or
disconnects complete instantly. They are English Emerald compatibility
choices, not a general Link Cable model.

## Tests and game observations

Synthetic system tests cover checksums in both directions, delayed delivery of
complete groups, handshake frame lead, reset/restart, SIO register lifetime,
queue credit and sequence errors. Frontend tests also cover device options,
profile startup and rejection of a device mismatch. These tests do not replace
game trade, battle or WAN validation.

Game observations include a local two-ROM subsystem trade, a delayed trade
between two localhost RetroArch instances and limited relay trades. The
conditions and completion stages differ; see the result table in the
[Traditional Chinese main article](gba-pokemon-cable.zh-TW.md#目前有哪些遊戲檢查).
Exact test cases, trace fields and source indexes are in the English
[implementation reference](gba-pokemon-cable-implementation-reference.en.md).
