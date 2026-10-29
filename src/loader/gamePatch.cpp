#include "loader/gamePatch.h"

#include "common/abi.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "loader/elf.h"
#include "loader/gamePatchFilter.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace Loader::GamePatch {

namespace {

constexpr uint64_t kPageSize = 0x4000;

struct Write {
	uint64_t             source_address = 0;
	uint64_t             target_address = 0;
	std::vector<uint8_t> off;
	std::vector<uint8_t> on;
};

struct Plan {
	std::string              title_id;
	std::string              version;
	std::string              process;
	std::vector<Write>       writes;
	std::vector<std::string> mod_names;
	std::vector<uint64_t>    cave_pages;
};

using Json = nlohmann::json;

std::unique_ptr<Plan> g_pending_plan;
std::vector<uint64_t> g_applied_cave_pages;

// KYTY_ASTRO_RESCUE_FIX (default 0 since build 23, see LoadPlan; 1 = on): Astro Bot 01.018.000 with a "Decouple gameplay timers
// from render FPS" patch. The community "[Fix] Bot rescue plays with the game's own 60 fps timing"
// mod switches Decouple off when the game calls the rescue-start function (0x5074e20) and back on
// only in the rescue-end function (0x5077e60). Some levels (Pumpkin Peril) call the start without
// the end, so Decouple stays off until the level unloads and the game runs on its fixed 1/60 s
// step: below 60 fps, slow motion (a tester at Vblank 40, 2026-10-26). The emulator now applies
// that fix itself in place of the mod (the mod is skipped, enabled or not), finds Decouple's
// on/off byte in the applied Decouple mod (its variants place it differently), and adds a
// watchdog: if Decouple stays off for this fix longer than KYTY_ASTRO_RESCUE_TIMEOUT_S seconds
// (default 6; a real rescue is a few seconds), it is switched back on as the rescue end does.
constexpr const char* kRescueModName = "Bot rescue plays with the game's own 60 fps timing";
constexpr uint64_t    kRescueStart   = 0x5074e20;
constexpr uint64_t    kRescueEnd     = 0x5077e60;
constexpr uint64_t    kDecoupleGetter = 0x3530; // Decouple's dt getter: cmp byte [gate], 0
constexpr uint8_t     kPrologue[6]   = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};

struct RescueFix {
	bool     planned = false;
	uint64_t gate    = 0; // source addresses until applied, then target addresses
	uint64_t state   = 0;
	uint64_t marker  = 0;
	uint64_t log     = 0; // the rescue log's count; its entries follow it
};
RescueFix g_rescue_plan;

// The built-in Decouple (SetUpDecouple), when on.
struct DecoupleState {
	bool     active  = false;
	uint64_t base    = 0;
	uint64_t gate    = 0; // target addresses (guest)
	uint64_t last_us = 0;
	uint64_t dt      = 0;
	uint64_t numer   = 0; // the game's own step: [numer] / [denom]
	uint64_t denom   = 0;
	uint64_t getter  = 0; // the getters and the frame update's call, for skipping conflicting mods
	uint64_t ticks   = 0;
	uint64_t single  = 0;
	uint64_t call    = 0;
	double   prev_raw = 0.0;
	int64_t  counter  = 0;
	double   cap      = 1.0 / 20.0;
};
DecoupleState g_decouple;
// Armed after the writes (target addresses); read by the flip watchdog.
std::atomic<uint64_t> g_rescue_gate {0};
std::atomic<uint64_t> g_rescue_state {0};
std::atomic<uint64_t> g_rescue_marker {0};
std::atomic<uint64_t> g_rescue_log {0};
std::atomic<uint64_t> g_rescue_base {0}; // the main program's base: log addresses as offsets
// The main program's executable segments (host addresses), for the rescue log's call chains.
std::vector<std::pair<uint64_t, uint64_t>> g_rescue_code;

uint32_t RescueTimeoutSeconds() {
	const auto* value = std::getenv("KYTY_ASTRO_RESCUE_TIMEOUT_S");
	const int   s     = value != nullptr ? std::atoi(value) : 6;
	return static_cast<uint32_t>(std::clamp(s, 2, 600));
}

bool RescueFixEnabled() { // (default off since build 23)
	const auto* value = std::getenv("KYTY_ASTRO_RESCUE_FIX");
	return value != nullptr && value[0] == '1';
}

void AppendRel32(std::vector<uint8_t>* code, uint64_t next_ip, uint64_t target) {
	const auto rel = static_cast<int64_t>(target) - static_cast<int64_t>(next_ip);
	const auto v   = static_cast<int32_t>(rel);
	for (int i = 0; i < 4; i++) {
		code->push_back(static_cast<uint8_t>((static_cast<uint32_t>(v) >> (8 * i)) & 0xff));
	}
}

bool FitsRel32(uint64_t from, uint64_t to) {
	const auto rel = static_cast<int64_t>(to) - static_cast<int64_t>(from);
	return rel >= INT32_MIN && rel <= INT32_MAX;
}

bool Fail(std::string* error, std::string message) {
	*error = std::move(message);
	return false;
}

bool ParseBytes(const std::string& text, std::vector<uint8_t>* bytes) {
	if (text.empty() || (text.size() % 2) != 0) {
		return false;
	}

	bytes->resize(text.size() / 2);
	for (size_t index = 0; index < bytes->size(); index++) {
		(*bytes)[index] = static_cast<uint8_t>(std::stoul(text.substr(index * 2, 2), nullptr, 16));
	}
	return true;
}

// The built-in rescue fix's writes (KYTY_ASTRO_RESCUE_FIX): the same code as the community mod, in a
// code-cave page of its own, using the applied Decouple mod's on/off byte.
void PlanRescueFix(Plan* plan) {
	g_rescue_plan = {};
	// Decouple's dt getter starts with cmp byte [rip+rel32], 0 (80 3D rel32 00): its on/off byte.
	const Write* getter = nullptr;
	for (const auto& write: plan->writes) {
		if (write.source_address == kDecoupleGetter && write.on.size() >= 7 && write.on[0] == 0x80 &&
		    write.on[1] == 0x3d && write.on[6] == 0x00) {
			getter = &write;
		}
	}
	uint64_t gate  = 0;
	uint64_t state = 0;
	if (g_decouple.active) {
		// the built-in Decouple's on/off byte and last frame time (source addresses: base 0)
		gate  = g_decouple.gate - g_decouple.base;
		state = g_decouple.last_us - g_decouple.base;
	} else {
		if (getter == nullptr) {
			return; // no Decouple mod applied: nothing to switch, no fix needed
		}
		int32_t rel = 0;
		std::memcpy(&rel, &getter->on[2], sizeof(rel));
		gate  = kDecoupleGetter + 7 + static_cast<int64_t>(rel);
		state = gate - 0x68; // Decouple's last frame timestamp (both known variants)
	}
	// A code-cave page no other write uses.
	uint64_t cave = 0x12008000;
	for (bool clash = true; clash;) {
		clash = false;
		for (const auto& write: plan->writes) {
			const auto page = write.source_address & ~(kPageSize - 1);
			if (page == cave) {
				clash = true;
				cave += kPageSize;
				break;
			}
		}
	}
	// Data after the code: the marker byte, then the rescue log (a count and 16 entries, each the
	// caller's return address, bit 62 = Decouple was on, bit 63 = an end call; KYTY_ASTRO_RESCUE_LOG)
	// and per entry a snapshot of the 64 stack qwords above that return address (its call chain).
	const uint64_t marker    = cave + 0x200;
	const uint64_t count     = cave + 0x208;
	const uint64_t entries   = cave + 0x210;
	const uint64_t snapshots = cave + 0x400;
	for (const uint64_t target: {gate, state, marker, kRescueStart + 6, kRescueEnd + 6}) {
		if (!FitsRel32(cave, target)) {
			Log::WriteToConsoleAndLog("Game patch: built-in bot rescue fix not applied (addresses out "
			                          "of range; KYTY_ASTRO_RESCUE_FIX)\n");
			return;
		}
	}
	std::vector<uint8_t> code;
	// rel32 to target, for an instruction that has `tail` more bytes after it
	const auto emit_rel = [&](uint64_t target, uint64_t tail) {
		AppendRel32(&code, cave + code.size() + 4 + tail, target);
	};
	const auto log_call = [&](bool end) {
		code.insert(code.end(), {0x50, 0x51, 0x52, 0x56, 0x57}); // push rax, rcx, rdx, rsi, rdi
		code.insert(code.end(), {0x48, 0x8b, 0x05});             // mov rax, [count]
		emit_rel(count, 0);
		code.insert(code.end(), {0x89, 0xc1, 0x83, 0xe1, 0x0f}); // mov ecx, eax; and ecx, 15
		// the stack above the return address into this entry's snapshot
		code.insert(code.end(), {0x48, 0x89, 0xcf});             // mov rdi, rcx
		code.insert(code.end(), {0x48, 0xc1, 0xe7, 0x09});       // shl rdi, 9 (0x200 bytes each)
		code.insert(code.end(), {0x48, 0x8d, 0x35});             // lea rsi, [snapshots]
		emit_rel(snapshots, 0);
		code.insert(code.end(), {0x48, 0x01, 0xf7});             // add rdi, rsi
		code.insert(code.end(), {0x48, 0x8d, 0x74, 0x24, 0x30}); // lea rsi, [rsp+0x30]
		code.insert(code.end(), {0x51});                         // push rcx
		code.insert(code.end(), {0xb9, 0x40, 0x00, 0x00, 0x00}); // mov ecx, 64
		code.insert(code.end(), {0xfc, 0xf3, 0x48, 0xa5});       // cld; rep movsq
		code.insert(code.end(), {0x59});                         // pop rcx
		code.insert(code.end(), {0x48, 0x8b, 0x54, 0x24, 0x28}); // mov rdx, [rsp+0x28] (return address)
		code.insert(code.end(), {0x80, 0x3d});                   // cmp byte [gate], 0
		emit_rel(gate, 1);
		code.push_back(0x00);
		code.insert(code.end(), {0x75, 0x05});                   // jne +5
		code.insert(code.end(), {0x48, 0x0f, 0xba, 0xea, 0x3e}); // bts rdx, 62
		if (end) {
			code.insert(code.end(), {0x48, 0x0f, 0xba, 0xea, 0x3f}); // bts rdx, 63
		}
		code.insert(code.end(), {0x48, 0x8d, 0x35});             // lea rsi, [entries]
		emit_rel(entries, 0);
		code.insert(code.end(), {0x48, 0x89, 0x14, 0xce});       // mov [rsi+rcx*8], rdx
		code.insert(code.end(), {0x48, 0xff, 0xc0});             // inc rax
		code.insert(code.end(), {0x48, 0x89, 0x05});             // mov [count], rax (after the entry)
		emit_rel(count, 0);
		code.insert(code.end(), {0x5f, 0x5e, 0x5a, 0x59, 0x58}); // pop rdi, rsi, rdx, rcx, rax
	};
	// start: if Decouple is on, switch it off and remember doing so
	const uint64_t start_entry = cave + code.size();
	log_call(false);
	code.insert(code.end(), {0x80, 0x3d});
	emit_rel(gate, 1);
	code.push_back(0x00);
	code.insert(code.end(), {0x75, 0x0e});
	code.insert(code.end(), {0xc6, 0x05});
	emit_rel(gate, 1);
	code.push_back(0x01);
	code.insert(code.end(), {0xc6, 0x05});
	emit_rel(marker, 1);
	code.push_back(0x01);
	code.insert(code.end(), std::begin(kPrologue), std::end(kPrologue));
	code.push_back(0xe9);
	emit_rel(kRescueStart + 6, 0);
	// end: if this fix switched it off, switch it back on and reset Decouple's timestamp
	const uint64_t end_entry = cave + code.size();
	log_call(true);
	code.insert(code.end(), {0x80, 0x3d});
	emit_rel(marker, 1);
	code.push_back(0x00);
	code.insert(code.end(), {0x74, 0x19});
	code.insert(code.end(), {0xc6, 0x05});
	emit_rel(marker, 1);
	code.push_back(0x00);
	code.insert(code.end(), {0xc6, 0x05});
	emit_rel(gate, 1);
	code.push_back(0x00);
	code.insert(code.end(), {0x48, 0xc7, 0x05});
	emit_rel(state, 4);
	code.insert(code.end(), {0x00, 0x00, 0x00, 0x00});
	code.insert(code.end(), std::begin(kPrologue), std::end(kPrologue));
	code.push_back(0xe9);
	emit_rel(kRescueEnd + 6, 0);
	EXIT_IF(code.size() > 0x200);
	code.resize(0x2400, 0x00); // marker, count, log entries and their stack snapshots

	Write cave_write;
	cave_write.source_address = cave;
	cave_write.on             = code;
	cave_write.off.assign(code.size(), 0);
	plan->writes.push_back(std::move(cave_write));
	for (const auto& [hook, entry]: {std::pair {kRescueStart, start_entry}, std::pair {kRescueEnd, end_entry}}) {
		Write write;
		write.source_address = hook;
		write.off.assign(std::begin(kPrologue), std::end(kPrologue));
		write.on.push_back(0xe9);
		AppendRel32(&write.on, hook + 5, entry);
		write.on.push_back(0x90);
		plan->writes.push_back(std::move(write));
	}
	g_rescue_plan = {true, gate, state, marker, count};
	plan->mod_names.emplace_back("[Built-in] Bot rescue fix with safety timeout (KYTY_ASTRO_RESCUE_FIX)");
}

bool LoadPlan(const std::filesystem::path& path, Plan* plan, std::string* error) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return Fail(error, "could not read cheat file");
	}

	const auto root = Json::parse(file, nullptr, false);
	if (!root.is_object() || !root.contains("id") || !root.contains("version") ||
	    !root.contains("process") || !root.contains("mods")) {
		return Fail(error, "expected a GoldHEN-style mods JSON file");
	}
	plan->title_id = root["id"].get<std::string>();
	plan->version  = root["version"].get<std::string>();
	plan->process  = root["process"].get<std::string>();

	const auto* disable_env = std::getenv(kDisableEnv);
	const auto  patterns    = ParseDisableList(disable_env != nullptr ? disable_env : "");
	std::vector<ModEntry> entries;
	for (const auto& mod: root["mods"]) {
		ModEntry entry;
		if (mod.contains("name") && mod["name"].is_string()) {
			entry.name = mod["name"].get<std::string>();
		}
		entry.enabled = mod.value("enabled", true);
		entries.push_back(std::move(entry));
	}
	const auto selection = SelectMods(entries, patterns);
	for (size_t index = 0; index < entries.size(); index++) {
		const char* reason = "applying";
		std::string detail;
		if (selection.decisions[index] == ModDecision::SkipDisabledInFile) {
			reason = "skipping";
			detail = " (disabled in the patch file)";
		} else if (selection.decisions[index] == ModDecision::SkipDisabledByEnv) {
			reason = "skipping";
			detail = fmt::format(" ({} matched \"{}\")", kDisableEnv,
			                     patterns[selection.pattern_of_mod[index]]);
		}
		Log::WriteToConsoleAndLog(fmt::format("Game patch: {} mod {} \"{}\"{}\n", reason,
		                                      index + 1, entries[index].name, detail));
	}
	for (size_t index = 0; index < patterns.size(); index++) {
		if (!selection.pattern_matched[index]) {
			Log::WriteToConsoleAndLog(fmt::format("Game patch: {} pattern \"{}\" matched no mod\n",
			                                      kDisableEnv, patterns[index]));
		}
	}

	const bool astro          = plan->title_id == "PPSA21564" || plan->title_id == "PPSA21567";
	const bool rescue_builtin = plan->title_id == "PPSA21564" && plan->version == "01.018.000" &&
	                            RescueFixEnabled();
	// With the built-in Decouple on (SetUpDecouple): a mod that writes to the code it changed
	// (the getters, the frame update's call) or that names Decouple, or needs it, is skipped.
	const auto touches_decouple = [&](const Json& mod) {
		if (!g_decouple.active || !mod.contains("memory")) return false;
		const uint64_t base = g_decouple.base;
		for (const auto& entry: mod["memory"]) {
			if (!entry.contains("offset") || !entry["offset"].is_string()) continue;
			const uint64_t at = base + std::stoull(entry["offset"].get<std::string>(), nullptr, 16);
			for (const uint64_t hook: {g_decouple.getter, g_decouple.ticks, g_decouple.single, g_decouple.call}) {
				if (at + 0x20 > hook && at < hook + 0x20) return true;
			}
		}
		return false;
	};
	size_t mod_index = 0;
	for (const auto& mod: root["mods"]) {
		const auto decision = selection.decisions[mod_index++];
		const std::string mod_name = mod.contains("name") && mod["name"].is_string() ? mod["name"].get<std::string>() : "";
		if (astro && mod_name.find(kRescueModName) != std::string::npos) {
			if (rescue_builtin) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "Game patch: mod {} (bot rescue fix) superseded by the built-in one, with a {} s "
				    "safety timeout (KYTY_ASTRO_RESCUE_FIX=1)\n",
				    mod_index, RescueTimeoutSeconds()));
			} else {
				Log::WriteToConsoleAndLog(fmt::format(
				    "Game patch: mod {} (bot rescue fix) skipped: its hooks mark entering and leaving a "
				    "level, not rescues, and slowed the game down; KYTY_ASTRO_RESCUE_FIX=1 uses the "
				    "built-in one instead\n",
				    mod_index));
			}
			continue;
		}
		if (astro && g_decouple.active &&
		    (mod_name.find("Decouple gameplay timers") != std::string::npos ||
		     mod_name.find("needs Decouple") != std::string::npos || touches_decouple(mod))) {
			if (decision == ModDecision::Apply) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "Game patch: mod {} \"{}\" skipped: the built-in Decouple replaces it "
				    "(KYTY_ASTRO_DECOUPLE)\n",
				    mod_index, mod_name));
			}
			continue;
		}
		if (decision != ModDecision::Apply) {
			continue;
		}
		const auto name = mod["name"].get<std::string>();
		plan->mod_names.push_back(name);
		for (const auto& entry: mod["memory"]) {
			Write write;
			write.source_address = std::stoull(entry["offset"].get<std::string>(), nullptr, 16);
			if (!ParseBytes(entry["off"].get<std::string>(), &write.off) ||
			    !ParseBytes(entry["on"].get<std::string>(), &write.on) ||
			    write.off.size() != write.on.size()) {
				return Fail(error, "invalid mod memory bytes");
			}
			plan->writes.push_back(std::move(write));
		}
	}
	if (rescue_builtin) {
		PlanRescueFix(plan);
	}
	return true;
}

bool ValidateGame(const Plan& plan, const Program* main_program, std::string* error) {
	if (main_program == nullptr || main_program->base_vaddr == 0) {
		return Fail(error, "main executable is not loaded");
	}
	std::string title_id;
	std::string version;
	if (!SystemContentParamSfoGetString("TITLE_ID", &title_id) ||
	    !SystemContentParamSfoGetString("APP_VER", &version) ||
	    !Common::EqualNoCase(main_program->file_name.filename().string(), plan.process) ||
	    !Common::EqualNoCase(title_id, plan.title_id) || version != plan.version) {
		return Fail(error, "cheat file does not match the loaded game");
	}
	return true;
}

void Translate(const Program& program, uint64_t source_base, uint64_t source_address,
               uint64_t* target_address) {
	if (source_address >= source_base) {
		*target_address = program.base_vaddr + source_address - source_base;
		return;
	}
	*target_address = program.base_vaddr - (source_base - source_address);
}

bool IsInsideProgram(const Program& program, uint64_t address, size_t size) {
	return size != 0 && address >= program.base_vaddr &&
	       address + size <= program.base_vaddr + program.mapped_size;
}

bool IsZero(const std::vector<uint8_t>& bytes) {
	return std::all_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte == 0; });
}

bool TranslateAndValidate(Plan* plan, const Program& program, uint64_t source_base) {
	for (auto& write: plan->writes) {
		Translate(program, source_base, write.source_address, &write.target_address);
		if (!IsZero(write.off) &&
		    (!IsInsideProgram(program, write.target_address, write.off.size()) ||
		     std::memcmp(reinterpret_cast<const void*>(write.target_address), write.off.data(),
		                 write.off.size()) != 0)) {
			return false;
		}
	}
	return true;
}

bool Matches(Plan* plan, const Program& program, uint64_t* source_base) {
	const auto anchor = std::find_if(plan->writes.begin(), plan->writes.end(),
	                                 [](const Write& write) { return !IsZero(write.off); });
	if (anchor == plan->writes.end() || program.mapped_size < anchor->off.size()) {
		return false;
	}

	const auto* ehdr = program.elf->GetEhdr();
	const auto* phdr = program.elf->GetPhdr();
	for (Elf64_Half index = 0; index < ehdr->e_phnum; index++) {
		const auto& segment = phdr[index];
		if ((segment.p_type != PT_LOAD && segment.p_type != PT_OS_RELRO) ||
		    segment.p_filesz < anchor->off.size()) {
			continue;
		}

		const auto* begin = reinterpret_cast<const uint8_t*>(program.base_vaddr + segment.p_vaddr);
		const auto* end   = begin + segment.p_filesz;
		for (auto* current = begin; current < end;) {
			const auto* found = std::search(current, end, anchor->off.begin(), anchor->off.end());
			if (found == end) {
				break;
			}
			const auto rva = static_cast<uint64_t>(found - begin) + segment.p_vaddr;
			if (anchor->source_address >= rva) {
				*source_base = anchor->source_address - rva;
				if (TranslateAndValidate(plan, program, *source_base)) {
					return true;
				}
			}
			current = found + 1;
		}
	}
	return false;
}

void ReleaseCaves(std::vector<uint64_t>* cave_pages) {
	for (auto page = cave_pages->rbegin(); page != cave_pages->rend(); ++page) {
		EXIT_IF(!Libs::LibKernel::Memory::FreeGuestMemory(*page, kPageSize));
	}
	cave_pages->clear();
}

bool AllocateCave(Plan* plan, const Program& program, std::string* error) {
	std::vector<uint64_t> pages;
	for (const auto& write: plan->writes) {
		if (IsInsideProgram(program, write.target_address, write.off.size())) {
			continue;
		}
		if (!IsZero(write.off)) {
			return Fail(error, "non-zero write is outside the matched module");
		}

		const auto write_end = write.target_address + write.off.size();
		for (auto page = write.target_address & ~(kPageSize - 1); page < write_end;
		     page += kPageSize) {
			if (std::find(pages.begin(), pages.end(), page) == pages.end()) {
				pages.push_back(page);
			}
		}
	}

	for (const auto page: pages) {
		const auto allocated = Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    page, kPageSize, Common::VirtualMemory::Mode::ExecuteReadWrite, "game_cheat_code_cave",
		    true);
		if (allocated != page) {
			if (allocated != 0) {
				Libs::LibKernel::Memory::FreeGuestMemory(allocated, kPageSize);
			}
			ReleaseCaves(&plan->cave_pages);
			return Fail(error, "could not allocate code-cave page");
		}
		plan->cave_pages.push_back(page);
	}
	return true;
}

bool ApplyWrites(Plan* plan, const Program& program, std::string* error) {
	if (!AllocateCave(plan, program, error)) {
		return false;
	}

	for (int external = 1; external >= 0; external--) {
		for (const auto& write: plan->writes) {
			if (static_cast<int>(
			        !IsInsideProgram(program, write.target_address, write.on.size())) != external) {
				continue;
			}
			std::memcpy(reinterpret_cast<void*>(write.target_address), write.on.data(),
			            write.on.size());
			if (!Common::VirtualMemory::FlushInstructionCache(write.target_address,
			                                                  write.on.size())) {
				return Fail(error, "could not finalize cheat write");
			}
		}
	}
	return true;
}

} // namespace

bool Apply(const std::filesystem::path& plan_path, Program* main_program,
           const std::vector<Program*>& programs) {
	Plan        plan;
	std::string error;
	if (!LoadPlan(plan_path, &plan, &error) || !ValidateGame(plan, main_program, &error)) {
		::printf("Game cheat error: %s\n", error.c_str());
		return false;
	}
	if (plan.writes.empty()) {
		Log::WriteToConsoleAndLog("Game patch: no mod writes left to apply\n");
		return true;
	}

	g_pending_plan = std::make_unique<Plan>(std::move(plan));
	for (auto* program: programs) {
		if (!ApplyPending(program)) {
			return false;
		}
		if (g_pending_plan == nullptr) {
			return true;
		}
	}
	::printf("Game cheat: waiting for the matching module\n");
	return true;
}

bool ApplyPending(Program* program) {
	uint64_t source_base = 0;
	if (g_pending_plan == nullptr || program == nullptr || program->elf == nullptr ||
	    !Matches(g_pending_plan.get(), *program, &source_base)) {
		return true;
	}
	::printf("Game cheat: matched %s with source base 0x%llx\n",
	         program->file_name.filename().string().c_str(),
	         static_cast<unsigned long long>(source_base));

	std::string error;
	if (!ApplyWrites(g_pending_plan.get(), *program, &error)) {
		ReleaseCaves(&g_pending_plan->cave_pages);
		g_pending_plan.reset();
		::printf("Game cheat error: %s\n", error.c_str());
		return false;
	}

	for (const auto& name: g_pending_plan->mod_names) {
		::printf("Successfully applied cheat: %s\n", name.c_str());
	}
	if (g_rescue_plan.planned) {
		uint64_t gate = 0, state = 0, marker = 0, log = 0;
		Translate(*program, source_base, g_rescue_plan.gate, &gate);
		Translate(*program, source_base, g_rescue_plan.state, &state);
		Translate(*program, source_base, g_rescue_plan.marker, &marker);
		Translate(*program, source_base, g_rescue_plan.log, &log);
		g_rescue_code.clear();
		if (const auto* elf = program->elf.get(); elf != nullptr) {
			const auto* ehdr = elf->GetEhdr();
			const auto* phdr = elf->GetPhdr();
			for (Elf64_Half index = 0; index < ehdr->e_phnum; index++) {
				if (phdr[index].p_type == PT_LOAD && (phdr[index].p_flags & PF_X) != 0) {
					const uint64_t begin = program->base_vaddr + phdr[index].p_vaddr;
					g_rescue_code.emplace_back(begin, begin + phdr[index].p_filesz);
				}
			}
		}
		g_rescue_base.store(program->base_vaddr);
		g_rescue_log.store(log);
		g_rescue_state.store(state);
		g_rescue_marker.store(marker);
		g_rescue_gate.store(gate);
		g_rescue_plan = {};
	}
	g_applied_cave_pages = std::move(g_pending_plan->cave_pages);
	g_pending_plan.reset();
	return true;
}

// KYTY_ASTRO_RENDER_RES for Astro's Playroom (PPSA01325): the same tiers through the community
// "Fixed render resolution" edits: 16 immediates, the width and height of the game's 2160p and
// 1080p render paths, in five groups of instructions. Each group is looked for in the executable
// segments by its instructions (the short jumps and stack offsets between them are wildcards, so
// other versions match if the code is the same), and all five must be found exactly once with
// their original values: otherwise (another build of the code, or a patch file that already set the
// resolution) nothing is changed. 01.905.000's groups are at 0x1c2beb1-0x1d59eaa.
void ApplyPlayroomRenderResolution(Program* main_program, const std::string& version, int height) {
	enum class Dim : uint8_t { Width, Height };
	struct Immediate {
		uint8_t offset; // of the instruction's last four bytes (its immediate) in the group
		Dim     dim;
	};
	struct Group {
		const char*            pattern; // hex bytes, "??" any byte
		std::vector<Immediate> immediates;
	};
	static const Group kGroups[] = {
	    // mov ebx, 2160; mov r12d, 3840; test r12d, r12d; jnz; jmp; mov ebx, 1080; mov r12d, 1920
	    {"bb70080000 41bc000f0000 4585e4 75?? eb?? bb38040000 41bc80070000",
	     {{1, Dim::Height}, {7, Dim::Width}, {19, Dim::Height}, {25, Dim::Width}}},
	    // mov r12d, 1080; mov r14d, 1920; jmp; mov rbx, [rsp+n]; mov r12d, 2160; mov r14d, 3840
	    {"41bc38040000 41be80070000 eb?? 488b5c24?? 41bc70080000 41be000f0000",
	     {{2, Dim::Height}, {8, Dim::Width}, {21, Dim::Height}, {27, Dim::Width}}},
	    // mov edx, 1920; mov [rsp], rcx; mov ecx, 1080
	    {"ba80070000 48890c24 b938040000", {{1, Dim::Width}, {10, Dim::Height}}},
	    // mov edx, 3840; mov r8, r12; mov r9, rbx; mov [rsp], rcx; mov ecx, 2160
	    {"ba000f0000 4d89e0 4989d9 48890c24 b970080000", {{1, Dim::Width}, {16, Dim::Height}}},
	    // mov r14d, 2160; mov ebx, 3840; jmp; mov r14d, 1080; mov ebx, 1920
	    {"41be70080000 bb000f0000 eb?? 41be38040000 bb80070000",
	     {{2, Dim::Height}, {7, Dim::Width}, {15, Dim::Height}, {20, Dim::Width}}},
	};
	uint32_t width = 0;
	switch (height) {
		case 540: width = 960; break;
		case 720: width = 1280; break;
		case 1080: width = 1920; break;
		case 1368: width = 2432; break;
		case 1872: width = 3328; break;
		case 2160: width = 3840; break;
		default: return;
	}
	const auto parse = [](const char* text, std::vector<int>& bytes) {
		for (const char* p = text; *p != '\0';) {
			if (*p == ' ') {
				++p;
				continue;
			}
			if (p[0] == '?' && p[1] == '?') {
				bytes.push_back(-1);
			} else {
				bytes.push_back(static_cast<int>(std::strtoul(std::string(p, 2).c_str(), nullptr, 16)));
			}
			p += 2;
		}
	};
	const auto* ehdr = main_program->elf->GetEhdr();
	const auto* phdr = main_program->elf->GetPhdr();
	std::vector<uint8_t*> found;
	for (const auto& group: kGroups) {
		std::vector<int> pattern;
		parse(group.pattern, pattern);
		uint8_t* hit   = nullptr;
		uint32_t count = 0;
		for (Elf64_Half index = 0; index < ehdr->e_phnum && count < 2; index++) {
			const auto& segment = phdr[index];
			if (segment.p_type != PT_LOAD || (segment.p_flags & PF_X) == 0 || segment.p_filesz < pattern.size()) {
				continue;
			}
			auto* const begin = reinterpret_cast<uint8_t*>(main_program->base_vaddr + segment.p_vaddr);
			auto* const last  = begin + segment.p_filesz - pattern.size();
			for (auto* at = begin; at <= last && count < 2; ++at) {
				if (*at != pattern[0]) continue;
				size_t k = 1;
				while (k < pattern.size() && (pattern[k] < 0 || at[k] == pattern[k])) ++k;
				if (k == pattern.size()) {
					hit = at;
					++count;
				}
			}
		}
		if (count != 1) {
			if (version == "01.905.000") {
				::printf("Game patch: render resolution left as set by a game patch (KYTY_ASTRO_RENDER_RES=%d "
				         "not applied)\n",
				         height);
			} else {
				::printf("Game patch: render resolution not applied: Astro's Playroom %s has %s "
				         "(KYTY_ASTRO_RENDER_RES)\n",
				         version.c_str(), count == 0 ? "different resolution code" : "several candidates");
			}
			return;
		}
		found.push_back(hit);
	}
	for (size_t g = 0; g < found.size(); ++g) {
		for (const auto& immediate: kGroups[g].immediates) {
			uint8_t* const at    = found[g] + immediate.offset;
			const uint32_t value = immediate.dim == Dim::Width ? width : static_cast<uint32_t>(height);
			std::memcpy(at, &value, sizeof(value));
			(void)Common::VirtualMemory::FlushInstructionCache(reinterpret_cast<uint64_t>(at), sizeof(value));
		}
	}
	::printf("Game patch: Astro's Playroom fixed render resolution %ux%d (%s, KYTY_ASTRO_RENDER_RES)\n",
	         width, height, version.c_str());
}

// KYTY_ASTRO_RENDER_RES=540|720|1080|1368|1872|2160 (the launcher's "Render resolution"; unset or
// anything else: the game's own choice): Astro Bot (PPSA21564 / PPSA21567 01.018.000) renders at
// that fixed resolution tier, as the community "Fixed render resolution" patches do (the same
// write: "mov eax, [rbx+0x23f0]" -> "mov eax, <tier>; nop", at 0xc62a97 in 01.018, elsewhere where
// the instruction is the only one of its kind), without a patch file. The
// game's TAA (or DLSS, KYTY_DLSS) upscales it. Only the original bytes are replaced: a patch file
// that already set the tier wins.
void ApplyAstroRenderResolution(Program* main_program, const std::string& title_id,
                                const std::string& version) {
	const char* value = std::getenv("KYTY_ASTRO_RENDER_RES");
	if (value == nullptr || value[0] == '\0') {
		return;
	}
	int       height = std::atoi(value);
	int       tier   = -1;
	switch (height) {
		case 540:
		case 720: tier = 0; break; // (Astro Bot: tier 0 made smaller, below)
		case 1080: tier = 0; break;
		case 1368: tier = 1; break;
		case 1872: tier = 3; break;
		case 2160: tier = 4; break;
		default: return;
	}
	if (title_id == "PPSA01325") {
		ApplyPlayroomRenderResolution(main_program, version, height);
		return;
	}
	// Astro Bot's title IDs (US, EU).
	if (title_id != "PPSA21564" && title_id != "PPSA21567") {
		::printf("Game patch: render resolution %dp is for Astro Bot and Astro's Playroom only "
		         "(KYTY_ASTRO_RENDER_RES)\n",
		         height);
		return;
	}
	// Below 1080p: tier 0 (1920x1080) made smaller: its entries in the game's per-tier size tables
	// (below) and in its tier size functions ("mov dword [rdi], width; mov dword [rsi], height;
	// ret" per tier, three copies in 01.018, checked against tier 4's pairs).
	const int low_height = height < 1080 ? height : 0;
	static constexpr uint64_t kOffset    = 0xc62a97; // 01.018.000
	static constexpr uint8_t  kOrigin[6] = {0x8b, 0x83, 0xf0, 0x23, 0x00, 0x00};
	static constexpr uint8_t  kPatched[2] = {0xb8, 0x90}; // first and last byte of "mov eax, n; nop"
	uint8_t* code = nullptr;
	const auto at = [&](uint64_t address) -> uint8_t* {
		return IsInsideProgram(*main_program, address, sizeof(kOrigin))
		           ? reinterpret_cast<uint8_t*>(address)
		           : nullptr;
	};
	if (auto* known = at(main_program->base_vaddr + kOffset); known != nullptr) {
		if (std::memcmp(known, kOrigin, sizeof(kOrigin)) == 0) {
			code = known;
		} else if (known[0] == kPatched[0] && known[5] == kPatched[1] && version == "01.018.000") {
			::printf("Game patch: render resolution left as set by a game patch (KYTY_ASTRO_RENDER_RES=%d "
			         "not applied)\n",
			         height);
			return;
		}
	}
	if (code == nullptr) {
		// Another version: the instruction where it is the only one of its kind in the executable
		// segments; none or several: left alone.
		const auto* ehdr  = main_program->elf->GetEhdr();
		const auto* phdr  = main_program->elf->GetPhdr();
		uint32_t    found = 0;
		for (Elf64_Half index = 0; index < ehdr->e_phnum && found < 2; index++) {
			const auto& segment = phdr[index];
			if (segment.p_type != PT_LOAD || (segment.p_flags & PF_X) == 0 ||
			    segment.p_filesz < sizeof(kOrigin)) {
				continue;
			}
			auto*       begin = reinterpret_cast<uint8_t*>(main_program->base_vaddr + segment.p_vaddr);
			auto* const end   = begin + segment.p_filesz;
			for (auto* current = begin; found < 2;) {
				auto* hit = std::search(current, end, std::begin(kOrigin), std::end(kOrigin));
				if (hit == end) break;
				code = hit;
				found++;
				current = hit + 1;
			}
		}
		if (found != 1) {
			::printf("Game patch: render resolution not applied: this version of Astro Bot (%s) has %s "
			         "(KYTY_ASTRO_RENDER_RES)\n",
			         version.c_str(), found == 0 ? "no known resolution code" : "several candidates");
			return;
		}
	}
	if (low_height != 0) {
		static constexpr uint8_t kTier0[12] = {0xc7, 0x07, 0x80, 0x07, 0x00, 0x00, 0xc7, 0x06, 0x38, 0x04, 0x00, 0x00};
		static constexpr uint8_t kTier4[12] = {0xc7, 0x07, 0x00, 0x0f, 0x00, 0x00, 0xc7, 0x06, 0x70, 0x08, 0x00, 0x00};
		std::vector<uint8_t*> tier0;
		uint32_t              tier4 = 0;
		const auto*           ehdr  = main_program->elf->GetEhdr();
		const auto*           phdr  = main_program->elf->GetPhdr();
		for (Elf64_Half index = 0; index < ehdr->e_phnum; index++) {
			const auto& segment = phdr[index];
			if (segment.p_type != PT_LOAD || (segment.p_flags & PF_X) == 0 || segment.p_filesz < sizeof(kTier0)) {
				continue;
			}
			auto*       begin = reinterpret_cast<uint8_t*>(main_program->base_vaddr + segment.p_vaddr);
			auto* const end   = begin + segment.p_filesz;
			for (auto* current = begin;;) {
				auto* hit = std::search(current, end, std::begin(kTier0), std::end(kTier0));
				if (hit == end) break;
				tier0.push_back(hit);
				current = hit + 1;
			}
			for (auto* current = begin;;) {
				auto* hit = std::search(current, end, std::begin(kTier4), std::end(kTier4));
				if (hit == end) break;
				tier4++;
				current = hit + 1;
			}
		}
		// The render targets' sizes come from per-tier tables in the read-only data (width, height,
		// width as a float; tier 2 repeats tier 1): three of each in 01.018, each read once.
		static constexpr uint32_t kWidths[5]  = {1920, 2432, 2432, 3328, 3840};
		static constexpr uint32_t kHeights[5] = {1080, 1368, 1368, 1872, 2160};
		static constexpr float    kWidthsF[5] = {1920.0f, 2432.0f, 2432.0f, 3328.0f, 3840.0f};
		std::vector<uint8_t*> widths, heights, widths_f;
		for (Elf64_Half index = 0; index < ehdr->e_phnum; index++) {
			const auto& segment = phdr[index];
			if (segment.p_type != PT_LOAD || (segment.p_flags & PF_X) != 0 || segment.p_filesz < sizeof(kWidths)) {
				continue;
			}
			auto*       begin = reinterpret_cast<uint8_t*>(main_program->base_vaddr + segment.p_vaddr);
			auto* const end   = begin + segment.p_filesz;
			const auto  find  = [&](const void* pattern, size_t size, std::vector<uint8_t*>& out) {
				const auto* p = static_cast<const uint8_t*>(pattern);
				for (auto* current = begin;;) {
					auto* hit = std::search(current, end, p, p + size);
					if (hit == end) break;
					out.push_back(hit);
					current = hit + 1;
				}
			};
			find(kWidths, sizeof(kWidths), widths);
			find(kHeights, sizeof(kHeights), heights);
			find(kWidthsF, sizeof(kWidthsF), widths_f);
		}
		if (widths.empty() || widths.size() != heights.size() || widths.size() > 8 || widths_f.size() > 8) {
			::printf("Game patch: Astro Bot render resolution %dp not applied: this version (%s) has %zu width and "
			         "%zu height tables (KYTY_ASTRO_RENDER_RES); 1080p instead\n",
			         low_height, version.c_str(), widths.size(), heights.size());
		}
		const bool tables_ok = !widths.empty() && widths.size() == heights.size() && widths.size() <= 8 &&
		                       widths_f.size() <= 8;
		if (!tables_ok) {
			height = 1080;
		} else if (tier0.empty() || tier0.size() != tier4 || tier0.size() > 8) {
			::printf("Game patch: Astro Bot render resolution %dp not applied: this version (%s) has %zu tier size "
			         "entries for 1080p and %u for 2160p (KYTY_ASTRO_RENDER_RES); 1080p instead\n",
			         low_height, version.c_str(), tier0.size(), tier4);
			height = 1080;
		} else {
			const uint32_t width = low_height == 540 ? 960u : 1280u;
			const uint32_t h0    = static_cast<uint32_t>(low_height);
			const float    w0f   = static_cast<float>(width);
			for (auto* entry: widths) std::memcpy(entry, &width, sizeof(width));
			for (auto* entry: heights) std::memcpy(entry, &h0, sizeof(h0));
			for (auto* entry: widths_f) std::memcpy(entry, &w0f, sizeof(w0f));
			for (auto* entry: tier0) {
				std::memcpy(entry + 2, &width, sizeof(width));
				const uint32_t h = static_cast<uint32_t>(low_height);
				std::memcpy(entry + 8, &h, sizeof(h));
				(void)Common::VirtualMemory::FlushInstructionCache(reinterpret_cast<uint64_t>(entry), sizeof(kTier0));
			}
			height = low_height;
		}
	}
	const auto address = reinterpret_cast<uint64_t>(code);
	const uint8_t patched[6] = {kPatched[0], static_cast<uint8_t>(tier), 0x00, 0x00, 0x00, kPatched[1]};
	std::memcpy(code, patched, sizeof(patched));
	(void)Common::VirtualMemory::FlushInstructionCache(address, sizeof(patched));
	::printf("Game patch: Astro Bot fixed render resolution %dp (%s, KYTY_ASTRO_RENDER_RES)\n", height,
	         version.c_str());
}

// The built-in Decouple (KYTY_ASTRO_DECOUPLE, default 1; 0 = the patch file's mod as before):
// Astro Bot (PPSA21564 / PPSA21567), any version. The game advances by a fixed 1/60 s per frame
// (three small getters return it as a double, a float and microseconds: 1.0 / target fps, at
// 0x3530, 0x3550 and 0x3520 in 01.018.000), so below 60 fps it runs slow. As the community
// "Decouple gameplay timers from render FPS" mod (v3) does, the frame update's call of the double
// getter (01.018: 0x2a4a2b) then measures the real frame time, smooths it (v3's rules: follows a
// lasting change after two frames, else eases 10% per frame, steps at most 1.5x the current one)
// and caps it (KYTY_ASTRO_DECOUPLE_DT_CAP: 20 = 1/20 s, the default; 30 = 1/30 s; 0 = 1/10 s), and
// the three getters return that. The code is found by its shape, not its address: the getters by
// their instructions and shared global, the frame update's call among the double getter's callers
// (01.018: pinned to 0x2a4a2b; other versions: the one caller whose next calls read the same
// global, the frame limiter). When either is not found exactly once nothing is changed and the
// patch file's Decouple mod applies as before. When it is applied, the patch file's Decouple mod
// and the mods that need it are skipped (the console says which). KYTY_DECOUPLE_PROBE=1 also
// logs every caller.
namespace {


constexpr uint8_t kDecoupleSave[] = {
	    0x9c, 0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x48, 0x81,
	    0xec, 0xe8, 0x01, 0x00, 0x00, 0xc5, 0xfe, 0x7f, 0x0c, 0x24, 0xc5, 0xfe, 0x7f, 0x54, 0x24, 0x20,
	    0xc5, 0xfe, 0x7f, 0x5c, 0x24, 0x40, 0xc5, 0xfe, 0x7f, 0x64, 0x24, 0x60, 0xc5, 0xfe, 0x7f, 0xac,
	    0x24, 0x80, 0x00, 0x00, 0x00, 0xc5, 0xfe, 0x7f, 0xb4, 0x24, 0xa0, 0x00, 0x00, 0x00, 0xc5, 0xfe,
	    0x7f, 0xbc, 0x24, 0xc0, 0x00, 0x00, 0x00, 0xc5, 0x7e, 0x7f, 0x84, 0x24, 0xe0, 0x00, 0x00, 0x00,
	    0xc5, 0x7e, 0x7f, 0x8c, 0x24, 0x00, 0x01, 0x00, 0x00, 0xc5, 0x7e, 0x7f, 0x94, 0x24, 0x20, 0x01,
	    0x00, 0x00, 0xc5, 0x7e, 0x7f, 0x9c, 0x24, 0x40, 0x01, 0x00, 0x00, 0xc5, 0x7e, 0x7f, 0xa4, 0x24,
	    0x60, 0x01, 0x00, 0x00, 0xc5, 0x7e, 0x7f, 0xac, 0x24, 0x80, 0x01, 0x00, 0x00, 0xc5, 0x7e, 0x7f,
	    0xb4, 0x24, 0xa0, 0x01, 0x00, 0x00, 0xc5, 0x7e, 0x7f, 0xbc, 0x24, 0xc0, 0x01, 0x00, 0x00,
};
constexpr uint8_t kDecoupleRestore[] = { // (ends with popfq; ret)
	    0xc5, 0xfe, 0x6f, 0x0c, 0x24, 0xc5, 0xfe, 0x6f, 0x54, 0x24, 0x20, 0xc5, 0xfe, 0x6f, 0x5c, 0x24,
	    0x40, 0xc5, 0xfe, 0x6f, 0x64, 0x24, 0x60, 0xc5, 0xfe, 0x6f, 0xac, 0x24, 0x80, 0x00, 0x00, 0x00,
	    0xc5, 0xfe, 0x6f, 0xb4, 0x24, 0xa0, 0x00, 0x00, 0x00, 0xc5, 0xfe, 0x6f, 0xbc, 0x24, 0xc0, 0x00,
	    0x00, 0x00, 0xc5, 0x7e, 0x6f, 0x84, 0x24, 0xe0, 0x00, 0x00, 0x00, 0xc5, 0x7e, 0x6f, 0x8c, 0x24,
	    0x00, 0x01, 0x00, 0x00, 0xc5, 0x7e, 0x6f, 0x94, 0x24, 0x20, 0x01, 0x00, 0x00, 0xc5, 0x7e, 0x6f,
	    0x9c, 0x24, 0x40, 0x01, 0x00, 0x00, 0xc5, 0x7e, 0x6f, 0xa4, 0x24, 0x60, 0x01, 0x00, 0x00, 0xc5,
	    0x7e, 0x6f, 0xac, 0x24, 0x80, 0x01, 0x00, 0x00, 0xc5, 0x7e, 0x6f, 0xb4, 0x24, 0xa0, 0x01, 0x00,
	    0x00, 0xc5, 0x7e, 0x6f, 0xbc, 0x24, 0xc0, 0x01, 0x00, 0x00, 0x48, 0x81, 0xc4, 0xe8, 0x01, 0x00,
	    0x00, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58, 0x9d, 0xc3,
};

uint64_t DecoupleNowUs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count()) +
	       1;
}

// Called by the game's frame update (through the cave's stub) in place of its fixed-step getter.
KYTY_SYSV_ABI double DecoupleUpdate() {
	auto&      d     = g_decouple;
	auto*      gate  = reinterpret_cast<volatile uint8_t*>(d.gate);
	auto*      last  = reinterpret_cast<volatile uint64_t*>(d.last_us);
	auto*      dt_d  = reinterpret_cast<volatile double*>(d.dt);
	auto*      dt_f  = reinterpret_cast<volatile float*>(d.dt + 8);
	auto*      dt_us = reinterpret_cast<volatile int64_t*>(d.dt + 16);
	if (*gate != 0) { // switched off (KYTY_ASTRO_RESCUE_FIX): the game's own step
		*last = 0;
		return *reinterpret_cast<const double*>(d.numer) / *reinterpret_cast<const double*>(d.denom);
	}
	const uint64_t now  = DecoupleNowUs();
	const uint64_t then = *last;
	*last               = now;
	double         dt   = *dt_d;
	if (then != 0 && now > then) {
		const double raw  = static_cast<double>(now - then) * 0.000001;
		const double prev = d.prev_raw;
		d.prev_raw        = raw;
		bool         jump = false;
		if (raw > dt * 1.25) {
			d.counter = d.counter > 0 ? d.counter + 1 : 1;
			if (d.counter >= 2) {
				dt   = std::min(raw, prev);
				jump = true;
			}
		} else if (raw < dt * 0.8) {
			d.counter = d.counter < 0 ? d.counter - 1 : -1;
			if (d.counter <= -2) {
				dt   = std::max(raw, prev);
				jump = true;
			}
		} else {
			d.counter = 0;
		}
		if (jump) {
			dt        = std::min(std::max(dt, 0.0001), 0.1);
			d.counter = 0;
		} else {
			const double step = std::max(std::min(raw, dt * 1.5), 0.0001);
			dt += (step - dt) * 0.1;
		}
	} else {
		d.counter = 0;
	}
	dt     = std::min(dt, d.cap);
	*dt_d  = dt;
	*dt_f  = static_cast<float>(dt);
	*dt_us = static_cast<int64_t>(dt * 1000000.0 + 0.5);
	return dt;
}

} // namespace

void SetUpDecouple(Program* main_program) {
	g_decouple = {};
	if (main_program == nullptr || main_program->elf == nullptr || main_program->elf->IsShared()) {
		return;
	}
	std::string title_id;
	std::string version;
	if (!SystemContentParamSfoGetString("TITLE_ID", &title_id) ||
	    !SystemContentParamSfoGetString("APP_VER", &version) ||
	    (title_id != "PPSA21564" && title_id != "PPSA21567")) {
		return;
	}
	if (const char* value = std::getenv("KYTY_ASTRO_DECOUPLE"); value != nullptr && value[0] == '0') {
		::printf("Decouple: built-in one off; the patch file's Decouple mod applies if enabled "
		         "(KYTY_ASTRO_DECOUPLE)\n");
		return;
	}
	const bool verbose = [] {
		const char* value = std::getenv("KYTY_DECOUPLE_PROBE");
		return value != nullptr && value[0] == '1';
	}();
	const auto  base = main_program->base_vaddr;
	const auto* ehdr = main_program->elf->GetEhdr();
	const auto* phdr = main_program->elf->GetPhdr();
	struct Range {
		uint8_t* begin;
		uint8_t* end;
	};
	std::vector<Range> code;
	for (Elf64_Half index = 0; index < ehdr->e_phnum; index++) {
		const auto& segment = phdr[index];
		if (segment.p_type == PT_LOAD && (segment.p_flags & PF_X) != 0 && segment.p_filesz >= 32) {
			auto* begin = reinterpret_cast<uint8_t*>(base + segment.p_vaddr);
			code.push_back({begin, begin + segment.p_filesz});
		}
	}
	const auto rel_target = [](const uint8_t* rel_at, size_t tail) {
		int32_t rel = 0;
		std::memcpy(&rel, rel_at, sizeof(rel));
		return reinterpret_cast<uint64_t>(rel_at) + 4 + tail + static_cast<int64_t>(rel);
	};
	const auto off = [&](uint64_t address) { return static_cast<unsigned long long>(address - base); };
	const auto give_up = [&](const char* why) {
		::printf("Decouple: built-in one not applied (%s, Astro Bot %s %s); the patch file's Decouple "
		         "mod applies if enabled (KYTY_ASTRO_DECOUPLE)\n",
		         why, title_id.c_str(), version.c_str());
	};
	// The getters: "vmovsd xmm0, [A]; vdivsd xmm0, [B]; ret" with "mov rax, [B+0x10]; ret" and
	// "vmovss xmm0, [B+8]; ret" within 0x40 bytes.
	static constexpr uint8_t kDouble[] = {0xc5, 0xfb, 0x10, 0x05};
	static constexpr uint8_t kDivide[] = {0xc5, 0xfb, 0x5e, 0x05};
	uint32_t found = 0;
	uint64_t getter = 0, ticks = 0, single = 0, numer = 0, denom = 0;
	for (const auto& range: code) {
		for (const auto* at = range.begin; at + 17 <= range.end; ++at) {
			if (std::memcmp(at, kDouble, 4) != 0 || std::memcmp(at + 8, kDivide, 4) != 0 || at[16] != 0xc3) {
				continue;
			}
			const uint64_t global = rel_target(at + 12, 0);
			uint64_t       t = 0, f = 0;
			const auto*    from = std::max<const uint8_t*>(range.begin, at - 0x40);
			const auto*    to   = std::min<const uint8_t*>(range.end - 9, at + 0x40);
			for (const auto* near = from; near <= to; ++near) {
				if (near[0] == 0x48 && near[1] == 0x8b && near[2] == 0x05 && near[7] == 0xc3 &&
				    rel_target(near + 3, 0) == global + 0x10) {
					t = reinterpret_cast<uint64_t>(near);
				}
				if (near[0] == 0xc5 && near[1] == 0xfa && near[2] == 0x10 && near[3] == 0x05 &&
				    near[8] == 0xc3 && rel_target(near + 4, 0) == global + 8) {
					f = reinterpret_cast<uint64_t>(near);
				}
			}
			if (t != 0 && f != 0) {
				found++;
				getter = reinterpret_cast<uint64_t>(at);
				ticks  = t;
				single = f;
				numer  = rel_target(at + 4, 0);
				denom  = global;
			}
		}
	}
	if (found != 1) {
		give_up(found == 0 ? "its timing code was not found" : "its timing code was found more than once");
		return;
	}
	if (!IsInsideProgram(*main_program, numer, 8) || !IsInsideProgram(*main_program, denom, 0x18)) {
		give_up("the timing values are outside the game");
		return;
	}
	// The double getter's callers; the frame update's is the one whose next calls (within 0x100
	// bytes) reach a function that reads the step global within its first 0x400 bytes.
	const auto reads_step = [&](uint64_t function) {
		if (!IsInsideProgram(*main_program, function, 0x400)) {
			return false;
		}
		const auto* f = reinterpret_cast<const uint8_t*>(function);
		for (size_t k = 0; k + 4 <= 0x400; k++) {
			for (size_t tail = 0; tail <= 4; tail++) {
				const uint64_t target = rel_target(f + k, tail);
				if (target == denom || target == denom + 8 || target == denom + 0x10) {
					return true;
				}
			}
		}
		return false;
	};
	std::vector<uint64_t> callers;
	std::vector<uint64_t> candidates;
	for (const auto& range: code) {
		for (const auto* at = range.begin; at + 5 <= range.end; ++at) {
			if (at[0] != 0xe8 || rel_target(at + 1, 0) != getter) {
				continue;
			}
			callers.push_back(reinterpret_cast<uint64_t>(at));
			bool        limiter = false;
			std::string next;
			for (const auto* after = at + 5; after + 5 <= range.end && after < at + 0x100; ++after) {
				if (*after != 0xe8) continue;
				const uint64_t target = rel_target(after + 1, 0);
				if (target == getter || !IsInsideProgram(*main_program, target, 1)) continue;
				if (reads_step(target)) {
					limiter = true;
					char text[48];
					std::snprintf(text, sizeof(text), " +0x%llx->0x%llx",
					              static_cast<unsigned long long>(after - at), off(target));
					next += text;
				}
				after += 4;
			}
			if (limiter) {
				candidates.push_back(reinterpret_cast<uint64_t>(at));
			}
			if (verbose) {
				::printf("Decouple probe: call of game+0x%llx at game+0x%llx%s%s (KYTY_DECOUPLE_PROBE)\n",
				         off(getter), off(reinterpret_cast<uint64_t>(at)), limiter ? ", then reads the step:" : "",
				         next.c_str());
			}
		}
	}
	uint64_t call = 0;
	if (version == "01.018.000") {
		// Pinned: the frame update's call, as the community mod hooks it.
		const uint64_t pinned = base + 0x2a4a2b;
		if (std::find(callers.begin(), callers.end(), pinned) != callers.end()) {
			call = pinned;
		}
		const bool agree = candidates.size() == 1 && candidates[0] == pinned;
		::printf("Decouple: the search for the frame update's call %s on 01.018.000 (%zu candidate(s)) "
		         "(KYTY_ASTRO_DECOUPLE)\n",
		         agree ? "agrees with the known one" : "does not agree with the known one", candidates.size());
	} else if (candidates.size() == 1) {
		call = candidates[0];
	}
	if (call == 0) {
		give_up(candidates.empty() ? "the frame update's call was not found"
		                           : "several calls could be the frame update's");
		return;
	}
	const char* cap_text = "1/20 s";
	double      cap      = 1.0 / 20.0;
	if (const char* value = std::getenv("KYTY_ASTRO_DECOUPLE_DT_CAP"); value != nullptr) {
		const int n = std::atoi(value);
		if (n == 30) {
			cap      = 1.0 / 30.0;
			cap_text = "1/30 s";
		} else if (n == 0) {
			cap      = 0.1;
			cap_text = "1/10 s";
		}
	}
	// A code-cave page within reach of the game's code and data.
	uint64_t cave = 0;
	for (uint64_t offset = 0x12100000; offset < 0x12100000 + 64 * kPageSize; offset += kPageSize) {
		const uint64_t page = base + offset;
		if (!FitsRel32(page, getter) || !FitsRel32(page, numer) || !FitsRel32(page, denom) ||
		    !FitsRel32(page + kPageSize, call)) {
			break;
		}
		const auto allocated = Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    page, kPageSize, Common::VirtualMemory::Mode::ExecuteReadWrite, "astro_decouple", true);
		if (allocated == page) {
			cave = page;
			break;
		}
		if (allocated != 0) {
			Libs::LibKernel::Memory::FreeGuestMemory(allocated, kPageSize);
		}
	}
	if (cave == 0) {
		give_up("no room for its code");
		return;
	}
	// Cave: +0x000 the frame update's stub, +0x200 the getters, +0x800 data: dt (double, float,
	// microseconds), the last frame's time, the on/off byte (0 on), the update function.
	const uint64_t data    = cave + 0x800;
	const uint64_t gate    = data + 0x20;
	const uint64_t fn_ptr  = data + 0x28;
	std::vector<uint8_t> out;
	const auto emit_rel = [&](uint64_t target, uint64_t tail) {
		AppendRel32(&out, cave + out.size() + 4 + tail, target);
	};
	out.insert(out.end(), std::begin(kDecoupleSave), std::end(kDecoupleSave));
	out.insert(out.end(), {0x48, 0x8b, 0x05}); // mov rax, [fn_ptr]
	emit_rel(fn_ptr, 0);
	out.insert(out.end(), {0xff, 0xd0}); // call rax (returns dt in xmm0)
	out.insert(out.end(), std::begin(kDecoupleRestore), std::end(kDecoupleRestore));
	EXIT_IF(out.size() > 0x200);
	out.resize(0x200, 0xcc);
	// double getter
	const uint64_t get_double = cave + out.size();
	out.insert(out.end(), {0x80, 0x3d});
	emit_rel(gate, 1);
	out.push_back(0x00);
	out.insert(out.end(), {0x75, 0x09, 0xc5, 0xfb, 0x10, 0x05});
	emit_rel(data, 0);
	out.push_back(0xc3);
	out.insert(out.end(), {0xc5, 0xfb, 0x10, 0x05});
	emit_rel(numer, 0);
	out.insert(out.end(), {0xc5, 0xfb, 0x5e, 0x05});
	emit_rel(denom, 0);
	out.push_back(0xc3);
	// microseconds getter
	const uint64_t get_ticks = cave + out.size();
	out.insert(out.end(), {0x80, 0x3d});
	emit_rel(gate, 1);
	out.push_back(0x00);
	out.insert(out.end(), {0x75, 0x08, 0x48, 0x8b, 0x05});
	emit_rel(data + 16, 0);
	out.push_back(0xc3);
	out.insert(out.end(), {0x48, 0x8b, 0x05});
	emit_rel(denom + 0x10, 0);
	out.push_back(0xc3);
	// float getter
	const uint64_t get_single = cave + out.size();
	out.insert(out.end(), {0x80, 0x3d});
	emit_rel(gate, 1);
	out.push_back(0x00);
	out.insert(out.end(), {0x75, 0x09, 0xc5, 0xfa, 0x10, 0x05});
	emit_rel(data + 8, 0);
	out.push_back(0xc3);
	out.insert(out.end(), {0xc5, 0xfa, 0x10, 0x05});
	emit_rel(denom + 8, 0);
	out.push_back(0xc3);
	EXIT_IF(out.size() > 0x800);
	std::memcpy(reinterpret_cast<void*>(cave), out.data(), out.size());
	// data: the game's own step to start with
	const double step = *reinterpret_cast<const double*>(numer) / *reinterpret_cast<const double*>(denom);
	const double start = std::isfinite(step) && step > 0.0 && step <= 0.1 ? step : 1.0 / 60.0;
	*reinterpret_cast<double*>(data)        = start;
	*reinterpret_cast<float*>(data + 8)     = static_cast<float>(start);
	*reinterpret_cast<int64_t*>(data + 16)  = static_cast<int64_t>(start * 1000000.0 + 0.5);
	*reinterpret_cast<uint64_t*>(data + 24) = 0;
	*reinterpret_cast<uint8_t*>(gate)       = 0;
	*reinterpret_cast<uint64_t*>(fn_ptr)    = reinterpret_cast<uint64_t>(&DecoupleUpdate);
	// the game's code: the getters jump to the cave's, the frame update calls the stub
	for (const auto& [from, to]: {std::pair {getter, get_double}, std::pair {ticks, get_ticks},
	                              std::pair {single, get_single}}) {
		std::vector<uint8_t> jump {0xe9};
		AppendRel32(&jump, from + 5, to);
		std::memcpy(reinterpret_cast<void*>(from), jump.data(), jump.size());
		(void)Common::VirtualMemory::FlushInstructionCache(from, jump.size());
	}
	{
		std::vector<uint8_t> call_stub {0xe8};
		AppendRel32(&call_stub, call + 5, cave);
		std::memcpy(reinterpret_cast<void*>(call), call_stub.data(), call_stub.size());
		(void)Common::VirtualMemory::FlushInstructionCache(call, call_stub.size());
	}
	(void)Common::VirtualMemory::FlushInstructionCache(cave, 0x800);
	g_decouple.active   = true;
	g_decouple.base     = base;
	g_decouple.gate     = gate;
	g_decouple.last_us  = data + 24;
	g_decouple.dt       = data;
	g_decouple.numer    = numer;
	g_decouple.denom    = denom;
	g_decouple.getter   = getter;
	g_decouple.ticks    = ticks;
	g_decouple.single   = single;
	g_decouple.call     = call;
	g_decouple.cap      = cap;
	::printf("Decouple: built-in one on (Astro Bot %s %s): game time follows the real frame time, "
	         "steps capped at %s; getters at game+0x%llx, frame update's call at game+0x%llx "
	         "(KYTY_ASTRO_DECOUPLE)\n",
	         title_id.c_str(), version.c_str(), cap_text, off(getter), off(call));
	std::fflush(stdout);
}

void ApplyBuiltin(Program* main_program) {
	if (main_program == nullptr || main_program->elf == nullptr || main_program->elf->IsShared()) {
		return;
	}
	{
		std::string title_id;
		std::string version;
		if (SystemContentParamSfoGetString("TITLE_ID", &title_id) &&
		    SystemContentParamSfoGetString("APP_VER", &version)) {
			ApplyAstroRenderResolution(main_program, title_id, version);
		}
	}
	// KYTY_ASTRO_CB_SCALE=<1-4> (default 2; 1 = off): Astro Bot (PPSA21567 01.018.000) sizes its
	// five draw threads' command buffers {7, 7, 4, 4, 6} MiB (table at 0x8491510, read by
	// sub_4A38C0); running out is its "too many objects/batches in this scene" assert (Bug C). In
	// speedrun_inflator the Draw Geometry thread's frame is mostly mesh-deformer dispatches with
	// inline bone palettes (22-48 KiB each, ~5.7 MiB per frame). When that thread falls a frame
	// behind (game threads short of CPU, or blocked on reads of GPU-written memory), one buffer
	// takes two frames' work: 11.4-12.1 MiB, measured flat at that ceiling also with the game on
	// 16 logical processors. The table is scaled before the game runs.
	int scale = 2;
	if (const auto* value = std::getenv("KYTY_ASTRO_CB_SCALE"); value != nullptr) {
		scale = std::clamp(std::atoi(value), 1, 4);
	}
	if (scale == 1) {
		return;
	}
	std::string title_id;
	std::string version;
	if (!SystemContentParamSfoGetString("TITLE_ID", &title_id) ||
	    !SystemContentParamSfoGetString("APP_VER", &version) || title_id != "PPSA21567" ||
	    version != "01.018.000") {
		return;
	}
	static constexpr uint64_t kTable     = 0x8491510;
	static constexpr uint32_t kSizes[5]  = {7, 7, 4, 4, 6};
	const auto                address    = main_program->base_vaddr + kTable;
	if (!IsInsideProgram(*main_program, address, sizeof(kSizes)) ||
	    std::memcmp(reinterpret_cast<const void*>(address), kSizes, sizeof(kSizes)) != 0) {
		::printf("Game patch: Astro Bot command buffer table not found (KYTY_ASTRO_CB_SCALE)\n");
		return;
	}
	uint32_t scaled[5] {};
	for (int i = 0; i < 5; i++) {
		scaled[i] = kSizes[i] * static_cast<uint32_t>(scale);
	}
	std::memcpy(reinterpret_cast<void*>(address), scaled, sizeof(scaled));
	::printf("Game patch: Astro Bot draw-thread command buffers x%d (%u, %u, %u, %u, %u MiB; "
	         "KYTY_ASTRO_CB_SCALE)\n",
	         scale, scaled[0], scaled[1], scaled[2], scaled[3], scaled[4]);
}

// KYTY_ASTRO_RESCUE_LOG (default 0; 1 = on): one console line per call of the rescue-start and
// rescue-end functions the built-in rescue fix hooks, with the caller (an offset in the game) and the
// time, to tell real rescues from the calls at a level's start (test build 22).
// A value that points just after a call instruction in the game's code (call rel32, call [rip+d],
// call reg, call [reg+d8/d32]).
bool IsGameReturnAddress(uint64_t value) {
	for (const auto& [begin, end]: g_rescue_code) {
		if (value < begin + 6 || value >= end) {
			continue;
		}
		const auto* b = reinterpret_cast<const uint8_t*>(value);
		return b[-5] == 0xe8 || (b[-6] == 0xff && (b[-5] == 0x15 || (b[-5] & 0xf8) == 0x90)) ||
		       (b[-2] == 0xff && (b[-1] & 0xf8) == 0xd0) ||
		       (b[-3] == 0x41 && b[-2] == 0xff && (b[-1] & 0xf8) == 0xd0) ||
		       (b[-3] == 0xff && (b[-2] & 0xf8) == 0x50);
	}
	return false;
}

void PrintRescueLog() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_ASTRO_RESCUE_LOG");
		return value != nullptr && value[0] == '1';
	}();
	const auto log = g_rescue_log.load(std::memory_order_relaxed);
	if (!enabled || log == 0) {
		return;
	}
	using Clock = std::chrono::steady_clock;
	static const auto started = Clock::now();
	static uint64_t   printed = 0;
	const uint64_t    count   = *reinterpret_cast<volatile uint64_t*>(log);
	if (count == printed) {
		return;
	}
	if (count - printed > 16) {
		::printf("Game patch: bot rescue log: %llu call(s) not shown (KYTY_ASTRO_RESCUE_LOG)\n",
		         static_cast<unsigned long long>(count - printed - 16));
		printed = count - 16;
	}
	const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
	const auto base    = g_rescue_base.load(std::memory_order_relaxed);
	const auto* entries = reinterpret_cast<volatile uint64_t*>(log + 8);
	for (; printed < count; printed++) {
		const uint64_t entry  = entries[printed & 15];
		const uint64_t caller = entry & ((uint64_t {1} << 62) - 1);
		::printf("Game patch: bot rescue log: %s called from game+0x%llx, Decouple was %s, %.1f s "
		         "(KYTY_ASTRO_RESCUE_LOG)\n",
		         (entry >> 63) != 0 ? "end  " : "start", static_cast<unsigned long long>(caller - base),
		         ((entry >> 62) & 1) != 0 ? "on " : "off", seconds);
		// its call chain: the game return addresses in the stack above the caller's
		const auto* stack = reinterpret_cast<volatile uint64_t*>(log + (0x400 - 0x208) + (printed & 15) * 0x200);
		std::string chain;
		int         found = 0;
		for (int i = 0; i < 64 && found < 10; i++) {
			const uint64_t value = stack[i];
			if (IsGameReturnAddress(value)) {
				char text[32];
				std::snprintf(text, sizeof(text), " <- 0x%llx", static_cast<unsigned long long>(value - base));
				chain += text;
				found++;
			}
		}
		::printf("Game patch: bot rescue log:   chain 0x%llx%s (KYTY_ASTRO_RESCUE_LOG)\n",
		         static_cast<unsigned long long>(caller - base), chain.c_str());
	}
	std::fflush(stdout);
}

void OnGuestFlip() {
	PrintRescueLog();
	const auto gate = g_rescue_gate.load(std::memory_order_relaxed);
	if (gate == 0) {
		return;
	}
	using Clock          = std::chrono::steady_clock;
	static Clock::time_point since {};
	static uint32_t          timeouts = 0;
	auto* marker = reinterpret_cast<volatile uint8_t*>(g_rescue_marker.load(std::memory_order_relaxed));
	if (*marker == 0) {
		since = {};
		return;
	}
	const auto now = Clock::now();
	if (since == Clock::time_point {}) {
		since = now;
		return;
	}
	if (now - since < std::chrono::seconds(RescueTimeoutSeconds())) {
		return;
	}
	// What the rescue end does: Decouple back on, its timestamp reset, the marker cleared.
	*reinterpret_cast<volatile uint64_t*>(g_rescue_state.load(std::memory_order_relaxed)) = 0;
	*reinterpret_cast<volatile uint8_t*>(gate) = 0;
	*marker = 0;
	since = {};
	timeouts++;
	::printf("Game patch: bot rescue fix: Decouple was off for %u s without the rescue's end; "
	         "switched back on (%u time(s) this run; KYTY_ASTRO_RESCUE_FIX)\n",
	         RescueTimeoutSeconds(), timeouts);
}

void Clear() {
	g_rescue_gate.store(0);
	if (g_pending_plan != nullptr) {
		ReleaseCaves(&g_pending_plan->cave_pages);
	}
	g_pending_plan.reset();
	ReleaseCaves(&g_applied_cave_pages);
}

} // namespace Loader::GamePatch
