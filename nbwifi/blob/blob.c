// nbwifi blob: the WiFi driver as one self-contained binary, at a fixed place
// in main RAM (NBW_BLOB_LOCATION), for nds-bootstrap. Its loader calls start()
// before the game and polls until it's online; then, in game, the ARM7 card
// engine calls frame() once a frame from its VBlank handler.
//
// In game: the game's local wireless goes over the network to WideMelon (blob_wm.c, wmhle),
// the radio is on the network only while the game uses its wireless (blob_radio.c), and
// WideMelon is found by hellos (blob_bridge.c).
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "nbwifi.h"
#include "blob.h"
#include "blob_radio.h"
#include "blob_bridge.h"

void nbwLogf(const char* fmt, ...);   // nbwifi: adds a line to its log
// blob_wm.c: the game's wireless over the bridge (wmhle)
void wmGlueInit(void);
void wmGlueFrame(void);

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef volatile uint16_t vu16;

#define REG_SPICNT  (*(vu16*)0x040001C0)
#define REG_SPIDATA (*(vu16*)0x040001C2)
#define REG_IME     (*(volatile uint32_t*)0x04000208)

static bool blobPrepare(void);
static int blobState(void);
static void blobPoll(void);
void blobFrame(void);           // frame_stack.s: blobFrameImpl on a stack of its own
void blobFrameImpl(void);
u8 s_frameStack[0x1000] __attribute__((aligned(8)));

__attribute__((section(".header"), used)) NbwBlob g_nbwBlob = {
	.magic = NBW_BLOB_MAGIC,
	.version = NBW_BLOB_VERSION,
	.prepare = blobPrepare,
	.state = blobState,
	.poll = blobPoll,
	.frame = blobFrame,
};

extern u8 __bss_start[], __bss_end[], __blob_end[];

// ---- the console's saved WiFi, read from the firmware over SPI ----

static void spiWait(void) { while (REG_SPICNT & 0x80); }

static u8 spiByte(u8 out, bool last)
{
	// firmware device, 4 MHz; chip select held until the last byte
	REG_SPICNT = 0x8100 | (last ? 0 : 0x0800);
	REG_SPIDATA = out;
	spiWait();
	return (u8)REG_SPIDATA;
}

static void readFirmware(u32 addr, u8* out, u32 len)
{
	const u32 ime = REG_IME;
	REG_IME = 0;
	spiWait();
	spiByte(0x03, false);       // READ
	spiByte((u8)(addr >> 16), false);
	spiByte((u8)(addr >> 8), false);
	spiByte((u8)addr, len == 0);
	for (u32 i = 0; i < len; i++) out[i] = spiByte(0, i == len - 1);
	REG_SPICNT = 0;
	REG_IME = ime;
}

// slots 1-3 (DS games: open/WEP) and 4-6 (DSi: WPA, with the key worked out);
// on a 3DS, the 3DS's own connections show up in 4-6
static unsigned readSavedNetworks(NbwNetwork* list)
{
	unsigned count = 0;
	u16 userOff16;
	readFirmware(0x20, (u8*)&userOff16, 2);
	const u32 user = (u32)userOff16 << 3;
	static u8 ap[512];
	for (int slot = 1; slot <= 6 && count < NBW_MAX_NETWORKS; slot++) {
		const bool ext = slot > 3;
		readFirmware(ext ? user - 0xA00 + (slot - 4) * 512 : user - 0x400 + (slot - 1) * 256, ap, ext ? 512 : 256);
		const u8 status = ap[0xE7], ssidLen = ap[0xE8], wep = ap[0xE6];
		if (status == 0xFF || ssidLen == 0 || ssidLen > 32) continue;
		bool psk = false;
		if (ext) for (int i = 0; i < 32; i++) if (ap[0x100 + i]) psk = true;
		const bool open = wep == 0 && (!ext || ap[0x181] == 0);
		if (!psk && !open) continue;    // WEP, or WPA without its key: not supported
		NbwNetwork* n = &list[count++];
		memset(n, 0, sizeof(*n));
		memcpy(n->ssid, ap + 0x40, ssidLen);
		n->ssidLen = ssidLen;
		n->hasPmk = psk;
		if (psk) memcpy(n->pmk, ap + 0x100, 32);
	}
	return count;
}

// (The wmping heartbeat that used to go out every frame, an early test of the link's
// timing, is gone: it kept the radio sending all the time, for nothing.)

static u32 s_frames;        // counted by frame(): no hardware timer is taken from the game
static u8 s_netMem[0x6000] __attribute__((aligned(32)));

// ---- the bridge to WideMelon: blob_bridge.c (finding it, hello and log messages) ----
static void onUdp(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, const void* data, unsigned len)
{
	(void)src_port;
	if (dst_port == BRIDGE_PORT) bridgeRx(src_ip, (const u8*)data, len);
}

// ---- the radio only while the game uses its wireless (blob_radio.c) ----

bool wmhleActive(void);         // wmhle.c: the game uses its wireless (WM Initialize ... End)
#define REG_KEYXY_ARM7      (*(vu16*)0x04000136)   // bit 7: the hinge (lid) closed

static NbwNetwork s_nets[NBW_MAX_NETWORKS];    // the console's saved networks (blobPrepare)
static unsigned s_netCount;

// ---- the entry points ----


// in the loader, before the game: the saved networks (the firmware is read
// over SPI, which the game uses itself once it runs), the DSi's WiFi brought
// up, and joining whichever saved network is in range started; the loader
// polls until joined (the address can come in game).
//
// (Shelved for now: waiting for the game to turn its wireless on before
// joining. Switching the radio under a running game hung Pokémon at Continue,
// and with the radio switched before the game, the game's own Union Room
// connection fails until the bridge carries it.)
static bool blobPrepare(void)
{
	memset(__bss_start, 0, __bss_end - __bss_start);
	if (nbwNetbufMemSize() > sizeof(s_netMem)) return false;
	g_nbwBlob.size = (u32)(__blob_end - (u8*)&g_nbwBlob);
	s_netCount = readSavedNetworks(s_nets);
	g_nbwBlob.networks = s_netCount;
	if (!s_netCount) return false;
	if (!nbwifiInit(s_netMem)) {
		*(vu16*)0x04004C04 |= 0x0100;   // back to the DS-compatible radio, for the game's own
		return false;
	}
	nbwifiSetUdpHandler(onUdp);
	wmGlueInit();
	nbwLogf("[NBW] loader: WiFi up, %u saved network(s)\n", s_netCount);
	return nbwifiConnectAny(s_nets, s_netCount);
}

static int blobState(void) { return (int)nbwifiState(); }

static void blobPoll(void)
{
	nbwifiPoll();
}

// (frameEarly, frameLate: below)
extern volatile bool g_nbwBusy;     // blob_wm.c: our VCount handler stays out while this runs
void blobTimeSpent(u16 v0, u16 v1, int vblank);   // blob_wm.c: how long we held the ARM7 (for the log): 1 VBlank run, 0 timer call, 2 the VBlank network poll
bool blobTimerActive(void);                        // blob_wm.c: our mid-frame timer is running
void blobProf(int section, u16 v0);                // blob_wm.c: time by section, for the log

// The frame's work is in two halves. VBlank only counts the frame; the rest (the wireless
// glue, the bridge's log and ping) waits for our mid-frame timer once the picture is being
// drawn (lines 24-175), while that timer runs. VBlank is when the game's ARM9 copies its
// sprites and palettes from main RAM, and the ARM7 (our code, in main RAM, with the bus
// priority) held it up: the copies ran late into the picture (lines at the top of the
// screen on hardware; the VBlank run took up to 173 lines, VBlank has 71)
static void frameEarly(void);
static void frameLate(void);
static volatile bool s_lateDue;     // this frame's late half still to run

void blobFrameImpl(void)
{
	// (if VBlank came in the middle of our VCount handler, the driver is mid-way:
	// not this frame)
	if (g_nbwBusy) { s_frames++; return; }
	g_nbwBusy = true;
	const u16 v0 = *(vu16*)0x04000006;     // VCOUNT
	if (s_lateDue) frameLate();            // (the last frame's never got its turn: now, late as it is)
	frameEarly();
	if (blobTimerActive() && nbwifiState() == NbwState_Online) s_lateDue = true;
	else frameLate();
	blobTimeSpent(v0, *(vu16*)0x04000006, 1);
	g_nbwBusy = false;
}

// (blob_wm.c's timer handler, with g_nbwBusy set: the frame's late half, if due and the
// picture is being drawn)
void blobFrameLateFromTimer(u16 vcount)
{
	if (s_lateDue && vcount >= 24 && vcount < 176) frameLate();
}

static void frameEarly(void)
{
	// called once a frame (VBlank), through blobFrame
	s_frames++;
	g_nbwBlob.frames = s_frames;
	u16 pv = *(vu16*)0x04000006;
	nbwifiFrameTick();
	blobProf(0, pv);
	// (with our mid-frame timer polling every ~2 ms, the network isn't polled here: the
	// VBlank run held the ARM7 longer than VBlank itself, holding up the game's graphics)
	if (!blobTimerActive() && blobRadioActive()) {     // (resting: the device isn't polled at all)
		g_nbwPollRounds = 16;     // (once a frame: all of it)
		const u16 p0 = *(vu16*)0x04000006;
		nbwifiPoll();
		blobTimeSpent(p0, *(vu16*)0x04000006, 2);
	}
}

static void frameLate(void)
{
	s_lateDue = false;
	// the game's wireless requests are answered whether or not the WiFi is up
	// (with no WiFi, its scans just find nobody), so it never sees an error
	u16 pv = *(vu16*)0x04000006;
	wmGlueFrame();
	blobProf(1, pv);
	radioTick(s_frames, wmhleActive(), (REG_KEYXY_ARM7 & 0x80) != 0, s_nets, s_netCount);   // (blob_radio.c)
	if (nbwifiState() != NbwState_Online) return;

	pv = *(vu16*)0x04000006;
	bridgeTick(s_frames);   // (blob_bridge.c)
	blobProf(2, pv);
}
