// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/dev/wpa.h>
#include <string.h>

//#define WPA_DEBUG

#ifdef WPA_DEBUG
#include <calico/system/dietprint.h>
#else
#define dietPrint(...) ((void)0)
#endif

typedef struct WpaWork {
	NetMacHdr* machdr;
	WpaEapolHdr* eapolhdr;
	WpaEapolKeyHdr* hdr;
	WpaEapolHdr* replyeapol;
	WpaEapolKeyHdr* reply;
	void* key_data;
	unsigned key_data_len;
	u16 key_info;
	bool is_rsn;
	u8 descr_ver;
} WpaWork;

void wpaPseudoRandomFunction(void* out, size_t out_len, const void* key, size_t key_size, void* pad, size_t pad_len)
{
	u8 temp[WPA_SHA1_LEN];
	u8* pCounter = (u8*)pad + pad_len - 1;

	*pCounter = 0;
	while (out_len) {
		size_t cur_sz = out_len > sizeof(temp) ? sizeof(temp) : out_len;
		out_len -= cur_sz;

		wpaHmacSha1(temp, key, key_size, pad, pad_len);
		(*pCounter)++;

		memcpy(out, temp, cur_sz);
		out = (u8*)out + cur_sz;
	}
}

bool wpaAesUnwrap(const void* kek, const void* in, void* out, size_t num_blk)
{
	u8 A[WPA_AES_WRAP_BLK_LEN], B[2*WPA_AES_WRAP_BLK_LEN];
	WpaAesContext ctx;
	wpaAesSetDecryptKey(kek, WPA_AES_BLOCK_LEN*8, &ctx);

	// Setup
	memcpy(A, in, WPA_AES_WRAP_BLK_LEN);
	memcpy(out, (u8*)in+WPA_AES_WRAP_BLK_LEN, num_blk*WPA_AES_WRAP_BLK_LEN);

	// Unwrap
	for (int j = 5; j >= 0; j --) {
		u8* R = (u8*)out + (num_blk-1) * 8;
		for (int i = num_blk; i >= 1; i --) {
			memcpy(&B[0], A, WPA_AES_WRAP_BLK_LEN);
			memcpy(&B[WPA_AES_WRAP_BLK_LEN], R, WPA_AES_WRAP_BLK_LEN);
			B[WPA_AES_WRAP_BLK_LEN-1] ^= i + num_blk * j;
			wpaAesDecrypt(B, B, &ctx);
			memcpy(A, &B[0], WPA_AES_WRAP_BLK_LEN);
			memcpy(R, &B[WPA_AES_WRAP_BLK_LEN], WPA_AES_WRAP_BLK_LEN);
			R -= WPA_AES_WRAP_BLK_LEN;
		}
	}

	// Check IV
	for (unsigned i = 0; i < WPA_AES_WRAP_BLK_LEN; i ++) {
		if (A[i] != 0xa6) {
			return false;
		}
	}

	return true;
}

static u64 _wpaDecodeReplayCounter(const u8* replay_buf)
{
	u64 ret = 0;
	for (unsigned i = 0; i < WPA_EAPOL_REPLAY_LEN; i ++) {
		ret = (ret << 8) | replay_buf[i];
	}
	return ret;
}

static void _wpaEapolCalcMic(WpaState* st, WpaWork* wk, WpaEapolHdr* eapolhdr)
{
	WpaEapolKeyHdr* hdr = (WpaEapolKeyHdr*)(eapolhdr+1);
	size_t totalsz = sizeof(WpaEapolHdr) + sizeof(WpaEapolKeyHdr) + __builtin_bswap16(hdr->key_data_len_be);
	memset(hdr->mic, 0, WPA_EAPOL_MIC_LEN);

	if (wk->descr_ver == 2) {
		u8 temp[WPA_SHA1_LEN];
		wpaHmacSha1(temp, st->ptk.kck, sizeof(st->ptk.kck), eapolhdr, totalsz);
		memcpy(hdr->mic, temp, WPA_EAPOL_MIC_LEN);
	} else {
		// XX: same thing but with HMAC-MD5
		dietPrint("[WPA] HMAC-MD5 not impl\n");
	}
}

static WlanIeHdr* _wpaRsnSnagGtk(u8* key_data, unsigned data_len)
{
	while (data_len > sizeof(WlanIeHdr)) {
		WlanIeHdr* ie = (WlanIeHdr*)key_data;
		data_len -= sizeof(WlanIeHdr);

		if (ie->len > data_len) {
			break;
		}

		key_data = &ie->data[ie->len];
		data_len -= ie->len;

		if (ie->id == WlanEid_Vendor && ie->len >= 0x16 &&
			ie->data[0] == 0x00 && ie->data[1] == 0x0f && ie->data[2] == 0xac && ie->data[3] == 0x01) {
			return ie; // Found it!
		}
	}

	return NULL;
}

MK_NOINLINE static NetBuf* _wpaEapolCreateReplyPacket(WpaState* st, WpaWork* wk, size_t keydatalen)
{
	NetBuf* pPacket = st->cb_alloc_packet(st, sizeof(WpaEapolHdr) + sizeof(WpaEapolKeyHdr) + keydatalen);
	if (!pPacket) {
		dietPrint("[WPA] Bad packet alloc\n");
		return NULL;
	}

	// Fill in EAPOL header
	WpaEapolHdr* eapolhdr = netbufGet(pPacket);
	eapolhdr->protocol_version = wk->eapolhdr->protocol_version;
	eapolhdr->packet_type = wk->eapolhdr->packet_type;
	eapolhdr->packet_body_len_be = __builtin_bswap16(pPacket->len - sizeof(WpaEapolHdr));
	wk->replyeapol = eapolhdr;

	// Fill in EAPOL-Key header
	WpaEapolKeyHdr* hdr = (WpaEapolKeyHdr*)(eapolhdr+1);
	memset(hdr, 0, sizeof(*hdr));
	hdr->descr_type = wk->hdr->descr_type;
	hdr->key_info_be = __builtin_bswap16(
		wk->descr_ver | WPA_EAPOL_KEY_MIC |
		(wk->key_info & (
			WPA_EAPOL_KEY_TYPE_MASK|WPA_EAPOL_OLD_KEY_IDX_MASK|
			WPA_EAPOL_SECURE|WPA_EAPOL_ERROR|WPA_EAPOL_REQUEST
		))
	);
	memcpy(hdr->key_replay_cnt, wk->hdr->key_replay_cnt, WPA_EAPOL_REPLAY_LEN);
	hdr->key_data_len_be = __builtin_bswap16(keydatalen);
	wk->reply = hdr;

	// Prepend MAC header
	MK_ASSUME(pPacket->pos >= sizeof(NetMacHdr));
	NetMacHdr* machdr = netbufPushHeaderType(pPacket, NetMacHdr);
	memcpy(machdr->dst_mac, wk->machdr->src_mac, 6);
	memcpy(machdr->src_mac, wk->machdr->dst_mac, 6);
	machdr->len_or_ethertype_be = wk->machdr->len_or_ethertype_be;

	return pPacket;
}

MK_INLINE bool _wpaEapolCreateAndSendReplyPacket(WpaState* st, WpaWork* wk)
{
	NetBuf* pPacket = _wpaEapolCreateReplyPacket(st, wk, 0);
	if (!pPacket) {
		return false;
	}

	_wpaEapolCalcMic(st, wk, wk->replyeapol);
	st->cb_tx(st, pPacket);
	return true;
}

MK_NOINLINE static void _wpaPairwiseHandshake12(WpaState* st, WpaWork* wk)
{
	dietPrint("[WPA] Handshake 1/4\n");

	// Obtain nonce from the AP
	memcpy(st->anonce, wk->hdr->key_nonce, WPA_EAPOL_NONCE_LEN);

	// Generate a nonce for ourselves
	alignas(4) u8 snonce[WPA_EAPOL_NONCE_LEN];
	wpaGenerateEapolNonce(snonce);

	// Initialize the pad used to generate the PTK
	WpaPtkPad pad;
	memcpy(pad.magic, "Pairwise key expansion", sizeof(pad.magic));
	pad.zero = 0;

	// Copy the BSSIDs
	if (memcmp(wk->machdr->dst_mac, wk->machdr->src_mac, 6) < 0) {
		memcpy(pad.min_bssid, wk->machdr->dst_mac, 6);
		memcpy(pad.max_bssid, wk->machdr->src_mac, 6);
	} else {
		memcpy(pad.min_bssid, wk->machdr->src_mac, 6);
		memcpy(pad.max_bssid, wk->machdr->dst_mac, 6);
	}

	// Copy the nonces
	if (memcmp(snonce, wk->hdr->key_nonce, WPA_EAPOL_NONCE_LEN) < 0) {
		memcpy(pad.min_nonce, snonce, WPA_EAPOL_NONCE_LEN);
		memcpy(pad.max_nonce, wk->hdr->key_nonce, WPA_EAPOL_NONCE_LEN);
	} else {
		memcpy(pad.min_nonce, wk->hdr->key_nonce, WPA_EAPOL_NONCE_LEN);
		memcpy(pad.max_nonce, snonce, WPA_EAPOL_NONCE_LEN);
	}

	// Calculate the PTK
	wpaPseudoRandomFunction(&st->ptk, sizeof(st->ptk), st->pmk, sizeof(st->pmk), &pad, sizeof(pad));

	// Prepare packet 2
	NetBuf* pPacket = _wpaEapolCreateReplyPacket(st, wk, st->ie_len);
	if (!pPacket) {
		return;
	}

	// Copy data to packet 2 and calc MIC
	WpaEapolKeyHdr* hdr = wk->reply;
	memcpy(hdr->key_nonce, snonce, WPA_EAPOL_NONCE_LEN);
	memcpy(hdr+1, st->ie_data, st->ie_len);
	_wpaEapolCalcMic(st, wk, wk->replyeapol);

	// Send packet 2
	st->cb_tx(st, pPacket);
}

MK_NOINLINE static void _wpaPairwiseHandshake34(WpaState* st, WpaWork* wk)
{
	dietPrint("[WPA] Handshake 3/4\n");

	if (memcmp(st->anonce, wk->hdr->key_nonce, WPA_EAPOL_NONCE_LEN) != 0) {
		dietPrint("[WPA] nonce error\n");
		return;
	}

	// Snag the GTK now if using WPA2
	WlanIeHdr* gtk_ie = NULL;
	if (wk->is_rsn) {
		gtk_ie = _wpaRsnSnagGtk((u8*)wk->key_data, wk->key_data_len);
		if (!gtk_ie) {
			dietPrint("[WPA2] No GTK found\n");
			return;
		}

		// Obtain RSC
		memcpy(&st->rsc, wk->hdr->key_rsc, WPA_RSC_LEN);
	}

	// Send packet 4
	if (!_wpaEapolCreateAndSendReplyPacket(st, wk)) {
		return;
	}

	// Install pairwise key
	st->cb_install_key(st, false, 0, wk->descr_ver == 2 ? WPA_AES_BLOCK_LEN : sizeof(WpaKey));

	// Install group key if available
	if (gtk_ie) {
		u8* data = (u8*)(gtk_ie+1);
		memcpy(&st->gtk, &data[6], gtk_ie->len-6);
		st->cb_install_key(st, true, data[4], gtk_ie->len-6);
	}
}

MK_NOINLINE static void _wpaGroupHandshake(WpaState* st, WpaWork* wk)
{
	dietPrint("[WPA] Group renew 1/2\n");

	// Obtain GTK from the keydata
	unsigned gtk_idx;
	unsigned gtk_len;
	if (wk->is_rsn) {
		WlanIeHdr* gtk_ie = _wpaRsnSnagGtk((u8*)wk->key_data, wk->key_data_len);
		if (!gtk_ie) {
			dietPrint("[WPA2] No GTK found\n");
			return;
		}

		u8* data = (u8*)(gtk_ie+1);
		gtk_idx = data[4];
		gtk_len = gtk_ie->len-6;
		memcpy(&st->gtk, &data[6], gtk_len);
	} else {
		gtk_idx = WPA_EAPOL_OLD_KEY_IDX(wk->key_info);
		gtk_len = wk->key_data_len;
		memcpy(&st->gtk, wk->key_data, gtk_len);
	}

	// Obtain RSC
	memcpy(&st->rsc, wk->hdr->key_rsc, WPA_RSC_LEN);

	// Send response packet
	if (!_wpaEapolCreateAndSendReplyPacket(st, wk)) {
		return;
	}

	// Install group key
	st->cb_install_key(st, true, gtk_idx, gtk_len);
}

static void _wpaEapolRx(WpaState* st, NetBuf* pPacket)
{
	WpaWork wk;

	wk.machdr = netbufPopHeaderType(pPacket, NetMacHdr);
	if (!wk.machdr) {
		dietPrint("[WPA] Missing MAC header\n");
		return;
	}

	wk.eapolhdr = netbufPopHeaderType(pPacket, WpaEapolHdr);
	if (!wk.eapolhdr) {
		dietPrint("[WPA] Missing EAPOL header\n");
		return;
	}

	// XX: validate EAPOL version/packet type?

	if (pPacket->len < __builtin_bswap16(wk.eapolhdr->packet_body_len_be)) {
		dietPrint("[WPA] Truncated packet\n");
		return;
	}

	wk.hdr = netbufPopHeaderType(pPacket, WpaEapolKeyHdr);
	if (!wk.hdr) {
		dietPrint("[WPA] Missing EAPOL-Key header\n");
		return;
	}

	switch (wk.hdr->descr_type) {
		default:
			dietPrint("[WPA] Bad EAPOL-Key descr %.2X\n", wk.hdr->descr_type);
			return;

		case WpaEapolDescrType_RSN: // aka WPA2
			wk.is_rsn = true;
			break;

		case WpaEapolDescrType_WPA:
			wk.is_rsn = false;
			break;
	}

	/*
	if (wk.is_rsn) {
		dietPrint("[WPA] Using WPA2/RSN\n");
	}
	*/

	wk.key_info = __builtin_bswap16(wk.hdr->key_info_be);
	wk.descr_ver = WPA_EAPOL_DESCR_VER(wk.key_info);
	wk.key_data_len = __builtin_bswap16(wk.hdr->key_data_len_be);

	if (wk.descr_ver != 1 && wk.descr_ver != 2) {
		dietPrint("[WPA] Bad EAPOL-Key ver %u\n", wk.descr_ver);
		return;
	}

	if (!(wk.key_info & WPA_EAPOL_KEY_ACK)) {
		dietPrint("[WPA] Bad EAPOL-Key direction\n");
		return;
	}

	if (pPacket->len < wk.key_data_len) {
		dietPrint("[WPA] Truncated packet\n");
		return;
	}

	if (wk.key_info & WPA_EAPOL_KEY_MIC) {
		// Verify MIC
		u8 micbkup[WPA_EAPOL_MIC_LEN];
		memcpy(micbkup, wk.hdr->mic, WPA_EAPOL_MIC_LEN);
		_wpaEapolCalcMic(st, &wk, wk.eapolhdr);
		if (memcmp(micbkup, wk.hdr->mic, WPA_EAPOL_MIC_LEN) != 0) {
			dietPrint("[WPA] Invalid EAPOL MIC\n");
			return;
		}
	}

	// Verify replay
	u64 new_replay = _wpaDecodeReplayCounter(wk.hdr->key_replay_cnt);
	if ((s64)(new_replay - st->replay) <= 0) {
		dietPrint("[WPA] EAPOL replay detected\n");
		return;
	}
	st->replay = new_replay;

	bool is_keydata_encrypted;
	if (wk.is_rsn) {
		// WPA2: Keydata is encrypted if the "encrypted" flag is set
		is_keydata_encrypted = (wk.key_info & WPA_EAPOL_ENCRYPTED) != 0;
	} else {
		// WPA: Keydata is encrypted if it's a group key
		is_keydata_encrypted = (wk.key_info & WPA_EAPOL_KEY_TYPE_MASK) == WPA_EAPOL_KEY_TYPE_GROUP;
	}

	if (wk.key_data_len && is_keydata_encrypted) {
		// Decrypt keydata
		wk.key_data = netbufGet(pPacket);

		if (wk.descr_ver == 2) {
			// Apply AES-Key-Unwrap
			wk.key_data_len -= WPA_AES_WRAP_BLK_LEN;
			if (!wpaAesUnwrap(st->ptk.kek, wk.key_data, wk.key_data, wk.key_data_len/WPA_AES_WRAP_BLK_LEN)) {
				dietPrint("[WPA] Corrupted keydata\n");
				return;
			}
		} else {
			// XX: Apply RC4
			dietPrint("[WPA] RC4 decrypt not impl\n");
			return;
		}
	}

	switch (wk.key_info & (WPA_EAPOL_KEY_TYPE_MASK|WPA_EAPOL_KEY_MIC)) {
		default: {
			dietPrint("[WPA] Bad EAPOL-Key state %.4X\n", wk.key_info);
			break;
		}

		case WPA_EAPOL_KEY_TYPE_PAIRWISE: {
			_wpaPairwiseHandshake12(st, &wk);
			break;
		}

		case WPA_EAPOL_KEY_TYPE_PAIRWISE | WPA_EAPOL_KEY_MIC: {
			_wpaPairwiseHandshake34(st, &wk);
			break;
		}

		case WPA_EAPOL_KEY_TYPE_GROUP | WPA_EAPOL_KEY_MIC: {
			_wpaGroupHandshake(st, &wk);
			break;
		}
	}
}

void wpaPrepare(WpaState* st)
{
	*st = (WpaState){0};
	mailboxPrepare(&st->mbox, &st->mbox_storage, 1);
}

void wpaReset(WpaState* st, const void* pmk)
{
	memcpy(st->pmk, pmk, WLAN_WPA_PSK_LEN);
	st->replay = UINT64_MAX;
}

// nbwifi: in place of the supplicant thread, an EAPOL packet is handled as soon
// as it arrives (and freed)
void wpaEapolProcess(WpaState* st, NetBuf* pPacket)
{
	_wpaEapolRx(st, pPacket);
	netbufFree(pPacket);
}
