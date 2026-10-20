# Local wireless over WiFi with WideMelon

This build of nds-bootstrap (**v2.17.0-M1**, based on the official v2.17.0)
lets a DS game's **local wireless** (DS-to-DS play) run over your WiFi network
instead, so a real console can play with
**[WideMelon](https://github.com/Dusknight4/widemelon/releases/tag/v1.0.4-mod.5)**
on a PC: a DS emulator with the matching multiplayer support. That is the only
reason this build exists; without WideMelon it's the same as nds-bootstrap
v2.17.0.

Both just connect to the same WiFi network. In Pokémon Platinum and HeartGold
(US) the Union Room, trades and battles work in both directions, whichever side
starts the talk, including Platinum with HeartGold. Other games are untested.
**This is an alpha.**

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
- A PC on the **same network** running
  [WideMelon 1.0.4-mod.5 or later](https://github.com/Dusknight4/widemelon/releases/tag/v1.0.4-mod.5).
  Networks that keep devices apart ("client isolation", common on guest and
  some public WiFi) won't work.

## Installing

1. Download `nds-bootstrap-M1.zip` from the
   [v2.17.0-M1 release](https://github.com/Dusknight4/nds-bootstrap/releases/tag/v2.17.0-M1).
2. On the SD card, back up `_nds/nds-bootstrap-nightly.nds` if you have it.
3. Copy the zip's `_nds` folder onto the SD card's root, replacing that file.
   It holds `nds-bootstrap-nightly.nds` (this build, in the *nightly* slot, so
   your official *release* nds-bootstrap stays as it is for everything else)
   and `nds-bootstrap-wifi.ini`, which turns Local as WiFi on.
4. In TWiLight Menu++, open the game's settings (press **Y** on the game) and
   set **Bootstrap** to **Nightly**. Do this for each game you'll play over
   WiFi. (Or set nightly as the default in TWiLight Menu++'s settings.)

The zip also has `nds-bootstrap-release.nds`, the same build under the
release name, in `release files/`, if you'd rather replace your release
nds-bootstrap instead (then games don't need the Nightly setting).

## Turning it on

1. On the console, Local as WiFi is already on (the zip's settings file).
   To switch, **hold X while the game starts** (from when you pick it in
   TWiLight Menu++ until a menu appears on the bottom screen), choose with
   Up/Down and press **A**: **Local as WiFi**, or **Regular Local** (the
   game's own wireless, with real DS systems nearby). The choice is kept for
   next time, in `_nds/nds-bootstrap-wifi.ini`.
2. In WideMelon, turn on **System > Multiplayer > Connect a real DS (WiFi
   bridge)** (it's remembered). WideMelon listens on **UDP port 7071**; when
   Windows asks, allow WideMelon through the firewall for the kind of network
   you're on.
3. Start the game on both and go into the Union Room (or the game's local
   wireless mode) as you would with two DS systems.

The console joins your WiFi as the game starts and finds WideMelon by itself.
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
- In WideMelon's folder, `WideMelon bridge log.txt` shows what WideMelon sees,
  including lines starting with `[DS]`: the console's own log (which network it
  joined, its IP address, when it found WideMelon). No `[DS]` lines at all
  usually means the firewall is blocking UDP port 7071, or the network keeps
  devices apart.
- The console finds WideMelon by a broadcast on the network; networks that
  block broadcasts between devices won't work yet.

## Known limits

- One console with one WideMelon.
- When WideMelon hosts the talk (the console started it) on a slow or busy
  network, WideMelon's frame rate drops while they're linked.
- Pokémon HeartGold/SoulSilver on WideMelon need WideMelon 1.0.4-mod.5 or
  later (an earlier WideMelon shows a blue error screen after cancelling a
  link trade or battle).

## How it works, and building it

See [nbwifi/README.md](nbwifi/README.md): a DSi WiFi driver, adapted from
devkitPro's calico, and a reimplementation of the ARM7 half of the DS wireless
library that carries the game's local wireless to WideMelon over UDP.
nds-bootstrap loads them as `nbwifi_blob.bin` when Local as WiFi is on, for DS
games in DSi mode.
