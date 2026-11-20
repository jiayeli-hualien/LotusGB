# Pokémon Emerald RFU protocol inventory

## Scope and evidence

This inventory follows the matching English Emerald RFU source, chiefly
`librfu.h`, `librfu_stwi.c`, `librfu_intr.c`, `librfu_rfu.c`,
`AgbRfu_LinkManager.c`, and `link_rfu_2.c` in
[pret/pokeemerald](https://github.com/pret/pokeemerald). It describes the
guest-visible STWI protocol and the game's connection manager. The adapter
profile recognizes Japanese and English Emerald. A complete synthetic NI
identity exchange is tested. Commercial-game validation records remain private;
this document describes implementation contracts and reproducible tests.
The synthetic guest driver is original test code, not the commercial ROM or
execution of its RFU library.

## STWI serial transaction

Emerald uses 32-bit normal-mode SIO words. A request starts with
`0x99660000 | (requestWordCount << 8) | command`, followed by that many words.
The RFU returns an ACK header whose command byte is normally `command | 0x80`
and whose length is the number of response words, followed by those response
words. A rejected command uses `0x996601EE` and an error word. These lengths
count 32-bit words, not bytes. The layouts are in `STWI_start_Command`,
`STWI_send_*REQ`, and the master/slave SIO interrupt handlers.

The GBA is normally the SIO clock master. A successful `0x25` (data plus clock
change), `0x27` (clock change), `0x35`, or `0x37` ACK moves it into clock-slave
mode. The adapter then clocks the next STWI request. In slave mode, Emerald
recognizes `0x28` (data ready and change) and `0x29` (disconnected and change),
returns `0xA8` or `0xA9`, and restores GBA-master mode after the ACK exchange.
The clock role is independent of the RFU parent/child role.

One timing detail remains easy to misstate: in `sio32intr_clock_slave`, the
guest first receives the adapter's event header, places the corresponding ACK
in `SIODATA32`, and the adapter clocks the next word to receive it. Thus “same
reverse-clock exchange” should mean the ACK is the next word of this
adapter-clocked exchange, without starting a new guest-master STWI request.
The existing adapter test also accepts `0xA8` as input concurrently with the
`0x28` header. That simultaneous case is a compatibility allowance, not what
the decompilation demonstrates; its necessity should be reconciled with the
captured trace before treating the test as proof of correct timing.

## Command inventory

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
| `24` | Send RFU link-layer data. Variable request body starts with role-specific byte metadata, then LLSF bytes. | Parent initiates an exchange; child updates its scheduled TX register. LLSF is passed intact for the ROM to parse. |
| `25` | Send data, then change the SIO clock role. | Accepts local TX and changes SIO role after ACK. Parent completion awaits the child radio reply. |
| `26` | Receive link-layer data. Response body is one role-specific receive-metadata word followed by LLSF bytes; empty poll has no body. | Returns completed adapter RX data. Transport credit follows bounded adapter admission, not ROM consumption. |
| `27` | Change SIO clock role, no request body. | Changes the SIO role after its ACK. |
| `28` | Adapter-to-guest data-ready-and-change event; normally no body, with an optional one-word form in the library API. Guest ACK is `A8`. | Synthesizes the event when the current scheduling condition is met. |
| `29` | Adapter-to-guest disconnected-and-change event with one packed slot/reason word; guest ACK is `A9`. | A network disconnect with adapter clock ownership and no event already in flight sends `0x29`, the slot/reason word, then waits for `0xA9` before restoring the guest clock. Other disconnect timing clears connection state directly. |
| `30` | Disconnect request with one slot bitmask word. | Clears local connection state and notifies the network peer. |
| `31`–`36` | Test/recovery and undocumented commands. `32`–`34` are child recovery; `35`/`36` have special clock/event behavior. | Not implemented. Emerald's ordinary profile disables link recovery, but these paths are not generally supported. |
| `37` | Parent resume-retransmit-and-change request. | Not implemented. |
| `3D` | Stop mode, no body. Emerald also sends it during normal RFU initialization. | ACKed and ends local search activity; it preserves the LAN peer session. |

IDs are defined in `librfu.h`. Request builders and word counts are in
`librfu_stwi.c`; semantic callbacks and LLSF parsing are in `librfu_rfu.c`.
The table separates commands implemented by LotusGBA from commands that the
decompilation defines. Default rejection is not evidence that an unimplemented
command is unnecessary for every Emerald screen or recovery path.

## Discovery, connection and radio data

Emerald first configures the game identity and adapter parameters. The default
link-manager profile sets `maxMFrame=4`, `MC_TimerCount=32`, disables link
recovery, and sets an NI communication failure limit of 300. Parent and child
then run different search/connect procedures. Parent search returns child IDs
and slot numbers; child search returns parent identity, serial, game name and
user name. The connected RFU roles are parent/child, while SIO clock master and
slave can switch several times during each data cycle.

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
modeled as a zero-byte peer packet. The former v5 zero-byte marker was therefore
removed as an incompatible approximation rather than retained as RFU behavior.

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

The remote path uses `LotusGBA-emerald-rfu-v9`; both endpoints require the
same protocol version. Parent `0x24`/`0x25` initiates an exchange with one
immutable network snapshot in flight. A later parent write may update the local
register awaiting admission, but cannot mutate that snapshot. This TX-register
update rule is a documented emulator compatibility choice, not hardware proof
that every uncompleted command may be overwritten arbitrarily.

A child `0x24`/`0x25` schedules local TX only. When a parent packet arrives, the
child selects that uplink once and returns an immutable reply snapshot. If no
uplink is scheduled, an empty reply still completes the exchange. Guest writes
can prepare the next uplink while a selected reply waits for transport credit;
they cannot change the selected reply. Parent RX and, for `0x25`, its `0x28`
completion become available when the child reply arrives. This keeps the two
directions in the same parent-triggered exchange instead of accumulating two
independent streams of radio packets.

Command ACKs establish local acceptance. Network credit establishes bounded
receiver-adapter admission. ROM `0x26` consumption and NI ACKs are independent.
A full receiver/reply slot returns backpressure without falsely acknowledging
unbuffered data. There is one network snapshot in flight per direction and no
frontend FIFO of unsent parent snapshots. Parent writes awaiting admission
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

## Connection and acknowledgement contracts

The adapter profile recognizes English Emerald (`BPEE`) and Japanese Emerald
(`BPEJ`). Original bootstrap fixtures check adapter login and reset response
for both game codes. This profile gate does not establish game compatibility.

### Source-derived connection stages

The matching English Emerald source exposes these stages:

1. `rfu_LMAN_establishConnection` selects parent child-search (`0x19/0x1A`)
   or child parent-search (`0x1C/0x1D`). Union Room uses a switching manager;
   Direct Corner explicitly selects leader/join. Transport host is independent
   of this game role.
2. `rfu_LMAN_CHILD_connectParent` stops discovery, then executes
   `0x1F/0x20/0x21`. The library's `rfu_CB_pollConnectParent` stores the response
   ID in `gRfuLinkStatus->my.id`; it keeps the selected parent's identity in
   `partner[slot]`. The v9 response returns the local adapter ID here; an independent
   system-level check verifies it differs from the selected parent ID.
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

### Separate acknowledgement contracts

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

## Remaining validation

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

## Consolidated two-system NI test gate

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
| `bounded_timeout` | No false success when all delivery is beyond the finite service deadline |

The SIO bridge suite additionally checks immutable in-flight parent snapshots,
updates to the unadmitted TX register, child writes not transmitting by
themselves, immutable selected child replies, admission credit before ROM
consumption, no duplicate credit at `0x26`, empty replies, subsequent-transfer
A8 and disconnect cancellation. The NI gate is included in the normal GBA test
target. Keep commercial ROMs, saves, captures and session logs outside the
repository and CI.
