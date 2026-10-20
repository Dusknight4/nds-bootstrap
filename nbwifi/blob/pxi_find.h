// pxi_find.h: finds a DS game's ARM7 PXI dispatch table (PXI tag -> handler; the WM entry is
// [10]) by the code of the SDK's FIFO receive handler, the same in Pokemon Platinum and HeartGold:
//   ldr r3, [r4, r0, lsl #2]; cmp r3, #0; beq +5; lsl r2, r6, #26; lsr r1, r6, #6; lsr r2, r2, #31;
//   mov lr, pc; bx r3
// whose literal pool, after the function's "bx lr", holds the table's address and then
// REG_IPC_FIFO_CR's (04000184). Platinum (US, the user's build): handler at 037FB62C, table
// 03806EEC; HeartGold (US, the user's build): 037FB4C4, 03806DFC. Shared by the blob and its test.
#ifndef PXI_FIND_H
#define PXI_FIND_H

#include <stdint.h>

#define PXI_FIND_START 0x037F8000u     // the ARM7's shared WRAM, then its own WRAM
#define PXI_FIND_END   0x03810000u

static const uint32_t s_pxiDispatch[8] = {
	0xE7943100, 0xE3530000, 0x0A000005, 0xE1A02D06, 0xE1A01326, 0xE1A02FA2, 0xE1A0E00F, 0xE12FFF13,
};

// the table, if the handler's dispatch is at a (0 if not); *literal: where the table's address is
static inline uint32_t pxiTableAt(uint32_t a, uint32_t (*read32)(uint32_t), uint32_t* literal)
{
	for (int i = 0; i < 8; i++)
		if (read32(a + 4 * i) != s_pxiDispatch[i]) return 0;
	// (the pool is 0x4C bytes on in both games; looked for in the 0x80 after the dispatch)
	for (uint32_t p = a + 32; p < a + 32 + 0x80 && p + 8 < PXI_FIND_END; p += 4) {
		if (read32(p) != 0xE12FFF1E || read32(p + 8) != 0x04000184) continue;
		const uint32_t t = read32(p + 4);
		if (t < PXI_FIND_START || t + 4 * 32 > PXI_FIND_END || (t & 3)) return 0;
		if (literal) *literal = p + 4;
		return t;
	}
	return 0;
}

#endif
