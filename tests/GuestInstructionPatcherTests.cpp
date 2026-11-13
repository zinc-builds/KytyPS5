// VRSQRTPS sites in functions with unresolved indirect branches: the preceding-jump rewrite
// (guestInstructionPatcher.cpp, RelocateGuestInstructions). Each fixture function loads four
// floats from rdi, takes VRSQRTPS of them and stores them back; esi selects an entry path. Every
// path must give the emulator's exact result: the trampoline's native sequence on the main path,
// the trap emulation (SIGILL) when entered past the overwritten instruction.
// KYTY_PATCHER_TEST_EXHAUSTIVE=1 also compares the native sequence with the emulator on all 2^32
// inputs (~1 min).
#include "loader/guestInstructionPatcher.h"
#include "loader/x64InstructionEmulator.h"

#include <array>
#include <bit>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sys/mman.h>
#include <vector>
#include <xbyak/xbyak.h>
#include <xmmintrin.h>

namespace {

#define CHECK(condition)                                                                           \
	do {                                                                                           \
		if (!(condition)) {                                                                        \
			std::fprintf(stderr, "GuestInstructionPatcherTests:%d: %s\n", __LINE__, #condition);  \
			std::abort();                                                                          \
		}                                                                                          \
	} while (false)

volatile uint64_t g_traps = 0;
uintptr_t         g_dump_from = 0; // the overwritten instruction of the fixture under test

void OnSigill(int /*signal*/, siginfo_t* /*info*/, void* context) {
	if (!Loader::X64InstructionEmulator::TryEmulate(context)) {
		std::signal(SIGILL, SIG_DFL);
		return;
	}
	g_traps = g_traps + 1;
}

using Fixture = void (*)(float* values, int path);

struct Built {
	Fixture   function;
	uintptr_t start;
	uintptr_t overwritten; // the instruction the near jump goes over
	uintptr_t site;        // the VRSQRTPS
	std::vector<int> fallback_paths; // paths that enter past the overwritten instruction
	std::vector<int> native_paths;
};

constexpr size_t CODE_SIZE       = 64 * 1024;
constexpr size_t TRAMPOLINE_SIZE = 1024 * 1024;

uint8_t* g_code = nullptr;

// The shapes. Path 0 is the straight path through the overwritten instruction; path 9 jumps
// through a register (the function's unresolved indirect branch) to `indirect_target`.
enum class Shape { Exactly5, LongRipRelative, BlockStart, InterveningTarget };

Built Build(Xbyak::CodeGenerator& g, Shape shape) {
	using namespace Xbyak::util;
	Built built {};
	g.align(16);
	built.start = reinterpret_cast<uintptr_t>(g.getCurr());
	Xbyak::Label indirect, indirect_target, overwritten, site_label, done, rip_zero, mid;

	g.cmp(esi, 9);
	g.je(indirect, Xbyak::CodeGenerator::T_NEAR);
	switch (shape) {
		case Shape::Exactly5:
			// mov eax, imm32 (5 bytes), then a 4-byte load, then the site.
			g.L(overwritten);
			g.mov(eax, 0x12345678);
			g.L(indirect_target);
			g.vmovups(xmm0, ptr[rdi]);
			built.native_paths = {0};
			built.fallback_paths = {9};
			break;
		case Shape::LongRipRelative:
			// The load, then vorps xmm0, xmm0, [rip+zero] (8 bytes, RIP-relative, bitwise identity).
			g.vmovups(xmm0, ptr[rdi]);
			g.L(overwritten);
			g.vorps(xmm0, xmm0, ptr[rip + rip_zero]);
			g.L(indirect_target);
			built.native_paths = {0};
			built.fallback_paths = {};
			break;
		case Shape::BlockStart:
			// The overwritten instruction is a direct branch target.
			g.vmovups(xmm0, ptr[rdi]);
			g.cmp(esi, 2);
			g.je(overwritten, Xbyak::CodeGenerator::T_NEAR);
			g.nop();
			g.L(overwritten);
			g.mov(eax, 0x1234);
			g.L(indirect_target);
			g.vmovaps(xmm1, xmm0);
			built.native_paths = {0, 2};
			built.fallback_paths = {9};
			break;
		case Shape::InterveningTarget:
			// A direct branch into the middle of the copied span: it runs in place and traps.
			g.cmp(esi, 3);
			g.je(mid, Xbyak::CodeGenerator::T_NEAR);
			g.L(overwritten);
			g.mov(eax, 0x5678);
			g.L(mid);
			g.L(indirect_target);
			g.vmovups(xmm0, ptr[rdi]);
			built.native_paths = {0};
			built.fallback_paths = {3, 9};
			break;
	}
	g.L(site_label);
	g.vrsqrtps(xmm0, xmm0);
	g.vmovups(ptr[rdi], xmm0);
	g.L(done);
	g.ret();
	g.L(indirect);
	if (shape == Shape::LongRipRelative) {
		g.vmovups(xmm0, ptr[rdi]);
		g.vorps(xmm0, xmm0, ptr[rip + rip_zero]);
	}
	g.lea(rax, ptr[rip + indirect_target]);
	g.jmp(rax);
	g.align(16);
	g.L(rip_zero);
	g.dd(0);
	g.dd(0);
	g.dd(0);
	g.dd(0);
	g.ready();
	if (shape == Shape::LongRipRelative) {
		built.fallback_paths = {9};
	}
	built.function    = reinterpret_cast<Fixture>(built.start);
	built.overwritten = reinterpret_cast<uintptr_t>(overwritten.getAddress());
	built.site        = reinterpret_cast<uintptr_t>(site_label.getAddress());
	return built;
}

uint32_t Bits(float f) {
	return std::bit_cast<uint32_t>(f);
}

void CheckLanes(Fixture f, int path, const std::array<uint32_t, 4>& in) {
	alignas(16) std::array<float, 4> values {};
	for (int i = 0; i < 4; i++) values[i] = std::bit_cast<float>(in[i]);
	f(values.data(), path);
	for (int i = 0; i < 4; i++) {
		const uint32_t expected = Loader::X64InstructionEmulator::ReciprocalSquareRootLane(in[i]);
		if (Bits(values[i]) != expected) {
			std::fprintf(stderr, "path %d input %08x: got %08x expected %08x\n", path, in[i],
			             Bits(values[i]), expected);
			if (g_dump_from != 0) {
				// The function and the trampoline it jumps to, for objdump -b binary.
				const auto* b = reinterpret_cast<const uint8_t*>(g_dump_from);
				int32_t     rel;
				std::memcpy(&rel, b + 1, 4);
				const auto* t = b + 5 + rel;
				if (FILE* out = std::fopen("/tmp/patcher-test-trampoline.bin", "wb")) {
					std::fwrite(t, 1, 256, out);
					std::fclose(out);
				}
				std::fprintf(stderr, "trampoline dumped to /tmp/patcher-test-trampoline.bin\n");
			}
			CHECK(false);
		}
	}
}

std::vector<uint32_t> Inputs() {
	std::vector<uint32_t> inputs = {
	    0x00000000, 0x80000000, 0x00000001, 0x807fffff, 0x00800000, 0x3f800000, 0x40800000,
	    0x7f7fffff, 0x7f800000, 0xff800000, 0x7f800001, 0x7fc00000, 0xffc12345, 0x7fbfffff,
	    0xbf800000, 0x3e99999a, 0x4b000001, 0x00400000, 0x34000000, 0x5f3759df};
	std::mt19937 random(12345);
	for (int i = 0; i < 4096; i++) inputs.push_back(random());
	while (inputs.size() % 4 != 0) inputs.push_back(0x3f800000);
	return inputs;
}

void TestShapes() {
	Xbyak::CodeGenerator  g(CODE_SIZE, g_code);
	std::vector<Built>    built;
	for (const auto shape: {Shape::Exactly5, Shape::LongRipRelative, Shape::BlockStart,
	                        Shape::InterveningTarget}) {
		built.push_back(Build(g, shape));
	}
	std::vector<uintptr_t> starts;
	for (const auto& b: built) starts.push_back(b.start);
	const size_t original_size = g.getSize();
	std::vector<uint8_t> original(g_code, g_code + original_size);

	Loader::RegisterGuestInstructionPatchModule(g_code, CODE_SIZE, g_code + CODE_SIZE,
	                                            TRAMPOLINE_SIZE);
	const auto result = Loader::PatchGuestInstructions(reinterpret_cast<uintptr_t>(g_code),
	                                                   original_size, starts, false, true,
	                                                   Loader::GuestInstructionHostFeatures {}, 0);
	std::printf("VRSQRTPS found=%llu native=%llu trapped=%llu preceding_jump=%llu\n",
	            static_cast<unsigned long long>(result.reciprocal_sqrt.found),
	            static_cast<unsigned long long>(result.reciprocal_sqrt.native),
	            static_cast<unsigned long long>(result.reciprocal_sqrt.trapped),
	            static_cast<unsigned long long>(result.reciprocal_sqrt_preceding_jump));
	CHECK(result.reciprocal_sqrt.found == built.size());
	CHECK(result.reciprocal_sqrt.native == built.size());
	CHECK(result.reciprocal_sqrt_preceding_jump == built.size());

	const auto inputs = Inputs();
	for (const auto& b: built) {
		// The near jump over the overwritten instruction, the site in its trap form, and nothing
		// else changed.
		CHECK(g_code[b.overwritten - reinterpret_cast<uintptr_t>(g_code)] == 0xe9);
		const size_t site = b.site - reinterpret_cast<uintptr_t>(g_code);
		CHECK(g_code[site] == 0xc5 && (g_code[site + 1] & 0x08) == 0 && g_code[site + 2] == 0x52);
		for (size_t i = 0; i < original_size; i++) {
			const bool in_jump = i >= b.overwritten - reinterpret_cast<uintptr_t>(g_code) &&
			                     i < b.overwritten - reinterpret_cast<uintptr_t>(g_code) + 5;
			if (i == site + 1 || in_jump) continue;
			bool other = false;
			for (const auto& o: built) {
				const size_t ow = o.overwritten - reinterpret_cast<uintptr_t>(g_code);
				const size_t os = o.site - reinterpret_cast<uintptr_t>(g_code);
				other |= (i >= ow && i < ow + 8) || i == os + 1;
			}
			if (!other) CHECK(g_code[i] == original[i]);
		}
		g_dump_from = b.overwritten;
		for (const int mxcsr: {0x1f80, 0x9fc0, 0x3f80}) { // default; FTZ+DAZ; round down
			_mm_setcsr(mxcsr);
			for (const int path: b.native_paths) {
				const uint64_t traps = g_traps;
				for (size_t i = 0; i < inputs.size(); i += 4) {
					CheckLanes(b.function, path, {inputs[i], inputs[i + 1], inputs[i + 2], inputs[i + 3]});
				}
				CHECK(g_traps == traps); // native: no trap
			}
			for (const int path: b.fallback_paths) {
				const uint64_t traps = g_traps;
				for (size_t i = 0; i < 64; i += 4) {
					CheckLanes(b.function, path, {inputs[i], inputs[i + 1], inputs[i + 2], inputs[i + 3]});
				}
				CHECK(g_traps == traps + 16); // entered past the jump: the site traps
			}
			CHECK(static_cast<int>(_mm_getcsr() & ~0x3fu) == (mxcsr & ~0x3f)); // MXCSR kept
			_mm_setcsr(0x1f80);
		}
	}

	if (const char* all = std::getenv("KYTY_PATCHER_TEST_EXHAUSTIVE"); all != nullptr && *all == '1') {
		alignas(16) std::array<float, 4> values {};
		uint64_t                         mismatches = 0;
		for (uint64_t x = 0; x < (1ull << 32); x += 4) {
			for (int i = 0; i < 4; i++) values[i] = std::bit_cast<float>(static_cast<uint32_t>(x + i));
			built[0].function(values.data(), 0);
			for (int i = 0; i < 4; i++) {
				mismatches += Bits(values[i]) !=
				              Loader::X64InstructionEmulator::ReciprocalSquareRootLane(static_cast<uint32_t>(x + i));
			}
		}
		std::printf("exhaustive: 2^32 inputs, %llu mismatches\n", static_cast<unsigned long long>(mismatches));
		CHECK(mismatches == 0);
	}
}

} // namespace

int main() {
	g_code = static_cast<uint8_t*>(mmap(nullptr, CODE_SIZE + TRAMPOLINE_SIZE,
	                                    PROT_READ | PROT_WRITE | PROT_EXEC,
	                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	CHECK(g_code != MAP_FAILED);
	struct sigaction action {};
	action.sa_sigaction = OnSigill;
	action.sa_flags     = SA_SIGINFO;
	sigaction(SIGILL, &action, nullptr);
	TestShapes();
	std::puts("Guest instruction patcher tests passed");
}
