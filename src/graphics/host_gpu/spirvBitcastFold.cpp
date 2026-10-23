#include "graphics/host_gpu/spirvBitcastFold.h"

#include <spirv-tools/libspirv.h>

#include <spirv/unified1/spirv.hpp>

#include <unordered_map>

namespace Libs::Graphics::SpirvBitcastFold {

namespace {

struct Operand {
	uint16_t offset = 0; // words from the instruction start
	uint16_t count  = 0;
	bool     id     = false; // an <id> operand (renamed through the replacement map)
};

struct Instruction {
	uint32_t             offset = 0; // words from the module start
	uint16_t             words  = 0;
	uint16_t             opcode = 0;
	uint32_t             result = 0;
	uint32_t             type   = 0;
	std::vector<Operand> operands;
};

struct Parsed {
	std::vector<Instruction> instructions;
	bool                     ok = true;
};

bool IsIdOperand(spv_operand_type_t type) {
	switch (type) {
		case SPV_OPERAND_TYPE_ID:
		case SPV_OPERAND_TYPE_SCOPE_ID:
		case SPV_OPERAND_TYPE_MEMORY_SEMANTICS_ID: return true;
		default: return false;
	}
}

spv_result_t OnInstruction(void* user, const spv_parsed_instruction_t* parsed) {
	auto&       out = *static_cast<Parsed*>(user);
	Instruction inst;
	inst.words  = parsed->num_words;
	inst.opcode = parsed->opcode;
	inst.result = parsed->result_id;
	inst.type   = parsed->type_id;
	inst.operands.reserve(parsed->num_operands);
	for (uint16_t i = 0; i < parsed->num_operands; i++) {
		const auto& operand = parsed->operands[i];
		inst.operands.push_back({operand.offset, operand.num_words, IsIdOperand(operand.type)});
	}
	out.instructions.push_back(std::move(inst));
	return SPV_SUCCESS;
}

// The id an id is replaced by, following chains (a duplicate of a removed bitcast).
uint32_t Resolve(const std::unordered_map<uint32_t, uint32_t>& replace, uint32_t id) {
	for (auto found = replace.find(id); found != replace.end(); found = replace.find(id)) {
		id = found->second;
	}
	return id;
}

struct PairHash {
	size_t operator()(const std::pair<uint32_t, uint64_t>& key) const noexcept {
		return std::hash<uint64_t>()(key.second * 0x9e3779b97f4a7c15ull ^ key.first);
	}
};

} // namespace

Result Fold(std::span<const uint32_t> module, std::vector<uint32_t>& out) {
	Result result;
	out.clear();
	if (module.size() < 5) return result;

	Parsed parsed;
	parsed.instructions.reserve(module.size() / 4);
	spv_context context = spvContextCreate(SPV_ENV_VULKAN_1_3);
	// Offsets: the parser hands out pointers into `module`; recovered below from the word position.
	struct Walk {
		Parsed*         parsed;
		const uint32_t* base;
	} walk {&parsed, module.data()};
	const auto status = spvBinaryParse(
	    context, &walk, module.data(), module.size(), nullptr,
	    [](void* user, const spv_parsed_instruction_t* inst) {
		    auto& w = *static_cast<Walk*>(user);
		    const auto rc = OnInstruction(w.parsed, inst);
		    w.parsed->instructions.back().offset = static_cast<uint32_t>(inst->words - w.base);
		    return rc;
	    },
	    nullptr);
	spvContextDestroy(context);
	if (status != SPV_SUCCESS) return result;
	result.instructions = static_cast<uint32_t>(parsed.instructions.size());

	// Result types of every id, the source of every bitcast.
	std::unordered_map<uint32_t, uint32_t> type_of;
	std::unordered_map<uint32_t, uint32_t> bitcast_source;
	type_of.reserve(parsed.instructions.size());
	for (const auto& inst: parsed.instructions) {
		if (inst.result != 0 && inst.type != 0) type_of[inst.result] = inst.type;
	}

	std::unordered_map<uint32_t, uint32_t> replace;
	// (block serial, type << 32 | source) -> the block's first bitcast of it.
	std::unordered_map<std::pair<uint32_t, uint64_t>, uint32_t, PairHash> in_block;
	uint32_t block     = 0;
	bool     in_body   = false;
	for (const auto& inst: parsed.instructions) {
		switch (inst.opcode) {
			case spv::OpFunction: in_body = true; break;
			case spv::OpFunctionEnd: in_body = false; break;
			case spv::OpLabel: block++; break;
			default: break;
		}
		if (!in_body || inst.opcode != spv::OpBitcast || inst.operands.size() != 3) continue;
		const uint32_t source = Resolve(replace, module[inst.offset + inst.operands[2].offset]);
		// A bitcast of a bitcast back to the first one's source type: that source.
		if (const auto inner = bitcast_source.find(source); inner != bitcast_source.end()) {
			const auto original_type = type_of.find(inner->second);
			if (original_type != type_of.end() && original_type->second == inst.type) {
				replace[inst.result] = inner->second;
				result.roundtrips++;
				continue;
			}
		}
		const std::pair<uint32_t, uint64_t> key {block, (uint64_t {inst.type} << 32u) | source};
		if (const auto first = in_block.find(key); first != in_block.end()) {
			replace[inst.result] = first->second;
			continue;
		}
		in_block.emplace(key, inst.result);
		bitcast_source[inst.result] = source;
	}
	if (replace.empty()) return result;
	result.removed = static_cast<uint32_t>(replace.size());

	out.reserve(module.size());
	out.insert(out.end(), module.begin(), module.begin() + 5);
	for (const auto& inst: parsed.instructions) {
		if (inst.result != 0 && replace.contains(inst.result)) continue;
		// Names and decorations of removed ids go with them.
		if ((inst.opcode == spv::OpName || inst.opcode == spv::OpDecorate ||
		     inst.opcode == spv::OpDecorateId || inst.opcode == spv::OpDecorateString) &&
		    !inst.operands.empty() &&
		    replace.contains(module[inst.offset + inst.operands[0].offset])) {
			continue;
		}
		const auto start = out.size();
		out.insert(out.end(), module.begin() + inst.offset, module.begin() + inst.offset + inst.words);
		for (const auto& operand: inst.operands) {
			if (!operand.id) continue;
			for (uint16_t w = 0; w < operand.count; w++) {
				auto& word = out[start + operand.offset + w];
				word       = Resolve(replace, word);
			}
		}
	}
	result.changed = true;
	return result;
}

} // namespace Libs::Graphics::SpirvBitcastFold
