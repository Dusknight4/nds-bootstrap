// nbwifi: the TMIO (SD/SDIO host controller) driver, after calico's
// source/dev/tmio.c (SPDX-License-Identifier: ZPL-2.1, Copyright fincs,
// devkitPro), made synchronous: a transaction is issued and then polled to
// completion, with no thread, no interrupts and no DMA. The SDIO card
// interrupt is left off; nbwifi polls the device instead.
#include <calico/types.h>
#include <calico/arm/common.h>
#include <calico/dev/tmio.h>
#include "nbw_internal.h"
#include "nbwifi.h"

#define REG_TMIO(_type,_name) (*(_type volatile*)(ctl->reg_base + TMIO_##_name))

#define REG_TMIO_CMD        REG_TMIO(u16, CMD)
#define REG_TMIO_PORTSEL    REG_TMIO(u16, PORTSEL)
#define REG_TMIO_CMDARGLO   REG_TMIO(u16, CMDARG+0)
#define REG_TMIO_CMDARGHI   REG_TMIO(u16, CMDARG+2)
#define REG_TMIO_STOP       REG_TMIO(u16, STOP)
#define REG_TMIO_BLKCNT     REG_TMIO(u16, BLKCNT)
#define REG_TMIO_CMDRESP    REG_TMIO(TmioResp, CMDRESP)
#define REG_TMIO_STAT       REG_TMIO(u32, STAT)
#define REG_TMIO_STATLO     REG_TMIO(u16, STAT+0)
#define REG_TMIO_STATHI     REG_TMIO(u16, STAT+2)
#define REG_TMIO_MASKLO     REG_TMIO(u16, MASK+0)
#define REG_TMIO_MASKHI     REG_TMIO(u16, MASK+2)
#define REG_TMIO_CLKCTL     REG_TMIO(u16, CLKCTL)
#define REG_TMIO_BLKLEN     REG_TMIO(u16, BLKLEN)
#define REG_TMIO_OPTION     REG_TMIO(u16, OPTION)
#define REG_TMIO_FIFO16     REG_TMIO(u16, FIFO16)
#define REG_TMIO_SDIOCTL    REG_TMIO(u16, SDIOCTL)
#define REG_TMIO_SDIO_MASK  REG_TMIO(u16, SDIO_MASK)
#define REG_TMIO_FIFOCTL    REG_TMIO(u16, FIFOCTL)
#define REG_TMIO_RESET      REG_TMIO(u16, RESET)
#define REG_TMIO_EXT_SDIO   REG_TMIO(u16, EXT_SDIO)
#define REG_TMIO_EXT_MASKLO REG_TMIO(u16, EXT_MASK+0)
#define REG_TMIO_EXT_MASKHI REG_TMIO(u16, EXT_MASK+2)
#define REG_TMIO_CNT32      REG_TMIO(u16, CNT32)
#define REG_TMIO_BLKLEN32   REG_TMIO(u16, BLKLEN32)
#define REG_TMIO_BLKCNT32   REG_TMIO(u16, BLKCNT32)

#define ERROR_IRQ_BITS ( \
	TMIO_STAT_BAD_CMD_INDEX | \
	TMIO_STAT_BAD_CRC | \
	TMIO_STAT_BAD_STOP_BIT | \
	TMIO_STAT_DATA_TIMEOUT | \
	TMIO_STAT_RX_OVERFLOW | \
	TMIO_STAT_TX_UNDERRUN | \
	TMIO_STAT_CMD_TIMEOUT | \
	TMIO_STAT_ILL_ACCESS )

#define TX_IRQ_BITS ( \
	TMIO_STAT_CMD_RESPEND | \
	TMIO_STAT_CMD_DATAEND | \
	ERROR_IRQ_BITS )

// how long a transaction may take before it's abandoned, in polling rounds
#define TMIO_POLL_LIMIT 2000000
void nbwWaitLoop(u32 count);    // nbw_asm.s: BIOS WaitByLoop, 4 ARM7 cycles a count

bool tmioInit(TmioCtl* ctl, uptr reg_base, uptr fifo_base, u32* mbox_slots, unsigned num_mbox_slots)
{
	*ctl = (TmioCtl){0};
	ctl->reg_base = reg_base;
	ctl->fifo_base = fifo_base;

	// fails fast if the controller is inaccessible (SCFG) or has no ports
	unsigned num_ports = TMIO_PORTSEL_NUMPORTS(REG_TMIO_PORTSEL);
	if (num_ports == 0) {
		return false;
	}

	ctl->cur_port.clock = 0xffff;
	ctl->cur_port.num = 0xff;
	ctl->cur_port.width = 0xff;
	mailboxPrepare(&ctl->mbox, mbox_slots, num_mbox_slots);

	// soft reset, then everything masked: nbwifi polls
	REG_TMIO_RESET &= ~1;
	REG_TMIO_RESET |= 1;
	REG_TMIO_MASKLO = ~0;
	REG_TMIO_MASKHI = ~0;
	REG_TMIO_SDIOCTL = 0;
	REG_TMIO_SDIO_MASK = ~0;
	REG_TMIO_EXT_SDIO = 7<<8;
	REG_TMIO_EXT_MASKLO = ~0;
	REG_TMIO_EXT_MASKHI = ~0;

	REG_TMIO_CNT32 = TMIO_CNT32_ENABLE | TMIO_CNT32_FIFO_CLEAR;
	REG_TMIO_FIFOCTL = TMIO_FIFOCTL_MODE32;
	REG_TMIO_OPTION = TMIO_OPTION_DETECT(14) | TMIO_OPTION_TIMEOUT(14) | TMIO_OPTION_NO_C2;

	return true;
}

void tmioSetPortCardIrqHandler(TmioCtl* ctl, unsigned port, TmioCardIrqHandler isr)
{
	// card interrupts stay off: the device is polled
	if (port == 0) ctl->port_isr[0].cardirq = isr;
}

void tmioAckPortCardIrq(TmioCtl* ctl, unsigned port)
{
	(void)ctl; (void)port;
}

static bool tmioTransact_(TmioCtl* ctl, TmioTx* tx);

// (timed, for the blob's log: how long the WiFi chip's SDIO transactions hold the ARM7)
bool tmioTransact(TmioCtl* ctl, TmioTx* tx)
{
	const unsigned short v0 = *(volatile unsigned short*)0x04000006;
	const bool ok = tmioTransact_(ctl, tx);
	const unsigned l = nbwLinesSince(v0);
	g_nbwProf.sdioN++; g_nbwProf.sdioSum += l; if (l > g_nbwProf.sdioMax) g_nbwProf.sdioMax = l;
	return ok;
}

static bool tmioTransact_(TmioCtl* ctl, TmioTx* tx)
{
	tx->status = TMIO_STAT_CMD_BUSY;
	ctl->cur_tx = tx;
	unsigned cmd_id = tx->type & 0x3f;

	// the controller must be idle
	// (the waits run in the BIOS: the polling loop alone, fetched from main RAM, took the
	// main RAM bus from the ARM9 and the display: graphics glitches)
	for (unsigned i = 0; (REG_TMIO_STAT & TMIO_STAT_CMD_BUSY) && i < TMIO_POLL_LIMIT / 8; i ++) { nbwWaitLoop(8); }

	if (tx->callback) {
		tx->callback(ctl, tx);
	}

	// port, bus width and clock, when they change
	if (ctl->cur_port.num != tx->port.num) {
		ctl->cur_port.num = tx->port.num;
		REG_TMIO_PORTSEL = (REG_TMIO_PORTSEL &~ TMIO_PORTSEL_PORT(3)) | TMIO_PORTSEL_PORT(ctl->cur_port.num);
	}
	if (ctl->cur_port.width != tx->port.width) {
		ctl->cur_port.width = tx->port.width;
		REG_TMIO_OPTION = (REG_TMIO_OPTION &~ TMIO_OPTION_BUS_WIDTH1) | (ctl->cur_port.width == 1 ? TMIO_OPTION_BUS_WIDTH1 : TMIO_OPTION_BUS_WIDTH4);
	}
	if (ctl->cur_port.clock != tx->port.clock) {
		ctl->cur_port.clock = tx->port.clock;
		u16 clkmask = TMIO_CLKCTL_DIV(0xff) | TMIO_CLKCTL_AUTO;
		u16 clk0 = REG_TMIO_CLKCTL &~ TMIO_CLKCTL_ENABLE;
		u16 clk1 = (clk0 &~ clkmask) | (ctl->cur_port.clock & clkmask);
		REG_TMIO_CLKCTL = clk0;
		REG_TMIO_CLKCTL = clk1;
		REG_TMIO_CLKCTL = clk1 | TMIO_CLKCTL_ENABLE;
	}

	// GO_IDLE needs 1 ms between applying the clock and the command
	if (cmd_id == 0) {
		nbwSleepUs(1000);
	}

	// block transfer FIFO
	bool use32 = false, use16 = false;
	if (tx->type & TMIO_CMD_TX) {
		u16 stop = REG_TMIO_STOP &~ (TMIO_STOP_DO_STOP|TMIO_STOP_AUTO_STOP);
		if (tx->type & TMIO_CMD_TX_MULTI) {
			stop |= TMIO_STOP_AUTO_STOP;
		}
		REG_TMIO_STOP = stop;
		REG_TMIO_BLKLEN = tx->block_size;
		REG_TMIO_BLKCNT = tx->num_blocks;
		REG_TMIO_BLKLEN32 = tx->block_size;
		REG_TMIO_BLKCNT32 = tx->num_blocks;

		if (tx->xfer_isr) {
			bool aligned = (((uptr)tx->user | tx->block_size) & 3) == 0;
			if (tx->block_size >= 0x80 && aligned) {
				use32 = true;
				REG_TMIO_CNT32 |= TMIO_CNT32_FIFO_CLEAR;
			} else {
				// 16-bit FIFO, also for small transfers (hardware bug #2 in tmio.h)
				use16 = true;
				REG_TMIO_CNT32 &= ~TMIO_CNT32_ENABLE;
				REG_TMIO_FIFOCTL = 0;
				REG_TMIO_STATHI = (u16)~((TMIO_STAT_FIFO16_RECV|TMIO_STAT_FIFO16_SEND)>>16);
			}
			ctl->num_pending_blocks = tx->num_blocks;
		}
	}
	if (!use32 && !use16) {
		ctl->num_pending_blocks = 0;
	}

	// the command
	REG_TMIO_STATLO = (u16)~TX_IRQ_BITS;
	REG_TMIO_STATHI = (u16)((~TX_IRQ_BITS)>>16);
	REG_TMIO_CMDARGLO = tx->arg;
	REG_TMIO_CMDARGHI = tx->arg >> 16;
	REG_TMIO_CMD = tx->type;

	// poll it to completion
	const bool reading = (tx->type & TMIO_CMD_TX_READ) != 0;
	bool done = false;
	for (unsigned i = 0; i < TMIO_POLL_LIMIT && !done; i ++) {
		if (i) nbwWaitLoop(2);
		if (ctl->num_pending_blocks) {
			bool ready;
			if (use32) {
				u16 cnt = REG_TMIO_CNT32;
				ready = reading ? (cnt & TMIO_CNT32_STAT_RECV) != 0 : (cnt & TMIO_CNT32_STAT_NOT_SEND) == 0;
			} else {
				u16 bit = (reading ? TMIO_STAT_FIFO16_RECV : TMIO_STAT_FIFO16_SEND) >> 16;
				ready = (REG_TMIO_STATHI & bit) != 0;
				if (ready) REG_TMIO_STATHI = (u16)~bit;
			}
			if (ready) {
				tx->xfer_isr(ctl, tx);
				ctl->num_pending_blocks --;
			}
		}

		u32 bits = REG_TMIO_STAT & TX_IRQ_BITS;
		if (bits) {
			REG_TMIO_STATLO = (u16)~bits;
			REG_TMIO_STATHI = (u16)((~bits)>>16);
			tx->status |= bits;
		}

		if (tx->status & ERROR_IRQ_BITS) {
			ctl->num_pending_blocks = 0;
			REG_TMIO_CNT32 |= TMIO_CNT32_FIFO_CLEAR;
			tx->status &= ERROR_IRQ_BITS;
			break;
		}

		if (bits & TMIO_STAT_CMD_RESPEND) {
			switch (tx->type & TMIO_CMD_RESP_MASK) {
				default: break;
				case TMIO_CMD_RESP_48:
				case TMIO_CMD_RESP_48_BUSY:
				case TMIO_CMD_RESP_48_NOCRC:
					tx->resp.value[0] = REG_TMIO_CMDRESP.value[0];
					break;
				case TMIO_CMD_RESP_136: {
					TmioResp resp = REG_TMIO_CMDRESP;
					unsigned id = TMIO_CMD_INDEX(REG_TMIO_CMD);
					if (id != 2 && id != 10) {
						tx->resp.value[0] = (resp.value[0]<<8);
						tx->resp.value[1] = (resp.value[1]<<8) | (resp.value[0]>>24);
						tx->resp.value[2] = (resp.value[2]<<8) | (resp.value[1]>>24);
						tx->resp.value[3] = (resp.value[3]<<8) | (resp.value[2]>>24);
					} else {
						tx->resp = resp;
					}
					break;
				}
			}
		}

		// done once the response is in and every block has moved (DATAEND
		// is unreliable on TMIO, as calico notes)
		if ((tx->status & TMIO_STAT_CMD_RESPEND) && !ctl->num_pending_blocks) {
			tx->status &= ERROR_IRQ_BITS;
			done = true;
		}
	}
	if (!done && !(tx->status & ERROR_IRQ_BITS)) {
		tx->status = TMIO_STAT_CMD_TIMEOUT;   // gave up waiting
		REG_TMIO_CNT32 |= TMIO_CNT32_FIFO_CLEAR;
	}

	// back to the 32-bit FIFO
	if (use16) {
		REG_TMIO_STATHI = (u16)~((TMIO_STAT_FIFO16_RECV|TMIO_STAT_FIFO16_SEND)>>16);
		REG_TMIO_CNT32 |= TMIO_CNT32_ENABLE;
		REG_TMIO_FIFOCTL = TMIO_FIFOCTL_MODE32;
	}

	if (tx->callback) {
		tx->callback(ctl, tx);
	}
	ctl->cur_tx = NULL;
	return tx->status == 0;
}

typedef union _TmioXferBuf {
	u32* buf32;
	u8* buf8;
	void* buf;
} _TmioXferBuf;

void tmioXferRecvByCpu(TmioCtl* ctl, TmioTx* tx)
{
	_TmioXferBuf u = { tx->user };
	if (REG_TMIO_CNT32 & TMIO_CNT32_ENABLE) {
		vu32* fifo = (vu32*)ctl->fifo_base;
		for (u32 i = 0; i < tx->block_size; i += 4) {
			*u.buf32++ = *fifo;
		}
	} else {
		for (u32 i = 0; i < tx->block_size; i += 2) {
			u16 data = REG_TMIO_FIFO16;
			*u.buf8++ = data & 0xff;
			if ((i+1) < tx->block_size) {
				*u.buf8++ = (data >> 8);
			}
		}
	}
	tx->user = u.buf;
}

void tmioXferSendByCpu(TmioCtl* ctl, TmioTx* tx)
{
	_TmioXferBuf u = { tx->user };
	if (REG_TMIO_CNT32 & TMIO_CNT32_ENABLE) {
		vu32* fifo = (vu32*)ctl->fifo_base;
		for (u32 i = 0; i < tx->block_size; i += 4) {
			*fifo = *u.buf32++;
		}
	} else {
		for (u32 i = 0; i < tx->block_size; i += 2) {
			u16 data = *u.buf8++;
			if ((i+1) < tx->block_size) {
				data |= (*u.buf8++) << 8;
			}
			REG_TMIO_FIFO16 = data;
		}
	}
	tx->user = u.buf;
}

unsigned tmioDecodeTranSpeed(u8 tran_speed)
{
	static const u16 units[] = { 10, 100, 1000, 10000 };
	static const u8 multipliers[] = { 0, 10, 12, 13, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 70, 80 };
	u8 unit = tran_speed & 7;
	u8 multiplier = (tran_speed >> 3) & 0xF;
	if (unit < 4) {
		return 1000*units[unit]*multipliers[multiplier];
	}
	return 0;
}
