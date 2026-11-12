#include "graphics/guest_gpu/streamDiag.h"

#include "graphics/guest_gpu/pm4.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace Libs::Graphics::StreamDiag {

bool ReadEnabled() noexcept {
	const char* value = std::getenv("KYTY_STREAM_DIAG");
	return value != nullptr && value[0] == '1';
}

namespace {

// ---- guest threads ---------------------------------------------------------------------------

constexpr uint32_t MaxThreads = 128;
constexpr uint32_t MaxFuncs   = 256;

struct ThreadStats {
	char                  name[32] = {};
	std::atomic<uint32_t> calls[MaxFuncs] {};
	std::atomic<uint32_t> submits[2] {};
};

ThreadStats           g_threads[MaxThreads];
std::atomic<uint32_t> g_thread_count {0};
thread_local uint32_t t_thread = UINT32_MAX;

std::mutex            g_func_mutex;
const char*           g_func_names[MaxFuncs] = {};
std::atomic<uint32_t> g_func_count {0};

uint32_t ThisThread() noexcept {
	if (t_thread == UINT32_MAX) {
		auto index = g_thread_count.fetch_add(1, std::memory_order_relaxed);
		if (index >= MaxThreads) {
			index = MaxThreads - 1; // shared overflow entry
		}
#if defined(__linux__)
		pthread_getname_np(pthread_self(), g_threads[index].name, sizeof(g_threads[index].name));
#endif
		t_thread = index;
	}
	return t_thread;
}

void Bump(std::atomic<uint32_t>& counter) noexcept {
	// One writer per counter (the owning thread); the command thread only reads.
	counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

// The thread that last wrote a packet into each 4 KiB page of command memory.
constexpr uint32_t    ChunkBits = 16;
std::atomic<uint64_t> g_chunks[1u << ChunkBits];

uint32_t ChunkSlot(uint64_t page) noexcept {
	return static_cast<uint32_t>((page * 0x9e3779b97f4a7c15ull) >> (64 - ChunkBits));
}

struct SubmitRecord {
	const uint32_t* commands;
	uint32_t        dw;
	uint32_t        thread;
};
std::mutex               g_submit_mutex;
std::deque<SubmitRecord> g_submits[2];

// ---- hashing ---------------------------------------------------------------------------------

uint64_t Mix(uint64_t x) noexcept {
	x ^= x >> 33u;
	x *= 0xff51afd7ed558ccdull;
	x ^= x >> 33u;
	x *= 0xc4ceb9fe1a85ec53ull;
	x ^= x >> 33u;
	return x;
}

void Seq(uint64_t& h, uint64_t v) noexcept {
	h = (h ^ v) * 0x100000001b3ull;
	h ^= h >> 29u;
}

// ---- register model --------------------------------------------------------------------------

enum : uint32_t { CX = 0, SH = 1, UC = 2, MISC = 3, Classes = 4 };
constexpr uint32_t ClassSize[Classes] = {Pm4::CX_NUM, Pm4::SH_NUM, Pm4::UC_NUM, 16};
constexpr char     ClassName[Classes] = {'c', 's', 'u', 'm'};

// MISC: persistent draw state set by packets that are not register writes.
enum : uint32_t {
	MiscIndexType = 0,
	MiscIndexBaseLo,
	MiscIndexBaseHi,
	MiscIndexBufferSize,
	MiscNumInstances,
	MiscDrawArgsBaseLo,
	MiscDrawArgsBaseHi,
	MiscDispatchArgsBaseLo,
	MiscDispatchArgsBaseHi,
};

// Values a parameterized template leaves open: the shader stages' user-data registers (table
// pointers and per-draw constants) and everything in MISC but the index type.
bool Masked(uint32_t c, uint32_t offset) noexcept {
	if (c == SH) {
		if (offset >= Pm4::COMPUTE_USER_DATA_0 && offset < Pm4::COMPUTE_USER_DATA_0 + 32u) {
			return true;
		}
		const auto in_stage = offset & 0x3fu;
		return offset < 0x200u && in_stage >= 0xcu && in_stage < 0x2cu;
	}
	return c == MISC && offset != MiscIndexType;
}

uint64_t RegHash(uint32_t c, uint32_t offset, uint32_t value) noexcept {
	return Mix((static_cast<uint64_t>(c) << 60u) ^ (static_cast<uint64_t>(offset) << 32u) ^ value ^
	           0x9e3779b97f4a7c15ull);
}

struct RegClass {
	std::vector<uint32_t> val, sub, seg; // value; submission and segment that last wrote it
	uint64_t              h_exact = 0, h_param = 0;
	uint32_t              live = 0, sub_written = 0, seg_written = 0, max_offset = 0;
};

// ---- per-frame records -----------------------------------------------------------------------

struct Segment {
	uint64_t e, p, s, de, dp;
	uint32_t kv_begin, kv_count;
	uint32_t packets, dwords, deps, written;
	bool     draw;
};

struct Span {
	uint64_t address, hash;
	uint32_t dw, units;
};

struct Submission {
	uint32_t thread = UINT32_MAX, dw = 0, packets = 0, draws = 0, dispatches = 0, spans = 0;
	uint32_t calls = 0, ctx_ops = 0, labels = 0, waits = 0, events = 0, e39 = 0, dma = 0;
	int32_t  first_ctx_op = -1;
	bool     continued = false, has_first = false;
	std::array<uint32_t, Classes>             inh_first {}, inh_max {};
	std::vector<std::pair<uint32_t, uint32_t>> builders; // thread, draws
	std::vector<std::pair<uint32_t, uint32_t>> targets;  // colour 0 base, depth write base
};

struct History {
	std::vector<uint64_t> e, p, s, de, dp; // sorted
};

constexpr uint32_t Ring      = 36;
constexpr uint32_t AnyFrames = 34;
constexpr std::array<uint32_t, 4> Backs = {1, 2, 3, 33};
constexpr uint32_t Buckets = 8;

struct State {
	RegClass                reg[Classes];
	std::vector<uint32_t>   saved_val, saved_sub, saved_seg;
	uint64_t                saved_exact = 0, saved_param = 0;
	bool                    saved = false;
	uint32_t                cur_sub = 0, cur_seg = 1;

	// The open segment.
	uint64_t seg_e = 0, seg_p = 0, seg_s = 0;
	uint32_t seg_packets = 0, seg_dwords = 0, seg_kv_begin = 0;

	const uint32_t*         next_packet = nullptr;
	bool                    sub_open    = false;
	Submission              sub;
	Span                    span {};
	bool                    span_open = false;

	// This frame.
	std::vector<Segment>    segments;
	std::vector<uint32_t>   kv_keys, kv_vals;
	std::vector<Span>       spans;
	std::vector<Submission> subs;
	uint32_t                compute_subs = 0, compute_packets = 0, compute_dispatches = 0;
	uint32_t                tail_packets = 0, out_of_range = 0;

	// The guest's debug markers: a path per nesting of names, and this frame's units by path.
	struct MarkerStat {
		uint32_t draws = 0, dispatches = 0, events = 0, e39 = 0;
	};
	std::vector<std::string>                 marker_paths {"-"};
	std::unordered_map<std::string, uint32_t> marker_ids {{"-", 0}};
	std::vector<uint32_t>                    marker_stack;
	std::vector<MarkerStat>                  marker_stats;

	// Earlier frames.
	History                                 ring[Ring];
	uint64_t                                frames = 0;
	std::unordered_map<uint64_t, uint32_t>  any_e, any_p, any_dp;
	std::vector<Segment>                    prev_segments;
	std::vector<uint32_t>                   prev_kv_vals;
	std::unordered_map<uint64_t, std::vector<uint32_t>> prev_by_shape;
	std::unordered_map<uint64_t, uint32_t>  prev_first_p, prev_first_e;
	std::unordered_map<uint64_t, std::pair<uint64_t, uint32_t>> prev_spans;
	std::unordered_set<uint64_t>            prev_span_hashes;

	// Since the last period summary.
	uint64_t                                opcodes[2][256] = {};
	std::unordered_map<uint32_t, uint64_t>  inherited, differing;
	uint64_t                                inherited_subs = 0, differing_pairs = 0;
	uint64_t                                run_p[Buckets] = {}, run_e[Buckets] = {};
	uint64_t                                ndiff[Buckets] = {}, seg_pk[Buckets] = {}, seg_dw[Buckets] = {};
	uint64_t                                event_types[64] = {};
	std::vector<uint32_t>                   last_calls; // MaxThreads * MaxFuncs
	uint32_t                                last_submits[MaxThreads][2] = {};

	State() {
		for (uint32_t c = 0; c < Classes; c++) {
			reg[c].val.assign(ClassSize[c], 0);
			reg[c].sub.assign(ClassSize[c], 0);
			reg[c].seg.assign(ClassSize[c], 0);
		}
		last_calls.assign(static_cast<size_t>(MaxThreads) * MaxFuncs, 0);
	}
};

State& Cmd() {
	static State state;
	return state;
}

uint32_t Bucket(uint32_t n) noexcept {
	// 0, 1, 2-3, 4-7, 8-15, 16-63, 64-255, 256+
	if (n == 0) {
		return 0;
	}
	if (n == 1) {
		return 1;
	}
	if (n < 4) {
		return 2;
	}
	if (n < 8) {
		return 3;
	}
	if (n < 16) {
		return 4;
	}
	if (n < 64) {
		return 5;
	}
	return n < 256 ? 6 : 7;
}

void ClearClass(State& s, uint32_t c) {
	auto& r = s.reg[c];
	std::fill(r.val.begin(), r.val.end(), 0u);
	std::fill(r.sub.begin(), r.sub.end(), 0u);
	std::fill(r.seg.begin(), r.seg.end(), 0u);
	r.h_exact = r.h_param = 0;
	r.live = r.sub_written = r.seg_written = r.max_offset = 0;
}

void Write(State& s, uint32_t c, uint32_t offset, uint32_t value) {
	if (offset >= ClassSize[c]) {
		s.out_of_range++;
		return;
	}
	auto&      r      = s.reg[c];
	const bool masked = Masked(c, offset);
	if (r.sub[offset] == 0) {
		r.live++;
	} else {
		const auto old = RegHash(c, offset, r.val[offset]);
		r.h_exact ^= old;
		if (!masked) {
			r.h_param ^= old;
		}
	}
	const auto now = RegHash(c, offset, value);
	r.h_exact ^= now;
	if (!masked) {
		r.h_param ^= now;
	}
	if (r.sub[offset] != s.cur_sub) {
		r.sub_written++;
		r.sub[offset] = s.cur_sub;
	}
	if (r.seg[offset] != s.cur_seg) {
		r.seg_written++;
		r.seg[offset] = s.cur_seg;
	}
	r.val[offset] = value;
	r.max_offset  = std::max(r.max_offset, offset);

	const uint32_t key = (c << 28u) | offset;
	Seq(s.seg_s, key);
	Seq(s.seg_e, (static_cast<uint64_t>(key) << 32u) | value);
	Seq(s.seg_p, masked ? key : (static_cast<uint64_t>(key) << 32u) | value);
	s.kv_keys.push_back(key);
	s.kv_vals.push_back(value);
}

// A payload word of a packet that is not a register write. `param`: part of the template.
void Payload(State& s, uint32_t opcode, uint32_t index, uint32_t value, bool param) {
	const uint32_t key = 0x80000000u | (opcode << 16u) | std::min(index, 0xffffu);
	Seq(s.seg_e, (static_cast<uint64_t>(key) << 32u) | value);
	if (param) {
		Seq(s.seg_p, (static_cast<uint64_t>(key) << 32u) | value);
	}
	s.kv_keys.push_back(key);
	s.kv_vals.push_back(value);
}

void BeginSegment(State& s) {
	s.cur_seg++;
	for (auto& r: s.reg) {
		r.seg_written = 0;
	}
	s.seg_e = s.seg_p = s.seg_s = 0;
	s.seg_packets = s.seg_dwords = 0;
	s.seg_kv_begin = static_cast<uint32_t>(s.kv_keys.size());
}

void ContextOp(State& s, uint32_t operation) {
	auto& r = s.reg[CX];
	if (s.sub_open) {
		if (!s.sub.has_first && s.sub.first_ctx_op < 0) {
			s.sub.first_ctx_op = static_cast<int32_t>(operation);
		}
		s.sub.ctx_ops++;
	}
	const auto save = [&] {
		s.saved_val   = r.val;
		s.saved_sub   = r.sub;
		s.saved_seg   = r.seg;
		s.saved_exact = r.h_exact;
		s.saved_param = r.h_param;
		s.saved       = true;
	};
	switch (operation) {
		case 0: ClearClass(s, CX); break;
		case 1: save(); break;
		case 2:
			if (s.saved) {
				r.val     = s.saved_val;
				r.sub     = s.saved_sub;
				r.seg     = s.saved_seg;
				r.h_exact = s.saved_exact;
				r.h_param = s.saved_param;
				r.live = r.sub_written = r.seg_written = 0;
				for (uint32_t i = 0; i < ClassSize[CX]; i++) {
					r.live += r.sub[i] != 0 ? 1u : 0u;
					r.sub_written += r.sub[i] == s.cur_sub ? 1u : 0u;
					r.seg_written += r.seg[i] == s.cur_seg ? 1u : 0u;
				}
				s.saved = false;
			}
			break;
		case 3:
			save();
			ClearClass(s, CX);
			break;
		default: break;
	}
}

uint32_t CurrentMarker(const State& s) noexcept {
	return s.marker_stack.empty() ? 0u : s.marker_stack.back();
}

State::MarkerStat& MarkerStats(State& s) {
	const auto id = CurrentMarker(s);
	if (s.marker_stats.size() <= id) {
		s.marker_stats.resize(id + 1u);
	}
	return s.marker_stats[id];
}

void PushMarker(State& s, const uint32_t* packet, uint32_t packet_dw) {
	const auto* text  = reinterpret_cast<const char*>(packet + 1);
	const auto  limit = static_cast<size_t>(packet_dw - 1u) * 4u;
	std::string name(text, strnlen(text, std::min<size_t>(limit, 48)));
	for (auto& ch: name) {
		if (ch == ' ' || ch == ',' || ch == '=' || ch == '/' || ch == ':' || ch < 0x20 || ch > 0x7e) {
			ch = '_';
		}
	}
	// At most three levels: the outermost two and the innermost.
	std::string path = s.marker_stack.empty() ? name : s.marker_paths[s.marker_stack.back()] + "/" + name;
	if (s.marker_stack.size() >= 3) {
		const auto& parent = s.marker_paths[s.marker_stack[1]];
		path               = parent + "/" + name;
	}
	auto [found, inserted] = s.marker_ids.try_emplace(path, static_cast<uint32_t>(s.marker_paths.size()));
	if (inserted) {
		s.marker_paths.push_back(path);
	}
	if (s.marker_stack.size() < 64) {
		s.marker_stack.push_back(found->second);
	}
}

void FinishSpan(State& s) {
	if (s.span_open) {
		s.spans.push_back(s.span);
		s.span_open = false;
	}
}

void FinishSubmission(State& s) {
	if (!s.sub_open) {
		return;
	}
	FinishSpan(s);
	s.subs.push_back(std::move(s.sub));
	s.sub      = {};
	s.sub_open = false;
}

bool IsDraw(uint32_t opcode) noexcept {
	switch (opcode) {
		case Pm4::IT_DRAW_INDIRECT:
		case Pm4::IT_DRAW_INDEX_INDIRECT:
		case Pm4::IT_DRAW_INDEX_2:
		case Pm4::IT_DRAW_INDIRECT_MULTI:
		case Pm4::IT_DRAW_INDEX_AUTO:
		case Pm4::IT_DRAW_INDEX_OFFSET_2:
		case Pm4::IT_DRAW_INDEX_INDIRECT_MULTI:
		case Pm4::IT_DRAW_INDEX_MULTI_INST: return true;
		default: return false;
	}
}

void EndUnit(State& s, const uint32_t* packet, uint32_t opcode, bool draw) {
	auto& sub = s.sub;
	uint32_t deps = 0, written = 0;
	for (uint32_t c = 0; c < Classes; c++) {
		const auto& r = s.reg[c];
		deps += r.live - r.seg_written;
		written += r.seg_written;
		const auto inherited = r.live - r.sub_written;
		if (!sub.has_first) {
			sub.inh_first[c] = inherited;
		}
		sub.inh_max[c] = std::max(sub.inh_max[c], inherited);
	}
	if (!sub.has_first) {
		// What the submission's first draw or dispatch takes from earlier submissions.
		s.inherited_subs++;
		for (uint32_t c = 0; c < Classes; c++) {
			const auto& r = s.reg[c];
			if (r.live == r.sub_written) {
				continue;
			}
			for (uint32_t i = 0; i <= r.max_offset; i++) {
				if (r.sub[i] != 0 && r.sub[i] != s.cur_sub) {
					s.inherited[(c << 28u) | i]++;
				}
			}
		}
		sub.has_first = true;
	}
	if (draw) {
		sub.draws++;
		const auto page    = reinterpret_cast<uint64_t>(packet) >> 12u;
		const auto entry   = g_chunks[ChunkSlot(page)].load(std::memory_order_relaxed);
		const auto builder = (entry >> 8u) == page ? static_cast<uint32_t>(entry & 0xffu) : UINT32_MAX;
		auto       found   = std::find_if(sub.builders.begin(), sub.builders.end(),
		                                  [&](const auto& b) { return b.first == builder; });
		if (found == sub.builders.end()) {
			sub.builders.emplace_back(builder, 1u);
		} else {
			found->second++;
		}
		const auto&    cx     = s.reg[CX];
		const uint32_t colour = cx.sub[Pm4::CB_COLOR0_BASE] != 0 ? cx.val[Pm4::CB_COLOR0_BASE] : 0;
		const uint32_t depth  = cx.sub[Pm4::DB_Z_WRITE_BASE] != 0 ? cx.val[Pm4::DB_Z_WRITE_BASE] : 0;
		const std::pair<uint32_t, uint32_t> target {colour, depth};
		if (sub.targets.size() < 32 &&
		    std::find(sub.targets.begin(), sub.targets.end(), target) == sub.targets.end()) {
			sub.targets.push_back(target);
		}
	} else {
		sub.dispatches++;
	}
	(draw ? MarkerStats(s).draws : MarkerStats(s).dispatches)++;
	s.span.units++;

	Segment seg {};
	seg.e = s.seg_e;
	seg.p = s.seg_p;
	seg.s = s.seg_s;
	uint64_t de = seg.e, dp = opcode;
	for (uint32_t c = 0; c < Classes; c++) {
		Seq(de, s.reg[c].h_exact);
		Seq(dp, s.reg[c].h_param);
	}
	seg.de       = de;
	seg.dp       = dp;
	seg.kv_begin = s.seg_kv_begin;
	seg.kv_count = static_cast<uint32_t>(s.kv_keys.size()) - s.seg_kv_begin;
	seg.packets  = s.seg_packets;
	seg.dwords   = s.seg_dwords;
	seg.deps     = deps;
	seg.written  = written;
	seg.draw     = draw;
	s.segments.push_back(seg);
	s.seg_pk[Bucket(seg.packets)]++;
	s.seg_dw[Bucket(seg.dwords / 8u)]++;
	BeginSegment(s);
}

void IndirectRegisters(State& s, uint32_t c, const uint32_t* packet, uint32_t nop) {
	const auto* regs = reinterpret_cast<const uint32_t*>(
	    (static_cast<uint64_t>(packet[1]) & 0xfffffffcu) | (static_cast<uint64_t>(packet[2]) << 32u));
	const uint32_t count = packet[4] & 0x3fffu;
	if (regs == nullptr) {
		return;
	}
	for (uint32_t i = 0; i < count; i++) {
		const auto raw    = regs[i * 2u];
		const auto offset = raw & ~0x70000000u;
		if (offset == nop || raw == 0xffffffffu) {
			continue;
		}
		Write(s, c, offset, regs[i * 2u + 1u]);
	}
	s.seg_dwords += count * 2u;
}

uint32_t Intersect(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
	uint32_t n = 0;
	size_t   i = 0, j = 0;
	while (i < a.size() && j < b.size()) {
		if (a[i] < b[j]) {
			i++;
		} else if (b[j] < a[i]) {
			j++;
		} else {
			n++;
			i++;
			j++;
		}
	}
	return n;
}

// Segments in runs that follow the previous frame's order; `at_least` splits the count.
void Runs(const std::vector<uint64_t>& cur, const std::vector<uint64_t>& prev,
          const std::unordered_map<uint64_t, uint32_t>& first, uint64_t* histogram, uint32_t& in8,
          uint32_t& in64) {
	int64_t  pos = -1;
	uint32_t run = 0;
	const auto close = [&] {
		if (run != 0) {
			histogram[Bucket(run)] += run;
			in8 += run >= 8 ? run : 0;
			in64 += run >= 64 ? run : 0;
		}
		run = 0;
	};
	for (const auto h: cur) {
		if (pos >= 0 && static_cast<size_t>(pos + 1) < prev.size() && prev[static_cast<size_t>(pos + 1)] == h) {
			pos++;
			run++;
			continue;
		}
		close();
		const auto found = first.find(h);
		if (found == first.end()) {
			histogram[0]++;
			pos = -1;
		} else {
			pos = found->second;
			run = 1;
		}
	}
	close();
}

std::string ThreadName(uint32_t index) {
	if (index >= MaxThreads) {
		return "?";
	}
	std::string name = g_threads[index].name;
	for (auto& ch: name) {
		if (ch == ' ' || ch == ',' || ch == ':' || ch == '=') {
			ch = '_';
		}
	}
	return name.empty() ? "t" + std::to_string(index) : name;
}

std::string Join(const std::unordered_map<std::string, uint32_t>& counts) {
	std::vector<std::pair<std::string, uint32_t>> sorted(counts.begin(), counts.end());
	std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
		return a.second != b.second ? a.second > b.second : a.first < b.first;
	});
	std::string out;
	for (const auto& [name, n]: sorted) {
		out += (out.empty() ? "" : ",") + name + ":" + std::to_string(n);
	}
	return out.empty() ? "-" : out;
}

std::string KeyName(uint32_t key) {
	char text[32];
	if ((key & 0x80000000u) != 0) {
		std::snprintf(text, sizeof(text), "op%02x.%u", (key >> 16u) & 0xffu, key & 0xffffu);
	} else {
		std::snprintf(text, sizeof(text), "%c%x", ClassName[key >> 28u], key & 0x0fffffffu);
	}
	return text;
}

std::string Top(const std::unordered_map<uint32_t, uint64_t>& counts, size_t limit) {
	std::vector<std::pair<uint32_t, uint64_t>> sorted(counts.begin(), counts.end());
	std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
		return a.second != b.second ? a.second > b.second : a.first < b.first;
	});
	std::string out;
	for (size_t i = 0; i < sorted.size() && i < limit; i++) {
		out += (out.empty() ? "" : ",") + KeyName(sorted[i].first) + ":" + std::to_string(sorted[i].second);
	}
	return out.empty() ? "-" : out;
}

std::string List(const uint64_t* values, uint32_t count) {
	std::string out;
	for (uint32_t i = 0; i < count; i++) {
		out += (i == 0 ? "" : ",") + std::to_string(values[i]);
	}
	return out;
}

void PrintSubmissions(State& s, uint64_t frame) {
	uint32_t draws = 0, dispatches = 0, packets = 0, dw = 0, spans = 0, calls = 0, with_draws = 0;
	uint32_t labels = 0, waits = 0, events = 0, e39 = 0, dma = 0, inheriting = 0, ctx_first = 0;
	std::array<uint32_t, Classes>             inherited {};
	std::vector<uint32_t>                     per_sub;
	std::unordered_map<std::string, uint32_t> by_submitter, by_builder;
	std::unordered_map<uint32_t, uint32_t>    colour_subs, depth_subs;
	for (const auto& sub: s.subs) {
		draws += sub.draws;
		dispatches += sub.dispatches;
		packets += sub.packets;
		dw += sub.dw;
		spans += sub.spans;
		calls += sub.calls;
		labels += sub.labels;
		waits += sub.waits;
		events += sub.events;
		e39 += sub.e39;
		dma += sub.dma;
		if (!sub.continued) {
			by_submitter[ThreadName(sub.thread)]++;
		}
		for (const auto& [thread, n]: sub.builders) {
			by_builder[ThreadName(thread)] += n;
		}
		if (sub.draws == 0) {
			continue;
		}
		with_draws++;
		per_sub.push_back(sub.draws);
		ctx_first += sub.first_ctx_op >= 0 ? 1u : 0u;
		bool any = false;
		for (uint32_t c = 0; c < Classes; c++) {
			inherited[c] += sub.inh_first[c];
			any = any || sub.inh_first[c] != 0;
		}
		inheriting += any ? 1u : 0u;
		for (const auto& [colour, depth]: sub.targets) {
			if (colour != 0) {
				colour_subs[colour]++;
			}
			if (depth != 0) {
				depth_subs[depth]++;
			}
		}
	}
	std::sort(per_sub.begin(), per_sub.end(), std::greater<>());
	uint32_t top3 = 0;
	for (size_t i = 0; i < per_sub.size() && i < 3; i++) {
		top3 += per_sub[i];
	}
	uint32_t targets = 0, shared = 0;
	for (const auto* map: {&colour_subs, &depth_subs}) {
		for (const auto& [address, n]: *map) {
			targets++;
			shared += n > 1 ? 1u : 0u;
		}
	}
	std::printf("STRM f=%" PRIu64 " subs=%zu dsubs=%u csubs=%u draws=%u disp=%u cdisp=%u pk=%u dw=%u"
	            " spans=%u calls=%u max_sub=%u top3=%u ctx_first=%u inh_subs=%u inh_cx=%u inh_sh=%u"
	            " inh_uc=%u inh_misc=%u labels=%u waits=%u events=%u e39=%u dma=%u targets=%u"
	            " shared_targets=%u tail_pk=%u oob=%u submitters=%s builders=%s\n",
	            frame, s.subs.size(), with_draws, s.compute_subs, draws, dispatches,
	            s.compute_dispatches, packets, dw, spans, calls, per_sub.empty() ? 0u : per_sub[0],
	            top3, ctx_first, inheriting, inherited[CX], inherited[SH], inherited[UC],
	            inherited[MISC], labels, waits, events, e39, dma, targets, shared, s.tail_packets,
	            s.out_of_range, Join(by_submitter).c_str(), Join(by_builder).c_str());
	if (frame % 400u < 40u) {
		uint32_t index = 0;
		for (const auto& sub: s.subs) {
			std::string builders, targets_text;
			for (const auto& [thread, n]: sub.builders) {
				builders += (builders.empty() ? "" : ",") + ThreadName(thread) + ":" + std::to_string(n);
			}
			for (size_t i = 0; i < sub.targets.size() && i < 6; i++) {
				char text[40];
				std::snprintf(text, sizeof(text), "%s%x/%x", i == 0 ? "" : ",", sub.targets[i].first,
				              sub.targets[i].second);
				targets_text += text;
			}
			std::printf("STRB f=%" PRIu64 " i=%u thread=%s cont=%d dw=%u pk=%u draws=%u disp=%u"
			            " spans=%u calls=%u ctx0=%d ctx_ops=%u inh1=%u/%u/%u/%u inhmax=%u/%u/%u/%u"
			            " labels=%u waits=%u events=%u e39=%u dma=%u ntargets=%zu builders=%s"
			            " targets=%s\n",
			            frame, index++, ThreadName(sub.thread).c_str(), sub.continued ? 1 : 0, sub.dw,
			            sub.packets, sub.draws, sub.dispatches, sub.spans, sub.calls, sub.first_ctx_op,
			            sub.ctx_ops, sub.inh_first[CX], sub.inh_first[SH], sub.inh_first[UC],
			            sub.inh_first[MISC], sub.inh_max[CX], sub.inh_max[SH], sub.inh_max[UC],
			            sub.inh_max[MISC], sub.labels, sub.waits, sub.events, sub.e39, sub.dma,
			            sub.targets.size(), builders.empty() ? "-" : builders.c_str(),
			            targets_text.empty() ? "-" : targets_text.c_str());
		}
	}
}

void PrintSegments(State& s, uint64_t frame) {
	const auto count = static_cast<uint32_t>(s.segments.size());
	History    now;
	std::vector<uint64_t> seq_p, seq_e;
	now.e.reserve(count);
	now.p.reserve(count);
	now.s.reserve(count);
	now.de.reserve(count);
	now.dp.reserve(count);
	uint64_t packets = 0, dwords = 0, deps = 0, written = 0;
	uint32_t draws = 0, any_e = 0, any_p = 0, any_dp = 0;
	for (const auto& seg: s.segments) {
		now.e.push_back(seg.e);
		now.p.push_back(seg.p);
		now.s.push_back(seg.s);
		now.de.push_back(seg.de);
		now.dp.push_back(seg.dp);
		packets += seg.packets;
		dwords += seg.dwords;
		deps += seg.deps;
		written += seg.written;
		draws += seg.draw ? 1u : 0u;
		any_e += s.any_e.count(seg.e) != 0 ? 1u : 0u;
		any_p += s.any_p.count(seg.p) != 0 ? 1u : 0u;
		any_dp += s.any_dp.count(seg.dp) != 0 ? 1u : 0u;
	}
	seq_p = now.p;
	seq_e = now.e;
	for (auto* v: {&now.e, &now.p, &now.s, &now.de, &now.dp}) {
		std::sort(v->begin(), v->end());
	}

	uint32_t match[4][5] = {};
	for (size_t b = 0; b < Backs.size(); b++) {
		if (s.frames < Backs[b]) {
			continue;
		}
		const auto& old = s.ring[(s.frames - Backs[b]) % Ring];
		match[b][0]     = Intersect(now.e, old.e);
		match[b][1]     = Intersect(now.p, old.p);
		match[b][2]     = Intersect(now.s, old.s);
		match[b][3]     = Intersect(now.de, old.de);
		match[b][4]     = Intersect(now.dp, old.dp);
	}

	uint32_t run_p8 = 0, run_p64 = 0, run_e8 = 0, run_e64 = 0;
	std::vector<uint64_t> prev_p, prev_e;
	prev_p.reserve(s.prev_segments.size());
	prev_e.reserve(s.prev_segments.size());
	for (const auto& seg: s.prev_segments) {
		prev_p.push_back(seg.p);
		prev_e.push_back(seg.e);
	}
	Runs(seq_p, prev_p, s.prev_first_p, s.run_p, run_p8, run_p64);
	Runs(seq_e, prev_e, s.prev_first_e, s.run_e, run_e8, run_e64);

	// Each segment against the previous frame's segment of the same shape, in order.
	uint32_t paired = 0, same = 0;
	uint64_t differing = 0, compared = 0;
	std::unordered_map<uint64_t, uint32_t> cursor;
	for (const auto& seg: s.segments) {
		const auto found = s.prev_by_shape.find(seg.s);
		if (found == s.prev_by_shape.end()) {
			continue;
		}
		auto& next = cursor[seg.s];
		if (next >= found->second.size()) {
			continue;
		}
		const auto& old = s.prev_segments[found->second[next++]];
		if (old.kv_count != seg.kv_count) {
			continue;
		}
		uint32_t n = 0;
		for (uint32_t i = 0; i < seg.kv_count; i++) {
			if (s.kv_vals[seg.kv_begin + i] != s.prev_kv_vals[old.kv_begin + i]) {
				n++;
				s.differing[s.kv_keys[seg.kv_begin + i]]++;
			}
		}
		paired++;
		same += n == 0 ? 1u : 0u;
		differing += n;
		compared += seg.kv_count;
		s.ndiff[Bucket(n)]++;
	}
	s.differing_pairs += paired;

	uint32_t span_same = 0, span_content = 0;
	for (const auto& span: s.spans) {
		const auto found = s.prev_spans.find(span.address);
		if (found != s.prev_spans.end() && found->second.first == span.hash &&
		    found->second.second == span.dw) {
			span_same += span.units;
		}
		if (s.prev_span_hashes.count(span.hash) != 0) {
			span_content += span.units;
		}
	}

	const double n = count != 0 ? count : 1;
	std::printf("STRA f=%" PRIu64 " units=%u draws=%u", frame, count, draws);
	for (size_t b = 0; b < Backs.size(); b++) {
		std::printf(" E%u=%u P%u=%u S%u=%u DE%u=%u DP%u=%u", Backs[b], match[b][0], Backs[b],
		            match[b][1], Backs[b], match[b][2], Backs[b], match[b][3], Backs[b], match[b][4]);
	}
	std::printf(" Eany=%u Pany=%u DPany=%u runP8=%u runP64=%u runE8=%u runE64=%u span_same=%u"
	            " span_content=%u pk_seg=%.2f dw_seg=%.1f wr_seg=%.1f deps=%.1f paired=%u"
	            " pair_same=%u pair_diff=%.2f pair_vals=%.1f\n",
	            any_e, any_p, any_dp, run_p8, run_p64, run_e8, run_e64, span_same, span_content,
	            static_cast<double>(packets) / n, static_cast<double>(dwords) / n,
	            static_cast<double>(written) / n, static_cast<double>(deps) / n, paired, same,
	            paired != 0 ? static_cast<double>(differing) / paired : 0.0,
	            paired != 0 ? static_cast<double>(compared) / paired : 0.0);

	// Rotate the history.
	if (s.frames >= AnyFrames) {
		const auto& old = s.ring[(s.frames - AnyFrames) % Ring];
		const auto drop = [](std::unordered_map<uint64_t, uint32_t>& map, const std::vector<uint64_t>& v) {
			for (const auto h: v) {
				const auto found = map.find(h);
				if (found != map.end() && --found->second == 0) {
					map.erase(found);
				}
			}
		};
		drop(s.any_e, old.e);
		drop(s.any_p, old.p);
		drop(s.any_dp, old.dp);
	}
	for (const auto h: now.e) {
		s.any_e[h]++;
	}
	for (const auto h: now.p) {
		s.any_p[h]++;
	}
	for (const auto h: now.dp) {
		s.any_dp[h]++;
	}
	s.ring[s.frames % Ring] = std::move(now);
	s.frames++;

	s.prev_by_shape.clear();
	s.prev_first_p.clear();
	s.prev_first_e.clear();
	for (uint32_t i = 0; i < count; i++) {
		const auto& seg = s.segments[i];
		s.prev_by_shape[seg.s].push_back(i);
		s.prev_first_p.emplace(seg.p, i);
		s.prev_first_e.emplace(seg.e, i);
	}
	s.prev_spans.clear();
	s.prev_span_hashes.clear();
	for (const auto& span: s.spans) {
		s.prev_spans[span.address] = {span.hash, span.dw};
		s.prev_span_hashes.insert(span.hash);
	}
	s.prev_segments.swap(s.segments);
	s.prev_kv_vals.swap(s.kv_vals);
	s.segments.clear();
	s.kv_vals.clear();
	s.kv_keys.clear();
	s.seg_kv_begin = 0;
}

void PrintPeriod(State& s, uint64_t frame) {
	const auto threads = std::min(g_thread_count.load(std::memory_order_relaxed), MaxThreads);
	const auto funcs   = std::min(g_func_count.load(std::memory_order_relaxed), MaxFuncs);
	std::vector<uint64_t> by_func(funcs, 0);
	for (uint32_t t = 0; t < threads; t++) {
		uint64_t total = 0;
		std::vector<std::pair<uint32_t, uint32_t>> top;
		for (uint32_t f = 0; f < funcs; f++) {
			const auto now   = g_threads[t].calls[f].load(std::memory_order_relaxed);
			auto&      last  = s.last_calls[static_cast<size_t>(t) * MaxFuncs + f];
			const auto delta = now - last;
			last             = now;
			if (delta != 0) {
				total += delta;
				by_func[f] += delta;
				top.emplace_back(delta, f);
			}
		}
		uint32_t submits[2];
		for (uint32_t q = 0; q < 2; q++) {
			const auto now = g_threads[t].submits[q].load(std::memory_order_relaxed);
			submits[q]     = now - s.last_submits[t][q];
			s.last_submits[t][q] = now;
		}
		if (total == 0 && submits[0] == 0 && submits[1] == 0) {
			continue;
		}
		std::sort(top.begin(), top.end(), std::greater<>());
		std::string text;
		for (size_t i = 0; i < top.size() && i < 8; i++) {
			text += (text.empty() ? "" : ",") + std::string(g_func_names[top[i].second]) + ":" +
			        std::to_string(top[i].first);
		}
		std::printf("STRH f=%" PRIu64 " thread=%s calls=%" PRIu64 " dcb_submits=%u acb_submits=%u"
		            " top=%s\n",
		            frame, ThreadName(t).c_str(), total, submits[1], submits[0],
		            text.empty() ? "-" : text.c_str());
	}
	std::string text;
	for (uint32_t f = 0; f < funcs; f++) {
		if (by_func[f] != 0) {
			text += (text.empty() ? "" : ",") + std::string(g_func_names[f]) + ":" + std::to_string(by_func[f]);
		}
	}
	std::printf("STRF f=%" PRIu64 " %s\n", frame, text.empty() ? "-" : text.c_str());
	for (uint32_t q = 0; q < 2; q++) {
		text.clear();
		for (uint32_t op = 0; op < 256; op++) {
			if (s.opcodes[q][op] != 0) {
				char item[40];
				std::snprintf(item, sizeof(item), "%s%02x:%" PRIu64, text.empty() ? "" : ",", op,
				              s.opcodes[q][op]);
				text += item;
			}
		}
		std::printf("STRO f=%" PRIu64 " queue=%s %s\n", frame, q == 1 ? "graphics" : "compute",
		            text.empty() ? "-" : text.c_str());
	}
	std::printf("STRI f=%" PRIu64 " subs=%" PRIu64 " %s\n", frame, s.inherited_subs,
	            Top(s.inherited, 48).c_str());
	std::printf("STRD f=%" PRIu64 " pairs=%" PRIu64 " %s\n", frame, s.differing_pairs,
	            Top(s.differing, 48).c_str());
	text.clear();
	for (uint32_t type = 0; type < 64; type++) {
		if (s.event_types[type] != 0) {
			text += (text.empty() ? "" : ",") + std::to_string(type) + ":" + std::to_string(s.event_types[type]);
		}
	}
	std::printf("STRL f=%" PRIu64 " runP=%s runE=%s ndiff=%s seg_pk=%s seg_dw8=%s events=%s\n", frame,
	            List(s.run_p, Buckets).c_str(), List(s.run_e, Buckets).c_str(),
	            List(s.ndiff, Buckets).c_str(), List(s.seg_pk, Buckets).c_str(),
	            List(s.seg_dw, Buckets).c_str(), text.empty() ? "-" : text.c_str());
	std::memset(s.opcodes, 0, sizeof(s.opcodes));
	s.inherited.clear();
	s.differing.clear();
	s.inherited_subs = s.differing_pairs = 0;
	std::memset(s.run_p, 0, sizeof(s.run_p));
	std::memset(s.run_e, 0, sizeof(s.run_e));
	std::memset(s.ndiff, 0, sizeof(s.ndiff));
	std::memset(s.seg_pk, 0, sizeof(s.seg_pk));
	std::memset(s.seg_dw, 0, sizeof(s.seg_dw));
	std::memset(s.event_types, 0, sizeof(s.event_types));
}

} // namespace

uint32_t RegisterHle(const char* name) {
	std::scoped_lock lock(g_func_mutex);
	const auto count = g_func_count.load(std::memory_order_relaxed);
	for (uint32_t i = 0; i < count; i++) {
		if (std::strcmp(g_func_names[i], name) == 0) {
			return i;
		}
	}
	if (count >= MaxFuncs) {
		return MaxFuncs - 1;
	}
	g_func_names[count] = name;
	g_func_count.store(count + 1, std::memory_order_relaxed);
	return count;
}

void OnHle(uint32_t id) noexcept {
	Bump(g_threads[ThisThread()].calls[id]);
}

void OnAllocate(const uint32_t* cmd) noexcept {
	const auto page = reinterpret_cast<uint64_t>(cmd) >> 12u;
	g_chunks[ChunkSlot(page)].store((page << 8u) | (ThisThread() & 0xffu), std::memory_order_relaxed);
}

void OnSubmit(const uint32_t* commands, uint32_t dw, bool graphics) {
	const auto thread = ThisThread();
	Bump(g_threads[thread].submits[graphics ? 1 : 0]);
	std::scoped_lock lock(g_submit_mutex);
	g_submits[graphics ? 1 : 0].push_back({commands, dw, thread});
}

void OnSubmissionBegin(bool graphics, const uint32_t* commands, uint32_t dw) {
	auto&    s      = Cmd();
	uint32_t thread = UINT32_MAX;
	{
		std::scoped_lock lock(g_submit_mutex);
		auto&            queue = g_submits[graphics ? 1 : 0];
		const auto       found = std::find_if(queue.begin(), queue.end(), [&](const auto& r) {
			return r.commands == commands && r.dw == dw;
		});
		if (found != queue.end()) {
			thread = found->thread;
			queue.erase(found);
		}
	}
	if (!graphics) {
		s.compute_subs++;
		return;
	}
	FinishSubmission(s);
	s.tail_packets += s.seg_packets;
	s.kv_keys.resize(s.seg_kv_begin);
	s.kv_vals.resize(s.seg_kv_begin);
	s.cur_sub++;
	for (auto& r: s.reg) {
		r.sub_written = 0;
	}
	BeginSegment(s);
	s.sub        = {};
	s.sub.thread = thread;
	s.sub_open   = true;
	s.next_packet = nullptr;
	s.marker_stack.clear();
}

void OnPacket(bool graphics, const uint32_t* packet, uint32_t packet_dw) {
	auto&          s      = Cmd();
	const uint32_t header = packet[0];
	const uint32_t opcode = (header >> 8u) & 0xffu;
	s.opcodes[graphics ? 1 : 0][opcode]++;
	if (!graphics) {
		s.compute_packets++;
		if (opcode == Pm4::IT_DISPATCH_DIRECT || opcode == Pm4::IT_DISPATCH_INDIRECT) {
			s.compute_dispatches++;
		}
		return;
	}
	if (!s.sub_open) {
		return;
	}
	if (packet != s.next_packet || !s.span_open) {
		FinishSpan(s);
		s.span         = {};
		s.span.address = reinterpret_cast<uint64_t>(packet);
		s.span_open    = true;
		s.sub.spans++;
	}
	s.next_packet = packet + packet_dw;
	for (uint32_t i = 0; i < packet_dw; i++) {
		Seq(s.span.hash, packet[i]);
	}
	s.span.dw += packet_dw;
	s.sub.packets++;
	s.sub.dw += packet_dw;
	s.seg_packets++;
	s.seg_dwords += packet_dw;
	Seq(s.seg_s, 0x40000000u | header);
	Seq(s.seg_e, 0x40000000u | header);
	Seq(s.seg_p, 0x40000000u | header);

	const uint32_t payload_dw = packet_dw - 1u;
	switch (opcode) {
		case Pm4::IT_SET_CONTEXT_REG: {
			const auto offset = packet[1] & ~0x70000000u;
			if (offset != Pm4::CX_NOP) {
				for (uint32_t i = 1; i < payload_dw; i++) {
					Write(s, CX, offset + i - 1u, packet[1u + i]);
				}
			}
			break;
		}
		case Pm4::IT_SET_SH_REG: {
			const auto offset = packet[1];
			if (offset != Pm4::SH_NOP) {
				for (uint32_t i = 1; i < payload_dw; i++) {
					Write(s, SH, offset + i - 1u, packet[1u + i]);
				}
			}
			break;
		}
		case Pm4::IT_SET_UCONFIG_REG:
		case Pm4::IT_SET_UCONFIG_REG_INDEX: {
			// The driver's internal data packets (waits, semaphores) are not register writes.
			if (opcode == Pm4::IT_SET_UCONFIG_REG && KYTY_PM4_R(header) == 1u) {
				break;
			}
			const auto offset = opcode == Pm4::IT_SET_UCONFIG_REG_INDEX ? packet[1] & 0x0fffffffu
			                                                           : packet[1] & ~0x70000000u;
			if (offset != Pm4::UC_NOP) {
				for (uint32_t i = 1; i < payload_dw; i++) {
					Write(s, UC, offset + i - 1u, packet[1u + i]);
				}
			}
			break;
		}
		case Pm4::IT_SET_CONTEXT_REG_INDIRECT:
			if (packet_dw == 5u) {
				IndirectRegisters(s, CX, packet, Pm4::CX_NOP);
			}
			break;
		case Pm4::IT_SET_SH_REG_INDIRECT:
			if (packet_dw == 5u) {
				IndirectRegisters(s, SH, packet, Pm4::SH_NOP);
			}
			break;
		case Pm4::IT_SET_UCONFIG_REG_INDIRECT:
			if (packet_dw == 5u) {
				IndirectRegisters(s, UC, packet, Pm4::UC_NOP);
			}
			break;
		case Pm4::IT_NOP:
			if (KYTY_PM4_R(header) == Pm4::R_CONTEXT_STATE && packet_dw >= 2u) {
				Payload(s, opcode, 0, packet[1], true);
				ContextOp(s, packet[1]);
			} else if (KYTY_PM4_R(header) == Pm4::R_PUSH_MARKER && packet_dw >= 2u) {
				PushMarker(s, packet, packet_dw);
			} else if (KYTY_PM4_R(header) == Pm4::R_POP_MARKER && !s.marker_stack.empty()) {
				s.marker_stack.pop_back();
			}
			break;
		case Pm4::IT_CLEAR_STATE: ContextOp(s, 0); break;
		case Pm4::IT_INDEX_TYPE: Write(s, MISC, MiscIndexType, packet[1]); break;
		case Pm4::IT_INDEX_BASE:
			Write(s, MISC, MiscIndexBaseLo, packet[1]);
			Write(s, MISC, MiscIndexBaseHi, packet[2]);
			break;
		case Pm4::IT_INDEX_BUFFER_SIZE: Write(s, MISC, MiscIndexBufferSize, packet[1]); break;
		case Pm4::IT_NUM_INSTANCES: Write(s, MISC, MiscNumInstances, packet[1]); break;
		case Pm4::IT_SET_BASE:
			if (packet_dw == 4u) {
				const auto base = ((header >> 1u) & 0x3u) == 0u ? MiscDrawArgsBaseLo : MiscDispatchArgsBaseLo;
				Write(s, MISC, base, packet[2]);
				Write(s, MISC, base + 1u, packet[3]);
			}
			break;
		case Pm4::IT_INDIRECT_BUFFER: s.sub.calls++; break;
		case Pm4::IT_EVENT_WRITE: {
			const auto type = packet[1] & 0x3fu;
			s.sub.events++;
			s.sub.e39 += type == 0x39u ? 1u : 0u;
			s.event_types[type]++;
			MarkerStats(s).events++;
			MarkerStats(s).e39 += type == 0x39u ? 1u : 0u;
			Payload(s, opcode, 0, packet[1], true);
			for (uint32_t i = 1; i < payload_dw; i++) {
				Payload(s, opcode, i, packet[1u + i], false);
			}
			break;
		}
		default: {
			switch (opcode) {
				case Pm4::IT_RELEASE_MEM:
				case Pm4::IT_WRITE_DATA: s.sub.labels++; break;
				case Pm4::IT_WAIT_REG_MEM:
				case Pm4::IT_WAIT_REG_MEM_64:
				case Pm4::IT_MEM_SEMAPHORE:
				case Pm4::IT_REWIND: s.sub.waits++; break;
				case Pm4::IT_DMA_DATA:
				case Pm4::IT_COPY_DATA: s.sub.dma++; break;
				default: break;
			}
			const bool draw     = IsDraw(opcode);
			const bool dispatch = opcode == Pm4::IT_DISPATCH_DIRECT || opcode == Pm4::IT_DISPATCH_INDIRECT;
			for (uint32_t i = 0; i < payload_dw && i < 24u; i++) {
				Payload(s, opcode, i, packet[1u + i], false);
			}
			if (draw || dispatch) {
				EndUnit(s, packet, opcode, draw);
			}
			break;
		}
	}
}

void OnProcessorReset(bool graphics) {
	if (!graphics) {
		return;
	}
	auto& s = Cmd();
	for (uint32_t c = 0; c < Classes; c++) {
		ClearClass(s, c);
	}
	s.saved = false;
}

void OnGuestFlip(uint64_t frame) {
	auto& s = Cmd();
	// The flip packet's submission stays open: what follows the flip counts for the next frame.
	const bool       open   = s.sub_open;
	const auto       thread = s.sub.thread;
	FinishSubmission(s);
	PrintSubmissions(s, frame);
	PrintSegments(s, frame);
	{
		// Units by marker path, the largest first.
		std::vector<std::pair<uint32_t, uint32_t>> order;
		for (uint32_t id = 0; id < s.marker_stats.size(); id++) {
			const auto& m = s.marker_stats[id];
			if (m.draws + m.dispatches + m.events != 0) {
				order.emplace_back(m.draws + m.dispatches, id);
			}
		}
		std::sort(order.begin(), order.end(), std::greater<>());
		std::string text;
		for (size_t i = 0; i < order.size() && i < 40; i++) {
			const auto& m = s.marker_stats[order[i].second];
			char        item[160];
			std::snprintf(item, sizeof(item), "%s%s=%u/%u/%u/%u", i == 0 ? "" : " ",
			              s.marker_paths[order[i].second].c_str(), m.draws, m.dispatches, m.events, m.e39);
			text += item;
		}
		std::printf("STRK f=%" PRIu64 " paths=%zu %s\n", frame, order.size(), text.empty() ? "-" : text.c_str());
		s.marker_stats.clear();
	}
	s.subs.clear();
	s.spans.clear();
	s.compute_subs = s.compute_packets = s.compute_dispatches = 0;
	s.tail_packets = s.out_of_range = 0;
	if (open) {
		s.sub           = {};
		s.sub.thread    = thread;
		s.sub.continued = true;
		s.sub.has_first = true;
		s.sub_open      = true;
		s.next_packet   = nullptr;
	}
	if (frame % 100u == 0u) {
		PrintPeriod(s, frame);
	}
	std::fflush(stdout);
}

} // namespace Libs::Graphics::StreamDiag
