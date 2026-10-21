// Nintendo-style Blowfish ("Fugu"), from calico (devkitPro), SPDX ZPL-2.1, Copyright fincs.
// Unchanged apart from building without calico's headers.
#pragma once
#include <nds/ndstypes.h>
#include <string.h>

#define FUGU_NUM_ROUNDS 16

typedef struct FuguState {
	u32 P[FUGU_NUM_ROUNDS+2];
	u32 S[4][256];
} FuguState;

typedef struct FuguNtr {
	FuguState state;
	u32 key[3];
} FuguNtr;

void fuguKeySchedule(FuguState* state, const u32* key, unsigned key_num_words);
void fuguEncrypt(FuguState const* state, u32 buf[2]);
void fuguDecrypt(FuguState const* state, u32 buf[2]);

static inline void fuguNtrInit(FuguNtr* ctx, const void* initial_state, u32 key)
{
	memcpy(&ctx->state, initial_state, sizeof(ctx->state));
	ctx->key[0] = key;
	ctx->key[1] = key>>1;
	ctx->key[2] = key<<1;
}

static inline void fuguNtrKeySchedule(FuguNtr* ctx, bool full_key, bool do_tweak)
{
	if (do_tweak) {
		ctx->key[1] <<= 1;
		ctx->key[2] >>= 1;
	}
	fuguEncrypt(&ctx->state, &ctx->key[1]);
	fuguEncrypt(&ctx->state, &ctx->key[0]);
	fuguKeySchedule(&ctx->state, ctx->key, full_key ? 3 : 2);
}
