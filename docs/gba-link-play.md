# LotusGBA link play in RetroArch

[繁體中文](gba-link-play.zh-TW.md) | English

Link play is experimental. Current GBA link support is built and tested
primarily for English Pokémon Emerald; it is not general-purpose GBA
multiplayer support. For this game, **Wireless Adapter** is the recommended
default link device and preferred profile for eventual WAN play. Use
**Link Cable** primarily on LAN. Each player uses their own save.

## Supported scope

- **English Pokémon Emerald — Wireless Adapter:** One real LAN trade exchanged
  one Pokémon per player (two total); both saves reopened offline with the
  traded Pokémon. Battle is unverified.
- **English Pokémon Emerald — Link Cable:** Use primarily on LAN. Relay
  performance and reliability are not established.
- **Other profiles:** Japanese Emerald Wireless Adapter is recognized, but
  gameplay and connection need testing. Japanese Emerald Link Cable and
  Ruby/Sapphire cable play are not supported by the current remote profile.
  Other games and languages are unverified.

These results cover only the listed tests; they do not guarantee other games,
activities or network conditions.

**Saves and safety:** LotusGBA does not send the full save file. During a
trade, the game sends the Pokémon data needed for that trade, and each side
updates its own save. Using an unmodified ROM does not eliminate security
risks in the core or network connection; connect only with players you trust
and back up your save before trading.

## Before connecting

1. Both players need the same ROM, compatible LotusGBA core versions, and their
   own save.
2. On each device, open **Quick Menu → Options → Link device** and select the
   same device. The core option label is currently shown in English. Close and
   reload the content after changing it.
3. Check RetroArch's active save path. Netplay settings may redirect it.

## Connect and play

1. **LAN:** Put both devices on the same local network. One player hosts
   directly, and the other connects to that host without a relay server.
2. **Relay:** The host enables **Use Relay Server** before hosting; the other
   player joins that same room and does not need to enable the option
   separately. One current-version Wireless Adapter relay trade has succeeded;
   this does not establish save persistence, repeated-session reliability or
   battle support.
3. In both games, enter the matching Wireless Adapter or Cable activity and
   follow the game's prompts. A RetroArch connection alone does not mean the
   games have linked.
4. End the activity normally, save in each game, and close the content or
   RetroArch normally. After a trade, reopen both saves separately and check
   the result.

If the game does not link, check that both players use the same ROM, device
option and in-game activity. When reporting a problem, include core and
RetroArch versions, device, LAN or relay, the stage reached, and logs from both
ends. Please do not attach ROM or save files to issue reports.

**Details:** [Wireless Adapter (RFU)](gba-rfu-protocol.md#scope-and-evidence) ·
[Link Cable](gba-pokemon-cable.md) ·
[RetroArch Netplay guide](https://docs.libretro.com/guides/netplay-getting-started/)
