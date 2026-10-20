// nbwifi blob: the interface between nds-bootstrap and the WiFi driver it
// loads at NBW_BLOB_LOCATION (see blob.c). Shared by both sides.
#ifndef NBW_BLOB_H
#define NBW_BLOB_H

#include <stdbool.h>
#include <stdint.h>

// main RAM the blob runs in: free while a DS (not DSi) game runs, kept out of
// nds-bootstrap's ROM cache in bridge mode (0x0CF40000-0x0CF5FFFF there)
#define NBW_BLOB_LOCATION 0x02F40000
#define NBW_BLOB_SIZE     0x20000
#define NBW_BLOB_MAGIC    0x4257424E    // "NBWB"
#define NBW_BLOB_VERSION  2

// nbwifiState() values, as start/state report them
enum { NbwBlobOff, NbwBlobReady, NbwBlobScanning, NbwBlobConnecting, NbwBlobGettingIp, NbwBlobOnline, NbwBlobFailed };

typedef struct NbwBlob {
	uint32_t magic;
	uint32_t version;
	bool (*prepare)(void);  // loader: saved networks read, WiFi up, joining started; false: no WiFi
	int (*state)(void);     // NbwBlob* state
	void (*poll)(void);     // while connecting, as often as possible (at least once a frame)
	void (*frame)(void);    // in game: once a frame, from VBlank: keeps the WiFi going

	uint32_t size;          // bytes in use from NBW_BLOB_LOCATION (set by prepare)
	uint32_t networks;      // saved networks it can use (set by prepare)
	uint32_t frames;        // frames seen in game
	uint32_t irqTable;      // where the loader found the game's ARM7 IRQ table: in the ARM7
	                        // binary's load copy, NOT where it runs (the game copies it into
	                        // ARM7 RAM and reuses this RAM); the blob finds the live one by:
	uint32_t irqVblank;     // what the loader put in its VBlank entry (nds-bootstrap's handler)
	uint32_t irqIpcSync;    // and in its IPC sync entry (16)
} NbwBlob;

#endif
