// kyty_shader_stats: translates every permutation of a shader precompile journal
// (_PipelineCache/<title>.shaders.journal) offline, as the replay does (no GPU, no game), and
// reports the SPIR-V each produces with KYTY_FLOAT_PROFILE=precise and =relaxed.
//
//   kyty_shader_stats <journal> [out.csv] [--dump <dir>] [--limit <n>] [--opt] [--hist]
//
// CSV: one row per permutation (stage, guest hash, guest code words, SPIR-V words with each
// profile, translate+emit time). --dump writes each permutation's two modules as
// <dir>/<hash>_<n>_{precise,relaxed}.spv for spirv-val / spirv-dis.
// Host emitter state is NVIDIA-like (no f32 denormal flush, robust buffer loads, shader clock).

#include "graphics/host_gpu/renderer/pipeline/pipelineList.h"
#include "graphics/host_gpu/spirvBitcastFold.h"
#include "graphics/host_gpu/renderer/pipeline/shaderPrecompile.h"
#include "graphics/shader/recompiler/CodegenOptions.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/ir/ProgramCodec.h"
#include "graphics/shader/shader.h"

#include "spirv-tools/libspirv.hpp"
#include "spirv-tools/optimizer.hpp"

#include <array>
#include <chrono>
#include <map>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace Libs::Graphics;
namespace SR = Libs::Graphics::ShaderRecompiler;

namespace {

struct Emitted {
	size_t   words = 0;
	double   ms    = 0;
	bool     ok    = false;
	std::vector<uint32_t> spirv;
};

template <typename InputInfo>
Emitted CompileOne(const ShaderJournal::Source& source, const ShaderJournal::Entry& entry,
                   SR::FloatProfile profile) {
	Emitted out;
	auto options          = SR::GetCodegenOptions();
	options.float_profile = profile;
	SR::SetCodegenOptions(options);

	auto info = std::make_unique<InputInfo>();
	constexpr auto stored = ShaderStoredInputInfoSize<InputInfo>();
	if (source.input_info.size() != stored) return out;
	std::memcpy(static_cast<void*>(info.get()), source.input_info.data(), stored);
	info->stage = {};
	ShaderSanitizeStoredInputInfo(*info);

	SR::IR::ResourceSpecialization specialization;
	if (!SR::IR::DecodeSpecialization(entry.specialization, specialization)) return out;

	SR::CompileOptions compile;
	compile.stage                   = static_cast<ShaderType>(source.stage);
	compile.shader_hash             = source.hash;
	compile.wave_size               = source.wave_size;
	compile.user_data_base          = source.user_data_base;
	compile.plain_mip_stats_variant = false;
	compile.dump_ir                 = false;
	compile.early_dump              = false;
	const std::vector<uint32_t> user_data(source.user_data_count);
	compile.user_data = user_data;
	compile.back_code = source.back_code;
	if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
		compile.input_info.vertex = info.get();
	} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
		compile.input_info.pixel = info.get();
	} else {
		compile.input_info.compute = info.get();
	}
	const auto begin      = std::chrono::steady_clock::now();
	auto       translated = SR::TranslateProgram(source.code, compile);
	if (translated.skip_dispatch) return out;
	const auto dwords = translated.program.bindings.ShaderDataDwords();
	auto result = SR::CompileProgram(std::move(translated), compile, specialization,
	                                 SR::IR::PushData::StartFor(entry.push_data_cursor, dwords));
	out.ms    = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
	out.words = result.spirv.size();
	out.spirv = std::move(result.spirv);
	out.ok    = out.words != 0;
	return out;
}

Emitted Compile(const ShaderJournal::Source& source, const ShaderJournal::Entry& entry,
                SR::FloatProfile profile) {
	switch (source.kind) {
		case ShaderJournal::Kind::Vertex: return CompileOne<ShaderVertexInputInfo>(source, entry, profile);
		case ShaderJournal::Kind::Pixel: return CompileOne<ShaderPixelInputInfo>(source, entry, profile);
		case ShaderJournal::Kind::Compute: return CompileOne<ShaderComputeInputInfo>(source, entry, profile);
	}
	return {};
}

const char* StageName(uint32_t stage) {
	switch (static_cast<ShaderType>(stage)) {
		case ShaderType::Vertex: return "vs";
		case ShaderType::Pixel: return "ps";
		case ShaderType::Compute: return "cs";
		case ShaderType::Mesh: return "ms";
		case ShaderType::Local: return "ls";
		case ShaderType::TessellationControl: return "hs";
		case ShaderType::TessellationEvaluation: return "ds";
		default: return "?";
	}
}

// spirv-val as the emulator runs it (Vulkan 1.3). Returns the first message, empty when valid.
std::string Validate(const std::vector<uint32_t>& words) {
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	std::string          message;
	tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&,
	                             const char* text) {
		if (message.empty()) message = text;
	});
	spvtools::ValidatorOptions options;
	options.SetAllowOffsetTextureOperand(true);
	return tools.Validate(words.data(), words.size(), options) ? std::string() : (message.empty() ? "invalid" : message);
}

// Words after spirv-opt's performance passes: what is left once redundancy is removed, a proxy
// for what the driver's own optimizer works with.
size_t OptimizedWords(const std::vector<uint32_t>& words) {
	spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
	optimizer.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&, const char*) {});
	optimizer.RegisterPerformancePasses();
	std::vector<uint32_t> out;
	spvtools::ValidatorOptions vopts;
	vopts.SetAllowOffsetTextureOperand(true);
	spvtools::OptimizerOptions options;
	options.set_run_validator(false);
	options.set_validator_options(vopts);
	if (!optimizer.Run(words.data(), words.size(), &out, options)) return 0;
	return out.size();
}

// Instruction counts by opcode in function bodies.
void CountOpcodes(const std::vector<uint32_t>& words, std::map<uint32_t, uint64_t>& counts) {
	bool in_function = false;
	for (size_t i = 5; i < words.size();) {
		const uint32_t count  = words[i] >> 16;
		const uint32_t opcode = words[i] & 0xffffu;
		if (count == 0) break;
		if (opcode == 54) in_function = true;      // OpFunction
		if (in_function) counts[opcode]++;
		if (opcode == 56) in_function = false;     // OpFunctionEnd
		i += count;
	}
}

void WriteWords(const std::filesystem::path& path, const std::vector<uint32_t>& words) {
	std::ofstream file(path, std::ios::binary);
	file.write(reinterpret_cast<const char*>(words.data()),
	           static_cast<std::streamsize>(words.size() * sizeof(uint32_t)));
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: kyty_shader_stats <journal> [out.csv] [--dump dir] [--limit n]\n");
		return 2;
	}
	std::filesystem::path journal_path = argv[1];
	std::string           csv_path     = "shader-stats.csv";
	std::filesystem::path dump_dir;
	size_t                limit = SIZE_MAX;
	bool                  optimize = false;
	bool                  histogram = false;
	for (int i = 2; i < argc; i++) {
		if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
			dump_dir = argv[++i];
		} else if (std::strcmp(argv[i], "--opt") == 0) {
			optimize = true;
		} else if (std::strcmp(argv[i], "--hist") == 0) {
			histogram = true;
		} else if (std::strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
			limit = std::strtoull(argv[++i], nullptr, 10);
		} else {
			csv_path = argv[i];
		}
	}
	if (!dump_dir.empty()) std::filesystem::create_directories(dump_dir);

	SR::Spirv::SetHostBufferRobustness({.storage_dword_loads_return_zero = true});

	std::vector<uint8_t> identity;
	if (!ShaderJournal::ReadIdentity(journal_path, identity)) {
		std::fprintf(stderr, "not a shader journal: %s\n", journal_path.string().c_str());
		return 1;
	}
	ShaderJournal journal({.path = journal_path, .identity = identity, .background_writer = false});
	const auto& sources = journal.Sources();
	const auto& entries = journal.Entries();
	std::printf("journal: %zu sources, %zu permutations\n", sources.size(), entries.size());

	FILE* csv = std::fopen(csv_path.c_str(), "w");
	std::fprintf(csv, "index,stage,hash,guest_words,precise_words,relaxed_words,precise_ms,relaxed_ms,"
	                  "precise_opt,relaxed_opt,precise_valid,relaxed_valid\n");
	size_t opt_precise = 0, opt_relaxed = 0, invalid_precise = 0, invalid_relaxed = 0;
	size_t fold_precise = 0, fold_relaxed = 0, invalid_fold = 0, fold_removed = 0, fold_roundtrips = 0;
	double fold_ms = 0;
	std::map<std::string, uint32_t> fold_errors;
	std::map<uint32_t, uint64_t> hist_precise, hist_relaxed;
	std::map<std::string, uint32_t> relaxed_errors;
	size_t total_guest = 0, total_precise = 0, total_relaxed = 0, done = 0, failed = 0;
	double ms_precise = 0, ms_relaxed = 0;
	std::vector<uint32_t> per_source(sources.size(), 0);
	for (size_t index = 0; index < entries.size() && done < limit; index++) {
		const auto& entry  = entries[index];
		const auto& source = sources[entry.source];
		if (source.code.empty() || MergedSourceHash(source.code, source.back_code) != source.hash) {
			failed++;
			continue;
		}
		const auto precise = Compile(source, entry, SR::FloatProfile::Precise);
		const auto relaxed = Compile(source, entry, SR::FloatProfile::Relaxed);
		if (!precise.ok || !relaxed.ok) {
			failed++;
			continue;
		}
		done++;
		total_guest += source.code.size();
		total_precise += precise.words;
		total_relaxed += relaxed.words;
		ms_precise += precise.ms;
		ms_relaxed += relaxed.ms;
		const auto precise_error = Validate(precise.spirv);
		const auto relaxed_error = Validate(relaxed.spirv);
		invalid_precise += precise_error.empty() ? 0 : 1;
		invalid_relaxed += relaxed_error.empty() ? 0 : 1;
		if (!relaxed_error.empty() && precise_error.empty()) {
			relaxed_errors[relaxed_error.substr(0, 160)]++;
		}
		for (const auto* emitted: {&precise, &relaxed}) {
			std::vector<uint32_t> folded;
			const auto begin = std::chrono::steady_clock::now();
			const auto fold  = SpirvBitcastFold::Fold(emitted->spirv, folded);
			fold_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
			const auto& words = fold.changed ? folded : emitted->spirv;
			(emitted == &precise ? fold_precise : fold_relaxed) += words.size();
			if (emitted == &precise) {
				fold_removed += fold.removed;
				fold_roundtrips += fold.roundtrips;
			}
			if (fold.changed) {
				const auto error = Validate(folded);
				if (!error.empty()) {
					invalid_fold++;
					fold_errors[error.substr(0, 160)]++;
				}
			}
		}
		size_t popt = 0, ropt = 0;
		if (optimize) {
			popt = OptimizedWords(precise.spirv);
			ropt = OptimizedWords(relaxed.spirv);
			opt_precise += popt;
			opt_relaxed += ropt;
		}
		if (histogram) {
			CountOpcodes(precise.spirv, hist_precise);
			CountOpcodes(relaxed.spirv, hist_relaxed);
		}
		std::fprintf(csv, "%zu,%s,%016" PRIx64 ",%zu,%zu,%zu,%.3f,%.3f,%zu,%zu,%d,%d\n", index,
		             StageName(source.stage), source.hash, source.code.size(), precise.words,
		             relaxed.words, precise.ms, relaxed.ms, popt, ropt, precise_error.empty() ? 1 : 0,
		             relaxed_error.empty() ? 1 : 0);
		if (!dump_dir.empty()) {
			char base[64];
			std::snprintf(base, sizeof(base), "%016" PRIx64 "_%u", source.hash, per_source[entry.source]++);
			WriteWords(dump_dir / (std::string(base) + "_precise.spv"), precise.spirv);
			WriteWords(dump_dir / (std::string(base) + "_relaxed.spv"), relaxed.spirv);
		}
		if (done % 200 == 0) {
			std::printf("  %zu done\n", done);
			std::fflush(stdout);
		}
	}
	std::fclose(csv);
	std::printf("translated %zu permutations (%zu skipped/failed)\n", done, failed);
	std::printf("guest words %zu, SPIR-V words precise %zu, relaxed %zu (%.2f%% smaller)\n",
	            total_guest, total_precise, total_relaxed,
	            total_precise ? 100.0 * (1.0 - double(total_relaxed) / double(total_precise)) : 0.0);
	std::printf("translate+emit ms precise %.0f, relaxed %.0f\n", ms_precise, ms_relaxed);
	std::printf("spirv-val failures: precise %zu, relaxed %zu\n", invalid_precise, invalid_relaxed);
	for (const auto& [message, count]: relaxed_errors) {
		std::printf("  relaxed only (%u): %s\n", count, message.c_str());
	}
	std::printf("bitcast fold: precise %zu, relaxed %zu words (removed %zu bitcasts in precise, %zu round trips), "
	            "%.0f ms, %zu invalid\n", fold_precise, fold_relaxed, fold_removed, fold_roundtrips, fold_ms, invalid_fold);
	for (const auto& [message, count]: fold_errors) std::printf("  fold invalid (%u): %s\n", count, message.c_str());
	if (optimize) {
		std::printf("after spirv-opt -O: precise %zu, relaxed %zu (%.2f%% smaller)\n", opt_precise,
		            opt_relaxed, opt_precise ? 100.0 * (1.0 - double(opt_relaxed) / double(opt_precise)) : 0.0);
	}
	if (histogram) {
		FILE* h = std::fopen((csv_path + ".opcodes.csv").c_str(), "w");
		std::fprintf(h, "opcode,precise,relaxed\n");
		for (const auto& [opcode, count]: hist_precise) {
			std::fprintf(h, "%u,%" PRIu64 ",%" PRIu64 "\n", opcode, count, hist_relaxed[opcode]);
		}
		for (const auto& [opcode, count]: hist_relaxed) {
			if (!hist_precise.contains(opcode)) std::fprintf(h, "%u,0,%" PRIu64 "\n", opcode, count);
		}
		std::fclose(h);
	}
	return 0;
}
