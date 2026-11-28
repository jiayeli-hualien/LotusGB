# Pokémon Emerald RFU Wireless Link: Design and Validation

[繁體中文（主要版本）](gba-rfu-protocol.zh-TW.md) · English · [Protocol details](gba-rfu-protocol-details.md) · [Terminology](gba-link-glossary.zh-TW.md)

## Key points

RFU is the data-exchange protocol Pokémon Emerald uses with the Game Boy Advance Wireless Adapter. LotusGBA's experimental link support currently targets English Emerald only; it is not general GBA wireless multiplayer support.

One English Emerald trade completed over LAN, and both players reopened their saves offline to confirm the result. One current-version relay trade was also reported successful. Colosseum single battles have completed over LAN and relay. Offline reload of both saves after a relay trade remains unverified.

## What happens on original hardware

Each GBA connects to a Wireless Adapter. The game gives link data to the adapter, which sends it to the other GBA over radio. The game on the other side reads the data and decides whether the players are connected, whether a trade is complete and when to close the link.

## How LotusGBA connects both endpoints

LotusGBA does not simulate radio waves. Each player runs a separate game instance. The core emulates the adapter interface used by the local game, then sends link data to the other endpoint through RetroArch Netplay:

```text
Game A ⇄ LotusGBA A ⇄ RetroArch Netplay ⇄ LotusGBA B ⇄ Game B
```

Each game runs independently and keeps its own save. Netplay carries game link data; it does not send or merge full save files. The games decide whether a trade succeeded; the core does not declare it complete on their behalf.

## Choices made to keep link play responsive

The physical Wireless Adapter sends data over radio. LotusGBA sends it over Netplay instead. To reduce waiting, the core does not make the game wait for a network round trip after every local send. This can reduce some waits, but the timing differs from the physical adapter.

The game still waits for the other endpoint at steps such as identity and trade confirmation. When a receive queue is full, the core pauses further sending and preserves data order. Network delay or a slower endpoint can therefore still make the game pause. These choices adapt Emerald's link play to Netplay; they do not reproduce the hardware exactly or establish a supported network-latency range.

## Current validation results

| Test | Current result | Not yet established |
|---|---|---|
| English Emerald, LAN | One trade completed. Both distinct-trainer saves were reopened offline and showed the original trainer and post-trade party. A separate single battle showed complementary Win/Loss screens. | Other game flows and long-term reliability. |
| English Emerald, relay | One current-version trade was reported successful on 2026-11-24. | Offline reload of both saves after the trade. |
| English Emerald, relay single battle | On 2026-11-27, a single battle and Trainer Card viewing were reported successful through the Singapore relay. After reversing host and client roles, play reached a faint and both players returned to the lobby. Viewing the Trainer Card kept the session connected. Neither run added artificial delay. | Formal Win/Loss screens were not captured in the reversed-role run. Long-term relay reliability remains unverified. |
| Japanese Emerald | The core recognizes game code `BPEJ`; synthetic startup and reset tests pass. | Actual Japanese Emerald link play. |
| Automated checks | Two emulated GBA systems exchange test data to check ordering, delay, retries, queues and some reset cases. | Whether the commercial game completes a connection, trade or battle. |

Automated tests schedule events in service ticks, not milliseconds. Their results cannot be converted into an RTT limit or the game's real timeout budget. Other completed and outstanding checks are listed above.

## Protocol details

For the game's commands, data formats, acknowledgements and synthetic-test scope, see the [RFU protocol details](gba-rfu-protocol-details.md).
