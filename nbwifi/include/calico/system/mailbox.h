// nbwifi: stand-in for calico's mailbox (a thread message queue). nbwifi runs
// single-threaded and polls, so a mailbox here is a plain ring of words that
// is only ever read by polling: nothing blocks.
// SPDX-License-Identifier: ZPL-2.1
#pragma once
#include "../types.h"

MK_EXTERN_C_START

typedef struct Mailbox {
	u32* slots;
	u8 num_slots;
	u8 cur_slot;
	u8 pending_slots;
	u8 _pad;
} Mailbox;

MK_INLINE void mailboxPrepare(Mailbox* mb, u32* slots, unsigned num_slots)
{
	mb->slots = slots;
	mb->num_slots = num_slots;
	mb->cur_slot = 0;
	mb->pending_slots = 0;
}

MK_INLINE bool mailboxTrySend(Mailbox* mb, u32 message)
{
	if (mb->pending_slots >= mb->num_slots) return false;
	unsigned slot = mb->cur_slot + mb->pending_slots;
	if (slot >= mb->num_slots) slot -= mb->num_slots;
	mb->slots[slot] = message;
	mb->pending_slots++;
	return true;
}

MK_INLINE bool mailboxTryRecv(Mailbox* mb, u32* out)
{
	if (!mb->pending_slots) return false;
	*out = mb->slots[mb->cur_slot];
	if (++mb->cur_slot >= mb->num_slots) mb->cur_slot = 0;
	mb->pending_slots--;
	return true;
}

MK_EXTERN_C_END
