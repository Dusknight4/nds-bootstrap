// r4save: the R4 save copy. Whenever a DSTT-type flashcard (an R4 SDHC with YSMenu) is in
// slot-1 instead of the game's own card, a game's save is also kept on the R4's microSD, where
// TWiLight on the R4 keeps it: roms/nds/saves/<ROM name>.sav. (The save relocation setting only
// matters for the game's own card, as before.)
//
// Before the game starts, r4SavePrepare starts the card (KEY1/KEY2, then 66h for its SD
// commands), sets up its microSD, mounts it as r4: and brings the two saves level: the copy
// that changed since the last launch wins (by the CRC32 kept in sd:/_nds/nds-bootstrap/r4save),
// the newer file when both changed; the copy replaced is kept in .../r4save/backup. It then
// gives the loader the R4 file's first cluster (conf->saveMirrorCluster), and the card engine
// writes every save to both copies through nitro:/dstt_sd.dldi. Notes: sd:/_nds/nds-bootstrap/r4save.log
#pragma once
#include "configuration.h"

void r4SavePrepare(configuration* conf);
