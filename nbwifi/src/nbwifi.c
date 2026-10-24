// nbwifi: connection management and a minimal IP layer (ARP, IPv4, UDP, DHCP,
// ping replies) over calico's Atheros driver. The connection code follows
// calico's source/nds/arm7/wifi.twl.32.c (SPDX-License-Identifier: ZPL-2.1,
// Copyright fincs, devkitPro), reworked into a state machine run by
// nbwifiPoll() instead of threads.
#include <string.h>
#include <calico/types.h>
#include <calico/dev/tmio.h>
#include <calico/dev/sdio.h>
#include <calico/dev/ar6k.h>
#include <calico/dev/wpa.h>
#include "nbw_internal.h"
#include "nbwifi.h"

#define dietPrint nbwLogf

void nbwNonceSeed(const void* data, size_t len);  // nbw_crypto.c
void nbwNetbufInit(void* mem);                    // nbw_runtime.c
void wpaEapolProcess(WpaState* st, NetBuf* pPacket);

#define REG_GPIO_WL   (*(vu16*)0x04004C04)  // bit 8: Mitsumi (DS-compatible) instead of Atheros
#define REG_SCFG_EXT  (*(vu32*)0x04004008)
#define TMIO1_BASE    0x04004A00
#define TMIO1_FIFO    0x04004B0C

// timeouts count video frames (from the display's line counter), so they hold
// however often nbwifiPoll is called, as long as it's at least once a frame
#define FRAMES_PER_SEC 60

static TmioCtl s_sdioCtl;
static u32 s_sdioCtlSlots[2];
static SdioCard s_sdioCard;
static Ar6kDev s_ar6kDev;
alignas(4) static u8 s_ar6kWorkBuf[AR6K_WORK_BUF_SIZE];
static WpaState s_wpaState;
static u8 s_wpaIeBuf[sizeof(WlanIeHdr) + 0xFF];

static NbwState s_state = NbwState_Off;
static u32 s_polls, s_frames, s_stateSince;
static u16 s_lastVcount;
static NbwUdpHandler s_udpHandler;

// the network being joined
static NbwNetwork s_nets[NBW_MAX_NETWORKS];   // the networks it may join
static unsigned s_netCount;
static unsigned s_netTried;                     // bit per network: tried and failed
static bool s_secondRound;                      // every saved network tried once: each gets one more go
static char s_seenNames[6][33];  // a few other networks heard, for the log
static unsigned s_seenCount;
static char s_ssid[WLAN_MAX_SSID_LEN];          // the one being joined
static unsigned s_ssidLen;
static u8 s_pmk[WLAN_WPA_PSK_LEN];
static bool s_hasPmk;
static unsigned s_scanTries;
static WlanBssDesc s_bssTable[WLAN_MAX_BSS_ENTRIES];   // wlanFindOrAddBss fills up to this many
static unsigned s_bssCount;
static bool s_heardSaved;           // a scan heard a saved network since nbwifiConnectAny
static unsigned s_bssEvents;    // scan results heard this scan, kept or not
static bool s_scanning;
static bool s_wpaWait;

// addresses (network byte order)
static u32 s_ip, s_netmask, s_gateway;

static void _setState(NbwState st)
{
	if (s_state != st) {
		static const char* names[] = { "off", "ready", "scanning", "connecting", "getting IP", "online", "failed", "disconnecting" };
		dietPrint("[NBW] %s\n", names[st]);
	}
	s_state = st;
	s_stateSince = s_frames;
}

// ---- byte order ----

static u16 _be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }
static u32 _rd32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }   // keeps network order
static void _put16(u8* p, u16 v) { p[0] = v >> 8; p[1] = v; }
static void _wr32(u8* p, u32 v) { memcpy(p, &v, 4); }
static u16 _bswap16(u16 v) { return (u16)(v << 8 | v >> 8); }

static const u8 s_macBroadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

// ---- Ethernet frames out ----

// headroom the driver needs in front of an Ethernet frame (see ar6kWmiTx)
#define TX_HEADROOM (sizeof(Ar6kHtcFrameHdr) + sizeof(Ar6kWmiDataHdr) + sizeof(NetLlcSnapHdr))

// a frame to fill in: returns where the payload goes, or NULL
static u8* _ethBegin(NetBuf** out, const u8* dst_mac, u16 ethertype, unsigned payload_len)
{
	NetBuf* nb = netbufAlloc(TX_HEADROOM, sizeof(NetMacHdr) + payload_len, NetBufPool_Tx);
	if (!nb) {
		dietPrint("[NBW] no TX buffer\n");
		return NULL;
	}
	NetMacHdr* mac = (NetMacHdr*)netbufGet(nb);
	memcpy(mac->dst_mac, dst_mac, 6);
	memcpy(mac->src_mac, s_ar6kDev.macaddr, 6);
	mac->len_or_ethertype_be = _bswap16(ethertype);
	*out = nb;
	return (u8*)(mac + 1);
}

static bool _ethSend(NetBuf* nb)
{
	bool ok = ar6kWmiTx(&s_ar6kDev, nb);
	netbufFree(nb);
	return ok;
}

// ---- ARP: a few known addresses ----

// (8: the router, and up to 7 other consoles playing local wireless with us over WiFi, which
// send many times a second and so stay; with 4, one found by another's packets crowded a
// partner out, and our next packets to it went while it was asked for again)
#define ARP_SLOTS 8
typedef struct { u32 ip; u8 mac[6]; u16 age; } ArpEntry;
static ArpEntry s_arp[ARP_SLOTS];

static void _arpLearn(u32 ip, const u8* mac)
{
	if (!ip || ip == 0xffffffff) return;
	ArpEntry* slot = &s_arp[0];
	for (unsigned i = 0; i < ARP_SLOTS; i++) {
		if (s_arp[i].ip == ip) { slot = &s_arp[i]; break; }
		if (s_arp[i].age > slot->age) slot = &s_arp[i];
	}
	slot->ip = ip;
	memcpy(slot->mac, mac, 6);
	slot->age = 0;
	for (unsigned i = 0; i < ARP_SLOTS; i++) if (&s_arp[i] != slot && s_arp[i].age < 0xffff) s_arp[i].age++;
}

static const u8* _arpFind(u32 ip)
{
	for (unsigned i = 0; i < ARP_SLOTS; i++) if (s_arp[i].ip == ip) return s_arp[i].mac;
	return NULL;
}

static void _arpSend(u16 oper, const u8* dst_mac, const u8* target_mac, u32 target_ip)
{
	NetBuf* nb;
	u8* p = _ethBegin(&nb, dst_mac, NetEtherType_ARP, 28);
	if (!p) return;
	_put16(p + 0, 1);           // Ethernet
	_put16(p + 2, 0x0800);      // IPv4
	p[4] = 6; p[5] = 4;
	_put16(p + 6, oper);
	memcpy(p + 8, s_ar6kDev.macaddr, 6);
	_wr32(p + 14, s_ip);
	memcpy(p + 18, target_mac, 6);
	_wr32(p + 24, target_ip);
	_ethSend(nb);
}

static void _arpRx(const u8* p, unsigned len)
{
	if (len < 28 || _be16(p) != 1 || _be16(p + 2) != 0x0800) return;
	const u32 sender_ip = _rd32(p + 14), target_ip = _rd32(p + 24);
	_arpLearn(sender_ip, p + 8);
	if (_be16(p + 6) == 1 && s_ip && target_ip == s_ip) {
		_arpSend(2, p + 8, p + 8, sender_ip);
	}
}

// ---- IPv4 ----

static u16 _ipChecksum(const u8* p, unsigned len)
{
	u32 sum = 0;
	for (unsigned i = 0; i + 1 < len; i += 2) sum += _be16(p + i);
	if (len & 1) sum += p[len - 1] << 8;
	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
	return (u16)~sum;
}

static bool _isBroadcast(u32 ip)
{
	return ip == 0xffffffff || (s_netmask && (ip | s_netmask) == 0xffffffff && ((ip ^ s_ip) & s_netmask) == 0);
}

// an IPv4 packet: returns where its payload goes
static u8* _ipBegin(NetBuf** out, const u8* dst_mac, u32 src_ip, u32 dst_ip, u8 proto, unsigned payload_len)
{
	static u16 s_ipId;
	u8* ip = _ethBegin(out, dst_mac, NetEtherType_IPv4, 20 + payload_len);
	if (!ip) return NULL;
	ip[0] = 0x45; ip[1] = 0;
	_put16(ip + 2, 20 + payload_len);
	_put16(ip + 4, ++s_ipId);
	_put16(ip + 6, 0x4000);     // don't fragment
	ip[8] = 64; ip[9] = proto;
	_put16(ip + 10, 0);
	_wr32(ip + 12, src_ip);
	_wr32(ip + 16, dst_ip);
	_put16(ip + 10, _ipChecksum(ip, 20));
	return ip + 20;
}

static bool _udpSendTo(const u8* dst_mac, u32 src_ip, u32 dst_ip, u16 src_port, u16 dst_port, const void* data, unsigned len)
{
	NetBuf* nb;
	u8* udp = _ipBegin(&nb, dst_mac, src_ip, dst_ip, 17, 8 + len);
	if (!udp) return false;
	_put16(udp + 0, src_port);
	_put16(udp + 2, dst_port);
	_put16(udp + 4, 8 + len);
	_put16(udp + 6, 0);     // no checksum (allowed over IPv4)
	memcpy(udp + 8, data, len);
	return _ethSend(nb);
}

static bool udpSend_(u32 dst_ip, u16 dst_port, u16 src_port, const void* data, unsigned len);

NbwProf g_nbwProf;
int g_nbwPollRounds = 16;

// (timed, for the blob's log)
bool nbwifiUdpSend(u32 dst_ip, u16 dst_port, u16 src_port, const void* data, unsigned len)
{
	const unsigned short v0 = *(volatile unsigned short*)0x04000006;
	const bool ok = udpSend_(dst_ip, dst_port, src_port, data, len);
	const unsigned l = nbwLinesSince(v0);
	g_nbwProf.sendN++; g_nbwProf.sendSum += l; if (l > g_nbwProf.sendMax) g_nbwProf.sendMax = l;
	return ok;
}

static bool udpSend_(u32 dst_ip, u16 dst_port, u16 src_port, const void* data, unsigned len)
{
	if (s_state != NbwState_Online || len > 1400) return false;
	if (_isBroadcast(dst_ip)) {
		return _udpSendTo(s_macBroadcast, s_ip, dst_ip, src_port, dst_port, data, len);
	}
	// on this network: straight to it; otherwise through the gateway
	u32 hop = ((dst_ip ^ s_ip) & s_netmask) == 0 ? dst_ip : s_gateway;
	const u8* mac = _arpFind(hop);
	if (!mac) {
		// ask, and let the caller try again (it sends every frame anyway)
		static const u8 s_zero[6] = { 0 };
		_arpSend(1, s_macBroadcast, s_zero, hop);
		return false;
	}
	return _udpSendTo(mac, s_ip, dst_ip, src_port, dst_port, data, len);
}

// ---- DHCP ----

static u32 s_dhcpXid, s_dhcpOffer, s_dhcpServer;
static unsigned s_dhcpTries;
static u32 s_dhcpLastSend;
static bool s_dhcpSendNow;      // send from _dhcpTick: callbacks can't send (see s_pending)
static bool s_dhcpRequesting;

static void _dhcpSend(void)
{
	u8 msg[300];
	memset(msg, 0, sizeof(msg));
	msg[0] = 1; msg[1] = 1; msg[2] = 6;     // request, Ethernet
	memcpy(msg + 4, &s_dhcpXid, 4);
	_put16(msg + 10, 0x8000);               // answer by broadcast (we have no address yet)
	memcpy(msg + 28, s_ar6kDev.macaddr, 6);
	msg[236] = 99; msg[237] = 130; msg[238] = 83; msg[239] = 99;    // magic cookie
	unsigned o = 240;
	msg[o++] = 53; msg[o++] = 1; msg[o++] = s_dhcpRequesting ? 3 : 1;  // REQUEST / DISCOVER
	if (s_dhcpRequesting) {
		msg[o++] = 50; msg[o++] = 4; _wr32(msg + o, s_dhcpOffer); o += 4;
		msg[o++] = 54; msg[o++] = 4; _wr32(msg + o, s_dhcpServer); o += 4;
	}
	msg[o++] = 55; msg[o++] = 3; msg[o++] = 1; msg[o++] = 3; msg[o++] = 28;  // mask, router, broadcast
	msg[o++] = 12; msg[o++] = 6; memcpy(msg + o, "nbwifi", 6); o += 6;      // host name
	msg[o++] = 255;
	_udpSendTo(s_macBroadcast, 0, 0xffffffff, 68, 67, msg, o < 300 ? 300 : o);
	s_dhcpLastSend = s_frames;
	s_dhcpTries++;
}

static void _dhcpStart(void)
{
	nbwNonceSeed(&s_polls, sizeof(s_polls));
	u8 rnd[WPA_EAPOL_NONCE_LEN];
	wpaGenerateEapolNonce(rnd);
	memcpy(&s_dhcpXid, rnd, 4);
	s_dhcpRequesting = false;
	s_dhcpTries = 0;
	s_ip = 0;
	_setState(NbwState_GettingIp);
	s_dhcpSendNow = true;
}

static void _dhcpRx(const u8* msg, unsigned len)
{
	// (only while asking: a late answer after nbwifiDisconnect mustn't bring the link back)
	if (s_state != NbwState_GettingIp) return;
	if (len < 240 || msg[0] != 2 || memcmp(msg + 4, &s_dhcpXid, 4) != 0) return;
	if (msg[236] != 99 || msg[237] != 130 || msg[238] != 83 || msg[239] != 99) return;
	u8 type = 0;
	u32 mask = 0, router = 0, server = 0;
	for (unsigned o = 240; o < len && msg[o] != 255; ) {
		u8 opt = msg[o];
		if (opt == 0) { o++; continue; }
		if (o + 1 >= len) break;
		u8 olen = msg[o + 1];
		const u8* v = msg + o + 2;
		if (o + 2 + olen > len) break;
		if (opt == 53 && olen >= 1) type = v[0];
		if (opt == 1 && olen >= 4) mask = _rd32(v);
		if (opt == 3 && olen >= 4) router = _rd32(v);
		if (opt == 54 && olen >= 4) server = _rd32(v);
		o += 2 + olen;
	}
	if (type == 2 && !s_dhcpRequesting) {               // OFFER
		s_dhcpOffer = _rd32(msg + 16);
		s_dhcpServer = server;
		s_dhcpRequesting = true;
		s_dhcpTries = 0;
		s_dhcpSendNow = true;
	} else if (type == 5 && s_dhcpRequesting) {         // ACK
		s_ip = _rd32(msg + 16);
		s_netmask = mask ? mask : 0x00ffffff;
		s_gateway = router;
		const u8* ip = (const u8*)&s_ip;
		dietPrint("[NBW] IP %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
		_setState(NbwState_Online);
	} else if (type == 6) {                             // NAK
		dietPrint("[NBW] DHCP NAK\n");
		_dhcpStart();
	}
}

static void _dhcpTick(void)
{
	// sent at once when due, then again every half second until answered
	if (!s_dhcpSendNow && s_frames - s_dhcpLastSend < FRAMES_PER_SEC / 2) return;
	s_dhcpSendNow = false;
	if (s_dhcpTries >= 20) {
		dietPrint("[NBW] no DHCP answer\n");
		_setState(NbwState_Failed);
		return;
	}
	_dhcpSend();
}

// ---- incoming IP ----

static void _icmpRx(const u8* eth_src, u32 src_ip, const u8* icmp, unsigned len)
{
	// answers pings, for checking the connection from a PC
	if (len < 8 || icmp[0] != 8) return;
	NetBuf* nb;
	u8* out = _ipBegin(&nb, eth_src, s_ip, src_ip, 1, len);
	if (!out) return;
	memcpy(out, icmp, len);
	out[0] = 0;
	_put16(out + 2, 0);
	_put16(out + 2, _ipChecksum(out, len));
	_ethSend(nb);
}

static void _ipRx(const u8* eth_src, const u8* ip, unsigned len)
{
	if (len < 20 || (ip[0] >> 4) != 4) return;
	unsigned ihl = (ip[0] & 15) * 4;
	unsigned total = _be16(ip + 2);
	if (ihl < 20 || total < ihl || total > len) return;
	if (_be16(ip + 6) & 0x3fff) return;     // a fragment: not handled
	const u32 src = _rd32(ip + 12), dst = _rd32(ip + 16);
	const bool forUs = (s_ip && dst == s_ip) || _isBroadcast(dst);
	const u8* payload = ip + ihl;
	unsigned plen = total - ihl;

	if (ip[9] == 17 && plen >= 8) {
		const u16 sport = _be16(payload), dport = _be16(payload + 2);
		unsigned ulen = _be16(payload + 4);
		if (ulen < 8 || ulen > plen) return;
		if (dport == 68 && s_state == NbwState_GettingIp) {
			_dhcpRx(payload + 8, ulen - 8);
			return;
		}
		if (!forUs || s_state != NbwState_Online) return;
		// the sender's address, so answers go straight back
		if (((src ^ s_ip) & s_netmask) == 0) _arpLearn(src, eth_src);
		if (s_udpHandler) s_udpHandler(src, sport, dport, payload + 8, ulen - 8);
	} else if (ip[9] == 1 && forUs && s_state == NbwState_Online && dst == s_ip) {
		_icmpRx(eth_src, src, payload, plen);
	}
}

// ---- the device's callbacks ----

static bool _isSaved(const WlanBssDesc* d)
{
	for (unsigned k = 0; k < s_netCount; k++)
		if (d->ssid_len == s_nets[k].ssidLen && memcmp(d->ssid, s_nets[k].ssid, d->ssid_len) == 0) return true;
	return false;
}

static void _onBssInfo(Ar6kDev* dev, Ar6kWmiBssInfoHdr* bssInfo, NetBuf* pPacket)
{
	s_bssEvents++;
	if (!s_scanning) return;
	unsigned rssi = bssInfo->snr >= 0 ? bssInfo->snr : 0;
	WlanBssDesc beacon;
	memset(&beacon, 0, sizeof(beacon));
	wlanParseBeacon(&beacon, NULL, pPacket);
	memcpy(beacon.bssid, bssInfo->bssid, 6);
	beacon.rssi = rssi;
	beacon.channel = wlanFreqToChannel(bssInfo->channel_mhz);

	// the table only holds our own networks, so a busy area can't crowd them out
	if (!_isSaved(&beacon)) {
		if (s_seenCount < 6 && beacon.ssid_len && beacon.ssid_len <= 32) {
			char name[33];
			for (unsigned k = 0; k < beacon.ssid_len; k++) name[k] = (beacon.ssid[k] >= 32 && beacon.ssid[k] < 127) ? beacon.ssid[k] : '?';
			name[beacon.ssid_len] = 0;
			bool dup = false;
			for (unsigned k = 0; k < s_seenCount; k++) if (strcmp(s_seenNames[k], name) == 0) dup = true;
			if (!dup) strcpy(s_seenNames[s_seenCount++], name);
		}
		return;
	}
	WlanBssDesc* desc = wlanFindOrAddBss(s_bssTable, &s_bssCount, bssInfo->bssid, rssi);
	if (desc) *desc = beacon;
}

static bool _associate(WlanBssDesc* bss);
static bool _tryAnother(void);
static int s_netIndex = -1;     // which of s_nets is being joined

// The device's callbacks run while its packets are being handled, when no
// command can wait for an answer or for send credits (see ar6kDevPoll). So
// they only note what to do next, and nbwifiPoll does it afterwards.
typedef enum { Pend_None, Pend_Join, Pend_Next, Pend_Rescan } NbwPending;
static NbwPending s_pending;
static WlanBssDesc s_pendingBss;
static NetBuf* s_pendingEapol;      // a WPA handshake message to answer

static bool _startScan(void)
{
	// one network: ask for it by name (finds hidden ones too); several: listen
	// on every channel for whatever is around, as calico does
	const bool active = s_netCount == 1;
	Ar6kWmiProbedSsid probed_ssid = { 0 };
	if (active) {
		probed_ssid.mode = Ar6kWmiSsidProbeMode_Specific;
		probed_ssid.ssid_len = s_nets[0].ssidLen;
		memcpy(probed_ssid.ssid, s_nets[0].ssid, s_nets[0].ssidLen);
	}
	if (!ar6kWmiSetProbedSsid(&s_ar6kDev, &probed_ssid)) return false;
	if (!ar6kWmiSetChannelParams(&s_ar6kDev, 0, s_ar6kDev.wmi_channel_mask)) return false;

	Ar6kWmiScanParams scan_params = { 0 };
	scan_params.fg_start_period_secs = UINT16_MAX;
	scan_params.fg_end_period_secs = UINT16_MAX;
	scan_params.bg_period_secs = UINT16_MAX;
	const unsigned dwell_ms = active ? 30 : 105;
	scan_params.minact_chdwell_time_ms = dwell_ms;
	scan_params.maxact_chdwell_time_ms = dwell_ms;
	scan_params.pas_chdwell_time_ms = dwell_ms;
	scan_params.scan_ctrl_flags = AR6K_WMI_SCAN_CONNECT | (active ? AR6K_WMI_SCAN_ACTIVE : 0);
	if (!ar6kWmiSetScanParams(&s_ar6kDev, &scan_params)) return false;
	// every network heard, not just ours: the log then shows what was around
	if (!ar6kWmiSetBssFilter(&s_ar6kDev, Ar6kWmiBssFilter_All, 0)) return false;

	s_bssCount = 0;
	s_bssEvents = 0;
	s_seenCount = 0;
	s_scanning = true;
	if (!ar6kWmiStartScan(&s_ar6kDev, Ar6kWmiScanType_Long, 20)) {
		s_scanning = false;
		return false;
	}
	s_scanTries++;
	_setState(NbwState_Scanning);
	return true;
}

static void _onScanComplete(Ar6kDev* dev, int status)
{
	if (!s_scanning) return;
	s_scanning = false;
	if (s_bssCount) s_heardSaved = true;
	dietPrint("[NBW] scan done (%d): %u results, %u of ours\n", status, s_bssEvents, s_bssCount);
	for (unsigned i = 0; i < s_bssCount; i++) {
		const WlanBssDesc* d = &s_bssTable[i];
		char name[33];
		unsigned n = d->ssid_len <= 32 ? d->ssid_len : 32;
		for (unsigned k = 0; k < n; k++) name[k] = (d->ssid[k] >= 32 && d->ssid[k] < 127) ? d->ssid[k] : '?';
		name[n] = 0;
		dietPrint("[NBW]   ours: \"%s\" ch %u, signal %u, security %.2X\n", name, d->channel, d->rssi, d->auth_mask);
	}
	for (unsigned i = 0; i < s_seenCount; i++) dietPrint("[NBW]   also heard: \"%s\"\n", s_seenNames[i]);

	// the strongest access point of any of our networks not yet tried
	WlanBssDesc* best = NULL;
	int bestNet = -1;
	bool heardTried = false;
	for (int pass = 0; pass < 2 && !best; pass++) {
		heardTried = false;
		for (unsigned i = 0; i < s_bssCount; i++) {
			WlanBssDesc* d = &s_bssTable[i];
			for (unsigned k = 0; k < s_netCount; k++) {
				if (d->ssid_len != s_nets[k].ssidLen || memcmp(d->ssid, s_nets[k].ssid, d->ssid_len) != 0) continue;
				if (s_netTried & (1u << k)) { heardTried = true; continue; }
				if (!best || d->rssi > best->rssi) { best = d; bestNet = (int)k; }
			}
		}
		// only ones that already failed in range: a lost handshake message or a busy access
		// point fails a join now and then, so each gets one more go (not 4 rescans for others)
		if (best || !heardTried || s_secondRound) break;
		dietPrint("[NBW] trying the same networks once more\n");
		s_secondRound = true;
		s_netTried = 0;
	}
	if (best) {
		const NbwNetwork* n = &s_nets[bestNet];
		memcpy(s_ssid, n->ssid, n->ssidLen);
		s_ssidLen = n->ssidLen;
		s_hasPmk = n->hasPmk;
		memcpy(s_pmk, n->pmk, sizeof(s_pmk));
		s_netIndex = bestNet;
		char name[33];
		for (unsigned k = 0; k < s_ssidLen; k++) name[k] = (s_ssid[k] >= 32 && s_ssid[k] < 127) ? s_ssid[k] : '?';
		name[s_ssidLen] = 0;
		dietPrint("[NBW] joining \"%s\" (saved network %d)\n", name, bestNet + 1);
		s_pendingBss = *best;
		s_pending = Pend_Join;
		return;
	}
	if (heardTried) {
		// the ones in range had their second go too: more scans won't help (the caller tries again)
		dietPrint("[NBW] the saved networks in range didn't let us join\n");
		_setState(NbwState_Failed);
		return;
	}
	if (s_scanTries < 4) {
		s_pending = Pend_Rescan;
		return;
	}
	dietPrint("[NBW] none of the saved networks found\n");
	_setState(NbwState_Failed);
}

// the network being joined didn't work out: another in range, if any
static bool _tryAnother(void)
{
	for (unsigned k = 0; k < s_netCount; k++) {
		if (!(s_netTried & (1u << k))) {
			dietPrint("[NBW] trying the other saved networks\n");
			s_scanTries = 0;
			return _startScan();
		}
	}
	if (!s_secondRound) {
		// (the scan's results pick the same one again: see _onScanComplete)
		dietPrint("[NBW] trying the same networks once more\n");
		s_secondRound = true;
		s_netTried = 0;
		s_scanTries = 0;
		return _startScan();
	}
	return false;
}

static void _linkUp(void)
{
	// (only while joining: an association that completes after nbwifiDisconnect is ignored;
	// the device was told to disconnect after it, so it doesn't stay)
	if (s_state != NbwState_Connecting) return;
	dietPrint("[NBW] associated\n");
	_dhcpStart();
}

static void _onAssoc(Ar6kDev* dev, Ar6kWmiEvtConnected* info)
{
	// the RSN/WPA element we sent, for the WPA handshake
	u8* ie_data = info->assoc_info + info->beacon_ie_len + 4;
	unsigned remsize = info->assoc_req_len - 4;
	WlanIeHdr* ie = wlanFindRsnOrWpaIe(ie_data, remsize);
	if (ie) {
		unsigned ie_len = sizeof(WlanIeHdr) + ie->len;
		memcpy(s_wpaIeBuf, ie, ie_len);
		s_wpaState.ie_len = ie_len;
	} else {
		s_wpaState.ie_len = 0;
	}
	if (!s_wpaWait) {
		_linkUp();
	}
}

static void _onDisassoc(Ar6kDev* dev, Ar6kWmiEvtDisconnected* info)
{
	dietPrint("[NBW] disconnected (IEEE reason %u, reason %u)\n", info->reason_ieee, info->reason);
	// the device keeps trying to connect until told to stop (as calico notes)
	if (info->reason != 3) ar6kWmiDisconnect(dev);
	if (s_state == NbwState_Disconnecting) {
		// the one nbwifiDisconnect asked for: off the network now
		_setState(NbwState_Ready);
	} else if (s_state == NbwState_Connecting) {
		// couldn't join this one (wrong key, refused...): another saved network, if any
		if (s_netIndex >= 0) s_netTried |= 1u << s_netIndex;
		s_pending = Pend_Next;
	} else if (s_state == NbwState_GettingIp || s_state == NbwState_Online) {
		_setState(NbwState_Failed);
	}
}

static void _onRx(Ar6kDev* dev, NetBuf* pPacket)
{
	NetMacHdr* machdr = (NetMacHdr*)netbufGet(pPacket);
	unsigned ethertype = _bswap16(machdr->len_or_ethertype_be);
	if (ethertype == NetEtherType_EAPOL) {
		// (only while joining or joined: none is answered while leaving or idle)
		if (s_state != NbwState_Connecting && s_state != NbwState_GettingIp && s_state != NbwState_Online) {
			netbufFree(pPacket);
			return;
		}
		// the WPA handshake sends its answer and installs keys: done by
		// nbwifiPoll, out of the packet handling (a newer one replaces an older)
		if (s_pendingEapol) netbufFree(s_pendingEapol);
		s_pendingEapol = pPacket;
		return;
	}
	const u8* payload = (const u8*)(machdr + 1);
	unsigned len = pPacket->len - sizeof(NetMacHdr);
	if (ethertype == NetEtherType_ARP) _arpRx(payload, len);
	else if (ethertype == NetEtherType_IPv4) _ipRx(machdr->src_mac, payload, len);
	netbufFree(pPacket);
}

static NetBuf* _wpaAllocPacket(WpaState* st, size_t len)
{
	return netbufAlloc(sizeof(Ar6kHtcFrameHdr) + sizeof(Ar6kWmiDataHdr) + sizeof(NetMacHdr) + sizeof(NetLlcSnapHdr),
		len, NetBufPool_Tx);
}

static void _wpaTx(WpaState* st, NetBuf* pPacket)
{
	_ethSend(pPacket);
}

static void _wpaInstallKey(WpaState* st, bool is_group, unsigned slot, unsigned len)
{
	Ar6kWmiCipherKey key;
	memset(&key, 0, sizeof(key));
	key.index = slot;
	key.type = len > WPA_AES_BLOCK_LEN ? Ar6kWmiCipherType_TKIP : Ar6kWmiCipherType_AES;
	key.usage = is_group ? AR6K_WMI_CIPHER_USAGE_GROUP : AR6K_WMI_CIPHER_USAGE_PAIRWISE;
	key.length = len;
	key.op_ctrl = AR6K_WMI_KEY_OP_INIT_TSC | AR6K_WMI_KEY_OP_INIT_RSC;

	WpaKey* inkey = is_group ? &st->gtk : &st->ptk.tk;
	memcpy(&key.key[0], inkey->main, 0x10);
	if (key.type == Ar6kWmiCipherType_TKIP) {
		memcpy(&key.key[0x10], inkey->rx, 8);
		memcpy(&key.key[0x18], inkey->tx, 8);
	}
	if (is_group) {
		memcpy(key.rsc, st->rsc, WPA_RSC_LEN);
	}

	ar6kWmiSimpleCmdWithParam8(&s_ar6kDev, Ar6kWmiCmdId_Synchronize, 0);
	if (!ar6kWmiAddCipherKey(&s_ar6kDev, &key)) {
		dietPrint("[NBW] %s key %u fail\n", is_group ? "group" : "pairwise", slot);
	}
	ar6kWmiSimpleCmdWithParam8(&s_ar6kDev, Ar6kWmiCmdId_Synchronize, 0);

	if (is_group) {
		dietPrint("[NBW] WPA keys installed\n");
		if (s_wpaWait) {
			s_wpaWait = false;
			_linkUp();
		}
	}
}

// ---- connecting ----

static bool _associate(WlanBssDesc* bss)
{
	static const Ar6kWmiProbedSsid dummy_probed_ssid = { 0 };
	static const Ar6kWmiScanParams dummy_scan_params = {
		.fg_start_period_secs = UINT16_MAX,
		.fg_end_period_secs = UINT16_MAX,
		.bg_period_secs = UINT16_MAX,
		.maxact_chdwell_time_ms = 200,
		.pas_chdwell_time_ms = 200,
		.short_scan_ratio = 0,
		.scan_ctrl_flags = AR6K_WMI_SCAN_CONNECT | AR6K_WMI_SCAN_ACTIVE | AR6K_WMI_SCAN_ROAM,
		.minact_chdwell_time_ms = 200,
		._pad = 0,
		.max_dfsch_act_time_ms = 0,
	};
	static const Ar6kWmiPstreamConfig pstream_params = {
		.min_service_int_msec = 20,
		.max_service_int_msec = 20,
		.inactivity_int_msec  = 9999999,
		.suspension_int_msec  = (u32)-1,
		.srv_start_time       = 0,
		.min_data_rate_bps    = 83200,
		.mean_data_rate_bps   = 83200,
		.peak_data_rate_bps   = 83200,
		.max_burst_size       = 0,
		.delay_bound          = 0,
		.min_phy_rate_bps     = 6000000,
		.sba                  = 0x2000,
		.medium_time          = 0,
		.nominal_msdu         = 0x80d0,
		.max_msdu             = 0xd0,
		.traffic_class        = 0,
		.traffic_dir          = Ar6kWmiTrafficDir_Bidir,
		.rx_queue_num         = 0xff,
		.traffic_type         = Ar6kWmiTrafficType_Periodic,
		.voice_ps_cap         = Ar6kWmiVoicePSCap_DisableForThisAC,
		.tsid                 = 5,
		.user_prio            = 0,
	};

	// the strongest security the network offers; a network that names none
	// that fits, with a key given, is tried as WPA2 (AES)
	// auth_mask and auth_type share a byte: read the mask before choosing
	const unsigned mask = bss->auth_mask;
	bss->auth_type = WlanBssAuthType_Open;
	for (int t = WlanBssAuthType_WPA2_PSK_AES; t >= WlanBssAuthType_WPA_PSK_TKIP; t--) {
		if (mask & (1U << t)) { bss->auth_type = (WlanBssAuthType)t; break; }
	}
	if (bss->auth_type == WlanBssAuthType_Open && s_hasPmk) {
		bss->auth_type = WlanBssAuthType_WPA2_PSK_AES;
	}
	if (bss->auth_type != WlanBssAuthType_Open && !s_hasPmk) {
		dietPrint("[NBW] network needs a password\n");
		return false;
	}
	dietPrint("[NBW] joining ch %u, auth %u (offered %.2X)\n", bss->channel, bss->auth_type, mask);

	if (!ar6kWmiSetBssFilter(&s_ar6kDev, Ar6kWmiBssFilter_None, 0)) return false;
	if (!ar6kWmiSetProbedSsid(&s_ar6kDev, &dummy_probed_ssid)) return false;
	if (!ar6kWmiSetScanParams(&s_ar6kDev, &dummy_scan_params)) return false;
	if (!ar6kWmiSetChannelParams(&s_ar6kDev, 0, 0)) return false;
	if (!ar6kWmiSetBitRate(&s_ar6kDev, Ar6kWmiBitRate_Auto, Ar6kWmiBitRate_1Mbps, Ar6kWmiBitRate_1Mbps)) return false;
	if (!ar6kWmiSetFrameRate(&s_ar6kDev, 4, 10, ~(1U<<Ar6kWmiBitRate_11Mbps))) return false; // PS-Poll
	ar6kWmiSimpleCmdWithParam8(&s_ar6kDev, Ar6kWmiCmdId_Synchronize, 0);
	if (!ar6kWmiSetPowerMode(&s_ar6kDev, Ar6kWmiPowerMode_MaxPerformance)) return false;
	ar6kWmiSimpleCmdWithParam8(&s_ar6kDev, Ar6kWmiCmdId_Synchronize, 0);
	if (!ar6kWmiCreatePstream(&s_ar6kDev, &pstream_params)) return false;
	if (!ar6kWmiSetWscStatus(&s_ar6kDev, false)) return false;
	if (!ar6kWmiSetDiscTimeout(&s_ar6kDev, 2)) return false;

	Ar6kWmiConnectParams conn_params = { 0 };
	conn_params.network_type = Ar6kWmiNetworkType_Infrastructure;
	conn_params.ssid_len = bss->ssid_len;
	memcpy(conn_params.ssid, bss->ssid, bss->ssid_len);
	conn_params.channel_mhz = wlanChannelToFreq(bss->channel);
	memcpy(conn_params.bssid, bss->bssid, 6);

	s_wpaWait = false;
	if (bss->auth_type == WlanBssAuthType_Open) {
		conn_params.auth_mode_ieee = Ar6kWmiAuthModeIeee_Open;
		conn_params.auth_mode = Ar6kWmiAuthMode_Open;
		conn_params.pairwise_cipher_type = Ar6kWmiCipherType_None;
		conn_params.group_cipher_type = Ar6kWmiCipherType_None;
	} else if (bss->auth_type >= WlanBssAuthType_WPA_PSK_TKIP) {
		static const u8 authtype[] = { Ar6kWmiAuthType_WPA_PSK, Ar6kWmiAuthType_WPA2_PSK, Ar6kWmiAuthType_WPA_PSK, Ar6kWmiAuthType_WPA2_PSK };
		static const u8 cipher[] = { Ar6kWmiCipherType_TKIP, Ar6kWmiCipherType_TKIP, Ar6kWmiCipherType_AES, Ar6kWmiCipherType_AES };
		s_wpaWait = true;
		conn_params.auth_mode_ieee = Ar6kWmiAuthModeIeee_Open;
		conn_params.auth_mode = authtype[bss->auth_type - WlanBssAuthType_WPA_PSK_TKIP];
		conn_params.pairwise_cipher_type = cipher[bss->auth_type - WlanBssAuthType_WPA_PSK_TKIP];
		conn_params.group_cipher_type = cipher[bss->auth_type - WlanBssAuthType_WPA_PSK_TKIP];
		wpaReset(&s_wpaState, s_pmk);
	} else {
		dietPrint("[NBW] WEP not supported\n");
		return false;
	}

	if (!ar6kWmiSetKeepAlive(&s_ar6kDev, 0)) return false;
	if (!ar6kWmiConnect(&s_ar6kDev, &conn_params)) return false;
	_setState(NbwState_Connecting);
	return true;
}

// ---- the API ----

bool nbwifiInit(void* mem)
{
	_setState(NbwState_Off);
	nbwNetbufInit(mem);
	dietPrint("[NBW] SCFG_EXT %.8X, GPIO_WL %.4X\n", REG_SCFG_EXT, REG_GPIO_WL);

	// the Atheros chip (DSi WiFi) rather than the DS-compatible one. The chip is
	// never reset here: that would wipe the firmware the system loaded into it.
	REG_GPIO_WL &= ~(1U << 8);

	if (!tmioInit(&s_sdioCtl, TMIO1_BASE, TMIO1_FIFO, s_sdioCtlSlots, 2)) {
		dietPrint("[NBW] SDIO controller unavailable (DS mode?)\n");
		return false;
	}
	if (!sdioCardInit(&s_sdioCard, &s_sdioCtl, 0)) {
		dietPrint("[NBW] SDIO init failed\n");
		return false;
	}
	s_sdioCard.dma_cb = NULL;
	if (!ar6kDevInit(&s_ar6kDev, &s_sdioCard, s_ar6kWorkBuf)) {
		dietPrint("[NBW] AR6K init failed\n");
		return false;
	}
	if (!ar6kWmiStartup(&s_ar6kDev)) {
		dietPrint("[NBW] WMI startup failed\n");
		return false;
	}

	wpaPrepare(&s_wpaState);
	s_ar6kDev.cb_onBssInfo = _onBssInfo;
	s_ar6kDev.cb_onScanComplete = _onScanComplete;
	s_ar6kDev.cb_onAssoc = _onAssoc;
	s_ar6kDev.cb_onDisassoc = _onDisassoc;
	s_ar6kDev.cb_rx = _onRx;
	s_wpaState.cb_alloc_packet = _wpaAllocPacket;
	s_wpaState.cb_tx = _wpaTx;
	s_wpaState.cb_install_key = _wpaInstallKey;
	s_wpaState.ie_data = s_wpaIeBuf;

	nbwNonceSeed(s_ar6kDev.macaddr, 6);
	const u8* m = s_ar6kDev.macaddr;
	dietPrint("[NBW] up, MAC %.2X:%.2X:%.2X:%.2X:%.2X:%.2X\n", m[0], m[1], m[2], m[3], m[4], m[5]);
	_setState(NbwState_Ready);
	return true;
}

bool nbwifiConnect(const char* ssid, unsigned ssid_len, const void* pmk)
{
	if (ssid_len == 0 || ssid_len > WLAN_MAX_SSID_LEN) return false;
	NbwNetwork n;
	memset(&n, 0, sizeof(n));
	memcpy(n.ssid, ssid, ssid_len);
	n.ssidLen = (u8)ssid_len;
	n.hasPmk = pmk != NULL;
	if (pmk) memcpy(n.pmk, pmk, sizeof(n.pmk));
	return nbwifiConnectAny(&n, 1);
}

bool nbwifiConnectAny(const NbwNetwork* list, unsigned count)
{
	if (s_state == NbwState_Off || count == 0) return false;
	if (count > NBW_MAX_NETWORKS) count = NBW_MAX_NETWORKS;
	s_netCount = 0;
	for (unsigned i = 0; i < count; i++) {
		if (list[i].ssidLen == 0 || list[i].ssidLen > WLAN_MAX_SSID_LEN) continue;
		s_nets[s_netCount] = list[i];
		char name[33];
		for (unsigned k = 0; k < list[i].ssidLen; k++) name[k] = (list[i].ssid[k] >= 32 && list[i].ssid[k] < 127) ? list[i].ssid[k] : '?';
		name[list[i].ssidLen] = 0;
		dietPrint("[NBW] saved network %u: \"%s\" (%s)\n", s_netCount + 1, name, list[i].hasPmk ? "WPA" : "open");
		s_netCount++;
	}
	if (!s_netCount) return false;
	s_netTried = 0;
	s_secondRound = false;
	s_heardSaved = false;
	s_netIndex = -1;
	s_pending = Pend_None;
	s_scanTries = 0;
	s_ip = 0;
	memset(s_arp, 0, sizeof(s_arp));
	if (!_startScan()) {
		_setState(NbwState_Failed);
		return false;
	}
	return true;
}

// polled once a frame, at the same point each time (a VBlank handler): the
// line counter can't show frames going by, so the caller counts them
static bool s_frameTicks;
u32 nbwFramesForLog(void) { return s_frames; }
void nbwifiFrameTick(void)
{
	s_frameTicks = true;
	s_frames++;
}

void nbwifiPoll(void)
{
	if (s_state == NbwState_Off) return;
	s_polls++;
	if (!s_frameTicks) {
		// polled often: a frame has gone by when the line counter wraps
		const u16 vcount = *(vu16*)0x04000006;     // REG_VCOUNT
		if (vcount < s_lastVcount) s_frames++;
		s_lastVcount = vcount;
	}
	// everything the device has waiting: one check may only find the first
	// of several packets (broadcasts on a busy network push answers back a
	// frame otherwise), but never more than g_nbwPollRounds rounds, so a flood
	// can't stall (polled every ~2 ms, few: the rest is there next time, and no
	// one poll holds the ARM7 long)
	for (int round = 0; round < g_nbwPollRounds; round++) {
		const unsigned before = g_nbwRxBatches;
		if (!ar6kDevPoll(&s_ar6kDev)) {
			dietPrint("[NBW] device poll failed\n");
			break;
		}
		if (g_nbwRxBatches == before) break;
	}

	const NbwPending pending = s_pending;
	s_pending = Pend_None;
	if (pending == Pend_Join) {
		if (!_associate(&s_pendingBss)) {
			dietPrint("[NBW] couldn't start joining\n");
			if (s_netIndex >= 0) s_netTried |= 1u << s_netIndex;
			if (!_tryAnother()) _setState(NbwState_Failed);
		}
	} else if (pending == Pend_Next) {
		if (!_tryAnother()) _setState(NbwState_Failed);
	} else if (pending == Pend_Rescan) {
		if (!_startScan()) _setState(NbwState_Failed);
	}
	if (s_pendingEapol) {
		NetBuf* eapol = s_pendingEapol;
		s_pendingEapol = NULL;
		wpaEapolProcess(&s_wpaState, eapol);     // frees it
	}

	const u32 inState = s_frames - s_stateSince;
	// (on the console a scan takes about 1.4 s and joining about 0.3 s: these are generous)
	if (s_state == NbwState_Scanning && inState > 5 * FRAMES_PER_SEC) {
		dietPrint("[NBW] scan timed out\n");
		s_scanning = false;
		if (s_scanTries >= 4 || !_startScan()) _setState(NbwState_Failed);
	} else if (s_state == NbwState_Connecting && inState > 6 * FRAMES_PER_SEC) {
		dietPrint("[NBW] connecting timed out\n");
		ar6kWmiDisconnect(&s_ar6kDev);
		if (s_netIndex >= 0) s_netTried |= 1u << s_netIndex;
		if (!_tryAnother()) _setState(NbwState_Failed);
	} else if (s_state == NbwState_GettingIp) {
		_dhcpTick();
	} else if (s_state == NbwState_Disconnecting && inState > FRAMES_PER_SEC) {
		// (no word from the device: not associated when asked, or the event lost) off anyway
		_setState(NbwState_Ready);
	}
}

bool nbwifiHeardSaved(void) { return s_heardSaved; }

void nbwifiDisconnect(void)
{
	if (s_state == NbwState_Off || s_state == NbwState_Ready || s_state == NbwState_Disconnecting) return;
	dietPrint("[NBW] leaving the network (radio quiet)\n");
	// nothing half-done carries over: a scan's results, a join, a handshake, DHCP
	s_pending = Pend_None;
	s_scanning = false;
	s_wpaWait = false;
	if (s_pendingEapol) {
		netbufFree(s_pendingEapol);
		s_pendingEapol = NULL;
	}
	s_ip = 0;
	// as calico's twlwifiDisassociate: no more background or roaming scans, the
	// recommended (power-saving) mode, then the disconnect
	static const Ar6kWmiScanParams quiet_scan_params = {
		.fg_start_period_secs = UINT16_MAX,
		.fg_end_period_secs = UINT16_MAX,
		.bg_period_secs = UINT16_MAX,
		.maxact_chdwell_time_ms = 200,
		.pas_chdwell_time_ms = 200,
		.short_scan_ratio = 0,
		.scan_ctrl_flags = AR6K_WMI_SCAN_CONNECT,
		.minact_chdwell_time_ms = 200,
		._pad = 0,
		.max_dfsch_act_time_ms = 0,
	};
	ar6kWmiSetScanParams(&s_ar6kDev, &quiet_scan_params);
	ar6kWmiSetPowerMode(&s_ar6kDev, Ar6kWmiPowerMode_Recommended);
	ar6kWmiDisconnect(&s_ar6kDev);
	_setState(NbwState_Disconnecting);
}

NbwState nbwifiState(void) { return s_state; }
const u8* nbwifiMac(void) { return s_ar6kDev.macaddr; }
u32 nbwifiIp(void) { return s_ip; }
u32 nbwifiNetmask(void) { return s_netmask; }
u32 nbwifiGateway(void) { return s_gateway; }
void nbwifiSetUdpHandler(NbwUdpHandler fn) { s_udpHandler = fn; }
