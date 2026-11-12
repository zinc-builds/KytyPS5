#ifndef KYTY_COMMON_PLAYTEST_H_
#define KYTY_COMMON_PLAYTEST_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// KYTY_PLAYTEST=1 (off by default): the playtest flight recorder (docs/playtest.md; launch with
// tools/playtest.sh, read with tools/playtest-report.py). While the user plays, it keeps the last
// seconds of per-frame numbers and discrete events in memory, and writes an incident (the window
// around it, a ranked cause, a screenshot) when a frame is long, frames are slow for a second, the
// game stops flipping, a new unresolved import is called, or the user presses the marker (F9, or
// the PS or microphone button). A fatal error, assert or crash signal writes crash.json on the way
// down. Session directory: KYTY_PLAYTEST_DIR, default ~/.cache/kyty-playtest/<timestamp>.
//
// Cost model: the command thread snapshots counters once per guest flip into a ring it alone
// writes (two system calls for its own CPU and kernel time); other threads publish rare events
// into per-thread rings; one helper thread (nice 10) drains them, evaluates the triggers and
// writes every file. Nothing here runs per draw. Off: a static flag test at each hook.
namespace Common::Playtest {

[[nodiscard]] bool ReadEnabled() noexcept;
// Inline so that the disabled path costs a guard check, not a call.
[[nodiscard]] inline bool Enabled() noexcept {
	static const bool enabled = ReadEnabled();
	return enabled;
}

// Process start, after the configuration is loaded: makes the session directory and starts the
// helper thread. Shutdown (at exit) closes an open incident and flushes every file.
void Init();
void Shutdown() noexcept;

enum class Kind : uint16_t {
	ShaderCompile,  // a = ns, b = shader hash, c = stage; flags 1 = injected miss
	PipelineCreate, // a = ns, b = 0 graphics / 1 compute
	TextureUpload,  // a = bytes, b = width | height << 32, c = vk::Format | mips << 32, d = ns
	BufferUpload,   // a = bytes, b = ns of the staging copy; flags 1 = inside a texture upload
	GarbageCollect, // a = ns
	StubFirstCall,  // a = record id; text = symbol
	AudioUnderrun,
	Level,          // text = level; a = 0 StartLevel, 1 "Level has started"
	Marker,         // a = marker number
	Inject,         // text = what; a = value
	Count
};

// Any thread; rare events only (never per draw). Lock-free: each thread writes its own ring.
void Event(Kind kind, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, const char* text = nullptr,
           uint16_t flags = 0, uint64_t d = 0) noexcept;

// Texture uploads (GPU thread): Begin before obtaining the source bytes, End after recording the
// upload. Every upload's time goes into the frame's texture-upload time; one of
// TextureUploadEventBytes or more is also an event. Buffer uploads inside one are flagged so
// their copy time is not counted twice.
[[nodiscard]] uint64_t BeginTextureUpload() noexcept; // 0 when off
void EndTextureUpload(uint64_t start, uint64_t bytes, uint32_t width, uint32_t height, uint32_t format,
                      uint32_t mips, const char* format_name) noexcept;
[[nodiscard]] bool InTextureUpload() noexcept;

// Thresholds for size-gated events (bytes): callers check these before calling Event.
inline constexpr uint64_t TextureUploadEventBytes = 1ull << 20;
inline constexpr uint64_t BufferUploadEventBytes  = 4ull << 20;

// The command thread, after it decoded and flushed a guest flip (once per guest frame). Returns
// the bytes of sampled textures to re-upload now for an injected upload fault (0: none).
[[nodiscard]] uint64_t OnGuestFlip(uint64_t presented_flips) noexcept;
// The presentation thread, at each presented flip of the main bus (serialized by its caller).
void OnPresent(uint64_t flip, double interval_ms) noexcept;
// The guest's flip queue depth when it requested a flip (the frame keeps the maximum).
void NoteFlipQueue(uint32_t depth) noexcept;
// An unresolved import stub was called; the first call of each record is an event and a trigger.
void OnStubCall(uint64_t record_id, const char* name) noexcept;
// Guest stdout/stderr bytes: level names ("StartLevel <name>", "Level has started: <name>").
void OnGuestOutput(const void* data, size_t size) noexcept;
// The user's marker key or button (window thread).
void Marker() noexcept;

// Pad reads at the pad HLE (guest threads): `data` is the state the game received (PadData,
// libs/padData.h, its timestamp zeroed by the caller), `count` the states PadRead returned. Each
// read that differs from the handle's previous one goes to input.jsonl with the guest frame and
// the frame since the last StartLevel, for replay and for reading what the user did.
void OnPadRead(int handle, const void* data, size_t size, int count) noexcept;

// Injected faults (KYTY_PLAYTEST_INJECT, for proving the attribution): a shader lookup on the
// command thread that should miss once (pipelineCache.cpp).
[[nodiscard]] bool TakeInjectedMiss() noexcept;

// Screenshots (presentation thread). PendingShot gives the path of a requested shot, or nullptr;
// SubmitShot hands over the copied pixels (4 bytes a pixel; layout: 0 RGBA8, 1 BGRA8, 2 A2RGB10,
// 3 A2BGR10) for the helper thread to write; FailShot gives the request up.
[[nodiscard]] const char* PendingShot() noexcept;
void SubmitShot(uint32_t width, uint32_t height, int layout, std::vector<uint8_t>&& pixels) noexcept;
void FailShot(const char* why) noexcept;

// The marker's on-screen confirmation: the marker number while it shows (2.5 s), else 0.
[[nodiscard]] uint32_t ToastMarker() noexcept;

// Fatal paths: EXIT/assert (assert.cpp), unhandled crash signals (hostException.cpp), abort and
// std::terminate. Writes crash.json from the rings without locks or allocation, then gives the
// helper thread up to ~1 s to flush. Safe to call more than once (the first call writes).
void OnFatal(const char* reason, const char* detail) noexcept;

} // namespace Common::Playtest

#endif // KYTY_COMMON_PLAYTEST_H_
