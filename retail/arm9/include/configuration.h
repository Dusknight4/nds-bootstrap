#ifndef CONFIGURATION_H
#define CONFIGURATION_H

//#include <limits.h> // ARG_MAX
#include <nds/ndstypes.h>

typedef struct configuration {
    bool debug;
	bool dsiWramAccess;
	int b4dsMode;
	int loaderType;
	char* ndsPath;
	char* appPath;
	char* savPath;
	char* prvPath;
	char* bnrPath;
	char bannerSavPath[64];
	bool useSdk20Donor;
	bool useSdk5DonorAlt;
	char* donor20Path;
	char* donor5Path;
	char* donor5PathAlt;
	char* donorTwl0Path;
	char* donorTwlPath;
	char* donorTwlOnly0Path;
	char* donorTwlOnlyPath;
	char* quitPath;
	// char* cleanDonorPath;
	// char* gbaPath;
	// char* gbaSavPath;
	char* apPatchPath;
	char* apPatchPostCardReadPath;
	char* dsi2dsSavePatchPath;
	u32 ndsBootstrapCluster;
	u32 donorFileSize;
	u32 donorFileOffset;
	u32 romSize;
	u32 saveSize;
	// u32 gbaRomSize;
	// u32 gbaSaveSize;
	u32 wideCheatSize;
	u32 apPatchOffset;
	u32 apPatchSize;
	u32 apPatchPostCardReadOffset;
	u32 apPatchPostCardReadSize;
	u32 mobiclipStartOffset;
	u32 mobiclipEndOffset;
	u32 dsi2dsSavePatchOffset;
	u32 dsi2dsSavePatchSize;
	u32 cheatSize;
	u32 musicsSize;
	u32 dataToPreloadAddr[4];
	u32 dataToPreloadSize[4];
	// u32 dataToPreloadFrame;
	u32 asyncDataAddr[2];
	u32 asyncDataSize[2];
	u8 language;
	char* guiLanguage;
	s8 region; // SDK 5
	bool useRomRegion;
	bool sdNand; // SDK 5
	bool dsiWareSlot1Mode; // SDK 5
	u8 dsiMode; // SDK 5
	u8 isDSiWare; // SDK 5
	u8 valueBits;
	u8 valueBits2;
	u8 valueBits3;
	u8 valueBits4;
	u8 donorSdkVer;
	u8 patchMpuRegion;
	u32 patchMpuSize;
	u32 dldiPatchBinaryOffset;
	u8 consoleModel;
	// int colorMode;
	u8 romRead_LED;
	u8 dmaRomRead_LED;
	u8 remappedKeys[12];
	// WideMelon WiFi bridge (sd:/_nds/nds-bootstrap-wifi.ini): what the wireless is for
	// this time. 0: the game's own local wireless, with nearby DSes, as normal.
	// 1: local wireless carried over the WiFi network, to WideMelon.
	u8 wifiBridge;
	char* wifiPc;		// the PC running WideMelon: an IP address, or "auto" to find it
	int asyncCardRead;
	bool cardReadDMA;
	bool boostCpu;
	bool boostVram;
	bool soundFreq;
	bool forceSleepPatch;
	bool volumeFix;
	bool preciseVolumeControl;
	bool logging;
	bool initDisc;
	bool sdFound;
	bool bootstrapOnFlashcard;
	bool gameOnFlashcard;
	bool saveOnFlashcard;
	bool macroMode;
	bool sleepMode;
	u16 hotkey;
	u16 screenSwapHotkey;
	bool specialCard;
	bool phatColors;
	char* manualPath;
	bool saveRelocation;
	u16 cacheBlockSize;
	u32 saveMirrorCluster;	// R4 save copy (r4save.cpp): the save's first cluster on the R4; 0: off
	u32 saveMirrorSdhc;
	u32 saveMirrorLogCluster;	// r4save.log, for the loader's notes
	long saveMtimeBefore;	// the save's date before nds-bootstrap's own date update (conf_sd.cpp)
} configuration;

#endif // CONFIGURATION_H