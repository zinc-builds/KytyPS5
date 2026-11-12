#pragma once
// KYTY_TEXBURST_DIAG=1 (diagnostic, not for merge): where a texture-upload burst's time goes, and
// how long before its first upload the guest wrote a texture's bytes. One TEXUP line per guest
// flip (f= is the same flip number as FrameDiag's GFRAME), for the uploads since the last one:
//   ups/reups       images uploaded for the first time / again (same address and size, CPU-dirty)
//   guest_b         guest bytes of those images; sync_b / stage_b: bytes the CPU copied through
//                   the buffer cache (SynchronizeBuffer) / a one-off staging upload
//   *_ns            command-thread time: find (FindImage, includes the rest), insert (image
//                   creation), obtain (guest bytes to a GPU buffer, the CPU copy), upload
//                   (detile and copy recording)
//   size_hist       first uploads by guest size: <64K, <1M, <4M, <16M, >=16M
//   lead_hist       first uploads by the time since a guest file read last wrote the image's
//                   bytes (APR reads, read/pread/preadv): <17ms, <50, <100, <500, <2000, >=2000,
//                   none (under half the bytes came from file reads)
// Guest threads report file reads; everything else is the command thread.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_map>

namespace Common::TexBurstDiag {

[[nodiscard]] inline bool Enabled() noexcept {
	static const bool on = [] {
		const char* value = std::getenv("KYTY_TEXBURST_DIAG");
		return value != nullptr && value[0] == '1';
	}();
	return on;
}

inline uint64_t NowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

enum Time : int { FindNs, InsertNs, ObtainNs, UploadNs, TimeCount };

struct Frame {
	uint64_t                     ups = 0, reups = 0, guest_b = 0, sync_b = 0, stage_b = 0;
	uint64_t                     detile_direct = 0, detile_linear = 0;
	std::array<uint64_t, TimeCount> ns {};
	std::array<uint64_t, 5>      size_hist {};
	std::array<uint64_t, 7>      lead_hist {};
	uint64_t                     lead_b[7] {}; // guest bytes by lead bucket
	std::array<uint64_t, 7>      reup_lead_hist {}; // the same for re-uploads
	uint64_t                     reup_lead_b[7] {};
	uint64_t                     file_b = 0;   // guest file bytes read since the last flip
};

struct Write {
	uint64_t end;
	uint64_t ns;
};

struct State {
	std::mutex                         write_mutex;
	std::map<uint64_t, Write>          writes; // disjoint ranges, latest write wins
	std::atomic<uint64_t>              file_b {0};
	Frame                              frame;  // command thread
	std::unordered_map<uint64_t, uint32_t> uploaded; // address ^ size << 40 -> uploads
};

inline State& Get() {
	static State state;
	return state;
}

// A guest file read wrote [vaddr, vaddr + size) (any guest thread).
inline void NoteFileWrite(uint64_t vaddr, uint64_t size) {
	if (!Enabled() || size == 0) {
		return;
	}
	auto&      s   = Get();
	const auto now = NowNs();
	const auto end = vaddr + size;
	s.file_b.fetch_add(size, std::memory_order_relaxed);
	std::lock_guard lock(s.write_mutex);
	auto            it = s.writes.lower_bound(vaddr);
	if (it != s.writes.begin()) {
		auto prev = std::prev(it);
		if (prev->second.end > vaddr) {
			const auto tail = prev->second;
			prev->second.end = vaddr;
			if (tail.end > end) {
				s.writes.emplace(end, Write {tail.end, tail.ns});
			}
		}
	}
	while (it != s.writes.end() && it->first < end) {
		if (it->second.end > end) {
			const auto tail = it->second;
			it = s.writes.erase(it);
			s.writes.emplace(end, tail);
			break;
		}
		it = s.writes.erase(it);
	}
	s.writes.emplace(vaddr, Write {end, now});
	if (s.writes.size() > 4'000'000) {
		s.writes.clear(); // a diagnostic: bounded memory over correctness of old entries
	}
}

// Latest file write over the bytes it covers in [vaddr, vaddr + size), and the bytes covered.
inline std::pair<uint64_t, uint64_t> LatestFileWrite(uint64_t vaddr, uint64_t size) {
	auto&           s   = Get();
	const auto      end = vaddr + size;
	uint64_t        latest = 0, covered = 0;
	std::lock_guard lock(s.write_mutex);
	auto            it = s.writes.upper_bound(vaddr);
	if (it != s.writes.begin()) {
		--it;
	}
	for (; it != s.writes.end() && it->first < end; ++it) {
		const auto b = std::max(vaddr, it->first);
		const auto e = std::min(end, it->second.end);
		if (b < e) {
			covered += e - b;
			latest = std::max(latest, it->second.ns);
		}
	}
	return {latest, covered};
}

// An image upload of [vaddr, vaddr + size) begins (command thread).
inline void NoteUpload(uint64_t vaddr, uint64_t size) {
	if (!Enabled()) {
		return;
	}
	auto&      s     = Get();
	auto&      f     = s.frame;
	const auto key   = vaddr ^ (size << 40u);
	auto&      count = s.uploaded[key];
	f.guest_b += size;
	const bool reup = count++ != 0;
	const auto [latest, covered] = LatestFileWrite(vaddr, size);
	int bucket = 6;
	if (covered * 2 >= size && latest != 0) {
		const double ms = static_cast<double>(NowNs() - latest) / 1e6;
		bucket = ms < 17 ? 0 : ms < 50 ? 1 : ms < 100 ? 2 : ms < 500 ? 3 : ms < 2000 ? 4 : 5;
	}
	if (reup) {
		f.reups++;
		f.reup_lead_hist[bucket]++;
		f.reup_lead_b[bucket] += size;
		return;
	}
	f.ups++;
	f.size_hist[size < (64u << 10) ? 0 : size < (1u << 20) ? 1 : size < (4u << 20) ? 2 : size < (16u << 20) ? 3 : 4]++;
	f.lead_hist[bucket]++;
	f.lead_b[bucket] += size;
}

inline void AddBytes(bool sync, uint64_t bytes) {
	if (Enabled()) {
		(sync ? Get().frame.sync_b : Get().frame.stage_b) += bytes;
	}
}

inline void NoteDetile(bool direct) {
	if (Enabled()) {
		(direct ? Get().frame.detile_direct : Get().frame.detile_linear)++;
	}
}

// Times a command-thread scope (nested scopes count in both).
struct Scope {
	Time     time;
	uint64_t start;
	explicit Scope(Time t): time(t), start(Enabled() ? NowNs() : 0) {}
	~Scope() {
		if (start != 0) {
			Get().frame.ns[time] += NowNs() - start;
		}
	}
};

// A guest flip (command thread): prints and resets this frame's counters.
inline void OnFlip(uint64_t flip) {
	if (!Enabled()) {
		return;
	}
	auto&      s = Get();
	auto&      f = s.frame;
	const auto file_b = s.file_b.exchange(0, std::memory_order_relaxed);
	const auto mb = [](uint64_t b) { return static_cast<double>(b) / 1048576.0; };
	const auto ms = [](uint64_t ns) { return static_cast<double>(ns) / 1e6; };
	std::printf("TEXUP f=%" PRIu64 " t=%" PRIu64 " ups=%" PRIu64 " reups=%" PRIu64
	            " guest_mb=%.1f sync_mb=%.1f stage_mb=%.1f file_mb=%.1f find_ms=%.2f insert_ms=%.2f"
	            " obtain_ms=%.2f upload_ms=%.2f detile_direct=%" PRIu64 " detile_linear=%" PRIu64
	            " size_hist=%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
	            " lead_hist=%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
	            " lead_mb=%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f"
	            " reup_lead_hist=%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
	            " reup_lead_mb=%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n",
	            flip, NowNs(), f.ups, f.reups, mb(f.guest_b), mb(f.sync_b), mb(f.stage_b), mb(file_b),
	            ms(f.ns[FindNs]), ms(f.ns[InsertNs]), ms(f.ns[ObtainNs]), ms(f.ns[UploadNs]),
	            f.detile_direct, f.detile_linear, f.size_hist[0], f.size_hist[1], f.size_hist[2],
	            f.size_hist[3], f.size_hist[4], f.lead_hist[0], f.lead_hist[1], f.lead_hist[2],
	            f.lead_hist[3], f.lead_hist[4], f.lead_hist[5], f.lead_hist[6], mb(f.lead_b[0]),
	            mb(f.lead_b[1]), mb(f.lead_b[2]), mb(f.lead_b[3]), mb(f.lead_b[4]), mb(f.lead_b[5]),
	            mb(f.lead_b[6]), f.reup_lead_hist[0], f.reup_lead_hist[1], f.reup_lead_hist[2],
	            f.reup_lead_hist[3], f.reup_lead_hist[4], f.reup_lead_hist[5], f.reup_lead_hist[6],
	            mb(f.reup_lead_b[0]), mb(f.reup_lead_b[1]), mb(f.reup_lead_b[2]), mb(f.reup_lead_b[3]),
	            mb(f.reup_lead_b[4]), mb(f.reup_lead_b[5]), mb(f.reup_lead_b[6]));
	std::fflush(stdout);
	f = {};
}

} // namespace Common::TexBurstDiag
