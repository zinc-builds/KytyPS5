#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_MEMORY_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_MEMORY_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/virtualMemory.h"

#include <utility>
#include <optional>
#include <vector>

namespace Libs::Graphics {
class RenderContext;
enum class PageFaultAccess;
} // namespace Libs::Graphics

namespace Libs::LibKernel::Memory {

void Initialize();
void Shutdown();

// Guest memory usage for the performance panel. Takes only bookkeeping locks, so it never
// waits on the GPU; call it only while the memory subsystem is initialized.
struct DebugStats {
	uint64_t direct_total        = 0;
	uint64_t direct_allocated    = 0; // sceKernelAllocateDirectMemory.
	uint64_t pooled_allocated    = 0; // Memory pool expansions.
	uint64_t automatic_allocated = 0; // Extended memory.
	uint64_t direct_mapped       = 0;
	uint64_t flexible_total      = 0;
	uint64_t flexible_used       = 0;
	uint64_t pool_committed      = 0;
	uint64_t cpu_page_entries       = 0; // 2 MiB page-table entries in use.
	uint64_t gpu_page_entries       = 0;
	uint64_t cpu_page_entries_total = 0;
	uint64_t gpu_page_entries_total = 0;
};
[[nodiscard]] DebugStats GetDebugStats();

struct Lifecycle {
	static constexpr const char* name       = "Memory";
	static constexpr auto        initialize = Libs::LibKernel::Memory::Initialize;
	static constexpr auto        shutdown   = Libs::LibKernel::Memory::Shutdown;
};

using callback_func_t = void (*)(uintptr_t addr, size_t size);

constexpr uint32_t KERNEL_MAXIMUM_NAME_LENGTH = 32;
constexpr uint64_t kExtendedMemoryBase       = 0x080000000000ull;
constexpr uint64_t kExtendedMemorySize       = 512ull * 1024 * 1024 * 1024;

struct VirtualQueryInfo {
	uintptr_t start;
	uintptr_t end;
	uint64_t  offset;
	int32_t   protection;
	int32_t   memory_type;
	uint32_t  is_flexible  : 1;
	uint32_t  is_direct    : 1;
	uint32_t  is_stack     : 1;
	uint32_t  is_pooled    : 1;
	uint32_t  is_committed : 1;
	uint32_t  is_gpu_prt   : 1;
	uint32_t  amm_usage    : 1;
	uint32_t  reserved     : 1;
	char      name[KERNEL_MAXIMUM_NAME_LENGTH];
	uint8_t   gpu_mask_id;
	uint8_t   reserved2;
};

static_assert(sizeof(VirtualQueryInfo) == 72, "VirtualQueryInfo struct size is incorrect");

struct KernelBatchMapEntry {
	void*         start;
	uint64_t      offset;
	uint64_t      length;
	unsigned char protection;
	unsigned char type;
	int16_t       reserved;
	int32_t       operation;
};

static_assert(sizeof(KernelBatchMapEntry) == 32, "KernelBatchMapEntry struct size is incorrect");

struct KernelMemoryPoolBatchEntry {
	uint32_t op;
	uint32_t flags;
	union {
		struct {
			void*    addr;
			uint64_t len;
			uint8_t  prot;
			uint8_t  type;
		} commit;
		struct {
			void*    addr;
			uint64_t len;
		} decommit;
		struct {
			void*    addr;
			uint64_t len;
			uint8_t  prot;
		} protect;
		struct {
			void*    addr;
			uint64_t len;
			uint8_t  prot;
			uint8_t  type;
		} type_protect;
		struct {
			void*    dst;
			void*    src;
			uint64_t len;
		} move;
		uintptr_t padding[3];
	};
};

static_assert(sizeof(KernelMemoryPoolBatchEntry) == 32,
              "KernelMemoryPoolBatchEntry struct size is incorrect");

struct KernelMemoryPoolBlockStats {
	int32_t available_flushed_blocks;
	int32_t available_cached_blocks;
	int32_t allocated_flushed_blocks;
	int32_t allocated_cached_blocks;
};

static_assert(sizeof(KernelMemoryPoolBlockStats) == 16,
              "KernelMemoryPoolBlockStats struct size is incorrect");

void                   RegisterCallbacks(callback_func_t alloc_func, callback_func_t free_func);
int AllocateDirectMemory(int64_t search_start, int64_t search_end, size_t size, size_t alignment,
                         int memory_type, int64_t* phys_addr_out, bool automatic = false);
int MapAutomaticMemory(uint64_t vaddr, size_t size, int type, int prot);
int UnmapAmmMemory(uint64_t vaddr, size_t size);
// `ranged`: the caller records the range with DrawPrepare::OpenStoredRange and CloseStoredRange
// around the store.
bool TryWriteBacking(uint64_t vaddr, const void* data, uint64_t size, bool ranged = false);
// One aligned 4- or 8-byte store through the backing (a label: never written twice).
bool                   TryStoreLabelBacking(uint64_t vaddr, uint64_t value, uint64_t size);
// count values of elem bytes (4 or 8) at vaddr + i * stride, stored in address order through the
// backing, each as TryStoreLabelBacking would; one guest mapping must hold them all. One epoch
// advance pair for the run.
bool TryStoreLabelsBacking(uint64_t vaddr, const void* values, uint32_t elem, uint32_t count,
                           uint32_t stride);
// Fills dword-aligned [vaddr, vaddr + size) with value through the backing alias: no write
// faults, so the caller invalidates the range. False (nothing written) if it is not all backed.
bool                   TryFillBacking(uint64_t vaddr, uint64_t size, uint32_t value);
// BDA pre-staging (docs/mt-rewrite/bda-prestage-design.md), any thread. ArmStaged registers
// [vaddr, vaddr + size) as staged and clears its written marks (stores found are kept for the
// next collection); from then on every consumption of a staged page's written mark moves its
// PageVersions version. nullopt: refused (not asynchronously write-protected, aliased, or
// private); otherwise the remap generation the arm holds for (GuestRemapGeneration).
std::optional<uint64_t> ArmStaged(uint64_t vaddr, uint64_t size);
void                    Unstage(uint64_t vaddr, uint64_t size);
struct PrestageCounters {
	uint64_t arms = 0, refused_async = 0, refused_alias = 0, refused_private = 0, prearm_pages = 0,
	         bumped_pages = 0, staged_pages = 0, remap_generation = 0;
};
// Counts since the last call (staged_pages and remap_generation: current values).
PrestageCounters TakePrestageCounters();
bool                   TryReadBacking(uint64_t vaddr, void* data, uint64_t size);
bool                   TryReadBufferBacking(uint64_t vaddr, void* data, uint64_t size);
bool                   TryReadSparseBacking(uint64_t vaddr, void* data, uint64_t size);
bool                   CopySparseMemory(uint64_t vaddr, void* data, uint64_t size);
[[nodiscard]] uint64_t ClampRangeSize(uint64_t vaddr, uint64_t size);
void WriteBacking(uint64_t vaddr, const void* data, uint64_t size, bool ranged = false) noexcept;
void                   InvalidateMemory(uint64_t vaddr, uint64_t size);
void                   InstallGpuResources(Graphics::RenderContext* renderer) noexcept;
// Protection: the host page protection forbids the access. WriteProtect: a bus error that
// userfaultfd write protection may have raised.
enum class GpuFaultCause { Protection, WriteProtect };
// Resolves a page-tracking fault. False when the guest's own protection forbids the access
// or the tracker does not account for the fault.
[[nodiscard]] bool HandleGpuFault(Graphics::PageFaultAccess access, uint64_t fault_vaddr,
                                  GpuFaultCause cause) noexcept;
// Hot write-tracked ranges use asynchronous write protection: their writes raise no fault
// the emulator sees. Appends the page runs written there since the last call (GPU thread).
// wanted: whether any page of [begin, end) has a write watcher (chunks without are not scanned).
void CollectAsyncGuestWrites(std::vector<std::pair<uint64_t, uint64_t>>& written,
                             bool (*wanted)(uint64_t begin, uint64_t end) = nullptr);
// The [begin, end) ranges under asynchronous write protection (for KYTY_ASYNC_WP_VERIFY).
void AsyncGuestRanges(std::vector<std::pair<uint64_t, uint64_t>>& ranges);
// KYTY_SUBMIT_SCAN: a submitting guest thread's scan of the asynchronous ranges; what it finds is
// kept for the GPU thread's next collection. False if asynchronous protection is off.
bool ScanAsyncGuestWritesAtSubmit(bool (*wanted)(uint64_t begin, uint64_t end));
// The GPU thread's collection without a scan of its own: kept writes (submit scans,
// re-protections) and pending chunk promotions.
void TakeKeptAsyncGuestWrites(std::vector<std::pair<uint64_t, uint64_t>>& written);
// KYTY_ASYNC_WP_VERIFY (GPU thread): the kept writes collections took since the last call.
void TakeVerifyKeptWrites(std::vector<std::pair<uint64_t, uint64_t>>& ranges);
// KYTY_ASYNC_WP_VERIFY: appends the asynchronous pages written since the last collection, and
// the writes kept for the next one (after any submit scan in flight).
// wp_only: only pages a collection's protecting scan can report (in write-protected views).
void PeekAsyncGuestWrites(std::vector<std::pair<uint64_t, uint64_t>>& written, bool wp_only = false);
// Whether part of the range is under asynchronous write protection, where a store raises no
// fault: emulator stores there must invalidate the caches themselves (GPU thread).
[[nodiscard]] bool IsAsyncWriteTracked(uint64_t vaddr, uint64_t size) noexcept;
// Asks for the chunk holding vaddr to move to asynchronous write protection (next collection).
void RequestAsyncWriteTracking(uint64_t vaddr) noexcept;
// One page of an asynchronous chunk: protect it now / whether a guest view stored to it since.
// ProtectAsyncGuestPage write-protects an asynchronously tracked page now (stores found there
// go to the next collection) and gives a token; false if the page is not one.
// AsyncGuestPageUntouched: no guest view has stored to the page since the token was given.
bool               ProtectAsyncGuestPage(uint64_t page, uint64_t& token);
// stale (optional): set when it is false because the token was voided (no longer the page's
// current one), not because of a guest store.
[[nodiscard]] bool AsyncGuestPageUntouched(uint64_t page, uint64_t token, bool* stale = nullptr);
// Advances on remaps and zeroing: guest bytes that change without a store.
[[nodiscard]] uint64_t GuestRemapGeneration() noexcept;
// KYTY_ASYNC_WP_VERIFY: takes the ranges the emulator wrote through the backing alias.
void TakeAliasWrites(std::vector<std::pair<uint64_t, uint64_t>>& ranges);

int KYTY_SYSV_ABI KernelMapNamedFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags,
                                               const char* name);
int KYTY_SYSV_ABI KernelMapFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags);
int KYTY_SYSV_ABI KernelSetVirtualRangeName(const void* addr, uint64_t len, const char* name);
int KYTY_SYSV_ABI KernelClearVirtualRangeName(const void* addr, uint64_t len);
int KYTY_SYSV_ABI KernelMunmap(uint64_t vaddr, size_t len);
size_t KYTY_SYSV_ABI KernelGetDirectMemorySize();
int KYTY_SYSV_ABI    KernelAvailableDirectMemorySize(int64_t search_start, int64_t search_end,
                                                     size_t alignment, int64_t* phys_addr_out,
                                                     size_t* size_out);
int KYTY_SYSV_ABI    KernelGetPageTableStats(int* cpu_total, int* cpu_available, int* gpu_total,
                                             int* gpu_available);
int KYTY_SYSV_ABI KernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len,
                                             size_t alignment, int memory_type,
                                             int64_t* phys_addr_out);
int KYTY_SYSV_ABI KernelAllocateMainDirectMemory(size_t len, size_t alignment, int memory_type,
                                                 int64_t* phys_addr_out);
int KYTY_SYSV_ABI KernelCheckedReleaseDirectMemory(int64_t start, size_t len);
int KYTY_SYSV_ABI KernelReleaseDirectMemory(int64_t start, size_t len);
int KYTY_SYSV_ABI KernelMapDirectMemory(void** addr, size_t len, int prot, int flags,
                                        int64_t direct_memory_start, size_t alignment);
int KYTY_SYSV_ABI KernelMapDirectMemory2(void** addr, size_t len, int type, int prot, int flags,
                                         int64_t direct_memory_start, size_t alignment);
int KYTY_SYSV_ABI KernelMapNamedDirectMemory(void** addr, size_t len, int prot, int flags,
                                             int64_t direct_memory_start, size_t alignment,
                                             const char* name);
int KYTY_SYSV_ABI KernelSetPrtAperture(int index, void* addr, size_t len);
int KYTY_SYSV_ABI KernelGetPrtAperture(int index, void** addr, size_t* len);
int KYTY_SYSV_ABI KernelIsAddressSanitizerEnabled();
int KYTY_SYSV_ABI KernelQueryMemoryProtection(void* addr, void** start, void** end, int* prot);
int KYTY_SYSV_ABI KernelDirectMemoryQuery(int64_t offset, int flags, void* info, size_t info_size);
int KYTY_SYSV_ABI KernelVirtualQuery(const void* addr, int flags, VirtualQueryInfo* info,
                                     uint64_t info_size);
int KYTY_SYSV_ABI KernelIsStack(void* addr, void** start, void** end);
int KYTY_SYSV_ABI KernelReserveVirtualRange(void** addr, size_t len, int flags, size_t alignment);
int KYTY_SYSV_ABI KernelAvailableFlexibleMemorySize(size_t* size);
int KYTY_SYSV_ABI KernelConfiguredFlexibleMemorySize(size_t* size);
int KYTY_SYSV_ABI KernelMprotect(const void* addr, size_t len, int prot);
int KYTY_SYSV_ABI KernelMtypeprotect(const void* addr, size_t len, int type, int prot);
int KYTY_SYSV_ABI KernelBatchMap(KernelBatchMapEntry* entries, int num_entries,
                                 int* num_entries_out);
int KYTY_SYSV_ABI KernelBatchMap2(KernelBatchMapEntry* entries, int num_entries,
                                  int* num_entries_out, int flags);
int KYTY_SYSV_ABI KernelMemoryPoolExpand(int64_t search_start, int64_t search_end, size_t len,
                                         size_t alignment, int64_t* phys_addr_out);
int KYTY_SYSV_ABI KernelMemoryPoolReserve(void* addr_in, size_t len, size_t alignment, int flags,
                                          void** addr_out);
int KYTY_SYSV_ABI KernelMemoryPoolCommit(void* addr, size_t len, int type, int prot, int flags);
int KYTY_SYSV_ABI KernelMemoryPoolDecommit(void* addr, size_t len, int flags);
int KYTY_SYSV_ABI KernelMemoryPoolBatch(const KernelMemoryPoolBatchEntry* entries, int num_entries,
                                        int* num_entries_out, int flags);
int KYTY_SYSV_ABI KernelMemoryPoolGetBlockStats(KernelMemoryPoolBlockStats* output,
                                                size_t                      output_size);

uint64_t AllocateProgramMemory(uint64_t search_addr, uint64_t size,
                               Common::VirtualMemory::Mode mode, const char* name);
// Loaded modules' segments use flexible memory without taking its backing.
void ChargeFlexibleMemory(uint64_t size);
void ReleaseFlexibleMemory(uint64_t size);
void SetProgramMemoryProtection(uint64_t vaddr, uint64_t size, Common::VirtualMemory::Mode mode);
uint64_t AllocateRuntimeMemory(uint64_t search_addr, uint64_t size,
                               Common::VirtualMemory::Mode mode, const char* name,
                               bool fixed = false);
uint64_t AllocateGuestStackMemory(uint64_t search_addr, uint64_t size,
                                  Common::VirtualMemory::Mode mode, const char* name);
bool     ProtectGuestMemory(uint64_t vaddr, uint64_t size, Common::VirtualMemory::Mode mode,
                            Common::VirtualMemory::Mode* old_mode = nullptr);
// Transient PageManager watch state; does not change the guest mapping's semantic protection.
// access_changed is false when only the write watch changes (Read <-> ReadWrite).
bool ProtectGuestHostMemory(uint64_t vaddr, uint64_t size, Common::VirtualMemory::Mode mode,
                            bool access_changed = true);
bool FreeGuestMemory(uint64_t vaddr, uint64_t size);

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
void     TestSetGpuRangeCallback(void (*callback)(bool mapped, uint64_t addr, size_t size));
void     TestBeforeNextBackingMap(callback_func_t callback);
void     TestSetBackingReadCallback(callback_func_t callback);
void     TestFailNextPhysicalMemoryUnmap();
void     TestFailPhysicalMemoryUnmapAfter(uint32_t successful_unmaps);
void     TestFailGuestBackingStoreUnmapAfter(uint32_t successful_unmaps);
void     TestFailNextFixedReserveRangeRegistration();
void     TestFailNextVirtualRangeReplacement();
bool     TestPlaceholderRangeIsFree(uint64_t vaddr, uint64_t size);
bool     TestGuestAddressRangeIsOwned(uint64_t vaddr, uint64_t size);
bool     TestGuestBackingOutsideAddressSpace();
uint64_t TestGuestBackingSize();
uint64_t TestFlexibleBackingHighWater();
uint64_t TestGuestFreeRangeVisits();
uint64_t TestDirectMappingCount();
uint64_t TestFlexibleMappingCount();
uint64_t TestBackingBytesZeroed();
bool     TestGuestFreeRangeBounds();
bool     TestGuestWriteProtectsViews();
void     TestFailNextWriteProtectRegistration();
#endif

} // namespace Libs::LibKernel::Memory

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_MEMORY_H_ */
