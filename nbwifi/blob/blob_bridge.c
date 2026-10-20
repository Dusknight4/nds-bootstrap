// nbwifi blob: the bridge to WideMelon, its protocol (DsBridge "DSB1") on its port: finding each
// other, and our log. The DS says Hello by broadcast until a WideMelon (listening on the bridge
// port) answers, then to it directly, once a second either way. WideMelon says hello back once a
// second, to wherever it last heard us from (so a DS that comes back with another address is
// found by its next hello). If WideMelon goes quiet for 15 s while we're online (it moved to
// another address, or it was closed), we forget it and say hello by broadcast again: the radio
// now leaves the network whenever the game's wireless is off, and much can change meanwhile.
#include <string.h>
#include "nbwifi.h"
#include "blob_bridge.h"

void nbwLogf(const char* fmt, ...);   // nbwifi: adds a line to its log
// blob_wm.c: the game's wireless over the bridge (wmhle)
void wmGlueBridgeRx(uint32_t srcIp, const uint8_t* data, unsigned len);
void wmGlueSetPeer(uint32_t ip);

enum { DSB_HELLO = 8, DSB_LOG = 9 };
#define DSB_HEAD_SIZE 40    // "DSB1", type, MAC, channel, AID, Seq, RefSeq, FC, duration, timestamp, header, body length

static uint32_t s_wmIp;             // the WideMelon answering, once one has
static uint32_t s_wmHellosIn, s_wmHellosOut;
static uint32_t s_onlineTicks;      // bridgeTick calls (frames online)
static uint32_t s_lastHelloIn;      // s_onlineTicks when WideMelon's hello last came (from s_wmIp)
static unsigned s_logSent;          // how far our log has gone out

void bridgeReset(void)
{
	s_wmIp = s_wmHellosIn = s_wmHellosOut = s_onlineTicks = s_lastHelloIn = 0;
	s_logSent = 0;
}

uint32_t bridgeWideMelonIp(void) { return s_wmIp; }

static void bridgeSendHello(uint32_t ip)
{
	uint8_t m[DSB_HEAD_SIZE + 4];
	memset(m, 0, sizeof(m));
	memcpy(m, "DSB1", 4);
	m[4] = DSB_HELLO;
	memcpy(m + 5, nbwifiMac(), 6);
	m[22] = 0xFF; m[23] = 0xFF;        // RefSeq: none
	m[38] = 4;                          // body: "DSHW", a real DS (WideMelon waits for our CMDs then)
	memcpy(m + DSB_HEAD_SIZE, "DSHW", 4);
	nbwifiUdpSend(ip, BRIDGE_PORT, BRIDGE_PORT, m, sizeof(m));
	s_wmHellosOut++;
}

void bridgeRx(uint32_t src_ip, const uint8_t* data, unsigned len)
{
	if (len < DSB_HEAD_SIZE || memcmp(data, "DSB1", 4) != 0) return;
	if (!s_wmIp) {
		s_wmIp = src_ip;
		s_lastHelloIn = s_onlineTicks;
		const uint8_t* ip = (const uint8_t*)&src_ip;
		nbwLogf("[BRIDGE] WideMelon found at %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
		wmGlueSetPeer(src_ip);     // where the game's wireless traffic goes
	}
	if (data[4] == DSB_HELLO) {
		s_wmHellosIn++;
		if (src_ip == s_wmIp) s_lastHelloIn = s_onlineTicks;
	}
	else wmGlueBridgeRx(src_ip, data, len);     // the game's wireless traffic
}

// our log goes to WideMelon, which adds it to its bridge log (the DS has nowhere to keep it)
static void bridgeSendLog(void)
{
	static uint8_t m[DSB_HEAD_SIZE + 480];
	const unsigned n = nbwifiLog((char*)m + DSB_HEAD_SIZE, 480, &s_logSent);
	if (!n) return;
	memset(m, 0, DSB_HEAD_SIZE);
	memcpy(m, "DSB1", 4);
	m[4] = DSB_LOG;
	memcpy(m + 5, nbwifiMac(), 6);
	m[22] = 0xFF; m[23] = 0xFF;
	m[38] = (uint8_t)n; m[39] = (uint8_t)(n >> 8);
	nbwifiUdpSend(s_wmIp, BRIDGE_PORT, BRIDGE_PORT, m, DSB_HEAD_SIZE + n);
}

void bridgeTick(uint32_t frames)
{
	s_onlineTicks++;
	if (s_wmIp && s_onlineTicks - s_lastHelloIn > BRIDGE_QUIET_FRAMES) {
		nbwLogf("[BRIDGE] no word from WideMelon for 15 s: looking for it again\n");
		s_wmIp = 0;    // (the game's traffic still goes to the old address until it answers)
	}
	if (s_wmIp && frames % 30 == 15) bridgeSendLog();
	if (frames % 60 != 0) return;
	bridgeSendHello(s_wmIp ? s_wmIp : (nbwifiIp() | ~nbwifiNetmask()));
}
