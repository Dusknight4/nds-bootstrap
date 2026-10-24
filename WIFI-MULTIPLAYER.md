# Local wireless over WiFi with WideMelon

This build of nds-bootstrap (**v2.17.4**, based on the official v2.17.0)
lets a DS game's **local wireless** (DS-to-DS play) run over your WiFi network
instead, so real consoles can play with each other and with
**[MultiMelon](https://github.com/Dusknight4/widemelon/releases)** on a PC: a
DS emulator with the matching multiplayer support. That is the only reason
this build exists; without it (and without the
[R4 save copy](R4-SAVE-COPY.md)) it's the same as nds-bootstrap v2.17.0.

Everyone just connects to the same WiFi network: consoles and MultiMelons all
hear each other, as DS systems in the same room do, up to four players (as
many as the game allows: the Platinum Underground takes eight). In Pokémon
Platinum and HeartGold (US) the Union Room, trades and battles work between
any of them, whoever starts the talk, including Platinum with HeartGold; the
Link Pokéathlon and the Underground work with four. A player whose WiFi drops
out for a few seconds carries on where they left off, as on the DS. Other
games are untested.

## What you need

- A **DSi**, or a **3DS** with custom firmware, running DS games from the SD
  card with **TWiLight Menu++** in **DSi mode** (the TWiLight Menu++ default on
  these consoles). Only the DSi's WiFi can join today's networks; flashcards and
  DS mode can't.
- A WiFi network that uses **WPA/WPA2** or is **open** (WEP isn't supported),
  saved in the console's own settings:
  - **3DS:** System Settings > Internet Settings > Connection Settings. The 3DS
    passes these networks to DS games in DSi mode; nothing else to enter.
  - **DSi:** System Settings > Internet > Connection Settings > Advanced Setup
    (connections 4 to 6 take WPA/WPA2).
- To play with a PC: a PC on the **same network** running
  [MultiMelon](https://github.com/Dusknight4/widemelon/releases) (consoles
  also play with each other without one). Networks that keep devices apart ("client isolation", common on guest and
  some public WiFi) won't work.

## Installing

1. Download `nds-bootstrap-v2.17.3.zip` from the
   [v2.17.3 release](https://github.com/Dusknight4/nds-bootstrap/releases/tag/v2.17.3).
2. On the SD card, back up `_nds/nds-bootstrap-nightly.nds` if you have it.
3. Copy the zip's `_nds` folder onto the SD card's root, replacing that file.
   It holds `nds-bootstrap-nightly.nds` (this build, in the *nightly* slot, so
   your official *release* nds-bootstrap stays as it is for everything else)
   and `nds-bootstrap-wifi.ini`, which turns Local as WiFi on.
4. In TWiLight Menu++, open the game's settings (press **Y** on the game) and
   set **Bootstrap** to **Nightly**. Do this for each game you'll play over
   WiFi. (Or set nightly as the default in TWiLight Menu++'s settings.)

## Turning it on

1. On the console, Local as WiFi is already on (the zip's settings file).
   To switch, **hold X while the game starts** (from when you pick it in
   TWiLight Menu++ until a menu appears on the bottom screen), choose with
   Up/Down and press **A**: **Local as WiFi**, or **Regular Local** (the
   game's own wireless, with real DS systems nearby). The choice is kept for
   next time, in `_nds/nds-bootstrap-wifi.ini`.
2. In MultiMelon, turn on **System > Multiplayer > Wifi as Local
   Multiplayer** (it's remembered). MultiMelon listens on **UDP port 7071**;
   when Windows asks, allow it through the firewall for the kind of network
   you're on.
3. Start the game on each and go into the Union Room (or the game's local
   wireless mode) as you would with DS systems side by side.

The console joins your WiFi as the game starts and finds the others by itself.
Its WiFi rests while the game isn't using its wireless and comes back when you
enter a wireless area.

## Settings file

`_nds/nds-bootstrap-wifi.ini` (in the zip; changed when you pick a mode with
X; without it, Regular Local):

```ini
; BRIDGE: 1 Local as WiFi, 0 Regular Local
[WIFI]
BRIDGE=1
```

## If it doesn't connect

- Make sure both are on the same network, and that the console's saved
  connection works (the 3DS's connection test).
- In MultiMelon's folder, `WideMelon bridge log.txt` shows what MultiMelon
  sees, including lines starting with `[DS ...]`: each console's own log (which
  network it joined, its IP address, whom it found). No `[DS ...]` lines at all
  usually means the firewall is blocking UDP port 7071, or the network keeps
  devices apart.
- The consoles and MultiMelons find each other by a broadcast on the network;
  networks that block broadcasts between devices won't work yet.

## Known limits

- Pokémon HeartGold/SoulSilver on the PC need WideMelon 1.0.4-mod.5 or later
  (an earlier one shows a blue error screen after cancelling a link trade or
  battle).
- Every console needs this version (v2.17.4) to play with more than one other
  player.
- Playing with more than one other player needs a MultiMelon with four-player
  support on each PC; an older WideMelon or MultiMelon plays with one console
  only.

## How it works, and building it

See [nbwifi/README.md](nbwifi/README.md): a DSi WiFi driver, adapted from
devkitPro's calico, and a reimplementation of the ARM7 half of the DS wireless
library that carries the game's local wireless to WideMelon over UDP.
nds-bootstrap loads them as `nbwifi_blob.bin` when Local as WiFi is on, for DS
games in DSi mode.
