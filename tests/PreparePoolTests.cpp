// DrawPrepare's pool, the posting ring and register-snapshot lifetime under KYTY_PREPARE_CHAOS
// delays, host only. A poster thread (as the decode thread) fills ring slots and register
// snapshots and posts them to a real DrawPrepare::Pool with two workers and an overflow worker;
// the main thread (as the command thread) waits for each draw, checks what the worker saw, and
// releases the slot. The ring follows RenderExecutor's rules: a slot is refilled once the draw
// one ring-length before it is released; a snapshot is reused through DrawPrepare::SnapshotRing
// once no unreleased draw uses it.
#include "graphics/host_gpu/renderer/drawPrepare.h"

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

namespace {

namespace DP    = Libs::Graphics::DrawPrepare;
namespace Chaos = Libs::Graphics::DrawPrepare::Chaos;

constexpr uint32_t kRing = 9; // queue depth 8, plus the free slot

struct Snapshot {
	std::array<uint64_t, 32> words {};
	uint64_t                 last_sequence = 0;
};

struct Slot {
	uint64_t        sequence = 0;
	const Snapshot* snapshot = nullptr;
	uint64_t        arg      = 0;
	// Written by the worker.
	uint64_t ran    = 0;
	uint64_t sum    = 0;
	uint64_t result = 0;
};

uint64_t Inputs(const Slot& slot) {
	auto hash = Chaos::Digest(slot.snapshot->words.data(), sizeof(slot.snapshot->words));
	hash      = Chaos::Digest(&slot.arg, sizeof(slot.arg), hash);
	return Chaos::Digest(&slot.sequence, sizeof(slot.sequence), hash);
}

uint64_t Work(const Slot& slot) {
	uint64_t result = slot.arg;
	for (const auto word: slot.snapshot->words) {
		result = result * 31 + word;
	}
	return result;
}

struct Run {
	std::array<Slot, kRing> slots;
	std::atomic<uint64_t>   posted {0};
	std::atomic<uint64_t>   released {0};
	std::atomic<uint64_t>   worker_mismatches {0};
};

void Prepare(void* context, uint64_t sequence) {
	auto& run  = *static_cast<Run*>(context);
	auto& slot = run.slots[(sequence - 1) % kRing];
	if (slot.sequence != sequence) {
		run.worker_mismatches.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	const auto sum    = Inputs(slot);
	const auto result = Work(slot);
	slot.sum          = sum;
	slot.result       = result;
	slot.ran          = sequence;
}

struct Outcome {
	uint64_t draws             = 0;
	uint64_t snapshots         = 0; // distinct snapshot objects at the end
	uint64_t reuse_violations  = 0; // a snapshot rewritten while an unreleased draw used it
	uint64_t stale_stamps      = 0; // the worker's stamp was not this draw's
	uint64_t changed_inputs    = 0; // the inputs changed between the worker's read and adoption
	uint64_t wrong_results     = 0;
	uint64_t worker_mismatches = 0; // a worker found another draw in its slot
};

// `retire_bug`: the poster treats a snapshot as free one draw too early (its last user is the
// oldest unreleased draw), as a broken retirement rule would.
Outcome RunPipeline(uint64_t seed, uint64_t draws, bool retire_bug) {
	Chaos::SetSeed(seed);
	Run     run;
	Outcome out;
	out.draws = draws;
	{
		DP::Pool   pool(kRing, 3);
		DP::Worker w0(&Prepare, &run, 0, &pool);
		DP::Worker w1(&Prepare, &run, 1, &pool);
		DP::Worker overflow(&Prepare, &run, 2, &pool, true);

		// Outlives the poster: the committer still reads the last draws' snapshots.
		DP::SnapshotRing<Snapshot> snapshots;
		std::thread poster([&] {
			Snapshot* current = nullptr;
			uint64_t                   rng     = seed * 0x9e3779b97f4a7c15ull + 1;
			for (uint64_t s = 1; s <= draws; s++) {
				// The slot is free once the draw a ring-length before it is released.
				while (s + 1 - run.released.load(std::memory_order_acquire) > kRing) {
					std::this_thread::yield();
				}
				Chaos::Point(Chaos::Site::SlotReuse);
				rng ^= rng << 13u;
				rng ^= rng >> 7u;
				rng ^= rng << 17u;
				// New registers about every third draw.
				if (current == nullptr || rng % 3 == 0) {
					const auto oldest = run.released.load(std::memory_order_acquire) + 1;
					Chaos::Point(Chaos::Site::SnapshotRetire);
					auto* snapshot = snapshots.Take(current, retire_bug ? oldest + 1 : oldest);
					bool  in_use   = false;
					for (const auto& slot: run.slots) {
						if (slot.snapshot == snapshot && slot.sequence >= oldest && slot.sequence < s) {
							in_use = true;
						}
					}
					if (in_use) {
						// Caught: an unreleased draw still reads it. Not rewritten (that would race
						// the reader); this draw keeps the current registers.
						out.reuse_violations++;
					} else {
						for (size_t i = 0; i < snapshot->words.size(); i++) {
							snapshot->words[i] = s * 1000 + i;
						}
						current = snapshot;
					}
				}
				current->last_sequence = s;
				auto& slot             = run.slots[(s - 1) % kRing];
				slot.sequence          = s;
				slot.snapshot          = current;
				slot.arg               = rng;
				if (pool.Post() != s) {
					std::fprintf(stderr, "PreparePool: Post returned the wrong sequence\n");
					std::abort();
				}
				Chaos::Point(Chaos::Site::DecodePost);
				run.posted.store(s, std::memory_order_release);
			}
			out.snapshots = snapshots.Size();
		});

		for (uint64_t s = 1; s <= draws; s++) {
			while (run.posted.load(std::memory_order_acquire) < s) {
				std::this_thread::yield();
			}
			Chaos::Point(Chaos::Site::CommitWait);
			(void)pool.Wait(s);
			Chaos::Point(Chaos::Site::Adopt);
			const auto& slot = run.slots[(s - 1) % kRing];
			if (slot.ran != s) {
				out.stale_stamps++;
			} else if (Inputs(slot) != slot.sum) {
				if (out.changed_inputs++ < 3) {
					std::fprintf(stderr,
					             "changed: draw %" PRIu64 " snapshot %p last_sequence %" PRIu64
					             " words[0] %" PRIu64 " words[31] %" PRIu64 " posted %" PRIu64 "\n",
					             s, static_cast<const void*>(slot.snapshot),
					             slot.snapshot->last_sequence, slot.snapshot->words[0],
					             slot.snapshot->words[31], run.posted.load());
				}
			} else if (Work(slot) != slot.result) {
				out.wrong_results++;
			}
			Chaos::Point(Chaos::Site::Release);
			run.released.store(s, std::memory_order_release);
		}
		poster.join();
	}
	out.worker_mismatches = run.worker_mismatches.load();
	Chaos::SetSeed(0);
	return out;
}

int g_failed = 0;

void Check(bool ok, const char* what, const Outcome& o) {
	std::printf("[host]    PreparePool %-44s %s (draws=%" PRIu64 " snapshots=%" PRIu64
	            " reuse=%" PRIu64 " stale=%" PRIu64 " changed=%" PRIu64 " wrong=%" PRIu64
	            " worker=%" PRIu64 ")\n",
	            what, ok ? "ok" : "FAILED", o.draws, o.snapshots, o.reuse_violations, o.stale_stamps,
	            o.changed_inputs, o.wrong_results, o.worker_mismatches);
	g_failed += ok ? 0 : 1;
}

bool Clean(const Outcome& o) {
	return o.reuse_violations == 0 && o.stale_stamps == 0 && o.changed_inputs == 0 &&
	       o.wrong_results == 0 && o.worker_mismatches == 0;
}

} // namespace

int main(int argc, char** argv) {
	const uint64_t draws = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100'000;
	// Without delays, then three seeds.
	for (const uint64_t seed: {0ull, 1ull, 2ull, 0x5eedull}) {
		char name[64];
		std::snprintf(name, sizeof(name), "chaos seed %" PRIu64, seed);
		const auto o = RunPipeline(seed, draws, false);
		Check(Clean(o), name, o);
	}
	uint64_t delays = 0;
	for (uint32_t site = 0; site < static_cast<uint32_t>(Chaos::Site::Count); site++) {
		delays += Chaos::Delays(static_cast<Chaos::Site>(site));
	}
	std::printf("[host]    PreparePool delays taken: %" PRIu64 "\n", delays);
	if (delays == 0) {
		std::printf("[host]    PreparePool no chaos delay was taken: FAILED\n");
		g_failed++;
	}
	// A retirement rule off by one must be caught by the invariants.
	{
		const auto o = RunPipeline(7, draws, true);
		Check(o.reuse_violations != 0, "retirement bug caught", o);
	}
	return g_failed == 0 ? 0 : 1;
}
