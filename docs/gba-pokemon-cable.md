# Pokémon Emerald Link Cable: Design and Validation

[繁體中文（主要版本）](gba-pokemon-cable.zh-TW.md) | English · [Protocol details](gba-pokemon-cable-details.md) · [Terminology](gba-link-glossary.zh-TW.md)

## Summary

The current Link Cable profile covers English Pokémon Emerald (`BPEE`) only; it does not imply support for other GBA games. LAN is the practical path. Relay remains experimental: some trades and save reloads succeeded, but disconnects also occurred across runs with different versions and delay conditions. This does not establish reliable relay support or a usable latency threshold. Battle, repeated sessions and general WAN support remain unverified.

Each endpoint runs its own game and keeps its own save. RetroArch Netplay transports link data; it does not send saves.

```text
Game A ⇄ LotusGBA A ⇄ RetroArch Netplay (LAN or relay) ⇄ LotusGBA B ⇄ Game B
```

Link Cable and RFU Wireless Adapter use different in-game protocols and are tested separately; see the [RFU article](gba-rfu-protocol.md).

## Device selection and connection setup

The core option `lotusgba_link_device` selects `Wireless Adapter` (the default) or `Link Cable`. Reload the game after changing it. ROM identification selects the compatible game profile; it does not override the selected device. Cable mode does not answer the Wireless Adapter bootstrap.

At Netplay startup, the endpoints exchange ROM identity, session clock, cartridge storage sizes and device choice, but not SRAM contents. Both players need the same ROM and device choice, and each keeps their own save. A device mismatch is rejected. Starting Netplay currently restarts the game.

## How game data is transferred

### The game owns the handshake

The Link Cable profile does not decide player identities, trade contents or whether a trade is complete. English Emerald first searches for a peer, then starts a new game link and exchanges player identities after entering the trade seat. Entering the lobby does not prove that this second handshake completed.

The initial search still waits for real peer data, so WAN delay can make the game wait or time out. The core cannot make arbitrary network delay or a disconnect instant, and it does not treat “packet received” as a game acknowledgement.

### Data groups after the handshake

After the handshake, the core sends complete eight-word game commands over Netplay instead of making a separate network round trip for every SIO word. Each game still sees local serial registers, transfer timing and interrupts.

Packets are delivered at game data-group boundaries so that new data cannot splice into a group already in progress. The network packet checksum validates one message; the game serial checksum validates the previous group from both players. A network-admission ACK only means the receiving core placed data in its bounded queue; it does not mean Emerald read or accepted it.

The core also has limited Emerald-specific compatibility behavior for handshake state, reset and receive-register lifetime. These policies do not replace game code or modify the ROM. The game still owns identity exchange, parties, completion, cancellation and link close.

## Emulation trade-offs vs a physical cable

A physical cable directly exchanges serial data between the GBAs. LotusGBA instead models each endpoint's SIO registers, transfer timing and interrupts, then sends game data over Netplay. It reconstructs the behavior Emerald expects; it does not reproduce the cable's electrical signal.

| Stage | Game/physical cable behavior | LotusGBA behavior | Play impact |
|---|---|---|---|
| Discovery and handshake | The game exchanges serial data and waits for the peer's reply. | The core uses actual peer data; it does not predict player identity or handshake results. | Network delay still adds wait time, and game timeouts can still occur. |
| After handshake | The cable exchanges serial data using SIO timing. | The core sends complete eight-word commands over the network, then reconstructs local SIO transfers at each endpoint. | Avoids a network round trip for every data word, but guest-visible timing differs from a physical cable. |
| Receiving data | The game reads serial registers and checksums, then decides game state. | Reliable Netplay delivery handles packet arrival and order; bounded queues and admission credit control the game-data flow separately. | The core does not redo TCP retransmission; a full game-data queue can still make the game wait. |

These are compatibility choices for cross-device English Emerald play. They do not reproduce the physical cable's electrical timing exactly or guarantee operation for arbitrary games or network conditions.

## Current game checks

| Scenario | Observation | Limit |
|---|---|---|
| Local two-ROM subsystem | Both guests entered the trade room, completed one trade and returned to party selection. | This is not two-host network acceptance. |
| Two RetroArch instances over localhost Netpacket | With 65 ms added in each direction, the trade completed and both players returned to the trade room. | That run did not check save reload or normal Cable Club exit. Host speed briefly fell to about 5.4 guest FPS during handshake. |
| LAN with 100 ms added per direction | One run reached the lobby, then showed a game communication error after sitting down; a later run at the same delay reportedly traded successfully. | The cause of the failed run is unknown; the successful run did not check normal exit or save reload. No latency threshold is established. |
| Relay | One run completed a trade and normal exit; save persistence was reported after restart, and both frontends logged SRAM writes. | The host logged peer disconnect during shutdown; other runs disconnected at different stages. Repeated reliability is unverified. |
| Reversed host/client roles | One initial handshake failed because the actual peer word differed from an unverified prediction. | This does not show that all reversed-role sessions fail; it shows peer data must not be predicted. |

These are limited observations from different runs and conditions. They do not establish stable Cable relay support. Automated tests cover data order, checksums, delay, queues, resets and disconnect handling; they do not establish game battle or all save paths.

## Latency and deferred investigation

LAN is the practical baseline; relay remains experimental. An earlier no-prediction handshake experiment avoided an incorrect prediction but fell to about 7.6 guest FPS during handshake and later failed during trade. A separate prediction mismatch saw `0x0000` as the first real peer reply instead of the predicted `0xB9A0`. These runs used different delay conditions, so they do not isolate a single RTT as the cause.

Any future latency study should cover the initial and post-seat handshake, delay and jitter, unequal endpoint progress, reset, disconnect and reversed roles. Consider a bounded optimization only if a controlled delay measurement shows longer guest-visible waits, then recheck trade, normal exit and independent offline save reload. This investigation is not currently underway.

## Automated test scope

Original synthetic tests cover game checksums, delayed group delivery, sequence and malformed-message rejection, queue limits, disconnect, reset, guest restart and receive-register lifetime. Frontend checks cover option registration, cable startup and device mismatch rejection.

Automated tests do not establish real game trade, battle, WAN compatibility or input response. For protocol details, see the [Link Cable detail document](gba-pokemon-cable-details.md); exact test names, commands and source indexes are in the English [implementation reference](gba-pokemon-cable-implementation-reference.en.md).
