// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include "common.h"

#define AR6K_BOARD_INIT_TIMEOUT 2000

static bool _ar6kDevReset(Ar6kDev* dev)
{
	// Soft reset the device
	if (!ar6kDevWriteRegDiag(dev, 0x00004000, 0x00000100)) {
		return false;
	}
	threadSleep(50000); // 50ms

	// Check reset cause
	u32 reset_cause = 0;
	if (!ar6kDevReadRegDiag(dev, 0x000040C0, &reset_cause)) {
		return false;
	}
	if ((reset_cause & 7) != 2) {
		return false;
	}

	return true;
}

bool ar6kDevInit(Ar6kDev* dev, SdioCard* sdio, void* workbuf)
{
	*dev = (Ar6kDev){0};
	dev->sdio = sdio;
	dev->workbuf = workbuf;

	if (sdio->manfid.code != 0x0271) {
		dietPrint("[AR6K] Bad SDIO manufacturer\n");
		return false;
	}

	// In case we are warmbooting the card and IRQs were enabled previously, disable them
	if (!_ar6kDevSetIrqEnable(dev, false)) {
		return false;
	}

	// Read chip ID
	if (!ar6kDevReadRegDiag(dev, 0x000040ec, &dev->chip_id)) {
		dietPrint("[AR6K] Cannot read chip ID\n");
		return false;
	}
	dietPrint("[AR6K] Chip ID = %.8lX\n", dev->chip_id);

	// Autodetect address of host interest area. Normally this would be retrieved
	// directly from a parameter in MM_ENV_TWL_WLFIRM_INFO, but this data may not
	// always be available. Instead, we will detect it based on chip identification.
	switch (sdio->manfid.id) {
		case 0x0200: // AR6002
			dev->hia_addr = 0x500400;
			break;
		case 0x0201: // AR6013/AR6014
			dev->hia_addr = 0x520000;
			break;
		default:
			dietPrint("[AR6K] Bad SDIO device ID\n");
			return false;
	}

	// Read board data initialized flag from HIA
	u32 hi_board_data_initialized = 0;
	if (!ar6kDevReadRegDiag(dev, dev->hia_addr+0x58, &hi_board_data_initialized)) {
		return false;
	}

	if (!hi_board_data_initialized) {
		// Board is not initialized, we are at BMI and we must upload Atheros firmware
		dietPrint("[AR6K] [!] Firmware not loaded\n");
	} else {
		// Board is initialized, we must soft-reset into BMI (keeping firmware uploaded)
		dietPrint("[AR6K] Soft resetting device\n");

		if (!_ar6kDevReset(dev)) {
			dietPrint("[AR6K] Failed to reset device\n");
			return false;
		}
	}

	// Read target info
	Ar6kBmiTargetInfo tgtinfo;
	if (!ar6kBmiGetTargetInfo(dev, &tgtinfo)) {
		dietPrint("[AR6K] Cannot read target info\n");
		return false;
	}
	dietPrint("[AR6K] BMI %.8lX (type %lu)\n", tgtinfo.target_ver, tgtinfo.target_type);

	// XX: Here the target version is compared against 0x20000188, and if it is lower
	// than said value (and the fw is not loaded yet), a specific sequence of register
	// writes takes place (matches ar6002_REV1_reset_force_host). This does not seem
	// necessary, as the oldest chip used on DSi/3DS (AR6002G) will return 0x20000188.

	// XX: enableuartprint would happen here. This is disabled in official code, and
	// is useless to us anyway (we don't actually have the means to connect UART to
	// the Atheros hardware, nor do we probably have a debugging build of the FW).

	// Set hi_app_host_interest to the requested HTC protocol version
	if (!ar6kBmiWriteMemoryWord(dev, dev->hia_addr+0x00, AR6K_HTC_PROTOCOL_VER)) {
		return false;
	}

	// Set WLAN_CLOCK_CONTROL
	if (!ar6kBmiWriteSocReg(dev, 0x4028, 5)) {
		dietPrint("[AR6K] WLAN_CLOCK_CONTROL fail\n");
		return false;
	}

	// Set WLAN_CPU_CLOCK
	if (!ar6kBmiWriteSocReg(dev, 0x4020, 0)) {
		dietPrint("[AR6K] WLAN_CPU_CLOCK fail\n");
		return false;
	}

	// XX: Firmware upload happens here.

	// Set hi_mbox_io_block_sz
	if (!ar6kBmiWriteMemoryWord(dev, dev->hia_addr+0x6c, SDIO_BLOCK_SZ)) {
		return false;
	}

	// Set hi_mbox_isr_yield_limit. Unknown what this does.
	if (!ar6kBmiWriteMemoryWord(dev, dev->hia_addr+0x74, 0x63)) {
		return false;
	}

	// Launch the firmware!
	dietPrint("[AR6K] Prep done, exiting BMI\n");
	if (!ar6kBmiDone(dev)) {
		return false;
	}

	// Wait for the board data to be initialized
	unsigned attempt;
	for (attempt = 0; attempt < AR6K_BOARD_INIT_TIMEOUT; attempt ++) {
		if (!ar6kDevReadRegDiag(dev, dev->hia_addr+0x58, &hi_board_data_initialized)) {
			dietPrint("[AR6K] Bad HIA read\n");
			return false;
		}

		if (hi_board_data_initialized) {
			break;
		}

		threadSleep(1000);
	}

	if (attempt == AR6K_BOARD_INIT_TIMEOUT) {
		dietPrint("[AR6K] Board init timed out\n");
		return false;
	} else {
		dietPrint("[AR6K] Init OK, %u attempts\n", attempt+1);
	}

	// XX: Here, hi_board_data would be read, which now points to a buffer
	// containing data read from an EEPROM chip. This chip contains settings
	// such as the firmware (?) version, country flags, as well as the MAC.
	// Even though official software does this, the data is only used to
	// verify that the MSB of the version number is 0x60 and nothing else.
	// This doesn't seem necessary, and the MAC is obtained later through
	// the WMI Ready event. So, we opt to not bother reading/parsing EEPROM.

	// Perform initial HTC handshake
	if (!ar6kHtcInit(dev)) {
		dietPrint("[AR6K] HTC init failed\n");
		return false;
	}

	// Connect WMI control service
	if (ar6kHtcConnectService(dev, Ar6kHtcSrvId_WmiControl, 0, &dev->wmi_ctrl_epid)) {
		return false;
	}

	// Connect WMI data services. There are four QoS levels
	const u16 datasrv_flags = AR6K_HTC_CONN_FLAG_REDUCE_CREDIT_DRIBBLE | AR6K_HTC_CONN_FLAG_THRESHOLD_0_5;

	if (ar6kHtcConnectService(dev, Ar6kHtcSrvId_WmiDataBe, datasrv_flags, &dev->wmi_data_epids[2])) {
		return false;
	}

	if (ar6kHtcConnectService(dev, Ar6kHtcSrvId_WmiDataBk, datasrv_flags, &dev->wmi_data_epids[3])) {
		return false;
	}

	if (ar6kHtcConnectService(dev, Ar6kHtcSrvId_WmiDataVi, datasrv_flags, &dev->wmi_data_epids[1])) {
		return false;
	}

	if (ar6kHtcConnectService(dev, Ar6kHtcSrvId_WmiDataVo, datasrv_flags, &dev->wmi_data_epids[0])) {
		return false;
	}

	// XX: Credit distribution setup would be handled here.

	// Inform HTC that we are done with setup
	if (!ar6kHtcSetupComplete(dev)) {
		dietPrint("[AR6K] HTC setup fail\n");
		return false;
	}

	dietPrint("[AR6K] HTC setup complete!\n");

	// Enable Atheros interrupts
	if (!_ar6kDevSetIrqEnable(dev, true)) {
		return false;
	}

	// Enable SDIO function1 interrupt
	if (!sdioCardSetIrqEnable(dev->sdio, 1, true)) {
		dietPrint("[AR6K] SDIO irq enable failed\n");
		return false;
	}

	// nbwifi: no interrupt handler; the device is polled (ar6kDevPoll)

	return true;
}

// nbwifi: in place of calico's interrupt thread. Checks the device for pending
// work (its interrupt status, read over SDIO, since the card interrupt is left
// off) and handles received packets. Returns false on a device error.
bool g_nbwInPoll = false;
unsigned g_nbwRxBatches = 0;   // times ar6kDevPoll found packets waiting

bool ar6kDevPoll(Ar6kDev* dev)
{
	// sending a packet may itself poll for credits; never handle packets twice over
	if (g_nbwInPoll) {
		return true;
	}

	Ar6kIrqProcRegs regs;
	if (!sdioCardReadExtended(dev->sdio, 1, 0x00400, &regs, sizeof(regs))) {
		dietPrint("[AR6K] IRQ status read fail\n");
		return false;
	}
	unsigned int_status = regs.host_int_status & dev->irq_regs.int_status_enable;

	// mbox0: a packet is waiting
	if (int_status & (1U<<0)) {
		int_status &= ~(1U<<0);
		if (regs.rx_lookahead_valid & (1U<<0)) {
			dev->lookahead = regs.rx_lookahead[0];
			if (dev->lookahead == 0) {
				dietPrint("[AR6K] Invalid packet received\n");
				return true;
			}
		}
	}

	// counter (target assertion): cleared by reading the counter
	if (int_status & (1U<<4)) {
		int_status &= ~(1U<<4);
		if (regs.counter_int_status & dev->irq_regs.counter_int_status_enable & 1) {
			dietPrint("[AR6K] Target assertion fail\n");
			u32 dummy;
			sdioCardReadExtended(dev->sdio, 1, 0x000440, &dummy, sizeof(dummy)); // COUNT_DEC_ADDRESS
		}
	}

	if (int_status) {
		dietPrint("[AR6K] Bad IRQ %.2X %.2X %.2X %.2X\n",
			int_status, regs.cpu_int_status, regs.error_int_status, regs.counter_int_status);
	}

	bool ok = true;
	if (dev->lookahead) {
		g_nbwRxBatches++;
		g_nbwInPoll = true;
		ok = _ar6kHtcRecvMessagePendingHandler(dev);
		g_nbwInPoll = false;
	}
	return ok;
}

static bool _ar6kDevSetAddrWinReg(Ar6kDev* dev, u32 reg, u32 addr)
{
	// Write bytes 1,2,3 first
	if (!sdioCardWriteExtended(dev->sdio, 1, reg+1, (u8*)&addr+1, 3)) {
		return false;
	}

	// Write LSB last to initiate the access cycle
	if (!sdioCardWriteExtended(dev->sdio, 1, reg, (u8*)&addr, 1)) {
		return false;
	}

	return true;
}

bool ar6kDevReadRegDiag(Ar6kDev* dev, u32 addr, u32* out)
{
	if (!_ar6kDevSetAddrWinReg(dev, 0x0047C, addr)) { // WINDOW_READ_ADDR
		return false;
	}

	if (!sdioCardReadExtended(dev->sdio, 1, 0x00474, out, 4)) { // WINDOW_DATA
		return false;
	}

	return true;
}

bool ar6kDevWriteRegDiag(Ar6kDev* dev, u32 addr, u32 value)
{
	if (!sdioCardWriteExtended(dev->sdio, 1, 0x00474, &value, 4)) { // WINDOW_DATA
		return false;
	}

	if (!_ar6kDevSetAddrWinReg(dev, 0x00478, addr)) { // WINDOW_WRITE_ADDR
		return false;
	}

	return true;
}

bool _ar6kDevSetIrqEnable(Ar6kDev* dev, bool enable)
{
	Ar6kIrqEnableRegs regs = { 0 };
	if (enable) {
		// bit7: enable_error
		// bit6: enable_cpu
		// bit4: enable_counter
		// bit0: enable_mbox_data (for mbox0)
		regs.int_status_enable = (1U<<7) | (1U<<6) | (1U<<4) | (1U<<0);
		// bit1: enable_rx_underflow
		// bit0: enable_tx_overflow
		regs.error_status_enable = (1U<<1) | (1U<<0);
		// bit0: enable (for debug interrupt)
		regs.counter_int_status_enable = (1U<<0);
	}

	bool rc = sdioCardWriteExtended(dev->sdio, 1, 0x000418, &regs, sizeof(regs));
	if (rc) {
		dev->irq_regs = regs;
	} else {
		dietPrint("[AR6K] ar6k irq %s failed\n", enable ? "enable" : "disable");
	}
	return rc;
}

bool _ar6kDevPollMboxMsgRecv(Ar6kDev* dev, u32* lookahead, unsigned attempts)
{
	Ar6kIrqProcRegs regs;
	unsigned attempt;

	for (attempt = 0; attempt < attempts; attempt ++) {
		if (!sdioCardReadExtended(dev->sdio, 1, 0x00400, &regs, sizeof(regs))) {
			return false;
		}

		if ((regs.host_int_status & 1) && (regs.rx_lookahead_valid & 1)) {
			break;
		}

		threadSleep(10000);
	}

	if (attempt == attempts) {
		dietPrint("[AR6K] mbox poll failed\n");
		return false;
	}

	*lookahead = regs.rx_lookahead[0];
	return true;
}

bool _ar6kDevSendPacket(Ar6kDev* dev, const void* pktmem, size_t pktsize)
{
	pktsize = (pktsize + SDIO_BLOCK_SZ - 1) &~ (SDIO_BLOCK_SZ - 1);
	bool ret = sdioCardWriteExtended(dev->sdio, 1, 0x000800 + 0x800 - pktsize, pktmem, pktsize);
	if (!ret) {
		dietPrint("[AR6K] DevSendPacket fail\n");
	}
	return ret;
}

bool _ar6kDevRecvPacket(Ar6kDev* dev, void* pktmem, size_t pktsize)
{
	pktsize = (pktsize + SDIO_BLOCK_SZ - 1) &~ (SDIO_BLOCK_SZ - 1);
	bool ret = sdioCardReadExtended(dev->sdio, 1, 0x000800 + 0x800 - pktsize, pktmem, pktsize);
	if (!ret) {
		dietPrint("[AR6K] DevRecvPacket fail\n");
	}
	return ret;
}
