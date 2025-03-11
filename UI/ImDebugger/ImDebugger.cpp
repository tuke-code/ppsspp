#include <algorithm>


#include "ext/imgui/imgui_internal.h"

#include "Common/StringUtils.h"
#include "Common/File/FileUtil.h"
#include "Common/Data/Format/IniFile.h"
#include "Common/Data/Text/Parsers.h"
#include "Common/Log/LogManager.h"
#include "Core/Config.h"
#include "Core/System.h"
#include "Core/RetroAchievements.h"
#include "Core/Core.h"
#include "Core/Debugger/DebugInterface.h"
#include "Core/Debugger/DisassemblyManager.h"
#include "Core/Debugger/Breakpoints.h"
#include "Core/MIPS/MIPSDebugInterface.h"
#include "Core/MIPS/MIPSTables.h"
#include "Core/HW/SimpleAudioDec.h"
#include "Core/FileSystems/MetaFileSystem.h"
#include "Core/Debugger/SymbolMap.h"
#include "Core/MemMap.h"
#include "Core/System.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/SocketManager.h"
#include "Core/HLE/NetInetConstants.h"
#include "Core/HLE/sceNp.h"
#include "Core/HLE/sceNet.h"
#include "Core/HLE/sceNetApctl.h"
#include "Core/HLE/sceNetAdhoc.h"
#include "Core/HLE/proAdhoc.h"
#include "Core/HLE/sceNetAdhocMatching.h"
#include "Common/System/Request.h"

#include "Core/HLE/sceAtrac.h"
#include "Core/HLE/sceAudio.h"
#include "Core/HLE/sceAudiocodec.h"
#include "Core/HLE/sceMp3.h"
#include "Core/HLE/AtracCtx.h"
#include "Core/HLE/sceSas.h"
#include "Core/HW/SasAudio.h"

#include "Core/CoreTiming.h"
// Threads window
#include "Core/HLE/sceKernelThread.h"

// Callstack window
#include "Core/MIPS/MIPSStackWalk.h"

// GPU things
#include "GPU/Common/GPUDebugInterface.h"
#include "GPU/Debugger/Stepping.h"

#include "UI/ImDebugger/ImDebugger.h"
#include "UI/ImDebugger/ImGe.h"

extern bool g_TakeScreenshot;
static ImVec4 g_normalTextColor;

void ShowInMemoryViewerMenuItem(uint32_t addr, ImControl &control) {
	if (ImGui::BeginMenu("Show in memory viewer")) {
		for (int i = 0; i < 4; i++) {
			if (ImGui::MenuItem(ImMemWindow::Title(i))) {
				control.command = { ImCmd::SHOW_IN_MEMORY_VIEWER, addr, (u32)i };
			}
		}
		ImGui::EndMenu();
	}
}

void ShowInMemoryDumperMenuItem(uint32_t addr, uint32_t size, MemDumpMode mode, ImControl &control) {
	if (ImGui::MenuItem(mode == MemDumpMode::Raw ? "Dump bytes to file..." : "Disassemble to file...")) {
		control.command = { ImCmd::SHOW_IN_MEMORY_DUMPER, addr, size, (u32)mode};
	}
}

void ShowInWindowMenuItems(uint32_t addr, ImControl &control) {
	// Enable when we implement the memory viewer
	ShowInMemoryViewerMenuItem(addr, control);
	if (ImGui::MenuItem("Show in CPU debugger")) {
		control.command = { ImCmd::SHOW_IN_CPU_DISASM, addr };
	}
	if (ImGui::MenuItem("Show in GE debugger")) {
		control.command = { ImCmd::SHOW_IN_GE_DISASM, addr };
	}
}

void StatusBar(std::string_view status) {
	if (!status.size()) {
		return;
	}
	ImGui::TextUnformatted(status.data(), status.data() + status.length());
	ImGui::SameLine();
	if (ImGui::SmallButton("Copy")) {
		System_CopyStringToClipboard(status);
	}
}

// TODO: Style it more
// Left click performs the preferred action, if any. Right click opens a menu for more.
void ImClickableValue(const char *id, uint32_t value, ImControl &control, ImCmd cmd) {
	ImGui::PushID(id);

	bool validAddr = Memory::IsValidAddress(value);

	char temp[32];
	snprintf(temp, sizeof(temp), "%08x", value);
	if (ImGui::Selectable(temp) && validAddr) {
		control.command = { cmd, value };
	}

	// Create a right-click popup menu. Restore the color while it's up. NOTE: can't query the theme, pushcolor modifies it!
	ImGui::PushStyleColor(ImGuiCol_Text, g_normalTextColor);
	if (ImGui::BeginPopupContextItem(temp)) {
		if (ImGui::MenuItem(validAddr ? "Copy address to clipboard" : "Copy value to clipboard")) {
			System_CopyStringToClipboard(temp);
		}
		if (validAddr) {
			ImGui::Separator();
			ShowInWindowMenuItems(value, control);
		}
		ImGui::EndPopup();
	}
	ImGui::PopStyleColor();
	ImGui::PopID();
}

// Left click performs the preferred action, if any. Right click opens a menu for more.
void ImClickableValueFloat(const char *id, float value) {
	ImGui::PushID(id);

	char temp[32];
	snprintf(temp, sizeof(temp), "%0.7f", value);
	if (ImGui::Selectable(temp)) {}

	// Create a right-click popup menu. Restore the color while it's up. NOTE: can't query the theme, pushcolor modifies it!
	ImGui::PushStyleColor(ImGuiCol_Text, g_normalTextColor);
	if (ImGui::BeginPopupContextItem(temp)) {
		if (ImGui::MenuItem("Copy value to clipboard")) {
			System_CopyStringToClipboard(temp);
		}
		ImGui::EndPopup();
	}
	ImGui::PopStyleColor();
	ImGui::PopID();
}

void DrawSchedulerView(ImConfig &cfg) {
	ImGui::SetNextWindowSize(ImVec2(420, 300), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Event Scheduler", &cfg.schedulerOpen)) {
		ImGui::End();
		return;
	}
	s64 ticks = CoreTiming::GetTicks();
	if (ImGui::BeginChild("event_list", ImVec2(300.0f, 0.0))) {
		const CoreTiming::Event *event = CoreTiming::GetFirstEvent();
		while (event) {
			ImGui::Text("%s (%lld): %d", CoreTiming::GetEventTypes()[event->type].name, event->time - ticks, (int)event->userdata);
			event = event->next;
		}
		ImGui::EndChild();
	}
	ImGui::SameLine();
	if (ImGui::BeginChild("general")) {
		ImGui::Text("CoreState: %s", CoreStateToString(coreState));
		ImGui::Text("downcount: %d", currentMIPS->downcount);
		ImGui::Text("slicelength: %d", CoreTiming::slicelength);
		ImGui::Text("Ticks: %lld", ticks);
		ImGui::Text("Clock (MHz): %0.1f", (float)CoreTiming::GetClockFrequencyHz() / 1000000.0f);
		ImGui::Text("Global time (us): %lld", CoreTiming::GetGlobalTimeUs());
		ImGui::EndChild();
	}
	ImGui::End();
}

static void DrawGPRs(ImConfig &config, ImControl &control, const MIPSDebugInterface *mipsDebug, const ImSnapshotState &prev) {
	ImGui::SetNextWindowSize(ImVec2(320, 600), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("GPRs", &config.gprOpen)) {
		ImGui::End();
		return;
	}

	bool noDiff = coreState == CORE_RUNNING_CPU || coreState == CORE_STEPPING_GE;

	if (ImGui::Button("Copy all to clipboard")) {
		char *buffer = new char[20000];
		StringWriter w(buffer);
		for (int i = 0; i < 32; i++) {
			u32 value = mipsDebug->GetGPR32Value(i);
			w.F("%s: %08x (%d)", mipsDebug->GetRegName(0, i).c_str(), value, value).endl();
		}
		w.F("hi: %08x", mipsDebug->GetHi()).endl();
		w.F("lo: %08x", mipsDebug->GetLo()).endl();
		w.F("pc: %08x", mipsDebug->GetPC()).endl();
		System_CopyStringToClipboard(buffer);
		delete[] buffer;
	}

	if (ImGui::BeginTable("gpr", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH)) {
		ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Decimal", ImGuiTableColumnFlags_WidthStretch);

		ImGui::TableHeadersRow();

		auto gprLine = [&](int index, const char *regname, int value, int prevValue) {
			bool diff = value != prevValue && !noDiff;
			bool disabled = value == 0xdeadbeef;

			ImGui::TableNextColumn();
			ImGui::TextUnformatted(regname);
			ImGui::TableNextColumn();
			if (diff) {
				ImGui::PushStyleColor(ImGuiCol_Text, !disabled ? ImDebuggerColor_Diff : ImDebuggerColor_DiffAlpha);
			} else if (disabled) {
				ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 128));
			}
			// TODO: Check if the address is in the code segment to decide default action.
			ImClickableValue(regname, value, control, index == MIPS_REG_RA ? ImCmd::SHOW_IN_CPU_DISASM : ImCmd::SHOW_IN_MEMORY_VIEWER);
			ImGui::TableNextColumn();
			if (value >= -1000000 && value <= 1000000) {
				ImGui::Text("%d", value);
			}
			if (diff || disabled) {
				ImGui::PopStyleColor();
			}
		};
		for (int i = 0; i < 32; i++) {
			ImGui::TableNextRow();
			gprLine(i, mipsDebug->GetRegName(0, i).c_str(), mipsDebug->GetGPR32Value(i), prev.gpr[i]);
		}
		ImGui::TableNextRow();
		gprLine(32, "hi", mipsDebug->GetHi(), prev.hi);
		ImGui::TableNextRow();
		gprLine(33, "lo", mipsDebug->GetLo(), prev.lo);
		ImGui::TableNextRow();
		gprLine(34, "pc", mipsDebug->GetPC(), prev.pc);
		ImGui::TableNextRow();
		gprLine(35, "ll", mipsDebug->GetLLBit(), prev.ll);
		ImGui::EndTable();
	}
	ImGui::End();
}

static void DrawFPRs(ImConfig &config, ImControl &control, const MIPSDebugInterface *mipsDebug, const ImSnapshotState &prev) {
	ImGui::SetNextWindowSize(ImVec2(320, 600), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("FPRs", &config.fprOpen)) {
		ImGui::End();
		return;
	}

	bool noDiff = coreState == CORE_RUNNING_CPU || coreState == CORE_STEPPING_GE;

	if (ImGui::BeginTable("fpr", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH)) {
		ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Hex", ImGuiTableColumnFlags_WidthStretch);

		ImGui::TableHeadersRow();

		// fpcond
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted("fpcond");
		ImGui::TableNextColumn();
		ImGui::Text("%08x", mipsDebug->GetFPCond());

		for (int i = 0; i < 32; i++) {
			float fvalue = mipsDebug->GetFPR32Value(i);
			float prevValue = prev.fpr[i];

			// NOTE: Using memcmp to avoid NaN problems.
			bool diff = memcmp(&fvalue, &prevValue, 4) != 0 && !noDiff;

			u32 fivalue;
			memcpy(&fivalue, &fvalue, sizeof(fivalue));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			if (diff) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImDebuggerColor_Diff);
			}
			ImGui::TextUnformatted(mipsDebug->GetRegName(1, i).c_str());
			ImGui::TableNextColumn();
			ImClickableValueFloat(mipsDebug->GetRegName(1, i).c_str(), fvalue);
			ImGui::TableNextColumn();
			ImGui::Text("%08x", fivalue);
			if (diff) {
				ImGui::PopStyleColor();
			}
		}

		ImGui::EndTable();
	}
	ImGui::End();
}

static void DrawVFPU(ImConfig &config, ImControl &control, const MIPSDebugInterface *mipsDebug, const ImSnapshotState &prev) {
	ImGui::SetNextWindowSize(ImVec2(320, 600), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("VFPU", &config.vfpuOpen)) {
		ImGui::End();
		return;
	}
	ImGui::Text("TODO");
	ImGui::End();
}

static const char *ThreadStatusToString(u32 status) {
	switch (status) {
	case THREADSTATUS_RUNNING: return "Running";
	case THREADSTATUS_READY: return "Ready";
	case THREADSTATUS_WAIT: return "Wait";
	case THREADSTATUS_SUSPEND: return "Suspended";
	case THREADSTATUS_DORMANT: return "Dormant";
	case THREADSTATUS_DEAD: return "Dead";
	case THREADSTATUS_WAITSUSPEND: return "WaitSuspended";
	default:
		break;
	}
	return "(unk)";
}

void WaitIDToString(WaitType waitType, SceUID waitID, char *buffer, size_t bufSize) {
	switch (waitType) {
	case WAITTYPE_AUDIOCHANNEL:
		snprintf(buffer, bufSize, "chan %d", (int)waitID);
		return;
	case WAITTYPE_IO:
		// TODO: More detail
		snprintf(buffer, bufSize, "fd: %d", (int)waitID);
		return;
	case WAITTYPE_ASYNCIO:
		snprintf(buffer, bufSize, "id: %d", (int)waitID);
		return;
	case WAITTYPE_THREADEND:
	case WAITTYPE_MUTEX:
	case WAITTYPE_LWMUTEX:
	case WAITTYPE_MODULE:
	case WAITTYPE_MSGPIPE:
	case WAITTYPE_FPL:
	case WAITTYPE_VPL:
	case WAITTYPE_MBX:
	case WAITTYPE_EVENTFLAG:
	case WAITTYPE_SEMA:
		// Get the name of the thread
		if (kernelObjects.IsValid(waitID)) {
			auto obj = kernelObjects.GetFast<KernelObject>(waitID);
			if (obj && obj->GetName()) {
				truncate_cpy(buffer, bufSize, obj->GetName());
				return;
			}
		}
		break;
	case WAITTYPE_DELAY:
	case WAITTYPE_SLEEP:
	case WAITTYPE_HLEDELAY:
	case WAITTYPE_UMD:
	case WAITTYPE_NONE:
	case WAITTYPE_VBLANK:
	case WAITTYPE_MICINPUT:
		truncate_cpy(buffer, bufSize, "-");
		return;
	default:
		truncate_cpy(buffer, bufSize, "(unimpl)");
		return;
	}

}

void DrawThreadView(ImConfig &cfg, ImControl &control) {
	ImGui::SetNextWindowSize(ImVec2(420, 300), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Threads", &cfg.threadsOpen)) {
		ImGui::End();
		return;
	}

	std::vector<DebugThreadInfo> info = GetThreadsInfo();
	if (ImGui::BeginTable("threads", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH)) {
		ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("PC", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Entry", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Wait Type", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Wait ID", ImGuiTableColumnFlags_WidthStretch);
		// .initialStack, .stackSize, etc
		ImGui::TableHeadersRow();

		for (int i = 0; i < (int)info.size(); i++) {
			const DebugThreadInfo &thread = info[i];
			ImGui::TableNextRow();
			ImGui::PushID(i);
			ImGui::TableNextColumn();
			ImGui::Text("%d", thread.id);
			ImGui::TableNextColumn();
			if (ImGui::Selectable(thread.name, cfg.selectedThread == i, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns)) {
				cfg.selectedThread = i;
			}
			if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
				cfg.selectedThread = i;
				ImGui::OpenPopup("threadPopup");
			}
			ImGui::TableNextColumn();
			ImClickableValue("curpc", thread.curPC, control, ImCmd::SHOW_IN_CPU_DISASM);
			ImGui::TableNextColumn();
			ImClickableValue("entry", thread.entrypoint, control, ImCmd::SHOW_IN_CPU_DISASM);
			ImGui::TableNextColumn();
			ImGui::Text("%d", thread.priority);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(ThreadStatusToString(thread.status));
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(getWaitTypeName(thread.waitType));
			ImGui::TableNextColumn();
			char temp[64];
			WaitIDToString(thread.waitType, thread.waitID, temp, sizeof(temp));
			ImGui::TextUnformatted(temp);
			if (ImGui::BeginPopup("threadPopup")) {
				DebugThreadInfo &thread = info[i];
				ImGui::Text("Thread: %s", thread.name);
				if (ImGui::MenuItem("Copy entry to clipboard")) {
					char temp[64];
					snprintf(temp, sizeof(temp), "%08x", thread.entrypoint);
					System_CopyStringToClipboard(temp);
				}
				if (ImGui::MenuItem("Copy thread PC to clipboard")) {
					char temp[64];
					snprintf(temp, sizeof(temp), "%08x", thread.curPC);
					System_CopyStringToClipboard(temp);
				}
				if (ImGui::MenuItem("Kill thread")) {
					// Dangerous!
					sceKernelTerminateThread(thread.id);
				}
				if (thread.status == THREADSTATUS_WAIT) {
					if (ImGui::MenuItem("Force run now")) {
						__KernelResumeThreadFromWait(thread.id, 0);
					}
				}
				ImGui::EndPopup();
			}
			ImGui::PopID();
		}

		ImGui::EndTable();
	}
	ImGui::End();
}

// TODO: Add popup menu, export file, export dir, etc...
static void RecurseFileSystem(IFileSystem *fs, std::string path, RequesterToken token) {
	std::vector<PSPFileInfo> fileInfo = fs->GetDirListing(path);
	for (auto &file : fileInfo) {
		if (file.type == FileType::FILETYPE_DIRECTORY) {
			if (file.name != "." && file.name != ".." && ImGui::TreeNode(file.name.c_str())) {
				std::string fpath = path + "/" + file.name;
				RecurseFileSystem(fs, fpath, token);
				ImGui::TreePop();
			}
		} else {
			ImGui::Selectable(file.name.c_str());
			if (ImGui::BeginPopupContextItem()) {
				if (ImGui::MenuItem("Copy Path")) {
					System_CopyStringToClipboard(path + "/" + file.name);
				}
				if (ImGui::MenuItem("Save file...")) {
					std::string fullPath = path + "/" + file.name;
					int size = file.size;
					// save dialog
					System_BrowseForFileSave(token, "Save file", file.name, BrowseFileType::ANY, [fullPath, fs, size](const char *responseString, int) {
						int fd = fs->OpenFile(fullPath, FILEACCESS_READ);
						if (fd >= 0) {
							std::string data;
							data.resize(size);
							fs->ReadFile(fd, (u8 *)data.data(), size);
							fs->CloseFile(fd);
							Path dest(responseString);
							File::WriteDataToFile(false, data.data(), data.size(), dest);
						}
					});
				}
				// your popup code
				ImGui::EndPopup();
			}
		}
	}
}

static void DrawFilesystemBrowser(ImConfig &cfg) {
	ImGui::SetNextWindowSize(ImVec2(420, 500), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("File System", &cfg.filesystemBrowserOpen)) {
		ImGui::End();
		return;
	}

	for (auto &fs : pspFileSystem.GetMounts()) {
		std::string path;
		char desc[256];
		fs.system->Describe(desc, sizeof(desc));
		char fsTitle[512];
		snprintf(fsTitle, sizeof(fsTitle), "%s - %s", fs.prefix.c_str(), desc);
		if (ImGui::TreeNode(fsTitle)) {
			auto system = fs.system;
			RecurseFileSystem(system.get(), path, cfg.requesterToken);
			ImGui::TreePop();
		}
	}

	ImGui::End();
}

static void DrawKernelObjects(ImConfig &cfg) {
	if (!ImGui::Begin("Kernel Objects", &cfg.kernelObjectsOpen)) {
		ImGui::End();
		return;
	}
	if (ImGui::BeginTable("kos", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH | ImGuiTableFlags_Resizable)) {
		ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Summary", ImGuiTableColumnFlags_WidthStretch);

		ImGui::TableHeadersRow();

		for (int i = 0; i < (int)KernelObjectPool::maxCount; i++) {
			int id = i + KernelObjectPool::handleOffset;
			if (!kernelObjects.IsValid(id)) {
				continue;
			}
			KernelObject *obj = kernelObjects.GetFast<KernelObject>(id);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushID(i);
			if (ImGui::Selectable("", cfg.selectedThread == i, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns)) {
				cfg.selectedThread = i;
			}
			ImGui::SameLine();
			/*
			if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
				cfg.selectedThread = i;
				ImGui::OpenPopup("kernelObjectPopup");
			}
			*/
			ImGui::Text("%04x", id);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(obj->GetTypeName());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(obj->GetName());
			ImGui::TableNextColumn();
			char qi[128];
			obj->GetQuickInfo(qi, sizeof(qi));
			ImGui::TextUnformatted(qi);
			ImGui::PopID();
		}

		ImGui::EndTable();
	}
	ImGui::End();
}

static void DrawNp(ImConfig &cfg) {
	if (!ImGui::Begin("NP", &cfg.npOpen)) {
		ImGui::End();
		return;
	}

	ImGui::Text("Signed in: %d", npSigninState);
	ImGui::Text("Title ID: %s", npTitleId.data);

	SceNpId id{};
	NpGetNpId(&id);
	ImGui::Text("User Handle: %s", id.handle.data);


	ImGui::End();
}

static void DrawApctl(ImConfig &cfg) {
	if (!ImGui::Begin("Apctl", &cfg.apctlOpen)) {
		ImGui::End();
		return;
	}

	ImGui::Text("State: %s", ApctlStateToString(netApctlState));
	if (netApctlState != PSP_NET_APCTL_STATE_DISCONNECTED && ImGui::CollapsingHeader("ApCtl Details")) {
		ImGui::Text("Name: %s", netApctlInfo.name);
		ImGui::Text("IP: %s", netApctlInfo.ip);
		ImGui::Text("SubnetMask: %s", netApctlInfo.ip);
		ImGui::Text("SSID: %.*s", netApctlInfo.ssidLength, netApctlInfo.ssid);
		ImGui::Text("SSID: %.*s", netApctlInfo.ssidLength, netApctlInfo.ssid);
		ImGui::Text("Gateway: %s", netApctlInfo.gateway);
		ImGui::Text("PrimaryDNS: %s", netApctlInfo.primaryDns);
		ImGui::Text("SecondaryDNS: %s", netApctlInfo.secondaryDns);
	}

	if (g_Config.bInfrastructureAutoDNS) {
		if (g_infraDNSConfig.loaded) {
			if (!g_infraDNSConfig.gameName.empty()) {
				ImGui::Text("Known game: %s", g_infraDNSConfig.gameName.c_str());
			}
			ImGui::Text("connectAdhocForGrouping: %s", BoolStr(g_infraDNSConfig.connectAdHocForGrouping));
			ImGui::Text("DNS: %s", g_infraDNSConfig.dns.c_str());
			if (!g_infraDNSConfig.dyn_dns.empty()) {
				ImGui::Text("DynDNS: %s", g_infraDNSConfig.dyn_dns.c_str());
			}
			if (!g_infraDNSConfig.fixedDNS.empty()) {
				ImGui::TextUnformatted("Fixed DNS");
				for (auto iter : g_infraDNSConfig.fixedDNS) {
					ImGui::Text("%s -> %s", iter.first.c_str(), iter.second.c_str());
				}
			}
		}
	}

	ImGui::End();
}

static void DrawInternals(ImConfig &cfg) {
	if (!ImGui::Begin("PPSSPP Internals", &cfg.internalsOpen)) {
		ImGui::End();
		return;
	}

	struct entry {
		PSPDirectories dir;
		const char *name;
	};

	static const entry dirs[] = {
		{DIRECTORY_PSP, "PSP"},
		{DIRECTORY_CHEATS, "CHEATS"},
		{DIRECTORY_SCREENSHOT, "SCREENSHOT"},
		{DIRECTORY_SYSTEM, "SYSTEM"},
		{DIRECTORY_GAME, "GAME"},
		{DIRECTORY_SAVEDATA, "SAVEDATA"},
		{DIRECTORY_PAUTH, "PAUTH"},
		{DIRECTORY_DUMP, "DUMP"},
		{DIRECTORY_SAVESTATE, "SAVESTATE"},
		{DIRECTORY_CACHE, "CACHE"},
		{DIRECTORY_TEXTURES, "TEXTURES"},
		{DIRECTORY_PLUGINS, "PLUGINS"},
		{DIRECTORY_APP_CACHE, "APP_CACHE"},
		{DIRECTORY_VIDEO, "VIDEO"},
		{DIRECTORY_AUDIO, "AUDIO"},
		{DIRECTORY_MEMSTICK_ROOT, "MEMSTICK_ROOT"},
		{DIRECTORY_EXDATA, "EXDATA"},
		{DIRECTORY_CUSTOM_SHADERS, "CUSTOM_SHADERS"},
		{DIRECTORY_CUSTOM_THEMES, "CUSTOM_THEMES"},
	};

	if (ImGui::CollapsingHeader("Directories", ImGuiTreeNodeFlags_DefaultOpen)) {
		for (auto &dir : dirs) {
			ImGui::Text("%s: %s", dir.name, GetSysDirectory(dir.dir).c_str());
		}
	}

	ImGui::End();
}

static void DrawAdhoc(ImConfig &cfg) {
	if (!ImGui::Begin("AdHoc", &cfg.adhocOpen)) {
		ImGui::End();
		return;
	}

	const char *discoverStatusStr = "N/A";
	switch (netAdhocDiscoverStatus) {
	case 0: discoverStatusStr = "NONE"; break;
	case 1: discoverStatusStr = "IN_PROGRESS"; break;
	case 2: discoverStatusStr = "COMPLETED"; break;
	default: break;
	}

	auto &io = ImGui::GetIO();

	/*
	ImGui::Text("WantCaptureMouse: %s", BoolStr(io.WantCaptureMouse));
	ImGui::Text("WantCaptureKeyboard: %s", BoolStr(io.WantCaptureKeyboard));
	ImGui::Text("WantCaptureMouseUnlessPopupClose: %s", BoolStr(io.WantCaptureMouseUnlessPopupClose));
	ImGui::Text("WantTextInput: %s", BoolStr(io.WantTextInput));
	*/

	ImGui::Text("sceNetAdhoc inited: %s", BoolStr(netAdhocInited));
	ImGui::Text("sceNetAdhocctl inited: %s", BoolStr(netAdhocctlInited));
	ImGui::Text("sceNetAdhocctl state: %s", AdhocCtlStateToString(NetAdhocctl_GetState()));
	ImGui::Text("sceNetAdhocMatching inited: %s", BoolStr(netAdhocctlInited));
	ImGui::Text("GameMode entered: %s", BoolStr(netAdhocGameModeEntered));
	ImGui::Text("FriendFinder running: %s", BoolStr(g_adhocServerConnected));
	ImGui::Text("sceNetAdhocDiscover status: %s", discoverStatusStr);

	if (ImGui::BeginTable("sock", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH | ImGuiTableFlags_Resizable)) {
		ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Non-blocking", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("BufSize", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("IsClient", ImGuiTableColumnFlags_WidthFixed);

		ImGui::TableHeadersRow();

		for (int i = 0; i < MAX_SOCKET; i++) {
			const AdhocSocket *socket = adhocSockets[i];
			if (!socket) {
				continue;
			}

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("%d", i + 1);
			ImGui::TableNextColumn();
			switch (socket->type) {
			case SOCK_PDP: ImGui::TextUnformatted("PDP"); break;
			case SOCK_PTP: ImGui::TextUnformatted("PTP"); break;
			default: ImGui::Text("(%d)", socket->type); break;
			}
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(socket->nonblocking ? "Non-blocking" : "Blocking");
			ImGui::TableNextColumn();
			ImGui::Text("%d", socket->buffer_size);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(BoolStr(socket->isClient));
		}
		ImGui::EndTable();
	}

	ImGui::End();
}

static void DrawSockets(ImConfig &cfg) {
	if (!ImGui::Begin("Sockets", &cfg.socketsOpen)) {
		ImGui::End();
		return;
	}
	if (ImGui::BeginTable("sock", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH | ImGuiTableFlags_Resizable)) {
		ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Port", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("IP address", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Non-blocking", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Created by", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Domain", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Protocol", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Host handle", ImGuiTableColumnFlags_WidthFixed);

		ImGui::TableHeadersRow();

		for (int i = SocketManager::MIN_VALID_INET_SOCKET; i < SocketManager::VALID_INET_SOCKET_COUNT; i++) {
			InetSocket *inetSocket;
			if (!g_socketManager.GetInetSocket(i, &inetSocket)) {
				continue;
			}

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("%d", i);
			ImGui::TableNextColumn();
			ImGui::Text("%d", inetSocket->port);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(inetSocket->addr.c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(inetSocket->nonblocking ? "Non-blocking" : "Blocking");
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(SocketStateToString(inetSocket->state));
			ImGui::TableNextColumn();
			std::string str = inetSocketDomain2str(inetSocket->domain);
			ImGui::TextUnformatted(str.c_str());
			ImGui::TableNextColumn();
			str = inetSocketType2str(inetSocket->type);
			ImGui::TextUnformatted(str.c_str());
			ImGui::TableNextColumn();
			str = inetSocketProto2str(inetSocket->protocol);
			ImGui::TextUnformatted(str.c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%d", (int)inetSocket->sock);
		}

		ImGui::EndTable();
	}
	ImGui::End();
}

static const char *MemCheckConditionToString(MemCheckCondition cond) {
	switch (cond) {
	case MEMCHECK_READ: return "Read";
	case MEMCHECK_WRITE: return "Write";
	case MEMCHECK_READWRITE: return "Read/Write";
	case MEMCHECK_WRITE | MEMCHECK_WRITE_ONCHANGE: return "Write Change";
	case MEMCHECK_READWRITE | MEMCHECK_WRITE_ONCHANGE: return "Read/Write Change";
	default:
		return "(bad!)";
	}
}

static void DrawBreakpointsView(MIPSDebugInterface *mipsDebug, ImConfig &cfg) {
	if (!ImGui::Begin("Breakpoints", &cfg.breakpointsOpen)) {
		ImGui::End();
		return;
	}
	if (ImGui::BeginTable("bp_window", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
		ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("right", ImGuiTableColumnFlags_WidthFixed, 150.0f);
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		auto &bps = g_breakpoints.GetBreakpointRefs();
		auto &mcs = g_breakpoints.GetMemCheckRefs();

		if (cfg.selectedBreakpoint >= bps.size()) {
			cfg.selectedBreakpoint = -1;
		}
		if (cfg.selectedMemCheck >= mcs.size()) {
			cfg.selectedMemCheck = -1;
		}

		if (ImGui::Button("Add Breakpoint")) {
			cfg.selectedBreakpoint = g_breakpoints.AddBreakPoint(0);
			cfg.selectedMemCheck = -1;
		}
		ImGui::SameLine();
		if (ImGui::Button("Add MemCheck")) {
			cfg.selectedMemCheck = g_breakpoints.AddMemCheck(0, 0, MemCheckCondition::MEMCHECK_WRITE, BreakAction::BREAK_ACTION_PAUSE);
			cfg.selectedBreakpoint = -1;
		}

		if (ImGui::BeginTable("breakpoints", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH)) {
			ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Size/Label", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("OpCode", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Cond", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			for (int i = 0; i < (int)bps.size(); i++) {
				auto &bp = bps[i];
				bool temp = bp.temporary;
				if (temp) {
					continue;
				}

				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::PushID(i);
				// TODO: This clashes with the checkbox!
				if (ImGui::Selectable("", cfg.selectedBreakpoint == i, ImGuiSelectableFlags_SpanAllColumns) && !bp.temporary) {
					cfg.selectedBreakpoint = i;
					cfg.selectedMemCheck = -1;
				}
				ImGui::SameLine();
				ImGui::CheckboxFlags("##enabled", (int *)&bp.result, (int)BREAK_ACTION_PAUSE);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted("Exec");
				ImGui::TableNextColumn();
				ImGui::Text("%08x", bp.addr);
				ImGui::TableNextColumn();
				const std::string sym = g_symbolMap->GetLabelString(bp.addr);
				if (!sym.empty()) {
					ImGui::TextUnformatted(sym.c_str());  // size/label
				} else {
					ImGui::TextUnformatted("-");  // size/label
				}
				ImGui::TableNextColumn();
				// disasm->getOpcodeText(displayedBreakPoints_[index].addr, temp, sizeof(temp));
				ImGui::TextUnformatted("-");  // opcode
				ImGui::TableNextColumn();
				if (bp.hasCond) {
					ImGui::TextUnformatted(bp.cond.expressionString.c_str());
				} else {
					ImGui::TextUnformatted("-");  // condition
				}
				ImGui::TableNextColumn();
				ImGui::Text("-");  // hits not available on exec bps yet
				ImGui::PopID();
			}

			// OK, now list the memchecks.
			for (int i = 0; i < (int)mcs.size(); i++) {
				auto &mc = mcs[i];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::PushID(i + 10000);
				if (ImGui::Selectable("", cfg.selectedMemCheck == i, ImGuiSelectableFlags_SpanAllColumns)) {
					cfg.selectedBreakpoint = -1;
					cfg.selectedMemCheck = i;
				}
				ImGui::SameLine();
				ImGui::CheckboxFlags("", (int *)&mc.result, BREAK_ACTION_PAUSE);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(MemCheckConditionToString(mc.cond));
				ImGui::TableNextColumn();
				ImGui::Text("%08x", mc.start);
				ImGui::TableNextColumn();
				ImGui::Text("%08x", mc.end ? (mc.end - mc.start) : 1);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted("-");  // opcode
				ImGui::TableNextColumn();
				ImGui::TextUnformatted("-");  // cond
				ImGui::TableNextColumn();
				ImGui::Text("%d", mc.numHits);
				ImGui::PopID();
			}

			ImGui::EndTable();
		}

		ImGui::TableNextColumn();

		if (cfg.selectedBreakpoint >= 0) {
			// Add edit form for breakpoints
			if (ImGui::BeginChild("bp_edit")) {
				auto &bp = bps[cfg.selectedBreakpoint];
				ImGui::TextUnformatted("Edit breakpoint");
				ImGui::CheckboxFlags("Enabled", (int *)&bp.result, (int)BREAK_ACTION_PAUSE);
				ImGui::InputScalar("Address", ImGuiDataType_U32, &bp.addr, nullptr, nullptr, "%08x", ImGuiInputTextFlags_CharsHexadecimal);
				if (ImGui::Button("Delete")) {
					g_breakpoints.RemoveBreakPoint(bp.addr);
				}
				ImGui::EndChild();
			}
		}

		if (cfg.selectedMemCheck >= 0) {
			// Add edit form for memchecks
			if (ImGui::BeginChild("mc_edit")) {
				auto &mc = mcs[cfg.selectedMemCheck];
				ImGui::TextUnformatted("Edit memcheck");
				ImGui::CheckboxFlags("Enabled", (int *)&mc.result, (int)BREAK_ACTION_PAUSE);
				ImGui::InputScalar("Start", ImGuiDataType_U32, &mc.start);
				ImGui::InputScalar("End", ImGuiDataType_U32, &mc.end);
				if (ImGui::Button("Delete")) {
					g_breakpoints.RemoveMemCheck(mcs[cfg.selectedMemCheck].start, mcs[cfg.selectedMemCheck].end);
				}
				ImGui::EndChild();
			}
		}
		ImGui::EndTable();
	}

	ImGui::End();
}

void DrawAudioDecodersView(ImConfig &cfg, ImControl &control) {
	if (!ImGui::Begin("Audio decoding contexts", &cfg.audioDecodersOpen)) {
		ImGui::End();
		return;
	}

	if (ImGui::CollapsingHeader("sceAtrac", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::BeginTable("atracs", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH)) {
			ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("OutChans", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("CurrentSample", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("RemainingFrames", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Impl", ImGuiTableColumnFlags_WidthFixed);

			ImGui::TableHeadersRow();

			for (int i = 0; i < PSP_NUM_ATRAC_IDS; i++) {
				u32 type = 0;
				const AtracBase *ctx = __AtracGetCtx(i, &type);
				if (!ctx) {
					continue;
				}

				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				char temp[16];
				snprintf(temp, sizeof(temp), "%d", i);
				if (ImGui::Selectable(temp, i == cfg.selectedAtracCtx, ImGuiSelectableFlags_SpanAllColumns)) {
					cfg.selectedAtracCtx = i;
				}
				ImGui::TableNextColumn();
				switch (type) {
				case PSP_MODE_AT_3_PLUS:
					ImGui::TextUnformatted("Atrac3+");
					break;
				case PSP_MODE_AT_3:
					ImGui::TextUnformatted("Atrac3");
					break;
				default:
					ImGui::Text("%04x", type);
					break;
				}
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(AtracStatusToString(ctx->BufferState()));
				ImGui::TableNextColumn();
				ImGui::Text("%d", ctx->GetOutputChannels());
				ImGui::TableNextColumn();
				ImGui::Text("%d", ctx->CurrentSample());
				ImGui::TableNextColumn();
				ImGui::Text("%d", ctx->RemainingFrames());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(ctx->IsNewAtracImpl() ? "NewImpl" : "Legacy");
			}

			ImGui::EndTable();
		}

		if (cfg.selectedAtracCtx >= 0 && cfg.selectedAtracCtx < PSP_NUM_ATRAC_IDS) {
			u32 type = 0;
			const AtracBase *ctx = __AtracGetCtx(cfg.selectedAtracCtx, &type);
			// Show details about the selected atrac context here.
			char header[32];
			snprintf(header, sizeof(header), "Atrac context %d", cfg.selectedAtracCtx);
			if (ctx && ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::ProgressBar((float)ctx->CurrentSample() / (float)ctx->GetTrack().endSample, ImVec2(200.0f, 0.0f));
				ImGui::Text("Status: %s", AtracStatusToString(ctx->BufferState()));
				ImGui::Text("first/cur/end sample: %d/%d/%d", ctx->GetTrack().FirstSampleOffsetFull(), ctx->CurrentSample(), ctx->GetTrack().endSample);
				if (ctx->context_.IsValid()) {
					ImGui::Text("ctx addr: ");
					ImGui::SameLine();
					ImClickableValue("ctx", ctx->context_.ptr, control, ImCmd::SHOW_IN_MEMORY_VIEWER);
				}
				if (ctx->context_.IsValid() && ctx->IsNewAtracImpl()) {
					const auto &info = ctx->context_->info;
					ImGui::Text("Buffer: (size: %d / %08x) Frame: %d", info.bufferByte, info.bufferByte, info.sampleSize);
					ImGui::SameLine();
					ImClickableValue("buffer", info.buffer, control, ImCmd::SHOW_IN_MEMORY_VIEWER);
					if (info.secondBuffer || info.secondBufferByte) {
						ImGui::Text("Second: (size: %d / %08x)", info.secondBufferByte, info.secondBufferByte);
						ImGui::SameLine();
						ImClickableValue("second", info.secondBuffer, control, ImCmd::SHOW_IN_MEMORY_VIEWER);
					}
					ImGui::Text("Data: %d/%d", info.dataOff, info.dataEnd);
					if (info.state != ATRAC_STATUS_STREAMED_WITHOUT_LOOP) {
						ImGui::Text("LoopNum: %d (%d-%d)", info.loopNum, info.loopStart, info.loopEnd);
					}
					ImGui::Text("DecodePos: %d EndSample: %d", info.decodePos, info.dataEnd);
					if (AtracStatusIsStreaming(info.state)) {
						ImGui::Text("Stream: offset %d, streamDataBytes: %d", info.streamOff, info.streamDataByte);
					}
					// Display unknown vars.
					ImGui::Text("numFrame: %d unk22: %d unk52: %d", info.numFrame, info.unk22, info.unk52);
				} else {
					ImGui::Text("loop: %d", ctx->LoopNum());
				}
			}
		}
	}

	if (ImGui::CollapsingHeader("sceAudiocodec", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::BeginTable("codecs", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH)) {
			ImGui::TableSetupColumn("CtxAddr", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);

			for (auto &iter : g_audioDecoderContexts) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::Text("%08x", iter.first);
				ImGui::TableNextColumn();
				switch (iter.second->GetAudioType()) {
				case PSP_CODEC_AAC: ImGui::TextUnformatted("AAC"); break;
				case PSP_CODEC_MP3: ImGui::TextUnformatted("MP3"); break;
				case PSP_CODEC_AT3PLUS: ImGui::TextUnformatted("Atrac3+"); break;
				case PSP_CODEC_AT3: ImGui::TextUnformatted("Atrac3"); break;
				default: ImGui::Text("%08x", iter.second->GetAudioType()); break;
				}
			}
			ImGui::EndTable();
		}
	}

	if (ImGui::CollapsingHeader("sceMp3", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::BeginTable("mp3", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH)) {
			ImGui::TableSetupColumn("Handle", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Channels", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("StartPos", ImGuiTableColumnFlags_WidthFixed);
			// TODO: more..

			for (auto &iter : mp3Map) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::Text("%d", iter.first);
				if (!iter.second) {
					continue;
				}
				ImGui::TableNextColumn();
				ImGui::Text("%d", iter.second->Channels);
				ImGui::TableNextColumn();
				ImGui::Text("%d", (int)iter.second->startPos);
			}
			ImGui::EndTable();
		}
	}

	ImGui::End();
}

void DrawAudioChannels(ImConfig &cfg, ImControl &control) {
	if (!ImGui::Begin("Raw audio channels", &cfg.audioChannelsOpen)) {
		ImGui::End();
		return;
	}

	if (ImGui::BeginTable("audios", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH)) {
		ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("SampleAddr", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("SampleCount", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Volume", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Waiting Thread", ImGuiTableColumnFlags_WidthFixed);

		ImGui::TableHeadersRow();

		// vaudio / output2 uses channel 8.
		for (int i = 0; i < PSP_AUDIO_CHANNEL_MAX + 1; i++) {
			if (!chans[i].reserved) {
				continue;
			}
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			if (i == 8) {
				ImGui::TextUnformatted("audio2");
			} else {
				ImGui::Text("%d", i);
			}
			ImGui::TableNextColumn();
			char id[2]{};
			id[0] = i + 1;
			ImClickableValue(id, chans[i].sampleAddress, control, ImCmd::SHOW_IN_MEMORY_VIEWER);
			ImGui::TableNextColumn();
			ImGui::Text("%08x", chans[i].sampleCount);
			ImGui::TableNextColumn();
			ImGui::Text("%d | %d", chans[i].leftVolume, chans[i].rightVolume);
			ImGui::TableNextColumn();
			switch (chans[i].format) {
			case PSP_AUDIO_FORMAT_STEREO:
				ImGui::TextUnformatted("Stereo");
				break;
			case PSP_AUDIO_FORMAT_MONO:
				ImGui::TextUnformatted("Mono");
				break;
			default:
				ImGui::TextUnformatted("UNK: %04x");
				break;
			}
			ImGui::TableNextColumn();
			for (auto t : chans[i].waitingThreads) {
				KernelObject *thread = kernelObjects.GetFast<KernelObject>(t.threadID);
				if (thread) {
					ImGui::Text("%s: %d", thread->GetName(), t.numSamples);
				}
			}
		}

		ImGui::EndTable();
	}
	ImGui::End();
}

void DrawLogConfig(ImConfig &cfg) {
	if (!ImGui::Begin("Logs", &cfg.logConfigOpen)) {
		ImGui::End();
		return;
	}

	static const char *logLevels[] = {
		"N/A",
		"Notice",  // starts at 1 for some reason
		"Error",
		"Warn",
		"Info",
		"Debug",
		"Verb."
	};
	_dbg_assert_(ARRAY_SIZE(logLevels) == (int)LogLevel::LVERBOSE + 1);

	if (ImGui::BeginTable("logs", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH)) {
		ImGui::TableSetupColumn("Log", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 150.0f);
		ImGui::TableHeadersRow();
		for (int i = 0; i < (int)Log::NUMBER_OF_LOGS; i++) {
			LogChannel *chan = g_logManager.GetLogChannel((Log)i);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();

			const char *logName = LogManager::GetLogTypeName((Log)i);

			ImGui::PushID(logName);

			ImGui::Checkbox(logName, &chan->enabled);
			ImGui::TableNextColumn();

			if (ImGui::BeginCombo("-", logLevels[(int)chan->level])) {
				for (int i = 1; i < ARRAY_SIZE(logLevels); ++i) {
					LogLevel current = static_cast<LogLevel>(i);
					bool isSelected = (chan->level == current);
					if (ImGui::Selectable(logLevels[(int)current], isSelected)) {
						chan->level = current;
					}
					if (isSelected) {
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	ImGui::End();
}

static const char *VoiceTypeToString(VoiceType type) {
	switch (type) {
	case VOICETYPE_OFF: return "OFF";
	case VOICETYPE_VAG: return "VAG";  // default
	case VOICETYPE_NOISE: return "NOISE";
	case VOICETYPE_TRIWAVE: return "TRIWAVE";  // are these used? there are functions for them (sceSetTriangularWave)
	case VOICETYPE_PULSEWAVE: return "PULSEWAVE";
	case VOICETYPE_PCM: return "PCM";
	case VOICETYPE_ATRAC3: return "ATRAC3";
	default: return "(unknown!)";
	}
}

void DrawSasAudio(ImConfig &cfg) {
	if (!ImGui::Begin("sasAudio", &cfg.sasAudioOpen)) {
		ImGui::End();
		return;
	}

	const SasInstance *sas = GetSasInstance();
	if (!sas) {
		ImGui::Text("Sas instance not available");
		ImGui::End();
		return;
	}

	ImGui::Checkbox("Show all voices", &cfg.sasShowAllVoices);

	if (ImGui::BeginTable("saschannels", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH)) {
		ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Loop", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Pause", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Cur", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Volume", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableHeadersRow();

		for (int i = 0; i < PSP_SAS_VOICES_MAX; i++) {
			const SasVoice &voice = sas->voices[i];
			if (!voice.on && !cfg.sasShowAllVoices) {
				continue;
			}
			if (!voice.on) {
				ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 128));
			}
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("%d", i);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(VoiceTypeToString(voice.type));
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(BoolStr(voice.on));
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(BoolStr(voice.loop));
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(BoolStr(voice.paused));
			ImGui::TableNextColumn();
			switch (voice.type) {
			case VOICETYPE_OFF: ImGui::TextUnformatted("(off)"); break;
			case VOICETYPE_VAG: ImGui::Text("%08x", voice.vag.GetReadPtr()); break;
			case VOICETYPE_PCM: ImGui::Text("%08x", voice.pcmAddr + voice.pcmIndex * 2); break;
			default:
				ImGui::TextUnformatted("N/A");
				break;
			}
			ImGui::TableNextColumn();
			switch (voice.type) {
			case VOICETYPE_OFF: ImGui::TextUnformatted("(off)"); break;
			case VOICETYPE_VAG: ImGui::Text("%08x", voice.vagAddr); break;
			case VOICETYPE_PCM: ImGui::Text("%08x", voice.pcmAddr); break;
			case VOICETYPE_ATRAC3: ImGui::Text("atrac: %d", voice.atrac3.id()); break;
			default:
				ImGui::TextUnformatted("N/A");
				break;
			}
			ImGui::TableNextColumn();
			switch (voice.type) {
			case VOICETYPE_OFF: ImGui::TextUnformatted("(off)"); break;
			case VOICETYPE_VAG: ImGui::Text("%08x", voice.vagSize); break;
			case VOICETYPE_PCM: ImGui::Text("%08x", voice.pcmSize); break;
			case VOICETYPE_ATRAC3: ImGui::Text("atrac: n/a"); break;
			default:
				ImGui::TextUnformatted("N/A");
				break;
			}
			ImGui::TableNextColumn();
			ImGui::Text("%d | %d", voice.volumeLeft, voice.volumeRight);
			if (!voice.on) {
				ImGui::PopStyleColor();
			}
		}

		ImGui::EndTable();
	}
	ImGui::End();
}

static void DrawCallStacks(const MIPSDebugInterface *debug, ImConfig &config, ImControl &control) {
	if (!ImGui::Begin("Callstacks", &config.callstackOpen)) {
		ImGui::End();
		return;
	}

	std::vector<DebugThreadInfo> info = GetThreadsInfo();
	// TODO: Add dropdown for thread choice, so you can check the callstacks of other threads.
	u32 entry = 0;
	u32 stackTop = 0;
	for (auto &thread : info) {
		if (thread.isCurrent) {
			entry = thread.entrypoint;
			stackTop = thread.initialStack;
			break;
		}
	}

	if (entry != 0 && ImGui::BeginTable("frames", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH)) {
		ImGui::TableSetupColumn("Entry", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("EntryAddr", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("CurPC", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("CurOpCode", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("CurSP", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch);

		ImGui::TableHeadersRow();

		std::vector<MIPSStackWalk::StackFrame> frames = MIPSStackWalk::Walk(debug->GetPC(), debug->GetRegValue(0, 31), debug->GetRegValue(0, 29), entry, stackTop);

		// TODO: Add context menu and clickability
		int i = 0;
		for (auto &frame : frames) {
			const std::string entrySym = g_symbolMap->GetLabelString(frame.entry);
			ImGui::PushID(i);
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(entrySym.c_str());
			ImGui::TableSetColumnIndex(1);
			ImClickableValue("frameentry", frame.entry, control, ImCmd::SHOW_IN_CPU_DISASM);
			ImGui::TableSetColumnIndex(2);
			ImClickableValue("framepc", frame.pc, control, ImCmd::SHOW_IN_CPU_DISASM);
			ImGui::TableSetColumnIndex(3);
			ImGui::TextUnformatted("N/A");  // opcode, see the old debugger
			ImGui::TableSetColumnIndex(4);
			ImClickableValue("framepc", frame.sp, control, ImCmd::SHOW_IN_MEMORY_VIEWER);
			ImGui::TableSetColumnIndex(5);
			ImGui::Text("%d", frame.stackSize);
			// TODO: More fields?
			ImGui::PopID();
			i++;
		}
		ImGui::EndTable();
	}
	ImGui::End();
}

static void DrawModules(const MIPSDebugInterface *debug, ImConfig &cfg, ImControl &control) {
	if (!ImGui::Begin("Modules", &cfg.modulesOpen) || !g_symbolMap) {
		ImGui::End();
		return;
	}

	std::vector<LoadedModuleInfo> modules = g_symbolMap->getAllModules();
	if (ImGui::BeginTable("modules", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersH)) {
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Active", ImGuiTableColumnFlags_WidthFixed);

		ImGui::TableHeadersRow();

		// TODO: Add context menu and clickability
		for (int i = 0; i < (int)modules.size(); i++) {
			auto &module = modules[i];
			ImGui::PushID(i);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			if (ImGui::Selectable(module.name.c_str(), cfg.selectedModule == i, ImGuiSelectableFlags_SpanAllColumns)) {
				cfg.selectedModule = i;
			}
			ImGui::TableNextColumn();
			ImClickableValue("addr", module.address, control, ImCmd::SHOW_IN_MEMORY_VIEWER);
			ImGui::TableNextColumn();
			ImGui::Text("%08x", module.size);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(module.active ? "yes" : "no");
			ImGui::PopID();
		}

		ImGui::EndTable();
	}

	if (cfg.selectedModule >= 0 && cfg.selectedModule < (int)modules.size()) {
		auto &module = modules[cfg.selectedModule];
		// TODO: Show details
	}
	ImGui::End();
}

void DrawHLEModules(ImConfig &config) {
	if (!ImGui::Begin("HLE Modules", &config.hleModulesOpen)) {
		ImGui::End();
		return;
	}

	const int moduleCount = GetNumRegisteredModules();
	std::vector<const HLEModule *> modules;
	modules.reserve(moduleCount);
	for (int i = 0; i < moduleCount; i++) {
		modules.push_back(GetModuleByIndex(i));
	}

	std::sort(modules.begin(), modules.end(), [](const HLEModule* a, const HLEModule* b) {
		return a->name < b->name;
	});
	std::string label;
	for (auto mod : modules) {
		label = mod->name;
		if (ImGui::TreeNode(label.c_str())) {
			for (int j = 0; j < mod->numFunctions; j++) {
				auto &func = mod->funcTable[j];
				ImGui::Text("%s(%s)", func.name, func.argmask);
			}
			ImGui::TreePop();
		}
	}

	ImGui::End();
}

ImDebugger::ImDebugger() {
	reqToken_ = g_requestManager.GenerateRequesterToken();
	cfg_.LoadConfig(ConfigPath());
	g_normalTextColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

ImDebugger::~ImDebugger() {
	cfg_.SaveConfig(ConfigPath());
}

void ImDebugger::Frame(MIPSDebugInterface *mipsDebug, GPUDebugInterface *gpuDebug, Draw::DrawContext *draw) {
	// Snapshot the coreState to avoid inconsistency.
	const CoreState coreState = ::coreState;

	if (Achievements::HardcoreModeActive()) {
		ImGui::Begin("RetroAchievements hardcore mode");
		ImGui::Text("The debugger may not be used when the\nRetroAchievements hardcore mode is enabled.");
		ImGui::Text("To use the debugger, go into Settings / Tools / RetroAchievements and disable them.");
		ImGui::End();
		return;
	}

	// TODO: Pass mipsDebug in where needed instead.
	g_disassemblyManager.setCpu(mipsDebug);
	disasm_.View().setDebugger(mipsDebug);
	for (int i = 0; i < 4; i++) {
		mem_[i].View().setDebugger(mipsDebug);
	}

	// Watch the step counters to figure out when to update things.

	if (lastCpuStepCount_ != Core_GetSteppingCounter()) {
		lastCpuStepCount_ = Core_GetSteppingCounter();
		snapshot_ = newSnapshot_;  // Compare against the previous snapshot.
		Snapshot(currentMIPS);
		disasm_.NotifyStep();
	}

	if (lastGpuStepCount_ != GPUStepping::GetSteppingCounter()) {
		// A GPU step has happened since last time. This means that we should re-center the cursor.
		// Snapshot();
		lastGpuStepCount_ = GPUStepping::GetSteppingCounter();
		SnapshotGPU(gpuDebug);
		geDebugger_.NotifyStep();
	}

	ImControl control{};

	if (ImGui::BeginMainMenuBar()) {
		if (ImGui::BeginMenu("Debug")) {
			switch (coreState) {
			case CoreState::CORE_STEPPING_CPU:
				if (ImGui::MenuItem("Run")) {
					Core_Resume();
				}
				break;
			case CoreState::CORE_RUNNING_CPU:
				if (ImGui::MenuItem("Break")) {
					Core_Break(BreakReason::DebugBreak);
				}
				break;
			default:
				break;
			}
			ImGui::Separator();
			ImGui::MenuItem("Ignore bad memory accesses", nullptr, &g_Config.bIgnoreBadMemAccess);
			ImGui::MenuItem("Break on frame timeout", nullptr, &g_Config.bBreakOnFrameTimeout);
			ImGui::MenuItem("Don't break on start", nullptr, &g_Config.bAutoRun);  // should really invert this bool!
			ImGui::MenuItem("Fast memory", nullptr, &g_Config.bFastMemory);
			ImGui::Separator();

			/*
			// Symbol stuff. Move to separate menu?
			// Doesn't quite seem to work yet.
			if (ImGui::MenuItem("Load symbol map...")) {
				System_BrowseForFile(reqToken_, "Load symbol map", BrowseFileType::SYMBOL_MAP, [&](const char *responseString, int) {
					Path path(responseString);
					if (!g_symbolMap->LoadSymbolMap(path)) {
						ERROR_LOG(Log::Common, "Failed to load symbol map");
					}
					disasm_.DirtySymbolMap();
				});
			}
			if (ImGui::MenuItem("Save symbol map...")) {
				System_BrowseForFileSave(reqToken_, "Save symbol map", "symbols.map", BrowseFileType::SYMBOL_MAP, [](const char *responseString, int) {
					Path path(responseString);
					if (!g_symbolMap->SaveSymbolMap(path)) {
						ERROR_LOG(Log::Common, "Failed to save symbol map");
					}
				});
			}
			*/
			if (ImGui::MenuItem("Reset symbol map")) {
				g_symbolMap->Clear();
				disasm_.DirtySymbolMap();
				// NotifyDebuggerMapLoaded();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Take screenshot")) {
				g_TakeScreenshot = true;
			}
			ImGui::MenuItem("Save screenshot as .png", nullptr, &g_Config.bScreenshotsAsPNG);
			if (ImGui::MenuItem("Restart graphics")) {
				System_PostUIMessage(UIMessage::RESTART_GRAPHICS);
			}
			if (System_GetPropertyBool(SYSPROP_HAS_TEXT_CLIPBOARD)) {
				if (ImGui::MenuItem("Copy memory base to clipboard")) {
					System_CopyStringToClipboard(StringFromFormat("%016llx", (uint64_t)(uintptr_t)Memory::base));
				}
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("CPU")) {
			ImGui::MenuItem("CPU debugger", nullptr, &cfg_.disasmOpen);
			ImGui::MenuItem("GPR regs", nullptr, &cfg_.gprOpen);
			ImGui::MenuItem("FPR regs", nullptr, &cfg_.fprOpen);
			ImGui::MenuItem("VFPU regs", nullptr, &cfg_.vfpuOpen);
			ImGui::MenuItem("Callstacks", nullptr, &cfg_.callstackOpen);
			ImGui::MenuItem("Breakpoints", nullptr, &cfg_.breakpointsOpen);
			ImGui::MenuItem("Scheduler", nullptr, &cfg_.schedulerOpen);
			ImGui::Separator();
			for (int i = 0; i < 4; i++) {
				char title[64];
				snprintf(title, sizeof(title), "Memory %d", i + 1);
				ImGui::MenuItem(title, nullptr, &cfg_.memViewOpen[i]);
			}
			ImGui::MenuItem("Memory Dumper", nullptr, &cfg_.memDumpOpen);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("HLE")) {
			ImGui::MenuItem("File System Browser", nullptr, &cfg_.filesystemBrowserOpen);
			ImGui::MenuItem("Kernel Objects", nullptr, &cfg_.kernelObjectsOpen);
			ImGui::MenuItem("Threads", nullptr, &cfg_.threadsOpen);
			ImGui::MenuItem("Modules",nullptr,  &cfg_.modulesOpen);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Graphics")) {
			ImGui::MenuItem("GE Debugger", nullptr, &cfg_.geDebuggerOpen);
			ImGui::MenuItem("GE State", nullptr, &cfg_.geStateOpen);
			ImGui::MenuItem("Display Output", nullptr, &cfg_.displayOpen);
			ImGui::MenuItem("Textures", nullptr, &cfg_.texturesOpen);
			ImGui::MenuItem("Framebuffers", nullptr, &cfg_.framebuffersOpen);
			ImGui::MenuItem("Pixel Viewer", nullptr, &cfg_.pixelViewerOpen);
			// More to come here...
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Audio")) {
			ImGui::MenuItem("SasAudio mixer", nullptr, &cfg_.sasAudioOpen);
			ImGui::MenuItem("Raw audio channels", nullptr, &cfg_.audioChannelsOpen);
			ImGui::MenuItem("Decoder contexts", nullptr, &cfg_.audioDecodersOpen);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Network")) {
			ImGui::MenuItem("ApCtl", nullptr, &cfg_.apctlOpen);
			ImGui::MenuItem("Sockets", nullptr, &cfg_.socketsOpen);
			ImGui::MenuItem("NP", nullptr, &cfg_.npOpen);
			ImGui::MenuItem("AdHoc", nullptr, &cfg_.adhocOpen);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Tools")) {
			ImGui::MenuItem("Debug stats", nullptr, &cfg_.debugStatsOpen);
			ImGui::MenuItem("Struct viewer", nullptr, &cfg_.structViewerOpen);
			ImGui::MenuItem("Log channels", nullptr, &cfg_.logConfigOpen);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Misc")) {
			if (ImGui::MenuItem("Close Debugger")) {
				g_Config.bShowImDebugger = false;
			}
			ImGui::MenuItem("PPSSPP Internals", nullptr, &cfg_.internalsOpen);
			ImGui::MenuItem("Dear ImGui Demo", nullptr, &cfg_.demoOpen);
			ImGui::MenuItem("Dear ImGui Style editor", nullptr, &cfg_.styleEditorOpen);
			ImGui::EndMenu();
		}
		ImGui::EndMainMenuBar();
	}

	if (cfg_.demoOpen) {
		ImGui::ShowDemoWindow(&cfg_.demoOpen);
	}

	if (cfg_.styleEditorOpen) {
		ImGui::ShowStyleEditor();
	}

	if (cfg_.disasmOpen) {
		disasm_.Draw(mipsDebug, cfg_, control, coreState);
	}

	if (cfg_.gprOpen) {
		DrawGPRs(cfg_, control, mipsDebug, snapshot_);
	}

	if (cfg_.fprOpen) {
		DrawFPRs(cfg_, control, mipsDebug, snapshot_);
	}

	if (cfg_.vfpuOpen) {
		DrawVFPU(cfg_, control, mipsDebug, snapshot_);
	}

	if (cfg_.breakpointsOpen) {
		DrawBreakpointsView(mipsDebug, cfg_);
	}

	if (cfg_.filesystemBrowserOpen) {
		DrawFilesystemBrowser(cfg_);
	}

	if (cfg_.audioChannelsOpen) {
		DrawAudioChannels(cfg_, control);
	}

	if (cfg_.sasAudioOpen) {
		DrawSasAudio(cfg_);
	}

	if (cfg_.kernelObjectsOpen) {
		DrawKernelObjects(cfg_);
	}

	if (cfg_.threadsOpen) {
		DrawThreadView(cfg_, control);
	}

	if (cfg_.callstackOpen) {
		DrawCallStacks(mipsDebug, cfg_, control);
	}

	if (cfg_.modulesOpen) {
		DrawModules(mipsDebug, cfg_, control);
	}

	if (cfg_.audioDecodersOpen) {
		DrawAudioDecodersView(cfg_, control);
	}

	if (cfg_.hleModulesOpen) {
		DrawHLEModules(cfg_);
	}

	if (cfg_.framebuffersOpen) {
		DrawFramebuffersWindow(cfg_, gpuDebug->GetFramebufferManagerCommon());
	}

	if (cfg_.texturesOpen) {
		DrawTexturesWindow(cfg_, gpuDebug->GetTextureCacheCommon());
	}

	if (cfg_.logConfigOpen) {
		DrawLogConfig(cfg_);
	}

	if (cfg_.displayOpen) {
		DrawDisplayWindow(cfg_, gpuDebug->GetFramebufferManagerCommon());
	}

	if (cfg_.debugStatsOpen) {
		DrawDebugStatsWindow(cfg_);
	}

	if (cfg_.structViewerOpen) {
		structViewer_.Draw(cfg_, control, mipsDebug);
	}

	if (cfg_.geDebuggerOpen) {
		geDebugger_.Draw(cfg_, control, gpuDebug, draw);
	}

	if (cfg_.geStateOpen) {
		geStateWindow_.Draw(cfg_, control, gpuDebug);
	}

	if (cfg_.schedulerOpen) {
		DrawSchedulerView(cfg_);
	}

	if (cfg_.pixelViewerOpen) {
		pixelViewer_.Draw(cfg_, control, gpuDebug, draw);
	}

	if (cfg_.memDumpOpen) {
		memDumpWindow_.Draw(cfg_, mipsDebug);
	}

	for (int i = 0; i < 4; i++) {
		if (cfg_.memViewOpen[i]) {
			mem_[i].Draw(mipsDebug, cfg_, control, i);
		}
	}

	if (cfg_.socketsOpen) {
		DrawSockets(cfg_);
	}

	if (cfg_.npOpen) {
		DrawNp(cfg_);
	}

	if (cfg_.adhocOpen) {
		DrawAdhoc(cfg_);
	}

	if (cfg_.apctlOpen) {
		DrawApctl(cfg_);
	}

	if (cfg_.internalsOpen) {
		DrawInternals(cfg_);
	}

	// Process UI commands
	switch (control.command.cmd) {
	case ImCmd::SHOW_IN_CPU_DISASM:
		disasm_.View().gotoAddr(control.command.param);
		cfg_.disasmOpen = true;
		ImGui::SetWindowFocus(disasm_.Title());
		break;
	case ImCmd::SHOW_IN_GE_DISASM:
		geDebugger_.View().GotoAddr(control.command.param);
		cfg_.geDebuggerOpen = true;
		ImGui::SetWindowFocus(geDebugger_.Title());
		break;
	case ImCmd::SHOW_IN_MEMORY_VIEWER:
	{
		u32 index = control.command.param2;
		_dbg_assert_(index < 4);
		mem_[index].GotoAddr(control.command.param);
		cfg_.memViewOpen[index] = true;
		ImGui::SetWindowFocus(ImMemWindow::Title(index));
		break;
	}
	case ImCmd::SHOW_IN_MEMORY_DUMPER:
	{
		cfg_.memDumpOpen = true;
		memDumpWindow_.SetRange(control.command.param, control.command.param2, (MemDumpMode)control.command.param3);
		ImGui::SetWindowFocus(memDumpWindow_.Title());
		break;
	}
	case ImCmd::TRIGGER_FIND_POPUP:
		// TODO
		break;
	case ImCmd::NONE:
		break;
	case ImCmd::SHOW_IN_PIXEL_VIEWER:
		break;
	}
}

void ImDebugger::Snapshot(MIPSState *mips) {
	memcpy(newSnapshot_.gpr, mips->r, sizeof(newSnapshot_.gpr));
	memcpy(newSnapshot_.fpr, mips->fs, sizeof(newSnapshot_.fpr));
	memcpy(newSnapshot_.vpr, mips->v, sizeof(newSnapshot_.vpr));
	newSnapshot_.pc = mips->pc;
	newSnapshot_.lo = mips->lo;
	newSnapshot_.hi = mips->hi;
	newSnapshot_.ll = mips->llBit;
	pixelViewer_.Snapshot();
}

void ImDebugger::SnapshotGPU(GPUDebugInterface *gpuDebug) {
	pixelViewer_.Snapshot();
}

void ImMemWindow::Draw(MIPSDebugInterface *mipsDebug, ImConfig &cfg, ImControl &control, int index) {
	ImGui::SetNextWindowSize(ImVec2(520, 600), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin(Title(index), &cfg.memViewOpen[index])) {
		ImGui::End();
		return;
	}

	// Toolbars

	ImGui::InputScalar("Go to addr: ", ImGuiDataType_U32, &gotoAddr_, NULL, NULL, "%08X");
	if (ImGui::IsItemDeactivatedAfterEdit()) {
		memView_.gotoAddr(gotoAddr_);
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("Go")) {
		memView_.gotoAddr(gotoAddr_);
	}

	ImVec2 size(0, -ImGui::GetFrameHeightWithSpacing());

	// Main views - list of interesting addresses to the left, memory view to the right.
	if (ImGui::BeginChild("addr_list", ImVec2(200.0f, size.y), ImGuiChildFlags_ResizeX)) {
		if (ImGui::Selectable("Scratch")) {
			GotoAddr(0x00010000);
		}
		if (ImGui::Selectable("Kernel RAM")) {
			GotoAddr(0x08000000);
		}
		if (ImGui::Selectable("User RAM")) {
			GotoAddr(0x08800000);
		}
		if (ImGui::Selectable("VRAM")) {
			GotoAddr(0x04000000);
		}
	}
	ImGui::EndChild();

	ImGui::SameLine();
	if (ImGui::BeginChild("memview", size)) {
		memView_.Draw(ImGui::GetWindowDrawList());
	}
	ImGui::EndChild();

	StatusBar(memView_.StatusMessage());

	ImGui::End();
}

const char *ImMemWindow::Title(int index) {
	static const char *const titles[4] = { "Memory 1", "Memory 2", "Memory 3", "Memory 4" };
	return titles[index];
}

void ImDisasmWindow::Draw(MIPSDebugInterface *mipsDebug, ImConfig &cfg, ImControl &control, CoreState coreState) {
	disasmView_.setDebugger(mipsDebug);

	ImGui::SetNextWindowSize(ImVec2(520, 600), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin(Title(), &cfg.disasmOpen)) {
		ImGui::End();
		return;
	}

	if (ImGui::IsWindowFocused()) {
		// Process stepping keyboard shortcuts.
		if (ImGui::IsKeyPressed(ImGuiKey_F10)) {
			u32 stepSize = disasmView_.getInstructionSizeAt(mipsDebug->GetPC());
			Core_RequestCPUStep(CPUStepType::Over, stepSize);
		}
		if (ImGui::IsKeyPressed(ImGuiKey_F11)) {
			u32 stepSize = disasmView_.getInstructionSizeAt(mipsDebug->GetPC());
			Core_RequestCPUStep(CPUStepType::Into, stepSize);
		}
	}

	if (coreState == CORE_STEPPING_GE || coreState == CORE_RUNNING_GE) {
		ImGui::Text("!!! Currently stepping the GE");
		ImGui::SameLine();
		if (ImGui::SmallButton("Open Ge Debugger")) {
			cfg.geDebuggerOpen = true;
			ImGui::SetWindowFocus("GE Debugger");
		}
	}

	ImGui::BeginDisabled(coreState != CORE_STEPPING_CPU);
	if (ImGui::SmallButton("Run")) {
		Core_Resume();
	}
	ImGui::EndDisabled();

	ImGui::SameLine();
	ImGui::BeginDisabled(coreState != CORE_RUNNING_CPU);
	if (ImGui::SmallButton("Pause")) {
		Core_Break(BreakReason::DebugBreak);
	}
	ImGui::EndDisabled();

	ImGui::BeginDisabled(coreState != CORE_STEPPING_CPU);

	ImGui::SameLine();
	ImGui::Text("Step: ");
	ImGui::SameLine();

	if (ImGui::SmallButton("Into")) {
		u32 stepSize = disasmView_.getInstructionSizeAt(mipsDebug->GetPC());
		Core_RequestCPUStep(CPUStepType::Into, stepSize);
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("F11");
	}

	ImGui::SameLine();
	if (ImGui::SmallButton("Over")) {
		u32 stepSize = disasmView_.getInstructionSizeAt(mipsDebug->GetPC());
		Core_RequestCPUStep(CPUStepType::Over, stepSize);
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("F10");
	}

	ImGui::SameLine();
	if (ImGui::SmallButton("Out")) {
		Core_RequestCPUStep(CPUStepType::Out, 0);
	}

	/*
	ImGui::SameLine();
	if (ImGui::SmallButton("Frame")) {
		Core_RequestCPUStep(CPUStepType::Frame, 0);
	}*/

	ImGui::SameLine();
	if (ImGui::SmallButton("Syscall")) {
		hleDebugBreak();
		Core_Resume();
	}

	ImGui::SameLine();
	ImGui::SmallButton("Skim");
	if (ImGui::IsItemActive()) {
		u32 stepSize = disasmView_.getInstructionSizeAt(mipsDebug->GetPC());
		Core_RequestCPUStep(CPUStepType::Into, stepSize);
	}

	ImGui::EndDisabled();

	ImGui::SameLine();
	if (ImGui::SmallButton("Goto PC")) {
		disasmView_.GotoPC();
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("Goto RA")) {
		disasmView_.GotoRA();
	}

	if (ImGui::BeginPopup("disSearch")) {
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
		}
		if (ImGui::InputText("Search", searchTerm_, sizeof(searchTerm_), ImGuiInputTextFlags_EnterReturnsTrue)) {
			disasmView_.Search(searchTerm_);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	ImGui::SameLine();
	if (ImGui::SmallButton("Search")) {
		// Open a small popup
		ImGui::OpenPopup("disSearch");
		ImGui::Shortcut(ImGuiKey_F | ImGuiMod_Ctrl);
	}

	ImGui::SameLine();
	if (ImGui::SmallButton("Next")) {
		disasmView_.SearchNext(true);
	}

	ImGui::SameLine();
	if (ImGui::SmallButton("Settings")) {
		ImGui::OpenPopup("disSettings");
	}

	if (ImGui::BeginPopup("disSettings")) {
		ImGui::Checkbox("Follow PC", &disasmView_.followPC_);
		ImGui::EndPopup();
	}

	ImGui::SetNextItemWidth(100);
	if (ImGui::InputScalar("Go to addr: ", ImGuiDataType_U32, &gotoAddr_, NULL, NULL, "%08X")) {
		disasmView_.setCurAddress(gotoAddr_);
		disasmView_.scrollAddressIntoView();
	}
	ImGui::SameLine();
	if (ImGui::Button("Go")) {
		disasmView_.setCurAddress(gotoAddr_);
		disasmView_.scrollAddressIntoView();
	}

	BreakReason breakReason = Core_BreakReason();
	ImGui::SameLine();
	ImGui::TextUnformatted(BreakReasonToString(breakReason));

	ImVec2 avail = ImGui::GetContentRegionAvail();
	avail.y -= ImGui::GetTextLineHeightWithSpacing();

	if (ImGui::BeginChild("left", ImVec2(150.0f, avail.y), ImGuiChildFlags_ResizeX)) {
		if (symCache_.empty() || symsDirty_) {
			symCache_ = g_symbolMap->GetAllSymbols(SymbolType::ST_FUNCTION);
			symsDirty_ = false;
		}

		if (selectedSymbol_ >= 0 && selectedSymbol_ < symCache_.size()) {
			auto &sym = symCache_[selectedSymbol_];
			if (ImGui::TreeNode("Edit Symbol", "Edit %s", sym.name.c_str())) {
				if (ImGui::InputText("Name", selectedSymbolName_, sizeof(selectedSymbolName_), ImGuiInputTextFlags_EnterReturnsTrue)) {
					g_symbolMap->SetLabelName(selectedSymbolName_, sym.address);
					symsDirty_ = true;
				}
				ImGui::Text("%08x (size: %0d)", sym.address, sym.size);
				ImGui::TreePop();
			}
		}

		if (ImGui::BeginListBox("##symbols", ImGui::GetContentRegionAvail())) {
			ImGuiListClipper clipper;
			clipper.Begin((int)symCache_.size(), -1);
			while (clipper.Step()) {
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
					if (ImGui::Selectable(symCache_[i].name.c_str(), selectedSymbol_ == i)) {
						disasmView_.gotoAddr(symCache_[i].address);
						disasmView_.scrollAddressIntoView();
						truncate_cpy(selectedSymbolName_, symCache_[i].name.c_str());
						selectedSymbol_ = i;
					}
				}
			}
			clipper.End();
			ImGui::EndListBox();
		}
	}
	ImGui::EndChild();

	ImGui::SameLine();
	if (ImGui::BeginChild("right", ImVec2(0.0f, avail.y))) {
		disasmView_.Draw(ImGui::GetWindowDrawList(), control);
	}
	ImGui::EndChild();

	StatusBar(disasmView_.StatusBarText());
	ImGui::End();
}

Path ImDebugger::ConfigPath() {
	return GetSysDirectory(DIRECTORY_SYSTEM) / "imdebugger.ini";
}

// TODO: Move this into the main config at some point.
// But, I don't really want Core to know about the ImDebugger..

void ImConfig::LoadConfig(const Path &iniFile) {
	requesterToken = g_requestManager.GenerateRequesterToken();

	IniFile ini;
	ini.Load(iniFile);  // Ignore return value, might not exist yet. In that case we'll end up loading defaults.
	SyncConfig(&ini, false);
}

void ImConfig::SaveConfig(const Path &iniFile) {
	IniFile ini;
	ini.Load(iniFile);  // ignore return value, might not exist yet
	SyncConfig(&ini, true);
	ini.Save(iniFile);
}

class Syncer {
public:
	explicit Syncer(bool save) : save_(save) {}
	void SetSection(Section *section) { section_ = section; }
	template<class T>
	void Sync(std::string_view key, T *value, T defaultValue) {
		if (save_) {
			section_->Set(key, *value);
		} else {
			section_->Get(key, value, defaultValue);
		}
	}
private:
	Section *section_ = nullptr;
	bool save_;
};

void ImConfig::SyncConfig(IniFile *ini, bool save) {
	Syncer sync(save);
	sync.SetSection(ini->GetOrCreateSection("Windows"));
	sync.Sync("disasmOpen", &disasmOpen, true);
	sync.Sync("demoOpen ", &demoOpen, false);
	sync.Sync("gprOpen", &gprOpen, false);
	sync.Sync("fprOpen", &fprOpen, false);
	sync.Sync("vfpuOpen", &vfpuOpen, false);
	sync.Sync("threadsOpen", &threadsOpen, false);
	sync.Sync("callstackOpen", &callstackOpen, false);
	sync.Sync("breakpointsOpen", &breakpointsOpen, false);
	sync.Sync("modulesOpen", &modulesOpen, false);
	sync.Sync("hleModulesOpen", &hleModulesOpen, false);
	sync.Sync("audioDecodersOpen", &audioDecodersOpen, false);
	sync.Sync("structViewerOpen", &structViewerOpen, false);
	sync.Sync("framebuffersOpen", &framebuffersOpen, false);
	sync.Sync("displayOpen", &displayOpen, true);
	sync.Sync("styleEditorOpen", &styleEditorOpen, false);
	sync.Sync("filesystemBrowserOpen", &filesystemBrowserOpen, false);
	sync.Sync("kernelObjectsOpen", &kernelObjectsOpen, false);
	sync.Sync("audioChannelsOpen", &audioChannelsOpen, false);
	sync.Sync("texturesOpen", &texturesOpen, false);
	sync.Sync("debugStatsOpen", &debugStatsOpen, false);
	sync.Sync("geDebuggerOpen", &geDebuggerOpen, false);
	sync.Sync("geStateOpen", &geStateOpen, false);
	sync.Sync("schedulerOpen", &schedulerOpen, false);
	sync.Sync("socketsOpen", &socketsOpen, false);
	sync.Sync("npOpen", &npOpen, false);
	sync.Sync("adhocOpen", &adhocOpen, false);
	sync.Sync("apctlOpen", &apctlOpen, false);
	sync.Sync("pixelViewerOpen", &pixelViewerOpen, false);
	sync.Sync("internalsOpen", &internalsOpen, false);
	sync.Sync("sasAudioOpen", &sasAudioOpen, false);
	sync.Sync("logConfigOpen", &logConfigOpen, false);
	for (int i = 0; i < 4; i++) {
		char name[64];
		snprintf(name, sizeof(name), "memory%dOpen", i + 1);
		sync.Sync(name, &memViewOpen[i], false);
	}

	sync.SetSection(ini->GetOrCreateSection("Settings"));
	sync.Sync("displayLatched", &displayLatched, false);
	sync.Sync("realtimePixelPreview", &realtimePixelPreview, false);
}
