#ifndef KYTY_GRAPHICS_GUEST_GPU_PASS_DIAG_H_
#define KYTY_GRAPHICS_GUEST_GPU_PASS_DIAG_H_

// KYTY_PASS_DIAG=1: per guest frame, the draws grouped by render-target set (colour 0 address,
// format and size, depth address and size, target mask), with per group: draws by kind, queue
// drains and their reasons, the distinct (VS, PS) shader pairs and screen scissors, and the
// draw sequence range; plus the frame's graphics submissions by guest thread. Diagnostic only:
// draws and drains are noted on the command thread (not with KYTY_DECODE_THREAD), submissions on
// the submitting threads. Debug markers (NOP push/pop) name the passes. Summary: passdiag.py.
// Lines, printed at the guest flip, keyed by g= (this diagnostic's own guest-frame sequence; f= is
// the presented-flip count, which can repeat): "PASS g=..." per frame, "PASST g=..." per target set (with
// EVENT_WRITE types, depth/stencil state and DB_COUNT_CONTROL, the packets before its first draws
// and its debug markers) and "PASSM g=..." per debug-marker path.

#include "graphics/guest_gpu/hardwareContext.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <pthread.h>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace Libs::Graphics::PassDiag {

inline bool Enabled() {
	static const bool enabled = [] {
		const char* v = std::getenv("KYTY_PASS_DIAG");
		return v != nullptr && v[0] == '1';
	}();
	return enabled;
}

namespace Detail {

struct Key {
	uint64_t c0 = 0, z = 0;
	uint32_t fmt = 0, w = 0, h = 0, zfmt = 0, zw = 0, zh = 0, mask = 0;
	bool     operator<(const Key& o) const {
		return std::tie(c0, z, fmt, w, h, zfmt, zw, zh, mask) <
		       std::tie(o.c0, o.z, o.fmt, o.w, o.h, o.zfmt, o.zw, o.zh, o.mask);
	}
};

struct Group {
	uint32_t                                       draws = 0, indexed = 0, automatic = 0, indirect = 0;
	uint32_t                                       drains = 0;
	uint32_t                                       first = 0, last = 0;
	std::set<std::pair<uint64_t, uint64_t>>        shaders;
	std::set<std::tuple<int, int, int, int>>       scissors;
	std::map<uint32_t, uint32_t>                   reasons;
	std::map<uint32_t, uint32_t>                   events;   // EVENT_WRITE type -> count
	std::set<std::tuple<int, int, int, uint32_t>>  ds;       // z_enable, z_write, stencil, DB_COUNT_CONTROL
	std::vector<std::string>                       between;  // packets before a draw (first 3 draws)
	std::map<std::string, uint32_t>                markers;  // innermost debug marker -> draws
};

struct State {
	std::map<Key, Group>         groups;
	std::map<uint32_t, uint32_t> reasons;
	const Group*                 current = nullptr;
	Key                          current_key;
	bool                         have_current = false;
	uint32_t                     seq = 0, drains = 0;
	uint64_t                     guest_frames = 0; // PASS g=: one per guest flip, never repeats
	std::map<uint32_t, uint32_t> events;
	std::string                  packets;
	uint32_t                     count_control = 0;
	std::vector<std::string>     marker_stack;
	std::map<std::string, std::pair<uint32_t, uint32_t>> marker_draws; // marker path -> draws, drains
};

inline State& Cmd() {
	static State s;
	return s;
}

struct Submits {
	std::mutex                      mutex;
	std::map<std::string, uint32_t> by_thread;
};

inline Submits& Sub() {
	static Submits s;
	return s;
}

} // namespace Detail

// kind: 0 indexed, 1 auto, 2 indirect.
inline void OnDraw(const HW::Context& ctx, const HW::Shader& sh, int kind) {
	auto&       s  = Detail::Cmd();
	const auto& rt = ctx.GetRenderTarget(0);
	const auto& db = ctx.GetDepthRenderTarget();
	Detail::Key key;
	key.c0   = rt.base.addr;
	key.fmt  = static_cast<uint32_t>(rt.info.format);
	key.w    = rt.base.addr != 0 ? rt.attrib2.width + 1 : 0;
	key.h    = rt.base.addr != 0 ? rt.attrib2.height + 1 : 0;
	key.z    = db.z_write_base_addr != 0 ? db.z_write_base_addr : db.z_read_base_addr;
	key.zfmt = static_cast<uint32_t>(db.z_info.format);
	key.zw   = key.z != 0 ? db.size.x_max + 1u : 0;
	key.zh   = key.z != 0 ? db.size.y_max + 1u : 0;
	key.mask = ctx.GetRenderTargetMask();
	auto& g  = s.groups[key];
	if (g.draws == 0) {
		g.first = s.seq;
	}
	g.last = s.seq++;
	g.draws++;
	(kind == 0 ? g.indexed : kind == 1 ? g.automatic : g.indirect)++;
	const auto& vs = sh.GetVs();
	g.shaders.emplace(vs.gs_regs.data_addr ^ (vs.es_regs.data_addr << 1u), sh.GetPs().ps_regs.data_addr);
	if (g.scissors.size() < 64) {
		const auto& sv = ctx.GetScreenViewport();
		g.scissors.emplace(sv.screen_scissor_left, sv.screen_scissor_top, sv.screen_scissor_right,
		                   sv.screen_scissor_bottom);
	}
	const auto& dc = ctx.GetDepthControl();
	if (g.ds.size() < 16) {
		g.ds.emplace(dc.z_enable, dc.z_write_enable, dc.stencil_enable, s.count_control);
	}
	if (g.between.size() < 3) {
		g.between.push_back(s.packets.empty() ? "-" : s.packets);
	}
	const std::string marker = s.marker_stack.empty() ? "-" : s.marker_stack.back();
	if (g.markers.size() < 6 || g.markers.contains(marker)) {
		g.markers[marker]++;
	}
	std::string path;
	for (size_t i = s.marker_stack.size() > 3 ? s.marker_stack.size() - 3 : 0; i < s.marker_stack.size(); i++) {
		path += (path.empty() ? "" : ">") + s.marker_stack[i];
	}
	s.marker_draws[path.empty() ? "-" : path].first++;
	s.packets.clear();
	s.current_key  = key;
	s.have_current = true;
}

// Every packet the command thread runs (opcode), and EVENT_WRITE's type.
inline void OnPacket(uint32_t opcode) {
	auto& s = Detail::Cmd();
	if (s.packets.size() < 200) {
		char b[8];
		std::snprintf(b, sizeof(b), "%s%02x", s.packets.empty() ? "" : ".", opcode);
		s.packets += b;
	}
}

inline void OnEvent(uint32_t type) {
	auto& s = Detail::Cmd();
	s.events[type]++;
	if (s.packets.size() < 200) {
		char b[8];
		std::snprintf(b, sizeof(b), "/%02x", type);
		s.packets += b;
	}
	if (s.have_current) {
		s.groups[s.current_key].events[type]++;
	}
}

inline void SetCountControl(uint32_t value) { Detail::Cmd().count_control = value; }

// A debug marker's payload: the first run of 3+ printable characters.
inline void PushMarker(const uint32_t* payload, uint32_t dwords) {
	auto&       s     = Detail::Cmd();
	const auto* bytes = reinterpret_cast<const char*>(payload);
	std::string name, run;
	for (uint32_t i = 0; i < dwords * 4u && name.empty(); i++) {
		const char c = bytes[i];
		if (c >= 0x20 && c < 0x7f && c != ' ') {
			run += c;
		} else {
			if (run.size() >= 3) {
				name = run;
			}
			run.clear();
		}
	}
	if (name.empty()) {
		name = run.size() >= 3 ? run : "?";
	}
	if (name.size() > 48) {
		name.resize(48);
	}
	if (s.marker_stack.size() < 32) {
		s.marker_stack.push_back(name);
	}
}

inline void PopMarker() {
	auto& s = Detail::Cmd();
	if (!s.marker_stack.empty()) {
		s.marker_stack.pop_back();
	}
}

inline void OnDrain(uint32_t reason) {
	auto& s = Detail::Cmd();
	s.drains++;
	s.reasons[reason]++;
	{
		std::string path;
		for (size_t i = s.marker_stack.size() > 3 ? s.marker_stack.size() - 3 : 0; i < s.marker_stack.size(); i++) {
			path += (path.empty() ? "" : ">") + s.marker_stack[i];
		}
		s.marker_draws[path.empty() ? "-" : path].second++;
	}
	if (s.have_current) {
		auto& g = s.groups[s.current_key];
		g.drains++;
		g.reasons[reason]++;
	}
}

inline void OnSubmit() {
	char name[32] = {};
	pthread_getname_np(pthread_self(), name, sizeof(name));
	auto&            sub = Detail::Sub();
	std::scoped_lock lock(sub.mutex);
	sub.by_thread[name]++;
}

inline std::string Reasons(const std::map<uint32_t, uint32_t>& reasons) {
	std::string out;
	for (const auto& [r, n] : reasons) {
		char b[48];
		std::snprintf(b, sizeof(b), "%s%s%02x:%u", out.empty() ? "" : ",",
		              r >= 512 ? "nop" : r >= 256 ? "site" : "op", r & 0xffu, n);
		out += b;
	}
	return out.empty() ? "-" : out;
}

inline std::string Events(const std::map<uint32_t, uint32_t>& events) {
	std::string out;
	for (const auto& [e, n] : events) {
		out += (out.empty() ? "" : ",") + std::to_string(e) + ":" + std::to_string(n);
	}
	return out.empty() ? "-" : out;
}

inline void OnGuestFlip(uint64_t frame) {
	auto&                           s = Detail::Cmd();
	std::map<std::string, uint32_t> threads;
	{
		auto&            sub = Detail::Sub();
		std::scoped_lock lock(sub.mutex);
		threads.swap(sub.by_thread);
	}
	std::string t;
	uint32_t    submits = 0;
	for (const auto& [name, n] : threads) {
		t += (t.empty() ? "" : ",") + name + ":" + std::to_string(n);
		submits += n;
	}
	std::printf("PASS g=%" PRIu64 " f=%" PRIu64 " draws=%u targets=%zu drains=%u submits=%u threads=%s reasons=%s"
	            " events=%s\n",
	            s.guest_frames, frame, s.seq, s.groups.size(), s.drains, submits, t.empty() ? "-" : t.c_str(),
	            Reasons(s.reasons).c_str(), Events(s.events).c_str());
	for (const auto& [k, g] : s.groups) {
		const auto& sc = *g.scissors.begin();
		std::string ds, between, markers;
		for (const auto& [m, n] : g.markers) {
			markers += (markers.empty() ? "" : ",") + m + ":" + std::to_string(n);
		}
		for (const auto& [ze, zw, st, cc] : g.ds) {
			char b[48];
			std::snprintf(b, sizeof(b), "%sz%dw%ds%d:cc%x", ds.empty() ? "" : ",", ze, zw, st, cc);
			ds += b;
		}
		for (const auto& p : g.between) {
			between += (between.empty() ? "" : "|") + p;
		}
		std::printf("PASST g=%" PRIu64 " f=%" PRIu64 " c0=0x%" PRIx64 " fmt=%u w=%u h=%u z=0x%" PRIx64
		            " zfmt=%u zw=%u zh=%u mask=0x%x draws=%u idx=%u auto=%u ind=%u drains=%u"
		            " seq=%u-%u shaders=%zu scissors=%zu s0=%d,%d,%d,%d reasons=%s events=%s ds=%s"
		            " between=%s markers=%s\n",
		            s.guest_frames, frame, k.c0, k.fmt, k.w, k.h, k.z, k.zfmt, k.zw, k.zh, k.mask, g.draws, g.indexed,
		            g.automatic, g.indirect, g.drains, g.first, g.last, g.shaders.size(),
		            g.scissors.size(), std::get<0>(sc), std::get<1>(sc), std::get<2>(sc),
		            std::get<3>(sc), Reasons(g.reasons).c_str(), Events(g.events).c_str(), ds.c_str(),
		            between.c_str(), markers.c_str());
	}
	for (const auto& [m, dd] : s.marker_draws) {
		std::printf("PASSM g=%" PRIu64 " f=%" PRIu64 " draws=%u drains=%u marker=%s\n", s.guest_frames, frame,
		            dd.first, dd.second, m.c_str());
	}
	s.marker_draws.clear();
	s.groups.clear();
	s.reasons.clear();
	s.events.clear();
	s.have_current = false;
	s.seq = s.drains = 0;
	s.guest_frames++;
}

} // namespace Libs::Graphics::PassDiag

#endif /* KYTY_GRAPHICS_GUEST_GPU_PASS_DIAG_H_ */
