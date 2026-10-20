// wmhle_air: DS SDK (WM) 802.11 frame bodies. See wmhle_air.h for the layouts.
#include <string.h>
#include "wmhle_air.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

static u16 rd16(const u8* p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8* p) { return (u32)rd16(p) | (u32)rd16(p + 2) << 16; }
static void wr16(u8* p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32(u8* p, u32 v) { wr16(p, (u16)v); wr16(p + 2, (u16)(v >> 16)); }

// the WL's rate tables (WlNic.c): rate set bit -> rate byte (500 kbit/s units)
static const u8 kRateBit2Element[16] = { 0x02, 0x04, 0x0B, 0x0C, 0x12, 0x16, 0x18, 0x24, 0x30, 0x48 };

static unsigned putRates(u8* p, u16 basic, u16 support)
{
	// one byte per supported rate, in bit order, 0x80 on the basic ones (SetSupRateSet)
	unsigned n = 2;
	for (int i = 0; i < 16; i++)
		if (support & (1u << i)) p[n++] = (u8)(kRateBit2Element[i] | ((basic & (1u << i)) ? 0x80 : 0));
	p[0] = 1;
	p[1] = (u8)(n - 2);
	return n;
}

static void getRates(const u8* e, u16* basic, u16* support)
{
	// WElement2RateSet: unknown rates all land on bit 15
	*basic = *support = 0;
	for (unsigned i = 0; i < e[1]; i++) {
		const u8 r = e[2 + i], v = r & 0x7F;
		int bit = 15;
		for (int b = 0; b < 10; b++)
			if (kRateBit2Element[b] == v) bit = b;
		*support |= (u16)(1u << bit);
		if (r & 0x80) *basic |= (u16)(1u << bit);
	}
}

static u16 orDefault(u16 v, u16 def) { return v ? v : def; }

// ---- beacon ----

uint16_t airVtsf(uint32_t usCount, uint16_t vcount)
{
	// WmspSetVTSF: (2 * us - 127 * vcount) >> 7, i.e. (us - 63.5 * vcount) / 64
	return (u16)((u32)(2 * usCount - 127u * vcount) >> 7);
}

unsigned airMakeGameInfo(uint8_t* out, const AirParentInfo* pi)
{
	unsigned ulen = pi->userGameInfoLength;
	if (ulen > AIR_USERGAMEINFO_MAX) ulen = AIR_USERGAMEINFO_MAX;
	wr16(out + 0x00, 0x0001);           // magic
	out[0x02] = 1;                      // ver
	out[0x03] = 0;                      // platform: NITRO
	wr32(out + 0x04, pi->ggid);
	wr16(out + 0x08, pi->tgid);
	out[0x0A] = (u8)ulen;
	out[0x0B] = (u8)((pi->entryFlag ? 1 : 0) | (pi->multiBootFlag ? 2 : 0) | (pi->ksFlag ? 4 : 0));
	wr16(out + 0x0C, pi->parentMaxSize);
	// a multiboot child never sends more than 8 bytes
	wr16(out + 0x0E, (pi->multiBootFlag && pi->childMaxSize >= 8) ? 8 : pi->childMaxSize);
	if (ulen) memcpy(out + AIR_GAMEINFO_HEADER, pi->userGameInfo, ulen);
	return AIR_GAMEINFO_HEADER + ulen;
}

unsigned airBuildBeacon(uint8_t* out, const AirParentInfo* pi)
{
	u8* p = out;
	for (int i = 0; i < 8; i++) p[i] = (u8)(pi->tsf >> (8 * i));
	wr16(p + 8, pi->beaconPeriod);
	wr16(p + 10, orDefault(pi->capability, AIR_CAPABILITY));
	p += 12;
	// no SSID element: the game's SSID only shows in the association request
	p += putRates(p, orDefault(pi->basicRates, AIR_RATES), orDefault(pi->supportRates, AIR_RATES));
	*p++ = 3; *p++ = 1; *p++ = pi->channel;
	// TIM: the hardware keeps it up to date; the WL starts it out empty
	*p++ = 5; *p++ = 5; *p++ = 0; *p++ = pi->dtimPeriod ? pi->dtimPeriod : AIR_DTIM_PERIOD;
	*p++ = 0; *p++ = 0; *p++ = 0;
	// the game info element
	u8* e = p;
	e[0] = 0xDD;
	e[2] = 0x00; e[3] = 0x09; e[4] = 0xBF; e[5] = 0x00;
	wr16(e + 6, orDefault(pi->activeZone, AIR_ACTIVE_ZONE));
	wr16(e + 8, pi->vtsf);
	const unsigned glen = airMakeGameInfo(e + 10, pi);
	e[1] = (u8)(8 + glen);
	p = e + 10 + glen;
	return (unsigned)(p - out);
}

bool airDecodeBeacon(const uint8_t* body, unsigned len, uint8_t scanChannel, AirBeaconInfo* info)
{
	memset(info, 0, sizeof(*info));
	// the WL drops anything that is just the fixed fields
	if (len <= 12) return false;
	info->body = body;
	info->bodyLength = len;
	for (int i = 7; i >= 0; i--) info->tsf = info->tsf << 8 | body[i];
	info->beaconPeriod = rd16(body + 8);
	info->capability = rd16(body + 10);
	info->channel = scanChannel;
	for (unsigned o = 12; o + 2 <= len;) {
		const u8* e = body + o;
		const unsigned elen = e[1];
		if (o + 2 + elen > len) return false;
		switch (e[0]) {
			case 0:     // SSID
				if (elen > 32) return false;
				info->ssidLength = (u8)elen;
				memcpy(info->ssid, e + 2, elen);
				break;
			case 1: getRates(e, &info->basicRates, &info->supportRates); break;
			case 3: if (elen >= 1) info->channel = e[2]; break;
			case 4: if (elen >= 2) info->cfpPeriod = e[3]; break;
			case 5: if (elen >= 2) { info->dtimCount = e[2]; info->dtimPeriod = e[3]; } break;
			default:
				if (e[0] == 0xDD && elen >= 8 && e[2] == 0x00 && e[3] == 0x09 && e[4] == 0xBF && e[5] == 0x00
				    && !info->hasGameInfo) {
					info->hasGameInfo = true;
					info->activeZone = rd16(e + 6);
					info->vtsf = rd16(e + 8);
					info->gameInfo = e + 10;
					info->gameInfoLength = (u16)(elen - 8);
				} else if (e[0] > 6) {
					// kept in the WMBssDesc only when there's no game info
					info->otherElementCount++;
					info->otherElementLength = (u16)(info->otherElementLength + 2 + elen);
				}
				break;
		}
		o += 2 + elen;
	}
	if (info->hasGameInfo && info->gameInfoLength >= AIR_GAMEINFO_HEADER) {
		const u8* g = info->gameInfo;
		info->magicNumber = rd16(g);
		info->ver = g[2];
		info->platform = g[3];
		info->ggid = rd32(g + 4);
		info->tgid = rd16(g + 8);
		info->userGameInfoLength = g[0x0A];
		info->attribute = g[0x0B];
		info->parentMaxSize = rd16(g + 0x0C);
		info->childMaxSize = rd16(g + 0x0E);
		info->userGameInfo = g + AIR_GAMEINFO_HEADER;
	}
	if (info->hasGameInfo) {
		info->otherElementCount = 0;
		info->otherElementLength = 0;
	}
	return true;
}

// without game info the WL keeps the elements it doesn't know, whole
static void copyOtherElements(u8* out, const u8* body, unsigned len)
{
	unsigned n = 0;
	for (unsigned o = 12; o + 2 <= len && o + 2 + body[o + 1] <= len; o += 2u + body[o + 1])
		if (body[o] > 6) {
			memcpy(out + n, body + o, 2u + body[o + 1]);
			n += 2u + body[o + 1];
		}
}

unsigned airMakeBssDesc(uint8_t desc[AIR_BSSDESC_SIZE], const AirBeaconInfo* info, const uint8_t bssid[6], uint8_t rssi)
{
	memset(desc, 0, AIR_BSSDESC_SIZE);
	// length in halfwords: the 64-byte head plus the game info (or other elements), rounded up
	const unsigned extra = info->hasGameInfo ? info->gameInfoLength : info->otherElementLength;
	const unsigned halfwords = (extra + 65) / 2;
	if (halfwords * 2 > AIR_BSSDESC_SIZE) return 0;
	wr16(desc + 0x00, (u16)halfwords);
	wr16(desc + 0x02, rssi);
	memcpy(desc + 0x04, bssid, 6);
	wr16(desc + 0x0A, info->ssidLength);
	memcpy(desc + 0x0C, info->ssid, info->ssidLength);
	wr16(desc + 0x2C, info->capability);
	wr16(desc + 0x2E, info->basicRates);
	wr16(desc + 0x30, info->supportRates);
	wr16(desc + 0x32, info->beaconPeriod);
	wr16(desc + 0x34, info->dtimPeriod);
	wr16(desc + 0x36, info->channel);
	wr16(desc + 0x38, info->cfpPeriod);
	// 0x3A cfpMaxDuration: the WL never fills it in
	if (info->hasGameInfo) {
		wr16(desc + 0x3C, info->gameInfoLength);
		memcpy(desc + 0x40, info->gameInfo, info->gameInfoLength);
	} else {
		wr16(desc + 0x3E, info->otherElementCount);
		if (info->otherElementCount && info->body) copyOtherElements(desc + 0x40, info->body, info->bodyLength);
	}
	return halfwords * 2;
}

bool airParseBeacon(const uint8_t* body, unsigned len, const uint8_t bssid[6], uint8_t scanChannel,
                    uint8_t desc[AIR_BSSDESC_SIZE], AirBeaconInfo* info)
{
	if (!airDecodeBeacon(body, len, scanChannel, info)) return false;
	return airMakeBssDesc(desc, info, bssid, 0) != 0;
}

bool airFillScanCallback(uint8_t* cb, const uint8_t desc[AIR_BSSDESC_SIZE])
{
	const u16 glen = rd16(desc + 0x3C);
	memcpy(cb + 0x0A, desc + 0x04, 6);
	memcpy(cb + 0x10, desc + 0x36, 2);
	if (glen > AIR_GAMEINFO_MAX) return false;
	memcpy(cb + 0x14, desc + 0x0A, 2);
	memcpy(cb + 0x16, desc + 0x0C, 32);
	wr16(cb + 0x36, glen);
	memset(cb + 0x38, 0, AIR_GAMEINFO_MAX);
	memcpy(cb + 0x38, desc + 0x40, (glen + 1u) & ~1u);
	return true;
}

// ---- association ----

void airMakeSsid(uint8_t ssid32[32], uint32_t ggid, uint16_t tgid, const uint8_t userSsid24[24])
{
	wr16(ssid32 + 0, (u16)ggid);
	wr16(ssid32 + 2, (u16)(ggid >> 16));
	wr16(ssid32 + 4, tgid);
	wr16(ssid32 + 6, 0);
	memcpy(ssid32 + 8, userSsid24, 24);
}

unsigned airMakeAssocReq(uint8_t* out, const AirAssocReq* req)
{
	wr16(out + 0, req->capability);
	wr16(out + 2, req->listenInterval);
	out[4] = 0;
	out[5] = req->ssidLength;
	memcpy(out + 6, req->ssid, req->ssidLength);
	unsigned n = 6u + req->ssidLength;
	n += putRates(out + n, req->basicRates, req->supportRates);
	return n;
}

unsigned airBuildAssocReq(uint8_t* out, const uint8_t desc[AIR_BSSDESC_SIZE], const uint8_t userSsid24[24])
{
	AirAssocReq req;
	req.capability = AIR_CAPABILITY;
	req.listenInterval = AIR_LISTEN_INTERVAL;
	req.ssidLength = 32;
	// the parent's ggid and tgid, from the game info in its WMBssDesc
	airMakeSsid(req.ssid, rd32(desc + 0x40 + 0x04), rd16(desc + 0x40 + 0x08), userSsid24);
	req.basicRates = req.supportRates = AIR_RATES;
	return airMakeAssocReq(out, &req);
}

bool airDecodeAssocReq(const uint8_t* body, unsigned len, AirAssocReq* req)
{
	memset(req, 0, sizeof(*req));
	if (len < 4) return false;
	req->capability = rd16(body);
	req->listenInterval = rd16(body + 2);
	bool ssid = false;
	for (unsigned o = 4; o + 2 <= len; o += 2u + body[o + 1]) {
		const u8* e = body + o;
		if (o + 2 + e[1] > len) return false;
		if (e[0] == 0 && e[1] <= 32) {
			req->ssidLength = e[1];
			memcpy(req->ssid, e + 2, e[1]);
			ssid = true;
		} else if (e[0] == 1) {
			getRates(e, &req->basicRates, &req->supportRates);
		}
	}
	return ssid;
}

bool airParseAssocReq(const uint8_t* body, unsigned len, uint8_t ssid32[32])
{
	AirAssocReq req;
	if (!airDecodeAssocReq(body, len, &req)) return false;
	memcpy(ssid32, req.ssid, 32);
	return true;
}

unsigned airMakeAssocResp(uint8_t* out, const AirAssocResp* resp)
{
	wr16(out + 0, resp->capability);
	wr16(out + 2, resp->status);
	// 802.11 sets the top two bits of an AID
	wr16(out + 4, (u16)(resp->aid | 0xC000));
	return 6 + putRates(out + 6, resp->basicRates, resp->supportRates);
}

unsigned airBuildAssocResp(uint8_t* out, uint16_t aid)
{
	AirAssocResp resp;
	resp.capability = AIR_CAPABILITY;
	resp.status = 0;
	resp.aid = aid;
	resp.basicRates = resp.supportRates = AIR_RATES;
	return airMakeAssocResp(out, &resp);
}

bool airDecodeAssocResp(const uint8_t* body, unsigned len, AirAssocResp* resp)
{
	memset(resp, 0, sizeof(*resp));
	if (len < 6) return false;
	resp->capability = rd16(body);
	resp->status = rd16(body + 2);
	resp->aid = (u16)(rd16(body + 4) & 0x3FFF);
	for (unsigned o = 6; o + 2 <= len && o + 2 + body[o + 1] <= len; o += 2u + body[o + 1])
		if (body[o] == 1) getRates(body + o, &resp->basicRates, &resp->supportRates);
	return true;
}

bool airParseAssocResp(const uint8_t* body, unsigned len, uint16_t* aid)
{
	AirAssocResp resp;
	if (!airDecodeAssocResp(body, len, &resp) || resp.status != 0) return false;
	*aid = resp.aid;
	return true;
}
