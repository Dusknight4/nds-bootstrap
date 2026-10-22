# R4 save copy: one save, on the 3DS and on an R4

From **v2.17.3**, when a DS game runs from a 3DS's (or DSi's) SD card with an **R4 flashcard in the
game card slot** instead of the game's own card, its save is also kept on the R4's microSD. You can
play the same game on the 3DS and on a DS Lite with the R4 (or sync it with
[WideMelon](https://github.com/Dusknight4/widemelon)) and always continue from your latest save.

## What you need

- A 3DS with custom firmware (or a DSi) running DS games from the SD card with **TWiLight Menu++ in
  DSi mode**, with this nds-bootstrap.
- A **DSTT-type R4** (an R4 SDHC running YSMenu) in the game card slot.
- The same game on the R4, **with the same file name**: the R4's save is
  `roms/nds/saves/<the ROM's name>.sav` on its microSD (`pokeheartgold.us.nds` on the 3DS uses
  `roms/nds/saves/pokeheartgold.us.sav` on the R4). If you rename the ROM on one, rename it on the other.

Nothing to turn on: it happens by itself whenever the R4 is in the slot. Without the R4 (or with the
game's own card in the slot) the game runs as normal.

## What happens

1. As the game starts, nds-bootstrap starts the R4 and opens its microSD.
2. It brings the two saves level: the copy that changed since the last launch wins; if both changed,
   the newer file wins. The copy that gets replaced is kept in `_nds/nds-bootstrap/r4save/backup` on
   the 3DS's SD card. If the R4 has no save for the game yet, it gets a copy of the 3DS's.
3. While you play, every save the game makes is written to both.

What it did at the last launch is in `_nds/nds-bootstrap/r4save.log` on the SD card.

**Tested:** Pokémon HeartGold (US) on a 3DS with an R4 SDHC (YSMenu): the save went to the 3DS's SD
card and to the R4 at once, byte for byte the same, and a save changed elsewhere (synced to the R4 by
WideMelon) was picked up on the next start. The R4's other files are left alone.
