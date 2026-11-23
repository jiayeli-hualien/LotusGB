# LotusGBA link play in RetroArch

LotusGBA lets two players connect their own games and saves through RetroArch.
Link play is experimental. For English Pokémon Emerald, choose **Wireless
Adapter** for LAN or internet play; choose **Link Cable** for a local network.
Link Cable can also connect over the internet, but the game may be noticeably
slow while connecting. Both players need their own save.

## What is supported?

| Game and device | Current scope |
| --- | --- |
| English Pokémon Emerald, Wireless Adapter | Recommended for LAN and internet play. Trade has been tested over LAN with added delay; an earlier implementation completed a relay trade. The current version still needs repeated internet and battle tests. |
| Japanese Pokémon Emerald, Wireless Adapter | Recognized by the core; **gameplay and connection need testing**. |
| English Pokémon Emerald, Link Cable | Primarily for LAN play. Trade has also succeeded in a limited relay test, but internet connections can be slow. Repeated sessions and cable battles need testing. |
| Japanese Pokémon Emerald, Link Cable | Not currently supported by the remote cable mode; it cannot be tested by selecting that mode yet. |
| Pokémon Ruby or Sapphire, Link Cable | The original games require a cable, but the current LotusGBA remote cable mode does not support them. This includes Japanese versions. |
| Other games or language versions | Not established as supported by the current remote link profiles. |

These are limited tests, not a guarantee for every network or game activity.
For technical details, see the [wireless adapter (RFU)](gba-rfu-protocol.md#scope-and-evidence)
and [link cable](gba-pokemon-cable.md) documents.

## Before connecting

1. On each device, install a matching LotusGBA core and its `.info` file in
   the directories used by that RetroArch installation. Both RetroArch builds
   must support the libretro Netpacket interface.
2. Both players must load the **same ROM image** and choose the same link
   device. Cross-game and mixed-ROM connections are not supported by the
   current frontend. ROMs and saves are not included with LotusGBA.
3. Each player needs their own playable save. **Back up both saves before
   trading** to reduce the risk of losing progress if a connection or game
   error damages a save. Netplay does not transfer a player's save to the
   other device. For a trade, prepare a party the game permits to trade.
4. Check RetroArch's save directory and any per-core or per-game override.
   Netplay can redirect a save path, so check the actual path in the log if
   unsure. Do not overwrite a personal save with a test copy.

## Select a link device

On **each** device, load the game with LotusGBA, open RetroArch's Quick Menu,
and find **Options → Link device**. Select **Wireless Adapter** or **Link
Cable** on both sides. Wireless Adapter is the default. Close and reload the
content to apply a change; selecting a device does not patch the ROM.

For online English Emerald play, try Wireless Adapter first. Use Link Cable
for the cable route, preferably on a local network. With Wireless Adapter,
the game may show an animated wireless-status icon in the upper-right corner;
this is part of the game's wireless interface. The icon alone does not mean
the other player has connected.

The two choices lead to different in-game connection flows: follow the game's
wireless prompts for Wireless Adapter, or choose the Cable Club trade or
battle counter for Link Cable. Both players must select compatible activities
in the same flow.

See RetroArch's [Quick Menu guide](https://docs.libretro.com/guides/quick-menu/)
and [core option settings](https://docs.libretro.com/guides/overrides/#core-options)
for menu and option-file details.

## Connect and play

1. Save each game normally and make sure the selected device is applied on
   both sides. Use a local controller on each computer; frontend player
   numbers are not the same as the game's link roles.
2. In RetroArch's Netplay menu, one player starts hosting. The other joins that
   room, or connects to the host's address for a direct LAN session. A relay
   can be selected for internet sessions. See the
   [RetroArch Netplay guide](https://docs.libretro.com/guides/netplay-getting-started/)
   for hosting, room discovery and network setup.
3. Connecting currently recreates the game core and may restart the running
   game. Continue from each player's own save after the session connects.
   A RetroArch connection alone does not mean the games have linked.
4. On both games, enter the corresponding wireless or cable activity and
   complete its prompts. In the cable trade room, both players use the trade
   seats and select Pokémon. Wait for both games to finish the exchange.
5. End the activity normally on both sides, leave the Club, and save in each
   game. Close content or RetroArch normally so the frontend writes battery
   saves. If the result matters, reopen each save and check the party before
   replacing a backup.

## If a connection stalls

- **Room not found:** refresh the Netplay room list; check that the host is
  announcing the room and that filters, core versions and network settings do
  not hide it. If using a relay, both players must join the same room.
- **RetroArch connected, games do not link:** verify that both devices use
  the same game image and the same applied link-device option. Then verify
  that both players entered matching in-game activities; LAN host/client
  roles are separate from game menus.
- **Cable handshake feels slow:** on delayed links the game can advance much
  more slowly than the frontend's displayed FPS suggests. Wait for each
  prompt before pressing again. If it does not complete, record the stage and
  try Wireless Adapter for English Emerald.
- **Missing or unexpected save:** close content normally and check the save
  path, overrides and any Netplay redirection in the RetroArch log. Restore
  from a backup only after identifying the file in use.

If reporting a problem, include the core and RetroArch versions, game version,
selected link device, LAN or relay, the stage reached and logs from both
devices. Do not publish commercial ROMs, saves or game-derived captures.
