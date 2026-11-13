// patchstats <eboot.img> <exec_size> <eh_frame_hdr_offset> <eh_frame_hdr_size> [sites.txt]
//
// Runs the emulator's guest instruction patcher (--amd-cpu) on a flat eboot image (self2img.py)
// offline and reports why VRSQRTPS sites are left to trap, and which other placements could make
// them native. Built by the patch_stats target (tools/astro-bench/build.sh patch_stats).
// Astro Bot (PPSA21567): exec segment 0x74bd57c bytes at 0, eh_frame_hdr 0x74c0000 + 0x269b04.
// sites.txt (optional): one trapped site per line as "<eboot offset> <name>", printed with its
// classification.
#include "loader/guestInstructionPatcher.h"

#include <Zydis/Zydis.h>
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <sys/mman.h>
#include <vector>

namespace {

struct Site {
	uintptr_t              site, start, end;
	bool                   indirect, rsqrt;
	std::vector<uintptr_t> unresolved;
};
std::vector<Site> g_sites;

void Observe(const Loader::TrappedSiteInfo& info) {
	g_sites.push_back({info.site, info.function_start, info.function_end, info.indirect_function,
	                   info.reciprocal_sqrt,
	                   {info.unresolved_indirect_branches.begin(), info.unresolved_indirect_branches.end()}});
}

ZydisDecoder g_decoder;

struct Insn {
	uintptr_t               address;
	ZydisDecodedInstruction instruction;
	ZydisDecodedOperand     operands[ZYDIS_MAX_OPERAND_COUNT];
};

bool Decode(const uint8_t* original, uintptr_t base, uintptr_t address, uintptr_t end, Insn& out) {
	out.address = address;
	return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&g_decoder, original + (address - base), end - address,
	                                           &out.instruction, out.operands));
}

bool IsBranch(const ZydisDecodedInstruction& i) {
	const auto c = i.meta.category;
	return c == ZYDIS_CATEGORY_COND_BR || c == ZYDIS_CATEGORY_UNCOND_BR || c == ZYDIS_CATEGORY_CALL ||
	       c == ZYDIS_CATEGORY_RET || c == ZYDIS_CATEGORY_SYSTEM || c == ZYDIS_CATEGORY_INTERRUPT;
}

const char* IndirectKind(const Insn& insn) {
	const auto& op = insn.operands[0];
	if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) return "jmp_reg";
	if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
		if (op.mem.base == ZYDIS_REGISTER_RIP) return "jmp_rip_mem";
		if (op.mem.index != ZYDIS_REGISTER_NONE) return "jmp_indexed_mem";
		return "jmp_base_disp_mem";
	}
	return "other";
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 5) {
		std::fprintf(stderr, "usage: patchstats <eboot.img> <exec_size> <eh_hdr_offset> <eh_hdr_size> [sites]\n");
		return 2;
	}
	FILE* f = std::fopen(argv[1], "rb");
	if (f == nullptr) return 1;
	std::fseek(f, 0, SEEK_END);
	const size_t size = static_cast<size_t>(std::ftell(f));
	std::fseek(f, 0, SEEK_SET);
	const uint64_t exec_size = std::strtoull(argv[2], nullptr, 0);
	const uint64_t eh_offset = std::strtoull(argv[3], nullptr, 0);
	const uint64_t eh_size   = std::strtoull(argv[4], nullptr, 0);

	constexpr uint64_t TRAMPOLINE = 8u << 20;
	const uint64_t     aligned    = (size + 0xffff) & ~0xffffull;
	auto*              base       = static_cast<uint8_t*>(mmap(nullptr, aligned + TRAMPOLINE,
	                                                           PROT_READ | PROT_WRITE | PROT_EXEC,
	                                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	if (base == MAP_FAILED) return 1;
	if (std::fread(base, 1, size, f) != size) return 1;
	std::fclose(f);
	std::vector<uint8_t> original(base, base + exec_size);
	const auto           b = reinterpret_cast<uintptr_t>(base);

	std::vector<uintptr_t> starts;
	if (!Loader::DecodeEhFrameFunctionStarts(b + eh_offset, eh_size, &starts)) {
		std::fprintf(stderr, "eh_frame_hdr decode failed\n");
		return 1;
	}
	Loader::RegisterGuestInstructionPatchModule(base, aligned, base + aligned, TRAMPOLINE);
	Loader::SetTrappedSiteObserver(Observe);
	const auto result = Loader::PatchGuestInstructions(b, exec_size, starts, false, true,
	                                                   Loader::GuestInstructionHostFeatures {}, 0);
	std::printf("functions=%" PRIu64 " VRSQRTPS found=%" PRIu64 " native=%" PRIu64 " trapped=%" PRIu64
	            " preceding_jump=%" PRIu64 " EXTRQ native=%" PRIu64 " trapped=%" PRIu64 "\n",
	            result.function_count, result.reciprocal_sqrt.found, result.reciprocal_sqrt.native,
	            result.reciprocal_sqrt.trapped, result.reciprocal_sqrt_preceding_jump,
	            result.extrq.native, result.extrq.trapped);

	ZydisDecoderInit(&g_decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

	// Relay slots: 5+ bytes the patch left dead (the tail of a patched span: a near jump followed
	// by padding), and runs of int3/nop padding between functions.
	std::vector<uintptr_t> relays;
	{
		const uint8_t* p = base;
		for (uint64_t i = 0; i + 10 <= exec_size; i++) {
			if (p[i] == 0xe9 && original[i] != 0xe9) {
				uint64_t j = i + 5;
				while (j < exec_size && p[j] != original[j]) j++;
				// Changed bytes beyond the jump: padding the patch wrote over relocated code.
				if (j - (i + 5) >= 5) relays.push_back(b + i + 5);
				i = j - 1;
			}
		}
		std::set<uintptr_t> start_set(starts.begin(), starts.end());
		for (uint64_t i = 0; i < exec_size;) {
			if (original[i] == 0xcc) {
				uint64_t j = i;
				while (j < exec_size && original[j] == 0xcc) j++;
				if (j - i >= 5) relays.push_back(b + i);
				i = j;
			} else {
				i++;
			}
		}
		std::sort(relays.begin(), relays.end());
	}

	struct Stats {
		uint64_t sites = 0, nonindirect = 0, indirect = 0;
		uint64_t long_prev_block = 0, long_prev_any = 0, relay_prev = 0, rest = 0;
		std::map<std::string, uint64_t> kinds;           // unresolved branch kinds per function
		std::map<std::string, uint64_t> kind_sites;      // sites whose functions have only this kind
		std::map<int, uint64_t>         prev_len;        // length of the instruction before the site
	} st;
	std::map<uintptr_t, std::string> classes;
	std::set<uintptr_t>              counted_functions;
	const uint8_t*                   orig = original.data();
	for (const auto& s: g_sites) {
		if (!s.rsqrt) continue;
		st.sites++;
		(s.indirect ? st.indirect : st.nonindirect)++;
		// Unresolved branch kinds.
		std::set<std::string> kinds;
		for (const uintptr_t br: s.unresolved) {
			Insn insn;
			if (Decode(orig, b, br, s.end, insn)) kinds.insert(IndirectKind(insn));
		}
		if (counted_functions.insert(s.start).second) {
			for (const auto& k: kinds) st.kinds[k]++;
		}
		std::string kind_key;
		for (const auto& k: kinds) kind_key += (kind_key.empty() ? "" : "+") + k;
		st.kind_sites[kind_key.empty() ? "none" : kind_key]++;

		// Linear sweep of the function: instruction boundaries and direct branch targets.
		std::vector<Insn>   insns;
		std::set<uintptr_t> targets;
		for (uintptr_t a = s.start; a < s.end;) {
			Insn insn;
			if (!Decode(orig, b, a, s.end, insn)) break;
			insns.push_back(insn);
			if (IsBranch(insn.instruction) && insn.operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
			    insn.operands[0].imm.is_relative) {
				targets.insert(a + insn.instruction.length + insn.operands[0].imm.value.s);
			}
			a += insn.instruction.length;
		}
		auto it = std::find_if(insns.begin(), insns.end(), [&](const Insn& i) { return i.address == s.site; });
		std::string cls = "trap";
		if (it == insns.end()) {
			cls = "trap(sweep-misaligned)";
		} else {
			st.prev_len[it == insns.begin() ? 0 : static_cast<int>(std::prev(it)->instruction.length)]++;
			// Walk back through straight-line code: no branches, calls or returns; untouched by
			// other patches. A preceding instruction of 5+ bytes can hold the near jump; the
			// instructions after it stay in place (the site keeps its trap), so a jump into them
			// from anywhere still runs correctly.
			bool crossed_target = targets.contains(s.site);
			bool found_block = false, found_any = false, found_relay = false;
			for (auto p = it; p != insns.begin();) {
				--p;
				if (IsBranch(p->instruction)) break;
				bool touched = false;
				for (uint64_t k = 0; k < p->instruction.length; k++) {
					touched |= base[p->address - b + k] != orig[p->address - b + k];
				}
				if (touched) break;
				const int len = p->instruction.length;
				if (len >= 5) {
					(crossed_target ? found_any : found_block) = true;
					break;
				}
				if (len >= 2 && !crossed_target && !found_relay) {
					const auto r = std::lower_bound(relays.begin(), relays.end(),
					                                p->address + 2 - 128);
					found_relay = r != relays.end() && *r <= p->address + 2 + 127;
				}
				if (targets.contains(p->address)) crossed_target = true;
			}
			if (found_block) {
				st.long_prev_block++;
				cls = "long-prev(block)";
			} else if (found_any) {
				st.long_prev_any++;
				cls = "long-prev(across-target)";
			} else if (found_relay) {
				st.relay_prev++;
				cls = "rel8-relay";
			} else {
				st.rest++;
			}
		}
		classes[s.site - b] = cls;
	}
	std::printf("trapped VRSQRTPS sites=%" PRIu64 " in_indirect_functions=%" PRIu64
	            " in_other_functions=%" PRIu64 "\n",
	            st.sites, st.indirect, st.nonindirect);
	std::printf("unresolved indirect branch kinds (functions with a trapped site):");
	for (const auto& [k, n]: st.kinds) std::printf(" %s=%" PRIu64, k.c_str(), n);
	std::printf("\nsites by their function's unresolved kinds:");
	for (const auto& [k, n]: st.kind_sites) std::printf(" %s=%" PRIu64, k.c_str(), n);
	std::printf("\nlength of the instruction before the site:");
	for (const auto& [k, n]: st.prev_len) std::printf(" %d:%" PRIu64, k, n);
	std::printf("\nplacements: long_prev_same_block=%" PRIu64 " long_prev_across_branch_target=%" PRIu64
	            " rel8_relay=%" PRIu64 " still_trap=%" PRIu64 " (relay slots=%zu)\n",
	            st.long_prev_block, st.long_prev_any, st.relay_prev, st.rest, relays.size());
	if (argc > 5) {
		if (FILE* list = std::fopen(argv[5], "r"); list != nullptr) {
			char line[256];
			while (std::fgets(line, sizeof(line), list) != nullptr) {
				const uint64_t off = std::strtoull(line, nullptr, 0);
				const auto     c   = classes.find(off);
				std::printf("site 0x%" PRIx64 " %s\n", off, c == classes.end() ? "not-trapped" : c->second.c_str());
			}
			std::fclose(list);
		}
	} else {
		for (const auto& [off, cls]: classes) std::printf("site 0x%" PRIx64 " %s\n", off, cls.c_str());
	}
	return 0;
}
