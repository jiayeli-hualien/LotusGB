# Pokémon Emerald RFU implementation reference (English)

[繁中設計說明](gba-rfu-protocol.zh-TW.md) · [繁中細節](gba-rfu-protocol-details.zh-TW.md) · [English overview](gba-rfu-protocol.md) · [English details](gba-rfu-protocol-details.md) · [Terminology](gba-link-glossary.zh-TW.md)

## Summary

This is a supplementary English source-level reference for implementation
behavior, test case names and diagnostics. The Traditional Chinese detail
document is the primary explanation, and its matching English detail document
is the translation.

LotusGBA's RFU path is experimental support for Wireless Adapter connectivity
in Pokémon Emerald, not general-purpose GBA wireless multiplayer. One real LAN
trade in English Emerald completed, and both saves were independently reopened
offline to confirm the result. One current-version relay trade was also
reported, but relay save reload, repeat sessions, role reversal and battle
remain unverified.

Each endpoint runs its own game and keeps its own save. RetroArch Netplay
transports link data; it does not exchange or merge player saves.

```text
Game A ⇄ LotusGBA A ⇄ RetroArch Netplay (LAN or relay) ⇄ LotusGBA B ⇄ Game B
```

The RFU and Link Cable paths use different in-game protocols and are tested
separately.

## Design choices and differences from hardware

Real Wireless Adapter hardware exchanges data over radio. LotusGBA does not
simulate radio signals; each endpoint runs its own game and RetroArch Netplay
carries the link data. Results therefore describe Emerald's use of the RFU
interface and LotusGBA's transport behavior, not the adapter's radio timing or
signal quality.

| Area | Game/hardware model | Current LotusGBA behavior | Trade-off |
|---|---|---|---|
| Data transfer | The RFU parent initiates a radio exchange; the child prepares data for that exchange. | `AsyncAdapter` carries each direction as an independently ordered Netplay stream. | This does not reproduce over-the-air timing; network delay and disconnects still affect the game. |
| Send completion | The game observes separate local command replies, RFU events and NI/UNI application results. | A local send can complete once Netplay owns the immutable data; remote packet delivery and game-level results arrive separately. | Local completion does not mean the peer received the data or the game accepted it. |
| Buffer pressure | The RFU uses its own slots, retries and status flow. | LotusGBA uses bounded queues and backpressure instead of dropping or reordering messages. | This protects data integrity but changes when the guest may wait; it is not hardware timing fidelity. |
| Game decisions | The game controls identity, trade, confirmation and close. | The core does not generate NI acknowledgements or game readiness messages. | Game-level barriers still wait for the real peer; this does not erase WAN latency. |

These are compatibility choices for running Pokémon link play over Netplay, not
claims that the RFU hardware has been reproduced exactly. The implementation
and test details follow below.

## Scope and evidence

This inventory follows the matching English Emerald RFU source, chiefly
`librfu.h`, `librfu_stwi.c`, `librfu_intr.c`, `librfu_rfu.c`,
`AgbRfu_LinkManager.c`, and `link_rfu_2.c` in
[pret/pokeemerald](https://github.com/pret/pokeemerald). It describes the
guest-visible STWI protocol and the game's connection manager. The adapter
profile recognizes Japanese and English Emerald. A complete synthetic NI
identity exchange is tested. This document describes implementation contracts and reproducible tests.
The synthetic guest driver is original test code, not the commercial ROM or
execution of its RFU library.

Current acceptance schedule: the 2026-11-24 distinct-trainer English Emerald
LAN trade passed in-game and file-content checks, and both isolated saves were
independently reopened offline with their trainer profiles and post-trade
parties visible in game. One current-version English Emerald RFU relay trade
also completed on 2026-11-24, per user report. This is a single exchange
observation, not full relay acceptance: post-trade save reload, repeat
reliability, role reversal and battle remain unverified. The relay used no
additional fixed-delay bridge.

## STWI protocol used by the game

### STWI serial transaction

Emerald uses 32-bit normal-mode SIO words. A request starts with
`0x99660000 | (requestWordCount << 8) | command`, followed by that many words.
The RFU returns an ACK header whose command byte is normally `command | 0x80`
and whose length gives the response size in the same 32-bit-word units, followed
by those words. A rejected command uses `0x996601EE` and an error word. The
layouts are in `STWI_start_Command`,
`STWI_send_*REQ`, and the master/slave SIO interrupt handlers.

The GBA normally provides the SIO clock. After certain successful command ACKs,
such as `0x25`, the adapter temporarily takes over the clock and clocks the
next event/request. The game responds, then clock control returns to the GBA.
This per-transfer clock handoff is separate from the RFU parent/child connection
roles and RetroArch host/client roles. In adapter-clocked mode, Emerald
recognizes `0x28` (data ready and change) and `0x29` (disconnected and change),
returns `0xA8` or `0xA9`, and restores GBA-master mode after the ACK exchange.

One timing detail remains easy to misstate: in `sio32intr_clock_slave`, the
guest first receives the adapter's event header, places the corresponding ACK
in `SIODATA32`, and the adapter clocks the next word to receive it. Thus “same
reverse-clock exchange” should mean the ACK is the next word of this
adapter-clocked exchange, without starting a new guest-master STWI request.
The existing adapter test also accepts `0xA8` as input concurrently with the
`0x28` header. That simultaneous case is a compatibility allowance, not what
the decompilation demonstrates; its necessity should be reconciled with the
captured trace before treating the test as proof of correct timing.

### Command inventory

| ID | Emerald meaning and shape | Current LotusGBA behavior |
|---|---|---|
| `10` | Hello/reset-library request, no words. The normal link-manager startup issues it after the `0x8001` ID check and before system/game configuration; the Emerald callback clears its software RFU state. | ACKed. The source does not by itself establish that this command must tear down the adapter's radio connection. |
| `11` | Link status; returns per-slot signal levels. | Synthesizes strength from the network connection. |
| `12` | Version status. | Returns a fixed Wireless Adapter profile word. |
| `13` | System status. | Synthesizes search, connection and slot state. |
| `14` | Slot status. | Not implemented; rejected by the default path. |
| `15` | Config status. | Not implemented; rejected by the default path. |
| `16` | Game configuration, six words containing serial/game/user identification. | Stores six words as the peer advertisement payload. Full RFU identity/checksum behavior is not modeled. |
| `17` | System configuration, one word: MC timer, max multi-frame count and available slots. | Stores the word; does not schedule RFU radio timing from it. |
| `19`–`1B` | Start, poll and end child search. | Mapped to network advertisement and connection state. |
| `1C`–`1E` | Start, poll and end parent search. | Mapped to network advertisement and parent discovery. |
| `1F`–`21` | Start, poll and finalize connection to selected parent. | Mapped to a two-peer network connection. Multi-child slot allocation is not modeled. |
| `24` | Send RFU link-layer data. Variable request body starts with role-specific byte metadata, then LLSF bytes. | Reference modes use a parent-triggered exchange and scheduled child TX; the async experiment immediately offers either endpoint's TX. LLSF remains intact for the ROM to parse. |
| `25` | Send data, then change the SIO clock role. | Accepts local TX and changes SIO role after ACK. Completion follows the selected policy: remote reply, explicit local attempts, grouped prefixes/fences, or async local transport ownership. |
| `26` | Receive link-layer data. Response body is one role-specific receive-metadata word followed by LLSF bytes; empty poll has no body. | Returns completed adapter RX data. Transport credit follows bounded adapter admission, not ROM consumption. |
| `27` | Change SIO clock role, no request body. | Changes the SIO role after its ACK. |
| `28` | Adapter-to-guest data-ready-and-change event; normally no body, with an optional one-word form in the library API. Guest ACK is `A8`. | Synthesizes the event when the current scheduling condition is met. |
| `29` | Adapter-to-guest disconnected-and-change event with one packed slot/reason word; guest ACK is `A9`. | A network disconnect with adapter clock ownership and no event already in flight sends `0x29`, the slot/reason word, then waits for `0xA9` before restoring the guest clock. Other disconnect timing clears connection state directly. |
| `30` | Disconnect request with one slot bitmask word. | Clears local connection state and notifies the network peer. |
| `31`–`36` | Test/recovery and undocumented commands. `32`–`34` are child recovery; `35`/`36` have special clock/event behavior. | Not implemented. Emerald's ordinary profile disables link recovery, but these paths are not generally supported. |
| `37` | Parent resume-retransmit-and-change request. | In local-poll mode, rechecks the retained outstanding exchange without adding a network copy; rejects requests without an exchange. |
| `3D` | Stop mode, no body. Emerald also sends it during normal RFU initialization. | ACKed and ends local search activity; it preserves the LAN peer session. |

IDs are defined in `librfu.h`. Request builders and word counts are in
`librfu_stwi.c`; semantic callbacks and LLSF parsing are in `librfu_rfu.c`.
The table separates commands implemented by LotusGBA from commands that the
decompilation defines. Default rejection is not evidence that an unimplemented
command is unnecessary for every Emerald screen or recovery path.

### Discovery, connection and radio data

Emerald first configures the game identity and adapter parameters. The default
link-manager profile sets `maxMFrame=4`, `MC_TimerCount=32`, disables link
recovery, and sets an NI communication failure limit of 300. Parent and child
then run different search/connect procedures. Parent search returns child IDs
and slot numbers; child search returns parent identity, serial, game name and
user name. The connected RFU roles are parent/child, while SIO clock master and
slave can switch several times during each data cycle.

The Emerald library calls its two RFU application data types **NI**
(**Numbered Information**) and **UNI** (**Un-Numbered Information**). NI
transfers are divided into numbered phases whose progress and acknowledgements
the game tracks. UNI uses a separate receive path with a new-data flag and
overwrite status; its behavior is not interchangeable with NI.

`0x24`/`0x25` carry a transport envelope and one or more Link Layer Subframes
(LLSF). Parent TX metadata is a total byte count; child TX metadata contains
per-slot byte counts in five-bit fields. Emerald's LLSF header is three bytes
for parent frames and two bytes for child frames. It carries NI/UNI type,
slot, phase, sequence/ack state and payload length. `0x26` receive metadata is
not an extra duplicate header: it is the one word after the STWI ACK and
before the raw LLSF bytes. Parent receive extracts per-child counts from bits
8..27; child receive uses the low seven bits as the total byte count.

The local source also establishes that “no newly constructed LLSF payload” is
not “no radio slot.” In `rfu_REQ_sendData`, an otherwise idle parent sets
`LLFBuffer[0] = 1` and asks `0x24` or `0x25` to send one byte. `LLFBuffer` is a
`u32[29]`; the accompanying `LLFBuffer[4] = 0xFF` lies outside this one-word
STWI request and is not the byte transmitted by that request. Public adapter
documentation describes this shape as a *ghost send*: the adapter retransmits
the requested prefix of its retained TX buffer. Emerald uses it to make a
radio exchange happen so children can return scheduled data. It must not be
modeled as a zero-byte peer packet.

The radio cycle is host-driven. A child `0x24`/`0x25` schedules data in its
adapter; it does not independently create an over-the-air exchange. A parent
send, including a ghost send, initiates the exchange. A waiting child receives
`0x28` when host data becomes available. The parent also receives `0x28` when
its exchange completes, including cycles with no new child application data;
the adapter must also complete an exchange whose child reply has no application
payload. Emerald's parent callback marks the cycle complete, then `RfuMain2`
uses `0x26` to collect the child data. Emerald's child callback handles `0x28`
by using `0x26` to collect host data and then scheduling its next `0x25` or
clock-change request.

Emerald's RFU library parses LLSF state, phase, ACK and retransmission fields,
then its link manager moves NI/UNI data into guest queues. `RfuRecvQueue_Enqueue`
and `RfuSendQueue_Enqueue` set full flags at 32 receive entries and 40 send
entries; `RfuCheckErrorStatus` turns either full flag into a fatal link error.
Those guest queues are distinct from LotusGBA's transport queue. A queue count
logged while handling `0x30`/`0x3D` is after ROM cleanup has begun and cannot
by itself identify the original failure.

## What the current emulator does not establish

The experimental async frontend uses `LotusGBA-emerald-rfu-v13-async`; both
endpoints require the same protocol version. It cannot connect to the stable
`LotusGBA-emerald-rfu-v12` checkpoint. The reference modes keep one immutable network snapshot
in flight; the grouped frontend mode below uses a bounded ordered pipeline.
A later parent write may update the local
register awaiting admission, but cannot mutate that snapshot. This TX-register
update rule is a documented emulator compatibility choice, not hardware proof
that every uncompleted command may be overwritten arbitrarily.

In the parent-triggered reference/grouped modes, a child `0x24`/`0x25`
schedules local TX only. When a parent packet arrives, the
child selects that uplink once and returns an immutable reply snapshot. If no
uplink is scheduled, an empty reply still completes the exchange. Guest writes
can prepare the next uplink while a selected reply waits for transport credit;
they cannot change the selected reply. Parent RX becomes available when the
child reply arrives. Group prefixes may report local slot success earlier;
group fences and reference successful polls require the real reply. Local
unsuccessful polls may also restore the clock earlier (see below). This keeps the two
directions in the same parent-triggered exchange instead of accumulating two
independent streams of radio packets.

Command ACKs establish local acceptance. Network credit establishes bounded
receiver-adapter admission. ROM `0x26` consumption and NI ACKs are independent.
A full receiver/reply slot returns backpressure without falsely acknowledging
unbuffered data. Reference modes retain one snapshot in flight; grouped mode
bounds outstanding rounds plus parent RX storage to 32 entries, with an equally
bounded frontend deferred window. Parent writes awaiting admission
update a bounded local register; child reply snapshots remain immutable.
Disconnect clears pending local state. Exact adapter hardware timings,
connection-generation handling across every reset race, general recovery,
and WAN gameplay are not established by these checks.

The initial one-byte parent ghost send uses an explicitly initialized zero
prefix when no retained TX bytes exist. Later ghost sends reuse the retained
prefix. The one-byte shape is shorter than an LLSF header and the reference
library ignores it as application data. This compatibility behavior is covered
by focused checks; power-on hardware TX memory contents remain unverified.

These contracts pass deterministic two-system NI tests using synthetic data,
including delayed credit and both directions of asymmetric delay. They do not
prove that the commercial ROM accepts the resulting connection, completes a
player exchange, or trades successfully. See the consolidated test gate below.

RFU data staging now lives in `GbaRfuDataChannel`, a standalone C++ component
used by `GbaSystem`. Its unit executable is built from the RFU protocol module
alone: it does not construct a `GbaSystem`, boot a BIOS, run a ROM, or advance
SIO. It tests sequence rejection, the parent's single scheduled child slot,
the `0x25` cycle that releases that slot, the child's bounded 32-message
receive queue, and disconnect draining. The GBA libretro suite separately
tests the ROM-facing STWI/SIO bridge, including the `0x28`/`0xA8` clock-role
exchange. These tests isolate adapter queue semantics from ROM execution, but
neither test establishes that Emerald accepts the resulting LLSF contents.

## Source map

- [`librfu.h`](https://github.com/pret/pokeemerald/blob/master/include/librfu.h): command IDs, packet structs, RFU API declarations and role/config constants.
- [`librfu_stwi.c`](https://github.com/pret/pokeemerald/blob/master/src/librfu_stwi.c): request construction and STWI command framing.
- [`librfu_intr.c`](https://github.com/pret/pokeemerald/blob/master/src/librfu_intr.c): master/slave SIO state machine, ACK exchange and clock-role transition.
- [`librfu_rfu.c`](https://github.com/pret/pokeemerald/blob/master/src/librfu_rfu.c): search/connect/data commands, LLSF construction and receive parser.
- [`AgbRfu_LinkManager.c`](https://github.com/pret/pokeemerald/blob/master/src/AgbRfu_LinkManager.c): parent/child callbacks and link-manager sequencing.
- [`link_rfu_2.c`](https://github.com/pret/pokeemerald/blob/master/src/link_rfu_2.c) and [`link_rfu_3.c`](https://github.com/pret/pokeemerald/blob/master/src/link_rfu_3.c): Emerald gameplay queues, VBlank flow and fatal full-queue check.

## How LotusGBA synchronizes both game instances

### Local parent completion polls

`RfuCompletionMode::GuardedLocalPoll` is the conservative reference policy.
The stable trade checkpoint used `GroupedCommands`, described below. The public
system interface retains `RemoteReply` as its default reference behavior.
The reference policies keep one immutable network exchange outstanding; NI ACKs and
application data remain guest-owned.

In the reference library, `rfu_UNI_PARENT_getDRAC_ACK` interprets a zero-length
`0x28` as success for all connected slots. A one-word event instead supplies
an explicit slot ACK mask. `RfuMain2_Parent` waits for the MSC callback before
returning to the main loop; a missing slot ACK selects the next frame's
`rfu_REQ_PARENT_resumeRetransmitAndChange` (`0x37`) path. These source contracts
allow an emulator compatibility policy that avoids holding the leader's main
loop for every WAN round trip:

1. Accept `0x25` locally and retain its immutable network snapshot.
2. Complete a local attempt with `0x99660128`, then a mask word, then the
   guest's subsequent-transfer A8. Mask zero means no remote success yet.
3. On `0x37`, poll the same exchange locally. Do not send another copy or
   overwrite its TX data. A new `0x25` is rejected until the old exchange's
   successful completion has been reported. The first `0x25` may queue behind
   an ordinary pre-finalization `0x24`; that older reply cannot acknowledge the
   queued clock-changing cycle.
4. A real child reply admits RX data and latches the connected slot's ACK.
   The next local event reports that ACK exactly once. Its mask is fixed at
   the event header, even if a reply arrives between header and body. RX may
   be read by `0x26` before the successful poll without losing the ACK latch.
5. Disconnect clears pending local attempts and success state. If a peer
   disconnects during an event body, finish that immutable event's body/A8;
   a full system reset discards the serial transaction.

This schedules local *unsuccessful attempts* independently of WAN delivery;
it does not predict peer data or manufacture an NI ACK. It is an intentional
emulator policy, not measured adapter radio timing. It can release main-loop
callbacks while communication waits, but does not remove the round trips
needed to exchange data. Actual animation smoothness, input response and
transfer duration still require private game-level measurements.

Focused original two-system fixtures cover eight local attempts during one
held exchange, delayed ordered bidirectional streams, jitter/credit delay,
immutable event masks across a late reply, early RX consumption, disconnect
and NI identity under the new policy. The helpers drive SIO through CPU frames;
local-attempt counts are not measurements of commercial-game visual FPS.

### Guarded application synchronization boundary

Unrestricted local polling (`LocalPoll`, retained as a focused reference mode)
can violate application timing assumptions even when it never invents a peer
or NI ACK. The source's trade flow starts a block transfer and advances a local
fade timer. `CB_WaitToStartRfuTrade` calls `SetLinkStandbyCallback`, then advances
the UI unconditionally. `Rfu_SetLinkStandbyCallback` installs the standby callback
only if the previous transfer callback is already idle. If local fade frames
outrun WAN block delivery, standby installation can be skipped. The later
callback-idle check cannot distinguish that from a completed standby barrier.

`GuardedLocalPoll` starts conservatively and keeps the entire data phase
synchronous. NI, unrecognized/malformed frames and non-key UNI commands in either
direction require a real peer reply before the parent's event. Idle slots and
ghost sends retain that boundary, including gaps between blocks and readiness
commands. Only a parent-owned `0xBE00` key command in an otherwise pure key UNI
frame reopens local polling; a relayed child's keys cannot do so. No application
readiness command is manufactured, and existing immutable TX/credit/NI rules
remain unchanged. This is an Emerald-profile compatibility policy, not hardware
RFU behavior or a general game-independent packet classifier.

The original two-system `trade_handoff_guarded` and `trade_handoff_idle_gap`
fixtures model a busy block callback, fixed local fade budget, conditional
standby installation and unconditional UI transition. Both fail with unrestricted
completion and pass with guarded completion; `trade_handoff_reference` covers
the original remote-wait behavior. These are source-derived guest drivers, not
commercial-ROM execution or an exact measurement of the ROM's frame budget.
`guarded_phase_boundaries` and `guarded_peer_boundary` cover conservative startup,
idle-gap retention, peer data, mixed player rows and explicit key-phase recovery.

The guarded data phase can still visibly wait for network round trips. This
change protects application synchronization; it does not establish smooth trade
animations, fast lobby movement or WAN compatibility. Real-game acceptance
remains separate and private.

### Grouped command pipeline

The v12 frontend selects `GroupedCommands`. A group begins with
the parent's own UNI `0x8800` block initializer, followed by contiguous
`0x8900 | index` fragments. Up to 20 data fragments and 28 total commands
(including repeated initialization/idle slots) enter a group. Excessive,
unknown, malformed or out-of-order content falls back to a peer fence.
Readiness, disconnect and other non-block/non-key commands also fence the
stream, including such commands in relayed player rows.

**Intentional ACK semantic divergence:** a buffered prefix reports slot ACK
mask 1 after its immutable bytes are accepted by the bounded local/transport
window. This is not proof of remote receipt. Unlike zero-mask polling, this
also advances the guest's block-send callback along with local UI frames.
The last fragment waits for its real child reply. Ordered reply ownership
makes that final reply a cumulative fence for all earlier commands. Ordinary
transport admission credits still exist per packet; they do not hold the
producer at every command while window capacity remains. Idle/key traffic
outside an explicit block group waits for a peer reply; only gaps within an
active, bounded block group may complete as prefixes. NI frames and application
readiness contents are never generated or predicted by the emulator.

The parent tracks each outstanding round's sequence, clock mode and completion
policy. A late reply to an already completed prefix cannot complete another
command. An ordinary `0x24` may remain in flight while the first clock-changing
`0x25` is queued. Its reply releases transport ownership without acknowledging
that newer radio event or clearing its group initializer. The
`grouped_clock_transition` fixture covers this transition through serial I/O.
Outstanding rounds plus queued parent RX never exceed 32 entries;
when capacity or transport credit is unavailable, local STWI acceptance may
retain one pending immutable write, but radio success waits for actual storage.
The last available RX/round slot always uses a real peer fence, even inside
a block group. This reserves a completion that lets a guest waiting on radio
success reach its next `0x26` read. Without this rule, late prefix replies can
fill RX while the next buffered send waits for the guest to free that same RX.
This is explicit profile-specific buffering, not an assertion of hardware RFU
slot ACK behavior or a general solution to all application timing assumptions.

A grouped child admits the next parent message only after the previous event,
A8 and RX transaction have released ownership. This prevents a burst from
queuing events that would otherwise be cleared by the first A8. The frontend
retains deferred messages in order within the same bounded window. Child
replies preserve the selected scheduled uplink; no application payload is
dropped or deduplicated to make the pipeline fit. Reset/disconnect clears
both round ownership and unfinished grouping state.

Original fixtures cover the prefix producer running before any delivery,
real final-fence waits, exact ordered bidirectional data, repeated initializer
plus a 17-fragment block, delayed admission credits, a two-entry transport
window, a full 32-entry RX window with a reserved completion fence, immutable
writes, unrelated replies,
unreachable peers, disconnect and NI identity. The source-derived standby
handoff model is also exercised in grouped mode. `grouped_wait_budget`
compares service ticks for the same ordered stream with and without per-command
waits; it is not measured network RTT or commercial-game frame rate.
`grouped_receive_pressure` models a long key stream followed by repeated block
groups, nonempty child replies, and one parent receive per completed command;
it checks that RX pressure cannot prevent the guest reaching its next receive.
See [Scope and evidence](#scope-and-evidence) for the bounded game checkpoint.

### Asynchronous adapter experiment

The v13 frontend selects `AsyncAdapter`; its bounded trade checkpoint is
described in [Scope and evidence](#scope-and-evidence). The reference modes remain
available to original two-system fixtures. This experiment changes only the
adapter and transport, without replacing guest functions or modifying ROM bytes.

TX and RX are independent ordered streams. A child `0x24`/`0x25` immediately
offers an immutable uplink to the bounded transport, rather than waiting for
the next parent packet to select it. A parent `0x25` completes locally once
its immutable TX is owned by that transport; its explicit slot mask is local
adapter success, not proof of peer receipt or application completion. Remote
packets later populate RX independently. Parent data arrival never creates a
synthetic child reply, and no NI ACK or application readiness is generated.

Unavailable transport capacity retains one immutable pending write and holds
its radio completion; further writes cannot overwrite it. RX admission uses
the existing bounded queues and credits. Unlike a lossy radio model, this
experiment uses backpressure instead of discarding application packets. A
child cannot admit the next packet before the previous event/A8/`0x26`
transaction releases ownership. These differences require separate latency
and game acceptance checks, rather than assuming equivalence with other cores.

An installed frontend pump receives and drains deferred packets up to eight
times per emulated frame. This reduces the extra frame boundary delay when a
child's receive transaction completes within a frame. Pump callbacks may
disconnect the link, so invocation retains its own callable and re-checks
connection state on subsequent polls. Exceptions reach the frontend ABI error
boundary; the core does not run guest CPUs recursively during a receive callback.

The strategy was researched against David Guillen Fandos's
[RFU write-up](https://www.davidgf.net/2024/01/13/gba-wireless-adapter/) and
[gpSP's adapter implementation](https://github.com/libretro/gpsp/blob/5819380c2ffb0900219d700a382ee68c464ebb99/rfu.c).
Behavior requirements and synthetic tests were implemented independently;
gpSP code is not incorporated or translated. Relevant distinctions include
proactive client sending, polling during an adapter wait, and bounded receive
storage; gpSP also permits packet loss and uses its own timed completion policy.

Original `async_uplink`, `async_local_completion`, `async_readiness_barrier`,
`async_receive_pump` and `async_pump_disconnect` fixtures cover unsolicited
ordered uplinks, immutable writes under delayed credit, local completion with
empty RX, actual peer readiness arriving after ordered block data, bounded
in-frame polling and reentrant disconnection. The readiness fixture is a
synthetic guest contract, not execution of Emerald's callback state machine.
`async_ni_identity` and `async_ni_lost_ack` check guest-owned NI recovery.
These service-tick tests do not measure visual FPS or establish a completed trade.

### Connection and acknowledgement contracts

The adapter profile recognizes English Emerald (`BPEE`) and Japanese Emerald
(`BPEJ`). Original bootstrap fixtures check adapter login and reset response
for both game codes. This profile gate does not establish game compatibility.

#### Source-derived connection stages

The matching English Emerald source exposes these stages:

1. `rfu_LMAN_establishConnection` selects parent child-search (`0x19/0x1A`)
   or child parent-search (`0x1C/0x1D`). Union Room uses a switching manager;
   Direct Corner explicitly selects leader/join. Transport host is independent
   of this game role.
2. `rfu_LMAN_CHILD_connectParent` stops discovery, then executes
   `0x1F/0x20/0x21`. The library's `rfu_CB_pollConnectParent` stores the response
   ID in `gRfuLinkStatus->my.id`; it keeps the selected parent's identity in
   `partner[slot]`. The response provides the local adapter ID here; an
   independent system-level check verifies it differs from the selected
   parent's ID.
3. `ID_CP_END_REQ` transitions the manager to clock change (`0x27`) and then
   `LMAN_STATE_SEND_CHILD_NAME`. `rfu_NI_CHILD_setSendGameName` starts an NI
   transfer; a successful adapter connection is not yet an accepted player.
4. Before finalization, `RfuMain1_Parent` receives (`0x26`) and transmits without
   clock change (`0x24`). Idle traffic starts the first child exchange. The
   child's MSC callback receives the parent frame and schedules its own data
   with clock change. STWI handles the command ACK before adapter-clocked
   `0x28`, subsequent `0xA8`, and return to guest clock.
5. `rfu_STC_NI_receive_Sender` checks slot state, phase, per-phase sequence and
   ACK flags before advancing the sender. `rfu_STC_NI_receive_Receiver` copies
   only the next sequence for that phase and renews status for retransmissions.
   Equal NI frames are therefore not necessarily new application messages.
   UNI has separate new-data/overwrite semantics; arbitrary byte-identical
   application packets cannot all be deduplicated.
6. `rfu_LMAN_PARENT_checkRecvChildName` accepts only completed NI game-name
   data with a permitted serial number. Both parent and child maintain name
   acceptance timers. Later player exchange and UNI command traffic still
   have their own success criteria.

References (function names are the stable anchors, not local file locations):
[Link manager](https://github.com/pret/pokeemerald/blob/master/src/AgbRfu_LinkManager.c),
[RFU library](https://github.com/pret/pokeemerald/blob/master/src/librfu_rfu.c),
[serial IRQ handler](https://github.com/pret/pokeemerald/blob/master/src/librfu_intr.c),
[STWI timers/retries](https://github.com/pret/pokeemerald/blob/master/src/librfu_stwi.c),
[game RFU tasks](https://github.com/pret/pokeemerald/blob/master/src/link_rfu_2.c).
These describe the game library, not a complete adapter hardware specification.

#### Separate acknowledgement contracts

| Layer | What completion establishes | What it cannot establish |
| --- | --- | --- |
| STWI A4/A5 | Local adapter accepted the send command/data | Peer received or game accepted the contents |
| Network storage/credit | Receiver adapter safely stored the network message | Receiver ROM executed `0x26` or parsed NI/UNI |
| Radio event / A8 | Adapter event and serial clock handoff completed | NI transfer or game-level join completed |
| NI ACK inside LLSF | Game library acknowledged the specified phase/sequence | Later game-level exchange or trade completed |

Network credit must not wait for `0x26`: a local TX waiting for credit can
otherwise block the radio event needed to let the peer consume its RX data.
Likewise, storing every parent retry in a frontend FIFO can turn repeated
adapter activity into an unbounded backlog. The local TX register and immutable
exchange snapshot are distinct to avoid these dependencies.

## Test evidence and remaining questions

### Remaining validation

- Connection generations and stale packets across reset/reconnect, including
  role reversal without restarting the frontend.
- Adapter timeout behavior, recovery commands and disconnect during an
  already outstanding `0x28` exchange. The external-clock `0x29`/`0xA9`
  disconnect handshake has a focused SIO test.
- UNI data overwrite behavior and sustained exchange throughput with unequal
  frontend pacing. A finite buffer cannot absorb a permanent rate difference.
- Real-network RTT, jitter and timeout budgets. Synthetic service ticks are
  not milliseconds. With one outstanding exchange, an 80 ms RTT permits at
  most 12.5 round trips per second before additional processing delays.
- Private game-level pairing, trade, battle and cancellation/reconnection
  checkpoints. Component tests cannot substitute for game-level validation.

### Consolidated two-system NI test gate

Run the focused gate, or run it as part of the standard GBA suite:

```sh
make -C src/gba_libretro test-rfu-ni build_dir="$PWD/build"
make -C src/gba_libretro test build_dir="$PWD/build"
```

`gba_rfu_ni_test.cpp` creates two actual `GbaSystem` instances with original
looping test ROMs. An independent C++ guest driver implements the NI oracle:
7-byte control record, 26-byte synthetic identity, per-phase sequence/ACK,
END and NULL termination, then serial-number acceptance. All command payloads
and responses traverse the public SIO registers and require real CPU progress.
The oracle has literal header checks, a wrong-sequence ACK check and duplicate
byte-copy assertions. It does not call the adapter's decoder to decide which
NI payload is valid.

The scheduler uses deterministic **service ticks**, not real milliseconds.
A tick advances delivery/credit queues and drives guest command service; each
SIO test helper may run a full emulated frame. These tests therefore cannot be
translated into a verified RTT limit or the commercial ROM's timer budget.
There are no wall-clock sleeps. A single case can be selected, for example:

```sh
./build/gba_rfu_ni_test asymmetric_delay
```

| Cases | Assertion |
| --- | --- |
| `oracle`, `connection_identity` | Independent NI oracle and own adapter ID |
| `paced_reference`, `zero_delay` | Complete identity at paced and normal retry/update rates |
| `paced_symmetric_delay`, `symmetric_delay` | Delayed parent/child traffic |
| `paced_asymmetric_delay`, `asymmetric_delay`, `reverse_asymmetric_delay` | Each directional timing skew |
| `paced_delayed_credit`, `delayed_credit` | Delayed transport acknowledgement without holding STWI ACK |
| `paced_lost_ni_ack`, `lost_ni_ack` | Ignore one NI ACK, retry, complete without duplicate byte copies |
| `jitter` | Deterministic per-message delay variation |
| `wrong_serial` | Finish NI transfer but reject the identity's serial number |
| `local_poll_progress`, `local_poll_reply_race`, `local_poll_disconnect`, `local_poll_credit` | Local attempt completion, explicit ACK ownership, event immutability, reset and transport-credit separation |
| `local_poll_stream`, `local_poll_stream_jitter`, `local_poll_ni_identity` | Ordered delayed streams and NI identity with the local-poll policy |
| `bounded_timeout` | No false success when all delivery is beyond the finite service deadline |

The SIO bridge suite additionally checks immutable in-flight parent snapshots,
updates to the unadmitted TX register, child writes not transmitting by
themselves, immutable selected child replies, admission credit before ROM
consumption, no duplicate credit at `0x26`, empty replies, subsequent-transfer
A8 and disconnect cancellation. The NI gate is included in the normal GBA test
target.

### Opt-in RFU diagnostics

Set `LOTUSGBA_RFU_TRACE=1` in the RetroArch process environment to enable the
extended trace. Tracing does not change the selected exchange/completion policy.
`RFU payload` records frame/cycle, local PC/LR, direction, sequence, byte count
and exact bytes at TX writes, immutable TX dispatch, peer RX admission and
ROM `0x26` reads. Compare dispatch/admission by sender and sequence; a local
TX write may be replaced before dispatch and is not proof of wire delivery.
The trace does not stop after the initial SIO-word logging limit.

Once per 60 guest frames, `RFU frame` records PC/LR/SP, display/blend registers,
black-pixel count and framebuffer hash, interrupt state and RFU queue/progress
state. A black core framebuffer and a frontend displaying black are distinct
observations. These samples do not establish a game's internal task state.
The original `diagnostics_observational` fixture checks unchanged guest cycles,
SIO state and pixels with tracing enabled, payload ownership boundaries, and
white forced blank versus an actually black framebuffer.
