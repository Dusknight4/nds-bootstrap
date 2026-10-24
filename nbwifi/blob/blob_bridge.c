// nbwifi blob: the link to the others on the network, its protocol (DsBridge "DSB1") on its port.
// As on the air, where every console in range hears every other, everyone on the network playing
// local wireless over WiFi (DSes running this, and MultiMelons) is a peer, kept by wmpeers
// (../wmhle/wmpeers.c, shared with MultiMelon, so both find each other the same way); the game
// itself picks whom it talks to. Once a second while we're online we say hello, by broadcast
// (finding the ones that don't know us yet) and to each one we know (so a lost broadcast loses no
// one); one we've heard nothing from for 10 s of our time online is forgotten (the radio leaves
// the network whenever the game's wireless is off, and comes back saying hello). wmhle's messages
// go to the console they name, the rest (beacons) to everyone. Our log goes to the MultiMelons,
// which keep it in their bridge log (the DS has nowhere to keep it).
#include <string.h>
#include "nbwifi.h"
#include "blob_bridge.h"
#include "wmpeers.h"

void nbwLogf(const char* fmt, ...);   // nbwifi: adds a line to its log
// blob_wm.c: the game's wireless over the link (wmhle)
void wmGlueBridgeRx(uint32_t srcIp, const uint8_t* data, unsigned len);

static WmPeers s_peers;
static bool s_ready;                // s_peers set up (once online: the MAC is known)
static uint32_t s_onlineTicks;      // bridgeTick calls (frames online): the clock peers' silence counts on
static unsigned s_logSent;          // how far our log has gone out

static void peersLog(void* ctx, const char* text) { (void)ctx; nbwLogf("[BRIDGE] %s\n", text); }

static uint32_t onlineMs(void) { return s_onlineTicks * 50 / 3; }   // (frames at 60 a second)

static void ensureReady(void)
{
	if (s_ready) return;
	const uint8_t* mac = nbwifiMac();
	// our hellos' tag (our own broadcast, heard back, is skipped; the MAC is checked too)
	const uint8_t tag[4] = { mac[2], mac[3], mac[4], (uint8_t)(mac[5] ^ 0x5A) };
	s_peers.log = peersLog;
	s_peers.logCtx = 0;
	wmpeersInit(&s_peers, mac, 'D', tag);
	s_ready = true;
}

void bridgeReset(void)
{
	memset(&s_peers, 0, sizeof(s_peers));
	s_ready = false;
	s_onlineTicks = 0;
	s_logSent = 0;
}

// (an older WideMelon's hello says nothing: anyone not a DS is one)
static bool isPc(const WmPeer* p) { return p->used && p->kind != 'D'; }

uint32_t bridgeWideMelonIp(void)
{
	for (int i = 0; i < WMPEERS_MAX; i++) if (isPc(&s_peers.p[i])) return s_peers.p[i].ip;
	return 0;
}

int bridgePeerCount(void) { return s_ready ? wmpeersCount(&s_peers) : 0; }

void bridgeRx(uint32_t src_ip, uint16_t src_port, const uint8_t* data, unsigned len)
{
	ensureReady();
	if (wmpeersRx(&s_peers, src_ip, src_port, data, len, onlineMs()) < 0) return;
	if (wmpeersIsHello(data, len) || wmpeersIsLog(data, len)) return;    // (the link's own)
	wmGlueBridgeRx(src_ip, data, len);
}

void bridgeSend(const uint8_t* msg, unsigned len)
{
	ensureReady();
	int idx[WMPEERS_MAX];
	const int n = wmpeersRoute(&s_peers, msg, len, idx, WMPEERS_MAX);
	for (int k = 0; k < n; k++) nbwifiUdpSend(s_peers.p[idx[k]].ip, s_peers.p[idx[k]].port, BRIDGE_PORT, msg, len);
}

// our log, to each MultiMelon (only once there's one: until then it waits)
static void bridgeSendLog(void)
{
	if (!bridgeWideMelonIp()) return;
	static uint8_t m[40 + 480];
	const unsigned n = nbwifiLog((char*)m + 40, 480, &s_logSent);
	if (!n) return;
	memset(m, 0, 40);
	memcpy(m, "DSB1", 4);
	m[4] = 9;                           // Log
	memcpy(m + 5, nbwifiMac(), 6);
	m[16] = m[17] = 0xFF;               // RefSeq: none
	m[38] = (uint8_t)n; m[39] = (uint8_t)(n >> 8);
	for (int i = 0; i < WMPEERS_MAX; i++)
		if (isPc(&s_peers.p[i])) nbwifiUdpSend(s_peers.p[i].ip, s_peers.p[i].port, BRIDGE_PORT, m, 40 + n);
}

void bridgeTick(uint32_t frames)
{
	ensureReady();
	s_onlineTicks++;
	if (frames % 30 == 15) bridgeSendLog();
	if (frames % 60 != 0) return;
	wmpeersExpire(&s_peers, onlineMs());
	uint8_t hello[WMPEERS_HELLO_LEN];
	wmpeersHello(&s_peers, hello);
	nbwifiUdpSend(nbwifiIp() | ~nbwifiNetmask(), BRIDGE_PORT, BRIDGE_PORT, hello, sizeof(hello));
	for (int i = 0; i < WMPEERS_MAX; i++)
		if (s_peers.p[i].used) nbwifiUdpSend(s_peers.p[i].ip, s_peers.p[i].port, BRIDGE_PORT, hello, sizeof(hello));
}
