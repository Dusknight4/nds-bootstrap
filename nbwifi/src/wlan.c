// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/dev/netbuf.h>
#include <calico/dev/wlan.h>
#include <string.h>

static const u8 s_wlanRates[] =
{
	2,   // 1mbps   (b/g)
	4,   // 2mbps   (b/g)
	11,  // 5.5mbps (b/g)
	12,  // 6mbps   (g only)
	18,  // 9mbps   (g only)
	22,  // 11mbps  (b/g)
	24,  // 12mbps  (g only)
	36,  // 18mbps  (g only)
	48,  // 24mbps  (g only)
	72,  // 36mbps  (g only)
	96,  // 48mbps  (g only)
	108, // 54mbps  (g only)
};

unsigned wlanGetRateBit(unsigned rate)
{
	unsigned bit = 0, i = 0, j = sizeof(s_wlanRates);
	while (!bit && i < j) {
		unsigned k = (i+j)/2;
		unsigned cur = s_wlanRates[k];
		if (cur == rate) {
			bit = 1U<<k;
		} else if (cur < rate) {
			i = k+1;
		} else /* if (cur > rate) */ {
			j = k;
		}
	}

	return bit;
}

WlanBssDesc* wlanFindOrAddBss(WlanBssDesc* desc_table, unsigned* num_entries, void* bssid, unsigned rssi)
{
	unsigned i;
	for (i = 0; i < *num_entries; i ++) {
		if (memcmp(desc_table[i].bssid, bssid, 6) == 0) {
			if (rssi > desc_table[i].rssi) {
				// Newer beacon has stronger signal - update the old one
				break;
			} else {
				// Drop this beacon as it has a weaker signal
				return NULL;
			}
		}
	}

	if (i == WLAN_MAX_BSS_ENTRIES) {
		// Out of space
		return NULL;
	} else if (i == *num_entries) {
		// We have a new BSS
		(*num_entries) ++;
	}

	return &desc_table[i];
}

WlanIeHdr* wlanFindRsnOrWpaIe(void* rawdata, unsigned rawdata_len)
{
	u8* pos = (u8*)rawdata;
	WlanIeHdr* wpa = NULL;
	while (rawdata_len > sizeof(WlanIeHdr)) {
		WlanIeHdr* ie = (WlanIeHdr*)pos;
		rawdata_len -= 2;

		if (ie->len <= rawdata_len) {
			pos = &ie->data[ie->len];
			rawdata_len -= ie->len;
		} else {
			break;
		}

		if (ie->id == WlanEid_RSN) {
			return ie;
		}

		if (ie->id == WlanEid_Vendor && ie->len >= 4 && wlanDecodeOui(ie->data) == WLAN_OUI_MICROSOFT && ie->data[3] == 0x01) {
			wpa = ie;
		}
	}

	return wpa;
}

static unsigned _wlanParseRsn(const WlanIeRsn* rsn, unsigned len, unsigned oui)
{
	bool is_wpa2 = oui == WLAN_OUI_IEEE;
	if (len < sizeof(WlanIeRsn)) {
		return 0;
	}

	unsigned num_ciphers = wlanDecode16(rsn->num_pairwise_ciphers);
	if (wlanDecode16(rsn->version) != 1 || len < (sizeof(WlanIeRsn) + 4*num_ciphers)) {
		return 0;
	}

	// Extract the pairwise ciphers
	unsigned mask = 0;
	for (unsigned i = 0; i < num_ciphers; i ++) {
		unsigned c_oui  = wlanDecodeOui(&rsn->pairwise_ciphers[4*i]);
		unsigned c_type = rsn->pairwise_ciphers[4*i+3];
		if (c_oui != oui) {
			continue;
		}

		if (c_type == 2) {
			mask |= is_wpa2 ? (1U<<WlanBssAuthType_WPA2_PSK_TKIP) : (1U<<WlanBssAuthType_WPA_PSK_TKIP);
		} else if (c_type == 4) {
			mask |= is_wpa2 ? (1U<<WlanBssAuthType_WPA2_PSK_AES) : (1U<<WlanBssAuthType_WPA_PSK_AES);
		}
	}

	return mask;
}

void wlanParseBeacon(WlanBssDesc* desc, WlanBssExtra* extra, NetBuf* pPacket)
{
	WlanBeaconHdr* hdr = netbufPopHeaderType(pPacket, WlanBeaconHdr);
	if (!hdr) {
		return;
	}

	// Fill in basic info
	memset(desc, 0, sizeof(*desc));
	desc->ieee_caps = hdr->capabilities;

	// Clear extra data
	if (extra) {
		memset(extra, 0, sizeof(*extra));
	}

	bool has_rsn = false;
	while (pPacket->len) {
		WlanIeHdr* elhdr = netbufPopHeaderType(pPacket, WlanIeHdr);
		if (!elhdr) {
			break;
		}

		void* data = netbufPopHeader(pPacket, elhdr->len);
		if (!data) {
			break;
		}

		switch (elhdr->id) {
			default: {
				//dietPrint(" eid[%.2X] len=%u\n", elhdr->type, elhdr->len);
				break;
			}

			case WlanEid_SSID: {
				if (elhdr->len <= WLAN_MAX_SSID_LEN) {
					desc->ssid_len = elhdr->len;
					memcpy(desc->ssid, data, elhdr->len);
				}
				break;
			}

			case WlanEid_SupportedRates:
			case WlanEid_SupportedRatesEx: {
				u8* rates = (u8*)data;
				for (unsigned i = 0; i < elhdr->len; i ++) {
					unsigned rate = rates[i];
					unsigned is_basic = rate>>7;
					unsigned bit = wlanGetRateBit(rate & 0x7f);
					if (!bit) {
						bit = 1U<<15; // "unsupported"
					}

					desc->ieee_all_rates |= bit;
					if (is_basic) {
						desc->ieee_basic_rates |= bit;
					}
				}
				break;
			}

			case WlanEid_DSParamSet: {
				if (elhdr->len > 0) {
					desc->channel = *(u8*)data;
				}
				break;
			}

			case WlanEid_CFParamSet: {
				if (extra && elhdr->len >= sizeof(WlanIeCfp)) {
					extra->cfp = (WlanIeCfp*)data;
				}
				break;
			}

			case WlanEid_TIM: {
				if (extra && elhdr->len > sizeof(WlanIeTim)) {
					extra->tim = (WlanIeTim*)data;
					extra->tim_bitmap_sz = elhdr->len - sizeof(WlanIeTim);
				}
				break;
			}

			case WlanEid_RSN: {
				// Standard WPA2 information element
				desc->auth_mask |= _wlanParseRsn((WlanIeRsn*)data, elhdr->len, WLAN_OUI_IEEE);
				has_rsn = true;
				break;
			}

			case WlanEid_Vendor: {
				if (elhdr->len < 4) {
					break;
				}

				WlanIeVendor* vnd = (WlanIeVendor*)data;
				unsigned oui = wlanDecodeOui(vnd->oui);
				unsigned type = vnd->type;
				data = vnd->data;

				if (oui == WLAN_OUI_MICROSOFT && type == 0x01) {
					// WPA1 information element
					desc->auth_mask |= _wlanParseRsn((WlanIeRsn*)data, elhdr->len, oui);
					has_rsn = true;
				} else if (oui == WLAN_OUI_NINTENDO && type == 0x00) {
					// Nintendo information element
					if (extra && elhdr->len >= 4 + sizeof(WlanIeNin)) {
						WlanIeNin* nin = (WlanIeNin*)data;
						if (elhdr->len >= 4 + sizeof(WlanIeNin) + nin->data_sz) {
							extra->nin = nin;
						}
					}
				}

				break;
			}
		}
	}

	// If above did not detect a RSN/WPA information element...
	if (!has_rsn) {
		// Base it off the 'Privacy' capability bit
		if (desc->ieee_caps & (1U<<4)) {
			// XX: I don't think it's possible to detect WEP key length from
			// beacon info, so list them all in the mask just in case.
			desc->auth_mask = (1U<<WlanBssAuthType_WEP_40) | (1U<<WlanBssAuthType_WEP_104) | (1U<<WlanBssAuthType_WEP_128);
		} else {
			// Open network
			desc->auth_mask = 1U<<WlanBssAuthType_Open;
		}
	}
}
