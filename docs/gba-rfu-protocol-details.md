# Pokémon Emerald RFU Protocol Details

[Traditional Chinese main article](gba-rfu-protocol.zh-TW.md) · [English overview](gba-rfu-protocol.md) · [繁中細節](gba-rfu-protocol-details.zh-TW.md) · [Terminology](gba-link-glossary.zh-TW.md)

This document is the English translation of the Traditional Chinese protocol
details. For exhaustive test names, trace fields and source indexes, see the
separate English [implementation reference](gba-rfu-implementation-reference.en.md).

## Scope and sources

This protocol description follows the RFU code used by English Pokémon
Emerald. The main sources are `librfu.h`, `librfu_stwi.c`, `librfu_intr.c`,
`librfu_rfu.c`, `AgbRfu_LinkManager.c` and `link_rfu_2.c`. LotusGBA recognizes
English (`BPEE`) and Japanese (`BPEJ`) Emerald profiles; actual link play with
the Japanese version has not been verified.

This document describes the game-facing interface and LotusGBA's implementation
contracts, not the complete RFU hardware specification. Synthetic NI identity
tests are not tests of the commercial game or physical Wireless Adapter
hardware.

## STWI and SIO commands

Emerald sends STWI commands through the GBA serial I/O (SIO) interface. Each
command uses 32-bit data words. Its header contains a command ID and the number
of following words. A normal successful response uses the command ID plus
`0x80` and reports its response length in words. A rejected command uses
`0x996601EE` followed by an error code.

The GBA normally supplies the SIO clock. After successful replies to commands
such as `0x25` and `0x27`, the adapter can temporarily take over the clock to
send the next event or command; after the game responds, clock control returns
to the GBA. This SIO clock handoff is separate from RFU parent/child roles.

| Commands | Purpose | LotusGBA behavior, in brief |
|---|---|---|
| `0x10`–`0x17` | Initialization, status and game/system configuration | Returns or stores the profile data it needs; it does not fully model every RFU identity or checksum behavior. |
| `0x19`–`0x21` | Search for a parent or child and connect | Maps to Netplay discovery and the two-endpoint connection; multi-child slot allocation is not modeled. |
| `0x24`, `0x25` | Send RFU data; `0x25` also changes the SIO clock role | Stores pending data and handles local replies and network delivery according to the selected synchronization policy. |
| `0x26` | Read received RFU data | Returns completed LLSF data; network credit does not mean the game has read it. |
| `0x27`–`0x29` | Change clock role, signal data-ready or report disconnection | Produces events from transfer state; the game normally answers `0x28` with `0xA8`. |
| `0x30`, `0x3D` | Disconnect or stop searching | Clears local connection state, or ends search while preserving the LAN Netplay session. |
| Other commands | Tests, recovery or unconfirmed uses | Unsupported commands are rejected by default; this does not prove every game flow can work without them. |

A successful RFU search does not mean that the game has joined the other
player. Emerald continues exchanging game names, serials and other identity
data before accepting the player and application messages.

## NI, UNI and LLSF

Emerald's RFU library has two application data types: NI (**Numbered
Information**) and UNI (**Un-Numbered Information**). NI sends data in phases
and tracks phase numbers and acknowledgements. UNI uses a separate receive
state, including a new-data flag and an overwrite error. Both types are carried
in LLSF (**Link Layer Subframes**).

The `0x24`/`0x25` transport envelope carries LLSF data. Parent and child LLSF
headers have different lengths: three bytes for a parent and two for a child.
The header contains the data type, slot, phase, sequence/acknowledgement state
and payload length. In a `0x26` response, receive metadata is the word after
the STWI ACK, not a duplicate LLSF header.

The game's RFU library owns NI retransmission and acknowledgement state.
LotusGBA does not treat Netplay sequence numbers or transport credit as an NI
acknowledgement. UNI also cannot be deduplicated solely because two payloads
have the same bytes.

## Ghost Send

When it has no new application data, Emerald's RFU parent may still start an
RFU exchange by sending one byte. This lets a child return data it has already
scheduled. A Ghost Send is not a zero-byte application packet. The source sets
the send length to one byte in `rfu_REQ_sendData`; the adapter uses a retained
TX prefix to complete the exchange. LotusGBA handles this case with an
explicitly initialized buffer. The contents of hardware TX memory at power-on
have not been verified.

## LotusGBA synchronization policies

In the RFU interface, the parent starts a wireless exchange and the child
queues data for that exchange. LotusGBA retains several synchronization
policies for reference and experiments:

| Policy | Behavior | Important difference |
|---|---|---|
| `RemoteReply` | Waits for the real peer's network reply. | A conservative reference for request/reply behavior. |
| `GuardedLocalPoll` | Lets the parent repeat local polls while waiting, but identity and data-block stages still wait for the real peer. | Does not predict peer data or create NI ACKs; local polling is limited by game-stage boundaries derived from source behavior. |
| `GroupedCommands` | Groups eligible commands and uses bounded buffering to reduce per-command waits. | Local completion of a prefix does not mean the peer received it; the final command waits for a real reply fence. |
| `AsyncAdapter` | Carries the two directions as independently ordered Netplay streams. | A local command may complete after transport accepts its data; this does not mean the peer or game has completed. Full queues apply backpressure. |

The observed LAN trade used the AsyncAdapter path. Unit-test results from the
other policies are not equivalent to that game-level acceptance result.

## An ACK, transport credit and game acknowledgement differ

LotusGBA sends packets with RetroArch Netpacket's reliable mode, which assigns
reliable, in-order delivery to the frontend. The core does not reimplement TCP
retransmission. The receive credit below serves a different purpose: it bounds
data that the peer core has not yet handed to its game.

| Layer | What completion means | What it does not mean |
|---|---|---|
| STWI ACK | The local RFU interface accepted the command. | The peer received the data. |
| Netplay receive credit | The other core put data in a bounded queue. | The ROM read `0x26` or parsed NI/UNI. |
| RFU event / `0xA8` | The event and SIO clock handoff completed. | NI transfer or game join completed. |
| NI ACK in LLSF | The game RFU library acknowledged a phase or sequence. | A later exchange or trade completed. |

These boundaries prevent the core from treating “packet queued” as “Pokémon
traded successfully.” The game still owns player identity, exchanged data,
completion, cancellation and close.

## Test scope

Synthetic two-system tests use an original loop ROM and an independent NI test
driver. They exchange data through public SIO registers while the CPUs advance.
Coverage includes:

- NI identity, phase sequence, acknowledgements, end markers and rejection of
  an invalid sequence.
- Zero delay, bidirectional and asymmetric delay, jitter and delayed transport
  credit.
- Local polling, reply races, disconnect cancellation and bounded timeouts.
- SIO clock handoff, immutable transfer snapshots, child replies and queue
  credit in the RFU bridge.

These synthetic tests advance in service ticks, not wall-clock time. The delay
tests check ordering, retries and timeout handling under the chosen event
schedules; they cannot establish a supported RTT limit or Emerald's real
timeout budget.

Actual-game results are tracked separately: one English Emerald trade has
completed over LAN, and one relay trade was reported successful. Battle, save
reload after the relay trade, reconnect after cancellation and physical
Wireless Adapter timing remain unverified; see the [RFU overview](gba-rfu-protocol.md).

For exact test names, commands, source references and diagnostic trace fields,
see the English [implementation reference](gba-rfu-implementation-reference.en.md).
