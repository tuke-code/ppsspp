// Copyright (C) 2012 PPSSPP Project

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "ppsspp_config.h"

#ifdef _WIN32
#pragma warning(disable:4091)
#include "Common/CommonWindows.h"
#include <ShlObj.h>
#include <string>
#if !PPSSPP_PLATFORM(UWP)
#include "Windows/W32Util/ShellUtil.h"
#endif
#endif

#include <mutex>

#include "ext/lua/lapi.h"

#include "Common/System/System.h"
#include "Common/System/Request.h"
#include "Common/System/OSD.h"
#include "Common/Data/Text/I18n.h"
#include "Common/File/Path.h"
#include "Common/File/FileUtil.h"
#include "Common/File/DirListing.h"
#include "Common/TimeUtil.h"
#include "Common/GraphicsContext.h"

#include "Core/RetroAchievements.h"
#include "Core/MemFault.h"
#include "Core/HDRemaster.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSAnalyst.h"
#include "Core/MIPS/MIPSVFPUUtils.h"
#include "Core/Debugger/SymbolMap.h"
#include "Core/System.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/Plugins.h"
#include "Core/HLE/ReplaceTables.h"
#include "Core/HLE/sceKernel.h"
#include "Core/Config.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/CoreParameter.h"
#include "Core/FileLoaders/RamCachingFileLoader.h"
#include "Core/LuaContext.h"
#include "Core/FileSystems/MetaFileSystem.h"
#include "Core/Loaders.h"
#include "Core/PSPLoaders.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/SaveState.h"
#include "Core/Util/RecentFiles.h"
#include "Common/StringUtils.h"
#include "Common/ExceptionHandlerSetup.h"
#include "GPU/GPUCommon.h"
#include "GPU/Debugger/Playback.h"
#include "GPU/Debugger/RecordFormat.h"

enum CPUThreadState {
	CPU_THREAD_NOT_RUNNING,
	CPU_THREAD_RUNNING,
};

MetaFileSystem pspFileSystem;
ParamSFOData g_paramSFO;
static GlobalUIState globalUIState;
CoreParameter g_CoreParameter;
static FileLoader *g_loadedFile;
// For background loading thread.
static std::mutex loadingLock;

bool coreCollectDebugStats = false;
static int coreCollectDebugStatsCounter = 0;

static volatile CPUThreadState cpuThreadState = CPU_THREAD_NOT_RUNNING;

static GPUBackend gpuBackend;
static std::string gpuBackendDevice;

// Ugly!
static volatile bool pspIsInited = false;
static volatile bool pspIsIniting = false;
static volatile bool pspIsQuitting = false;
static volatile bool pspIsRebooting = false;

void ResetUIState() {
	globalUIState = UISTATE_MENU;
}

void UpdateUIState(GlobalUIState newState) {
	// Never leave the EXIT state.
	if (globalUIState != newState && globalUIState != UISTATE_EXIT) {
		globalUIState = newState;
		System_Notify(SystemNotification::DISASSEMBLY);
		System_Notify(SystemNotification::UI_STATE_CHANGED);
		System_SetKeepScreenBright(globalUIState == UISTATE_INGAME);
	}
}

GlobalUIState GetUIState() {
	return globalUIState;
}

void SetGPUBackend(GPUBackend type, const std::string &device) {
	gpuBackend = type;
	gpuBackendDevice = device;
}

GPUBackend GetGPUBackend() {
	return gpuBackend;
}

std::string GetGPUBackendDevice() {
	return gpuBackendDevice;
}

bool CPU_IsReady() {
	if (coreState == CORE_POWERUP)
		return false;
	return cpuThreadState == CPU_THREAD_RUNNING || cpuThreadState == CPU_THREAD_NOT_RUNNING;
}

bool CPU_IsShutdown() {
	return cpuThreadState == CPU_THREAD_NOT_RUNNING;
}

bool CPU_HasPendingAction() {
	return cpuThreadState != CPU_THREAD_RUNNING;
}

void CPU_Shutdown();

static Path SymbolMapFilename(const Path &currentFilename, const char *ext) {
	File::FileInfo info{};
	// can't fail, definitely exists if it gets this far
	File::GetFileInfo(currentFilename, &info);
	if (info.isDirectory) {
		return currentFilename / (std::string(".ppsspp-symbols") + ext);
	}
	return currentFilename.WithReplacedExtension(ext);
};

static bool LoadSymbolsIfSupported() {
	if (System_GetPropertyBool(SYSPROP_HAS_DEBUGGER)) {
		if (!g_symbolMap)
			return false;

		if (PSP_CoreParameter().fileToStart.Type() == PathType::HTTP) {
			// We don't support loading symbols over HTTP.
			g_symbolMap->Clear();
			return true;
		}

		bool result1 = g_symbolMap->LoadSymbolMap(SymbolMapFilename(PSP_CoreParameter().fileToStart, ".ppmap"));
		// Load the old-style map file.
		if (!result1)
			result1 = g_symbolMap->LoadSymbolMap(SymbolMapFilename(PSP_CoreParameter().fileToStart, ".map"));
		bool result2 = g_symbolMap->LoadNocashSym(SymbolMapFilename(PSP_CoreParameter().fileToStart, ".sym"));
		return result1 || result2;
	} else {
		g_symbolMap->Clear();
		return true;
	}
}

static bool SaveSymbolMapIfSupported() {
	if (g_symbolMap) {
		return g_symbolMap->SaveSymbolMap(SymbolMapFilename(PSP_CoreParameter().fileToStart, ".ppmap"));
	}
	return false;
}

bool DiscIDFromGEDumpPath(const Path &path, FileLoader *fileLoader, std::string *id) {
	using namespace GPURecord;

	// For newer files, it's stored in the dump.
	Header header;
	if (fileLoader->ReadAt(0, sizeof(header), &header) == sizeof(header)) {
		const bool magicMatch = memcmp(header.magic, HEADER_MAGIC, sizeof(header.magic)) == 0;
		if (magicMatch && header.version <= VERSION && header.version >= 4) {
			size_t gameIDLength = strnlen(header.gameID, sizeof(header.gameID));
			if (gameIDLength != 0) {
				*id = std::string(header.gameID, gameIDLength);
				return true;
			}
		}
	}

	// Fall back to using the filename.
	std::string filename = path.GetFilename();
	// Could be more discerning, but hey..
	if (filename.size() > 10 && (filename[0] == 'U' || filename[0] == 'N') && filename[9] == '_') {
		*id = filename.substr(0, 9);
		return true;
	} else {
		return false;
	}
}

bool CPU_Init(std::string *errorString, FileLoader *loadedFile, IdentifiedFileType type) {
	coreState = CORE_POWERUP;
	currentMIPS = &mipsr4k;

	g_symbolMap = new SymbolMap();

	g_lua.Init();

	// Default memory settings
	// Seems to be the safest place currently..
	Memory::g_MemorySize = Memory::RAM_NORMAL_SIZE; // 32 MB of ram by default

	g_RemasterMode = false;
	g_DoubleTextureCoordinates = false;
	Memory::g_PSPModel = g_Config.iPSPModel;

	g_CoreParameter.fileType = type;

	MIPSAnalyst::Reset();
	Replacement_Init();

	bool allowPlugins = true;
	std::string geDumpDiscID;

	switch (type) {
	case IdentifiedFileType::PSP_ISO:
	case IdentifiedFileType::PSP_ISO_NP:
	case IdentifiedFileType::PSP_DISC_DIRECTORY:
		if (!MountGameISO(loadedFile)) {
			*errorString = "Failed to mount ISO file - invalid format?";
			return false;
		}
		if (LoadParamSFOFromDisc()) {
			InitMemorySizeForGame();
		}
		break;
	case IdentifiedFileType::PSP_PBP:
	case IdentifiedFileType::PSP_PBP_DIRECTORY:
		// This is normal for homebrew.
		// ERROR_LOG(Log::Loader, "PBP directory resolution failed.");
		if (LoadParamSFOFromPBP(loadedFile)) {
			InitMemorySizeForGame();
		}
		break;
	case IdentifiedFileType::PSP_ELF:
		if (Memory::g_PSPModel != PSP_MODEL_FAT) {
			INFO_LOG(Log::Loader, "ELF, using full PSP-2000 memory access");
			Memory::g_MemorySize = Memory::RAM_DOUBLE_SIZE;
		}
		break;
	case IdentifiedFileType::PPSSPP_GE_DUMP:
		// Try to grab the disc ID from the filename or GE dump.
		if (DiscIDFromGEDumpPath(g_CoreParameter.fileToStart, loadedFile, &geDumpDiscID)) {
			// Store in SFO, otherwise it'll generate a fake disc ID.
			g_paramSFO.SetValue("DISC_ID", geDumpDiscID, 16);
		}
		allowPlugins = false;
		break;
	default:
		// Can we even get here?
		ERROR_LOG(Log::Loader, "CPU_Init didn't recognize file. %s", errorString->c_str());
		return false;
	}

	// Here we have read the PARAM.SFO, let's see if we need any compatibility overrides.
	// Homebrew usually has an empty discID, and even if they do have a disc id, it's not
	// likely to collide with any commercial ones.
	g_CoreParameter.compat.Load(g_paramSFO.GetDiscID());

	// Initialize the memory map as early as possible (now that we've read the PARAM.SFO).
	if (!Memory::Init()) {
		// We're screwed.
		*errorString = "Memory init failed";
		return false;
	}

	InitVFPU();

	if (allowPlugins)
		HLEPlugins::Init();

	LoadSymbolsIfSupported();

	CoreTiming::Init();

	// Init all the HLE modules
	HLEInit();

	// TODO: Put this somewhere better?
	if (!g_CoreParameter.mountIso.empty()) {
		g_CoreParameter.mountIsoLoader = ConstructFileLoader(g_CoreParameter.mountIso);
	}

	mipsr4k.Reset();

	// TODO: Check Game INI here for settings, patches and cheats, and modify coreParameter accordingly

	// If they shut down early, we'll catch it when load completes.
	// Note: this may return before init is complete, which is checked if CPU_IsReady().
	g_loadedFile = loadedFile;
	if (!LoadFile(&loadedFile, type, &g_CoreParameter.errorString)) {
		CPU_Shutdown();
		g_CoreParameter.fileToStart.clear();
		return false;
	}

	if (g_CoreParameter.updateRecent) {
		g_recentFiles.Add(g_CoreParameter.fileToStart.ToString());
	}

	InstallExceptionHandler(&Memory::HandleFault);
	return true;
}

PSP_LoadingLock::PSP_LoadingLock() {
	loadingLock.lock();
}

PSP_LoadingLock::~PSP_LoadingLock() {
	loadingLock.unlock();
}

void CPU_Shutdown() {
	UninstallExceptionHandler();

	// Since we load on a background thread, wait for startup to complete.
	PSP_LoadingLock lock;
	PSPLoaders_Shutdown();

	GPURecord::Replay_Unload();

	if (g_Config.bAutoSaveSymbolMap) {
		SaveSymbolMapIfSupported();
	}

	Replacement_Shutdown();

	CoreTiming::Shutdown();
	__KernelShutdown();
	HLEShutdown();

	pspFileSystem.Shutdown();
	mipsr4k.Shutdown();
	Memory::Shutdown();
	HLEPlugins::Shutdown();

	delete g_loadedFile;
	g_loadedFile = nullptr;

	delete g_CoreParameter.mountIsoLoader;
	delete g_symbolMap;
	g_symbolMap = nullptr;

	g_lua.Shutdown();

	g_CoreParameter.mountIsoLoader = nullptr;
}

// TODO: Maybe loadedFile doesn't even belong here...
void UpdateLoadedFile(FileLoader *fileLoader) {
	delete g_loadedFile;
	g_loadedFile = fileLoader;
}

void PSP_UpdateDebugStats(bool collectStats) {
	bool newState = collectStats || coreCollectDebugStatsCounter > 0;
	if (coreCollectDebugStats != newState) {
		coreCollectDebugStats = newState;
		mipsr4k.ClearJitCache();
	}

	if (!PSP_CoreParameter().frozen && !Core_IsStepping()) {
		kernelStats.ResetFrame();
		gpuStats.ResetFrame();
	}
}

void PSP_ForceDebugStats(bool enable) {
	if (enable) {
		coreCollectDebugStatsCounter++;
	} else {
		coreCollectDebugStatsCounter--;
	}
	_assert_(coreCollectDebugStatsCounter >= 0);
}

bool PSP_InitStart(const CoreParameter &coreParam, std::string *error_string) {
	if (pspIsIniting || pspIsQuitting) {
		return false;
	}

	if (!Achievements::IsReadyToStart()) {
		return false;
	}

#if defined(_WIN32) && PPSSPP_ARCH(AMD64)
	NOTICE_LOG(Log::Boot, "PPSSPP %s Windows 64 bit", PPSSPP_GIT_VERSION);
#elif defined(_WIN32) && !PPSSPP_ARCH(AMD64)
	NOTICE_LOG(Log::Boot, "PPSSPP %s Windows 32 bit", PPSSPP_GIT_VERSION);
#else
	NOTICE_LOG(Log::Boot, "PPSSPP %s", PPSSPP_GIT_VERSION);
#endif

	Core_NotifyLifecycle(CoreLifecycle::STARTING);
	GraphicsContext *temp = g_CoreParameter.graphicsContext;
	g_CoreParameter = coreParam;
	if (g_CoreParameter.graphicsContext == nullptr) {
		g_CoreParameter.graphicsContext = temp;
	}
	g_CoreParameter.errorString.clear();
	pspIsIniting = true;

	Path filename = g_CoreParameter.fileToStart;
	FileLoader *loadedFile = ResolveFileLoaderTarget(ConstructFileLoader(filename));

	IdentifiedFileType type = Identify_File(loadedFile, &g_CoreParameter.errorString);
	g_CoreParameter.fileType = type;

	if (System_GetPropertyBool(SYSPROP_ENOUGH_RAM_FOR_FULL_ISO)) {
		if (g_Config.bCacheFullIsoInRam) {
			switch (g_CoreParameter.fileType) {
			case IdentifiedFileType::PSP_ISO:
			case IdentifiedFileType::PSP_ISO_NP:
				loadedFile = new RamCachingFileLoader(loadedFile);
				break;
			default:
				INFO_LOG(Log::System, "RAM caching is on, but file is not an ISO, so ignoring");
				break;
			}
		}
	}

	if (g_Config.bAchievementsEnable) {
		// Need to re-identify after ResolveFileLoaderTarget - although in practice probably not,
		// but also, re-using the identification would require some plumbing, to be done later.
		std::string errorString;
		Achievements::SetGame(filename, type, loadedFile);
	}

	if (!CPU_Init(&g_CoreParameter.errorString, loadedFile, type)) {
		*error_string = g_CoreParameter.errorString;
		if (error_string->empty()) {
			*error_string = "Failed initializing CPU/Memory";
		}
		pspIsIniting = false;
		return false;
	}

	// Compat flags get loaded in CPU_Init (which is a bit of a misnomer) so we check for SW renderer here.
	if (g_Config.bSoftwareRendering || PSP_CoreParameter().compat.flags().ForceSoftwareRenderer) {
		g_CoreParameter.gpuCore = GPUCORE_SOFTWARE;
	}

	*error_string = g_CoreParameter.errorString;
	bool success = !g_CoreParameter.fileToStart.empty();
	if (!success) {
		Core_NotifyLifecycle(CoreLifecycle::START_COMPLETE);
		pspIsRebooting = false;
		// In this case, we must call shutdown since the caller won't know to.
		// It must've partially started since CPU_Init returned true.
		PSP_Shutdown();
	}
	return success;
}

bool PSP_InitUpdate(std::string *error_string) {
	if (pspIsInited || !pspIsIniting) {
		return true;
	}

	if (!CPU_IsReady()) {
		return false;
	}

	bool success = !g_CoreParameter.fileToStart.empty();
	if (!g_CoreParameter.errorString.empty()) {
		*error_string = g_CoreParameter.errorString;
	}

	if (success && gpu == nullptr) {
		INFO_LOG(Log::System, "Starting graphics...");
		Draw::DrawContext *draw = g_CoreParameter.graphicsContext ? g_CoreParameter.graphicsContext->GetDrawContext() : nullptr;
		success = GPU_Init(g_CoreParameter.graphicsContext, draw);
		if (!success) {
			*error_string = "Unable to initialize rendering engine.";
		}
	}
	if (!success) {
		pspIsRebooting = false;
		PSP_Shutdown();
		return true;
	}

	pspIsInited = GPU_IsReady();
	pspIsIniting = !pspIsInited;
	if (pspIsInited) {
		Core_NotifyLifecycle(CoreLifecycle::START_COMPLETE);
		pspIsRebooting = false;

		// If GPU init failed during IsReady checks, bail.
		if (!GPU_IsStarted()) {
			*error_string = "Unable to initialize rendering engine.";
			pspIsRebooting = false;
			PSP_Shutdown();
			return true;
		}
	}
	return pspIsInited;
}

bool PSP_Init(const CoreParameter &coreParam, std::string *error_string) {
	// Spawn a lua instance

	if (!PSP_InitStart(coreParam, error_string))
		return false;

	while (!PSP_InitUpdate(error_string))
		sleep_ms(10, "psp-init-poll");
	return pspIsInited;
}

bool PSP_IsIniting() {
	return pspIsIniting;
}

bool PSP_IsInited() {
	return pspIsInited && !pspIsQuitting && !pspIsRebooting;
}

bool PSP_IsRebooting() {
	return pspIsRebooting;
}

bool PSP_IsQuitting() {
	return pspIsQuitting;
}

void PSP_Shutdown() {
	// Reduce the risk for weird races with the Windows GE debugger.
	gpuDebug = nullptr;

	Achievements::UnloadGame();

	// Do nothing if we never inited.
	if (!pspIsInited && !pspIsIniting && !pspIsQuitting) {
		return;
	}

	// Make sure things know right away that PSP memory, etc. is going away.
	pspIsQuitting = !pspIsRebooting;
	if (coreState == CORE_RUNNING_CPU)
		Core_Stop();

	if (g_Config.bFuncHashMap) {
		MIPSAnalyst::StoreHashMap();
	}

	if (pspIsIniting)
		Core_NotifyLifecycle(CoreLifecycle::START_COMPLETE);
	Core_NotifyLifecycle(CoreLifecycle::STOPPING);
	CPU_Shutdown();
	GPU_Shutdown();
	g_paramSFO.Clear();
	System_SetWindowTitle("");
	currentMIPS = 0;
	pspIsInited = false;
	pspIsIniting = false;
	pspIsQuitting = false;
	g_Config.unloadGameConfig();
	Core_NotifyLifecycle(CoreLifecycle::STOPPED);
}

bool PSP_Reboot(std::string *error_string) {
	if (!pspIsInited || pspIsQuitting)
		return false;

	pspIsRebooting = true;
	Core_Stop();
	Core_WaitInactive();
	PSP_Shutdown();
	std::string resetError;
	return PSP_Init(PSP_CoreParameter(), error_string);
}

void PSP_BeginHostFrame() {
	if (gpu) {
		gpu->BeginHostFrame();
	}
}

void PSP_EndHostFrame() {
	if (gpu) {
		gpu->EndHostFrame();
	}
	SaveState::Cleanup();
}

void PSP_RunLoopWhileState() {
	// We just run the CPU until we get to vblank. This will quickly sync up pretty nicely.
	// The actual number of cycles doesn't matter so much here as we will break due to CORE_NEXTFRAME, most of the time hopefully...
	int blockTicks = usToCycles(1000000 / 10);
	// Run until CORE_NEXTFRAME
	PSP_RunLoopFor(blockTicks);
	// TODO: Check for frame timeout?
}

void PSP_RunLoopFor(int cycles) {
	Core_RunLoopUntil(CoreTiming::GetTicks() + cycles);
}

Path GetSysDirectory(PSPDirectories directoryType) {
	const Path &memStickDirectory = g_Config.memStickDirectory;
	Path pspDirectory;
	if (!strcasecmp(memStickDirectory.GetFilename().c_str(), "PSP")) {
		// Let's strip this off, to easily allow choosing a root directory named "PSP" on Android.
		pspDirectory = memStickDirectory;
	} else {
		pspDirectory = memStickDirectory / "PSP";
	}

	switch (directoryType) {
	case DIRECTORY_PSP:
		return pspDirectory;
	case DIRECTORY_CHEATS:
		return pspDirectory / "Cheats";
	case DIRECTORY_GAME:
		return pspDirectory / "GAME";
	case DIRECTORY_SAVEDATA:
		return pspDirectory / "SAVEDATA";
	case DIRECTORY_SCREENSHOT:
		return pspDirectory / "SCREENSHOT";
	case DIRECTORY_SYSTEM:
		return pspDirectory / "SYSTEM";
	case DIRECTORY_PAUTH:
		return memStickDirectory / "PAUTH";  // This one's at the root...
	case DIRECTORY_EXDATA:
		return memStickDirectory / "EXDATA";  // This one's traditionally at the root...
	case DIRECTORY_DUMP:
		return pspDirectory / "SYSTEM/DUMP";
	case DIRECTORY_SAVESTATE:
		return pspDirectory / "PPSSPP_STATE";
	case DIRECTORY_CACHE:
		return pspDirectory / "SYSTEM/CACHE";
	case DIRECTORY_TEXTURES:
		return pspDirectory / "TEXTURES";
	case DIRECTORY_PLUGINS:
		return pspDirectory / "PLUGINS";
	case DIRECTORY_APP_CACHE:
		if (!g_Config.appCacheDirectory.empty()) {
			return g_Config.appCacheDirectory;
		}
		return pspDirectory / "SYSTEM/CACHE";
	case DIRECTORY_VIDEO:
		return pspDirectory / "VIDEO";
	case DIRECTORY_AUDIO:
		return pspDirectory / "AUDIO";
	case DIRECTORY_CUSTOM_SHADERS:
		return pspDirectory / "shaders";
	case DIRECTORY_CUSTOM_THEMES:
		return pspDirectory / "themes";

	case DIRECTORY_MEMSTICK_ROOT:
		return g_Config.memStickDirectory;
	// Just return the memory stick root if we run into some sort of problem.
	default:
		ERROR_LOG(Log::FileSystem, "Unknown directory type.");
		return g_Config.memStickDirectory;
	}
}

bool CreateSysDirectories() {
#if PPSSPP_PLATFORM(ANDROID)
	const bool createNoMedia = true;
#else
	const bool createNoMedia = false;
#endif

	Path pspDir = GetSysDirectory(DIRECTORY_PSP);
	INFO_LOG(Log::IO, "Creating '%s' and subdirs:", pspDir.c_str());
	File::CreateFullPath(pspDir);
	if (!File::Exists(pspDir)) {
		INFO_LOG(Log::IO, "Not a workable memstick directory. Giving up");
		return false;
	}

	// Create the default directories that a real PSP creates. Good for homebrew so they can
	// expect a standard environment. Skipping THEME though, that's pointless.
	static const PSPDirectories sysDirs[] = {
		DIRECTORY_CHEATS,
		DIRECTORY_SAVEDATA,
		DIRECTORY_SAVESTATE,
		DIRECTORY_GAME,
		DIRECTORY_SYSTEM,
		DIRECTORY_TEXTURES,
		DIRECTORY_PLUGINS,
		DIRECTORY_CACHE,
	};

	for (auto dir : sysDirs) {
		Path path = GetSysDirectory(dir);
		File::CreateFullPath(path);
		if (createNoMedia) {
			// Create a nomedia file in each specified subdirectory.
			File::CreateEmptyFile(path / ".nomedia");
		}
	}
	return true;
}

const char *CoreStateToString(CoreState state) {
	switch (state) {
	case CORE_RUNNING_CPU: return "RUNNING_CPU";
	case CORE_NEXTFRAME: return "NEXTFRAME";
	case CORE_STEPPING_CPU: return "STEPPING_CPU";
	case CORE_POWERUP: return "POWERUP";
	case CORE_POWERDOWN: return "POWERDOWN";
	case CORE_BOOT_ERROR: return "BOOT_ERROR";
	case CORE_RUNTIME_ERROR: return "RUNTIME_ERROR";
	case CORE_STEPPING_GE: return "STEPPING_GE";
	case CORE_RUNNING_GE: return "RUNNING_GE";
	default: return "N/A";
	}
}

const char *DumpFileTypeToString(DumpFileType type) {
	switch (type) {
	case DumpFileType::EBOOT: return "EBOOT";
	case DumpFileType::PRX: return "PRX";
	case DumpFileType::Atrac3: return "AT3";
	default: return "N/A";
	}
}

const char *DumpFileTypeToFileExtension(DumpFileType type) {
	switch (type) {
	case DumpFileType::EBOOT: return ".BIN";
	case DumpFileType::PRX: return ".prx";
	case DumpFileType::Atrac3: return ".at3";
	default: return "N/A";
	}
}

void DumpFileIfEnabled(const u8 *dataPtr, const u32 length, const char *name, DumpFileType type) {
	if (!(g_Config.iDumpFileTypes & (int)type)) {
		return;
	}
	if (!dataPtr) {
		ERROR_LOG(Log::System, "Error dumping %s: invalid pointer", DumpFileTypeToString(DumpFileType::EBOOT));
		return;
	}
	if (length == 0) {
		ERROR_LOG(Log::System, "Error dumping %s: invalid length", DumpFileTypeToString(DumpFileType::EBOOT));
		return;
	}

	const char *extension = DumpFileTypeToFileExtension(type);
	const std::string filenameToDumpTo = StringFromFormat("%s_%s%s", g_paramSFO.GetDiscID().c_str(), name, extension);
	const Path dumpDirectory = GetSysDirectory(DIRECTORY_DUMP);
	const Path fullPath = dumpDirectory / filenameToDumpTo;

	auto s = GetI18NCategory(I18NCat::SYSTEM);

	std::string_view titleStr = "Dump Decrypted Eboot";
	if (type != DumpFileType::EBOOT) {
		titleStr = s->T(DumpFileTypeToString(type));
	}

	// If the file already exists, don't dump it again.
	if (File::Exists(fullPath)) {
		INFO_LOG(Log::sceModule, "%s already exists for this game, skipping dump.", filenameToDumpTo.c_str());

		char *path = new char[strlen(fullPath.c_str()) + 1];
		strcpy(path, fullPath.c_str());

		g_OSD.Show(OSDType::MESSAGE_INFO, titleStr, fullPath.ToVisualString(), 5.0f);
		if (System_GetPropertyBool(SYSPROP_CAN_SHOW_FILE)) {
			g_OSD.SetClickCallback("file_dumped", [](bool clicked, void *userdata) {
				char *path = (char *)userdata;
				if (clicked) {
					System_ShowFileInFolder(Path(path));
				} else {
					delete[] path;
				}
			}, path);
		}
		return;
	}

	// Make sure the dump directory exists before continuing.
	if (!File::Exists(dumpDirectory)) {
		if (!File::CreateDir(dumpDirectory)) {
			ERROR_LOG(Log::sceModule, "Unable to create directory for EBOOT dumping, aborting.");
			return;
		}
	}

	FILE *file = File::OpenCFile(fullPath, "wb");
	if (!file) {
		ERROR_LOG(Log::sceModule, "Unable to write decrypted EBOOT.");
		return;
	}

	const size_t lengthToWrite = length;

	fwrite(dataPtr, sizeof(u8), lengthToWrite, file);
	fclose(file);

	INFO_LOG(Log::sceModule, "Successfully wrote %s to %s", DumpFileTypeToString(type), fullPath.c_str());

	char *path = new char[strlen(fullPath.c_str()) + 1];
	strcpy(path, fullPath.c_str());

	// Re-suing the translation string here.
	g_OSD.Show(OSDType::MESSAGE_SUCCESS, titleStr, fullPath.ToVisualString(), 5.0f, "decr");
	if (System_GetPropertyBool(SYSPROP_CAN_SHOW_FILE)) {
		g_OSD.SetClickCallback("decr", [](bool clicked, void *userdata) {
			char *path = (char *)userdata;
			if (clicked) {
				System_ShowFileInFolder(Path(path));
			} else {
				delete[] path;
			}
		}, path);
	}
}
