// nbwifi crypto: SHA-1, and on it the HMAC-SHA1, EAPOL nonce and WPA key
// derivation that calico's WPA supplicant needs. Calico takes SHA-1 from the
// DSi BIOS; inside a DS game nds-bootstrap may have switched the BIOS back to
// the DS one, so nbwifi carries its own.
#include <string.h>
#include <calico/types.h>
#include <calico/dev/wpa.h>
#include "nbw_internal.h"
#include "nbwifi.h"

typedef struct Sha1 {
	u32 h[5];
	u64 total;
	u8 block[64];
	unsigned used;
} Sha1;

static u32 _rol(u32 x, unsigned n) { return (x << n) | (x >> (32 - n)); }

static void _sha1Block(Sha1* s, const u8* p)
{
	u32 w[80];
	for (unsigned i = 0; i < 16; i++)
		w[i] = (u32)p[4*i] << 24 | (u32)p[4*i+1] << 16 | (u32)p[4*i+2] << 8 | p[4*i+3];
	for (unsigned i = 16; i < 80; i++)
		w[i] = _rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
	u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
	for (unsigned i = 0; i < 80; i++) {
		u32 f, k;
		if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
		else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
		else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
		else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
		u32 t = _rol(a, 5) + f + e + k + w[i];
		e = d; d = c; c = _rol(b, 30); b = a; a = t;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void _sha1Init(Sha1* s)
{
	s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE; s->h[3] = 0x10325476; s->h[4] = 0xC3D2E1F0;
	s->total = 0;
	s->used = 0;
}

static void _sha1Update(Sha1* s, const void* data, size_t len)
{
	const u8* p = (const u8*)data;
	s->total += len;
	while (len) {
		unsigned n = 64 - s->used;
		if (n > len) n = len;
		memcpy(s->block + s->used, p, n);
		s->used += n;
		p += n;
		len -= n;
		if (s->used == 64) {
			_sha1Block(s, s->block);
			s->used = 0;
		}
	}
}

static void _sha1Final(Sha1* s, void* out)
{
	u64 bits = s->total * 8;
	u8 pad = 0x80;
	_sha1Update(s, &pad, 1);
	pad = 0;
	while (s->used != 56) _sha1Update(s, &pad, 1);
	u8 len[8];
	for (unsigned i = 0; i < 8; i++) len[i] = (u8)(bits >> (56 - 8*i));
	_sha1Update(s, len, 8);
	u8* o = (u8*)out;
	for (unsigned i = 0; i < 5; i++) {
		o[4*i] = s->h[i] >> 24; o[4*i+1] = s->h[i] >> 16; o[4*i+2] = s->h[i] >> 8; o[4*i+3] = s->h[i];
	}
}

void wpaHmacSha1(void* out, const void* key, size_t key_len, const void* data, size_t data_len)
{
	u8 keyblock[64];
	u8 keyhash[WPA_SHA1_LEN];
	const u8* keyp = (const u8*)key;
	if (key_len > sizeof(keyblock)) {
		Sha1 s;
		_sha1Init(&s);
		_sha1Update(&s, key, key_len);
		_sha1Final(&s, keyhash);
		keyp = keyhash;
		key_len = WPA_SHA1_LEN;
	}

	size_t i;
	for (i = 0; i < key_len; i ++) keyblock[i] = keyp[i] ^ 0x36;
	for (; i < sizeof(keyblock); i ++) keyblock[i] = 0x36;

	Sha1 s;
	u8 inner[WPA_SHA1_LEN];
	_sha1Init(&s);
	_sha1Update(&s, keyblock, sizeof(keyblock));
	_sha1Update(&s, data, data_len);
	_sha1Final(&s, inner);

	for (i = 0; i < sizeof(keyblock); i ++) keyblock[i] ^= 0x36 ^ 0x5c;
	_sha1Init(&s);
	_sha1Update(&s, keyblock, sizeof(keyblock));
	_sha1Update(&s, inner, sizeof(inner));
	_sha1Final(&s, out);
}

// a nonce has to be unpredictable to others, not secret: whatever varies
// (the MAC, timings, a counter) hashed together
static u8 s_nonceSeed[32];
static u32 s_nonceCount;

void nbwNonceSeed(const void* data, size_t len)
{
	Sha1 s;
	u8 out[WPA_SHA1_LEN];
	_sha1Init(&s);
	_sha1Update(&s, s_nonceSeed, sizeof(s_nonceSeed));
	_sha1Update(&s, data, len);
	_sha1Final(&s, out);
	memcpy(s_nonceSeed, out, sizeof(out));
}

void wpaGenerateEapolNonce(void* out)
{
	u32 extra[3] = { ++s_nonceCount, *(vu16*)0x04000006 /* VCOUNT */, *(vu16*)0x04000100 /* timer 0 */ };
	nbwNonceSeed(extra, sizeof(extra));
	u8* o = (u8*)out;
	for (unsigned i = 0; i < WPA_EAPOL_NONCE_LEN; i += WPA_SHA1_LEN) {
		u8 block[WPA_SHA1_LEN];
		u32 n = i;
		wpaHmacSha1(block, s_nonceSeed, sizeof(s_nonceSeed), &n, sizeof(n));
		unsigned take = WPA_EAPOL_NONCE_LEN - i < WPA_SHA1_LEN ? WPA_EAPOL_NONCE_LEN - i : WPA_SHA1_LEN;
		memcpy(o + i, block, take);
	}
}

// WPA pre-shared key from the network name and password (PBKDF2-HMAC-SHA1,
// 4096 rounds): slow on a DS, a few seconds, so worth keeping once worked out
void nbwifiDerivePmk(void* pmk, const char* ssid, unsigned ssid_len, const char* password, unsigned password_len)
{
	u8* out = (u8*)pmk;
	for (u32 blockIndex = 1; blockIndex <= 2; blockIndex++) {
		u8 salt[WLAN_MAX_SSID_LEN + 4];
		memcpy(salt, ssid, ssid_len);
		salt[ssid_len] = blockIndex >> 24; salt[ssid_len+1] = blockIndex >> 16;
		salt[ssid_len+2] = blockIndex >> 8; salt[ssid_len+3] = blockIndex;

		u8 u[WPA_SHA1_LEN], t[WPA_SHA1_LEN];
		wpaHmacSha1(u, password, password_len, salt, ssid_len + 4);
		memcpy(t, u, sizeof(t));
		for (unsigned r = 1; r < 4096; r++) {
			wpaHmacSha1(u, password, password_len, u, sizeof(u));
			for (unsigned i = 0; i < sizeof(t); i++) t[i] ^= u[i];
		}
		unsigned take = blockIndex == 1 ? WPA_SHA1_LEN : WLAN_WPA_PSK_LEN - WPA_SHA1_LEN;
		memcpy(out + (blockIndex - 1) * WPA_SHA1_LEN, t, take);
	}
}
