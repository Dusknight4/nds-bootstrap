// wmhle_air: the 802.11 frame bodies a DS SDK (WM) game puts on the air, built
// and parsed byte for byte as the real ARM7 wireless code (WMSP + the "WL"
// driver) does. Bodies only: no 24-byte 802.11 header, no FCS. WideMelon's
// DsBridge wraps them (HostInfo = beacon, Join = association request,
// JoinOk = association response, Cmd/Reply/Ack = MP frames).
//
// Checked against frames two emulated Platinums sent in the Union Room
// (harness/mp/rec5.txt, see test_air.c) and the TwlSDK 5.5 ARM7 decomp
// (NathaanTFM/arm7decomp: marionea/TxCtrl.c MakeBeaconFrame, RxCtrl.c
// RxProbeResFrame, WlNic.c rate tables, wm/ARM7 req_StartParent.c,
// req_StartScan.c, req_StartConnect.c, wmsp_system.c WMSP_CopyParentParam).
// All values little-endian. Portable C99, no allocation.
//
// ---- Beacon body (parent, frame control 0x0080, sent to FF:FF:FF:FF:FF:FF,
//      addr2 = addr3 = parent MAC, sent at 2 Mbit/s) ----
//  off  size  value
//  00   8     TSF (the hardware stamps it on air; melonDS writes USCOUNTER)
//  08   2     beacon interval = WMParentParam.beaconPeriod as is (Platinum
//             uses ~0x32..0x36; the WL writes the number straight in)
//  0A   2     capability 0x0021 (ESS + short preamble)
//             no SSID element: WM parents use a beacon type that leaves it out
//  0C   4     01 02 82 84   supported rates 1 and 2 Mbit/s, both basic
//  10   3     03 01 ch      DS parameter set: channel
//  13   7     05 05 00 02 00 00 00   TIM: DTIM count 0, DTIM period 2,
//             bitmap control 0, 2 bitmap bytes (the hardware updates the TIM)
//  1A   1     DD            vendor element ("game info element")
//  1B   1     8 + gameInfoLength
//  1C   4     00 09 BF 00   Nintendo OUI + subtype 0
//  20   2     active zone: 0x000A (GBATEK's "stepping offset"); 0xFFFF when
//             the parent doesn't use power management
//  22   2     VTSF: the microsecond counter at the start of the current video
//             frame, in 64 us units (GBATEK's "LCD video sync"; the WL rewrites
//             it at every beacon; see airVtsf)
//  24   n     WMGameInfo, gameInfoLength = 16 + userGameInfoLength bytes:
//   24  2       magicNumber 0x0001  }  these 4 bytes (01 00 01 00) are what
//   26  1       ver 1               }  GBATEK calls "Fixed ID 00400001h"; the
//   27  1       platform 0 (NITRO)  }  recordings show 00010001h
//   28  4       ggid
//   2C  2       tgid
//   2E  1       userGameInfoLength
//   2F  1       attribute: bit0 entryFlag, bit1 multiBootFlag, bit2 KS_Flag
//               (the SDK does not put CS_Flag in the beacon)
//   30  2       parentMaxSize (as set, without the KS extra)
//   32  2       childMaxSize (clamped to 8 when multiBootFlag is set)
//   34  u       userGameInfo
//  Union Room: userGameInfoLength 0x5C, body 144 bytes.
//  (After the body the WL leaves a 4-byte buffer marker B8 B6 46 1D at the
//  next 4-byte boundary; the hardware overwrites it with the FCS, so the
//  recordings show it where the FCS goes. It is not part of the body.)
//
// ---- Association request body (child, frame control 0x0000) ----
//  00   2     capability 0x0021
//  02   2     listen interval 1
//  04   34    00 20 + 32-byte SSID:
//   06  2       ggid low   06+2 ggid high   06+4 tgid   06+6 0x0000
//   0E  24      the user SSID from WMStartConnectReq.ssid (the parent gets
//               these 24 bytes in WMStartParentCallback.ssid)
//  26   4     01 02 82 84   supported rates
//  total 42 bytes.
//
// ---- Association response body (parent, frame control 0x0010) ----
//  00   2     capability 0x0021
//  02   2     status 0 = success (19 = too many stations: the child's WM
//             reports that as WM_ERRCODE_OVER_MAX_ENTRY)
//  04   2     AID | 0xC000
//  06   4     01 02 82 84
//  total 10 bytes.
//
// ---- Other management bodies seen in the recordings (no helpers) ----
//  authentication (0x00B0): u16 algorithm 0, u16 sequence 1/2, u16 status 0
//  disassociation (0x00A0) / deauthentication (0x00C0): u16 reason
//  (the recording has a deauthentication with reason 1)
//
// ---- MP bodies (from the recordings; see also the research notes 3.6/3.7) ----
//  CMD   (0x0228, duration 0x0278 seen, addr1 03:09:BF:00:00:00, addr2 = addr3
//        = parent): u16 txop (reply time per child, 0x017E), u16 poll bitmap
//        (bit n = aid n), then the WM port packets. Union Room: 9E60 = VSYNC |
//        DEST_BITMAP | port 14 | 0x60 halfwords, 192 payload bytes, u16
//        sequence number (port >= 8), u16 destination bitmap: 202 bytes.
//        With no data the CMD is 6 bytes: txop, poll bitmap, 0x8000 (VSYNC,
//        nothing else; the poll bitmap may be 0).
//  REPLY (0x0118; 0x0158 with no body; addr1 = parent, addr2 = child,
//        addr3 03:09:BF:00:00:10): the child's port packets straight away.
//        Union Room: 8E13 = VSYNC | port 14 | 0x13 halfwords, 38 payload
//        bytes, u16 sequence number: 42 bytes. With no data: 2 bytes, 0x8000
//        or 0x0000.
//  ACK   (0x0218, addr1 03:09:BF:00:00:03, addr2 = addr3 = parent): 4 bytes,
//        u16 (0x05B6/0x05B7 seen; meaning unknown), u16 bitmap of children
//        that did not reply (0 in all recorded frames).
#ifndef WMHLE_AIR_H
#define WMHLE_AIR_H

#include <stdbool.h>
#include <stdint.h>

#define AIR_CAPABILITY        0x0021  // ESS + short preamble, as every WM frame has it
#define AIR_RATES             0x0003  // rate set bitmap: bit0 1 Mbit/s, bit1 2 Mbit/s
#define AIR_DTIM_PERIOD       2       // what WMSP_StartParent asks the WL for
#define AIR_ACTIVE_ZONE       0x000A
#define AIR_LISTEN_INTERVAL   1
#define AIR_GAMEINFO_HEADER   16      // WMGameInfo up to userGameInfo
#define AIR_USERGAMEINFO_MAX  112
#define AIR_GAMEINFO_MAX      128     // a longer one doesn't fit a WMBssDesc
#define AIR_BSSDESC_SIZE      192     // WM_BSS_DESC_SIZE
#define AIR_BEACON_MAX        (0x24 + AIR_GAMEINFO_MAX)

// what a parent's beacon is built from: WMParentParam (+ its user game info)
// plus what the WL adds. Zero in capability, dtimPeriod, activeZone, basicRates
// or supportRates means the SDK's usual value (AIR_*), so memset(0) works.
typedef struct AirParentInfo {
	uint8_t mac[6];             // not in the body (the 802.11 header has it)
	uint32_t ggid;
	uint16_t tgid;
	uint8_t entryFlag, multiBootFlag, ksFlag, csFlag;   // csFlag: not on air
	uint16_t maxEntry;          // not in the body (for accepting children)
	uint16_t parentMaxSize, childMaxSize;
	uint16_t beaconPeriod;      // goes into the beacon interval field as is
	uint8_t channel;
	uint64_t tsf;
	uint16_t vtsf;              // see airVtsf; 0 if unknown
	uint16_t activeZone;
	uint8_t dtimPeriod;
	uint16_t capability;
	uint16_t basicRates, supportRates;
	uint16_t userGameInfoLength;
	uint8_t userGameInfo[AIR_USERGAMEINFO_MAX];
} AirParentInfo;

// what a beacon says, decoded (pointers point into the parsed body)
typedef struct AirBeaconInfo {
	uint64_t tsf;
	uint16_t beaconPeriod;
	uint16_t capability;
	uint16_t basicRates, supportRates;   // WMBssDesc rateSet bitmaps
	uint8_t channel;            // from the DS parameter set, else the scan channel
	uint8_t dtimCount, dtimPeriod;
	uint8_t cfpPeriod;
	uint8_t ssidLength;         // 0: WM beacons have no SSID element
	uint8_t ssid[32];
	bool hasGameInfo;           // a Nintendo vendor element (DD, 00 09 BF 00)
	uint16_t activeZone, vtsf;
	const uint8_t* gameInfo;    // WMGameInfo bytes
	uint16_t gameInfoLength;
	// the WMGameInfo fields (when gameInfoLength >= 16)
	uint16_t magicNumber;
	uint8_t ver, platform;
	uint32_t ggid;
	uint16_t tgid;
	uint8_t userGameInfoLength, attribute;
	uint16_t parentMaxSize, childMaxSize;
	const uint8_t* userGameInfo;
	// without a game info element: elements with IDs above 6, as the WL keeps them
	uint16_t otherElementCount, otherElementLength;
	// the decoded body (airMakeBssDesc copies the other elements from it)
	const uint8_t* body;
	unsigned bodyLength;
} AirBeaconInfo;

// the decoded association request / response
typedef struct AirAssocReq {
	uint16_t capability, listenInterval;
	uint8_t ssidLength;
	uint8_t ssid[32];           // ggid lo, ggid hi, tgid, 0, then the 24-byte user SSID
	uint16_t basicRates, supportRates;
} AirAssocReq;

typedef struct AirAssocResp {
	uint16_t capability, status;
	uint16_t aid;               // without the 0xC000 bits
	uint16_t basicRates, supportRates;
} AirAssocResp;

// ---- beacon ----

// VTSF as the SDK computes it (WmspSetVTSF): the microsecond counter
// (W_US_COUNT) less the time since line 0 (63.5 us per line), in 64 us units
uint16_t airVtsf(uint32_t usCount, uint16_t vcount);

// the WMGameInfo the SDK puts in the beacon (WMSP_CopyParentParam).
// out: at least 16 + userGameInfoLength bytes. Returns its length
unsigned airMakeGameInfo(uint8_t* out, const AirParentInfo* pi);

// the beacon body. out: AIR_BEACON_MAX bytes. Returns its length
unsigned airBuildBeacon(uint8_t* out, const AirParentInfo* pi);

// decode a beacon (or probe response) body. scanChannel: the channel it came
// in on (for a beacon without a DS parameter set). False if it isn't one
bool airDecodeBeacon(const uint8_t* body, unsigned len, uint8_t scanChannel, AirBeaconInfo* info);

// the WMBssDesc the SDK makes of a decoded beacon (SDK_VERSION_WL >= 17100:
// otherElementCount at 0x3E, gameInfo at 0x40). All 192 bytes are written
// (unused ones 0); rssi at 0x02 is the WL's 8-bit RSSI. Returns the bytes the
// scan copies to the ARM9 (2 * the length field), 0 if the game info is
// longer than 128 bytes (the SDK then reports PARENT_NOT_FOUND)
unsigned airMakeBssDesc(uint8_t desc[AIR_BSSDESC_SIZE], const AirBeaconInfo* info,
                        const uint8_t bssid[6], uint8_t rssi);

// both of the above (rssi 0). False if it isn't a beacon or won't fit
bool airParseBeacon(const uint8_t* body, unsigned len, const uint8_t bssid[6], uint8_t scanChannel,
                    uint8_t desc[AIR_BSSDESC_SIZE], AirBeaconInfo* info);

// fill a WMStartScanCallback from a WMBssDesc the way WMSP_StartScan does:
// macAddress 0x0A, channel 0x10, ssidLength 0x14, ssid 0x16, gameInfoLength
// 0x36, gameInfo 0x38 (128 bytes, cleared, then the even-rounded length
// copied). apiid/errcode/state/linkLevel are the caller's. cb: 0xB8 bytes.
// False (PARENT_NOT_FOUND) if gameInfoLength > 128
bool airFillScanCallback(uint8_t* cb, const uint8_t desc[AIR_BSSDESC_SIZE]);

// ---- association ----

// the 32-byte SSID a child joins with: ggid, tgid, 0, then the user SSID
void airMakeSsid(uint8_t ssid32[32], uint32_t ggid, uint16_t tgid, const uint8_t userSsid24[24]);

// association request body from its fields (42 bytes for a 32-byte SSID)
unsigned airMakeAssocReq(uint8_t* out, const AirAssocReq* req);
// ... as a DS child sends it, joining the parent in a WMBssDesc (its gameInfo
// has the ggid and tgid) with the user SSID from WMStartConnectReq. out: 42 bytes
unsigned airBuildAssocReq(uint8_t* out, const uint8_t desc[AIR_BSSDESC_SIZE], const uint8_t userSsid24[24]);
bool airDecodeAssocReq(const uint8_t* body, unsigned len, AirAssocReq* req);
// just the 32-byte SSID (zero-padded if shorter). False if it isn't one
bool airParseAssocReq(const uint8_t* body, unsigned len, uint8_t ssid32[32]);

// association response body (10 bytes); resp->aid without the 0xC000 bits
unsigned airMakeAssocResp(uint8_t* out, const AirAssocResp* resp);
// ... a DS parent accepting a child with this AID. out: 10 bytes
unsigned airBuildAssocResp(uint8_t* out, uint16_t aid);
bool airDecodeAssocResp(const uint8_t* body, unsigned len, AirAssocResp* resp);
// the AID, if the response is a success. False if not one, or a refusal
bool airParseAssocResp(const uint8_t* body, unsigned len, uint16_t* aid);

#endif
