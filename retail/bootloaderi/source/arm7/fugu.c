// Nintendo-style Blowfish ("Fugu"), from calico (devkitPro), SPDX ZPL-2.1, Copyright fincs.
#include "fugu.h"

static inline u32 _fuguSubstitute(FuguState const* state, u32 x)
{
	u32 h = state->S[0][x>>24] + state->S[1][(x>>16)&0xff];
	return (h ^ state->S[2][(x>>8)&0xff]) + state->S[3][x&0xff];
}

static inline void _fuguSwap(u32* L, u32* R)
{
	u32 temp = *L;
	*L = *R;
	*R = temp;
}

void fuguKeySchedule(FuguState* state, const u32* key, unsigned key_num_words)
{
	for (unsigned i = 0, p = 0; i < FUGU_NUM_ROUNDS+2; i ++) {
		state->P[i] ^= __builtin_bswap32(key[p++]);
		if (p>=key_num_words) p = 0;
	}

	u32 pad[2] = { 0, 0 };
	u32* out = (u32*)state;
	for (unsigned i = 0; i < sizeof(*state)/8; i ++) {
		fuguEncrypt(state, pad);
		out[0] = pad[1];
		out[1] = pad[0];
		out += 2;
	}
}

void fuguEncrypt(FuguState const* state, u32 buf[2])
{
	u32 L = buf[1];
	u32 R = buf[0];

	for (unsigned i = 0; i < FUGU_NUM_ROUNDS; i ++) {
		L ^= state->P[i];
		R ^= _fuguSubstitute(state, L);
		_fuguSwap(&L, &R);
	}

	buf[0] = L ^ state->P[FUGU_NUM_ROUNDS+0];
	buf[1] = R ^ state->P[FUGU_NUM_ROUNDS+1];
}

void fuguDecrypt(FuguState const* state, u32 buf[2])
{
	u32 L = buf[1];
	u32 R = buf[0];

	for (unsigned i = FUGU_NUM_ROUNDS+1; i > 1; i --) {
		L ^= state->P[i];
		R ^= _fuguSubstitute(state, L);
		_fuguSwap(&L, &R);
	}

	buf[0] = L ^ state->P[1];
	buf[1] = R ^ state->P[0];
}
