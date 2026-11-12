// Internal memory address-space helpers included by memory.cpp.
// Kept in the same translation unit because they depend on memory.cpp-local helpers.

static constexpr uint64_t HOST_SYSTEM_MANAGED_MIN  = 0x0000040000ull;
static constexpr uint64_t HOST_SYSTEM_MANAGED_MAX  = 0x07ffffbfffull;
#if defined(__APPLE__) && (defined(__x86_64__) || defined(_M_X64))
static constexpr uint64_t HOST_SYSTEM_RESERVED_MAX = 0x0fbfffffffull;
static constexpr uint64_t HOST_USER_MIN            = 0x7000000000ull;
#else
static constexpr uint64_t HOST_SYSTEM_RESERVED_MAX = 0x0fffffffffull;
static constexpr uint64_t HOST_USER_MIN            = 0x1000000000ull;
#endif
static constexpr uint64_t HOST_USER_MAX = 0xfbffffffffull;

static bool GuestFreeRangeContains(uint64_t base, uint64_t range_size, uint64_t candidate,
                                   uint64_t size) {
	const auto range_end = base + range_size;
	return candidate <= range_end && size <= range_end - candidate;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static bool CoalescePlaceholders(uint64_t vaddr, uint64_t size) {
	return VirtualFree(reinterpret_cast<void*>(vaddr), size,
	                   MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS) != 0;
}
#endif

template <typename Regions, typename Region, typename Gap>
static void ForEachSparseRange(const Regions& regions, uint64_t vaddr, uint64_t size,
                               Region&& region, Gap&& gap) {
	const auto end     = vaddr + size;
	auto       mapping = regions.lower_bound(vaddr);
	if (mapping != regions.begin()) {
		auto previous = std::prev(mapping);
		if (previous->second.vaddr + previous->second.size > vaddr) {
			mapping = previous;
		}
	}

	auto cursor = vaddr;
	for (; mapping != regions.end() && mapping->second.vaddr < end; ++mapping) {
		const auto& entry = mapping->second;
		const auto  start = std::max(vaddr, entry.vaddr);
		const auto  bytes = std::min(end, entry.vaddr + entry.size) - start;
		if (cursor < start) {
			gap(cursor, start - cursor);
		}
		region(entry, start, bytes);
		cursor = start + bytes;
	}
	if (cursor < end) {
		gap(cursor, end - cursor);
	}
}

template <typename Regions, typename Source, typename Gap>
static void CopySparseRange(const Regions& regions, uint64_t vaddr, void* data, uint64_t size,
                            Source&& source, Gap&& gap) {
	auto* destination = static_cast<uint8_t*>(data);
	ForEachSparseRange(
	    regions, vaddr, size,
	    [&](const auto& entry, uint64_t start, uint64_t bytes) {
		    std::memcpy(destination + (start - vaddr), source(entry, start), bytes);
	    },
	    [&](uint64_t start, uint64_t bytes) { gap(start, destination + (start - vaddr), bytes); });
}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
// Page-tracker write watches as page-table write protection (userfaultfd): watching a page
// neither changes nor splits its view. A write to a protected page raises SIGBUS, which the
// host fault handler resolves like a protection fault.
class GuestWriteProtection {
public:
	GuestWriteProtection() {
		const int fd = static_cast<int>(
		    syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
		if (fd < 0) {
			LOGF("\t guest write tracking: mprotect (userfaultfd unavailable: %d)\n", errno);
			return;
		}
		uffdio_api api {};
		api.api      = UFFD_API;
		api.features = UFFD_FEATURE_SIGBUS | UFFD_FEATURE_WP_HUGETLBFS_SHMEM;
		if (ioctl(fd, UFFDIO_API, &api) != 0 ||
		    (api.features & UFFD_FEATURE_WP_HUGETLBFS_SHMEM) == 0 ||
		    (api.ioctls & (uint64_t {1} << _UFFDIO_REGISTER)) == 0) {
			LOGF("\t guest write tracking: mprotect (no shared-memory write protection: %d)\n",
			     errno);
			close(fd);
			return;
		}
		m_fd = fd;
		LOGF("\t guest write tracking: userfaultfd write protection\n");
#if defined(UFFD_FEATURE_WP_ASYNC) && defined(PAGEMAP_SCAN)
		// Hot ranges move to a second context whose write faults the kernel resolves itself
		// (no signal); their written pages are collected with PAGEMAP_SCAN instead.
		if (std::getenv("KYTY_NO_ASYNC_WP") != nullptr) {
			return;
		}
		const int async_fd = static_cast<int>(
		    syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
		const int pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
		uffdio_api async_api {};
		async_api.api      = UFFD_API;
		async_api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED |
		                     UFFD_FEATURE_WP_HUGETLBFS_SHMEM;
		if (async_fd >= 0 && pagemap >= 0 && ioctl(async_fd, UFFDIO_API, &async_api) == 0 &&
		    (async_api.features & UFFD_FEATURE_WP_ASYNC) != 0) {
			m_async_fd = async_fd;
			m_pagemap  = pagemap;
			LOGF("\t guest write tracking: asynchronous write protection for hot ranges\n");
		} else {
			if (async_fd >= 0) {
				close(async_fd);
			}
			if (pagemap >= 0) {
				close(pagemap);
			}
		}
#endif
	}

	~GuestWriteProtection() {
		if (m_fd >= 0) {
			close(m_fd);
		}
		if (m_async_fd >= 0) {
			close(m_async_fd);
		}
		if (m_pagemap >= 0) {
			close(m_pagemap);
		}
	}

	[[nodiscard]] bool AsyncEnabled() const { return m_async_fd >= 0; }

	// Moves a registered range to the asynchronous context. Its pages lose their protection
	// and read as written until the next Scan re-protects them.
	[[nodiscard]] bool MakeAsync(uint64_t vaddr, uint64_t size) const {
		uffdio_range range {vaddr, size};
		if ([&] {
			    Common::KernelStats::Syscall kernel(Common::KernelStats::Kind::UffdReg);
			    return ioctl(m_fd, UFFDIO_UNREGISTER, &range);
		    }() != 0) {
			EXIT("guest write tracking: unregistering 0x%016" PRIx64 "+0x%" PRIx64 " failed: %d\n",
			     vaddr, size, errno);
		}
		uffdio_register reg {};
		reg.range = {vaddr, size};
		reg.mode  = UFFDIO_REGISTER_MODE_WP;
		if ([&] {
			    Common::KernelStats::Syscall kernel(Common::KernelStats::Kind::UffdReg);
			    return ioctl(m_async_fd, UFFDIO_REGISTER, &reg);
		    }() == 0 &&
		    (reg.ioctls & (uint64_t {1} << _UFFDIO_WRITEPROTECT)) != 0) {
			return true;
		}
		// Unregistering dropped the range's write protection, which nothing here can restore.
		EXIT("guest write tracking: moving 0x%016" PRIx64 "+0x%" PRIx64
		     " to asynchronous write protection failed: %d\n",
		     vaddr, size, errno);
	}

	// Calls found(begin, end) for each run of pages written since they were last protected and,
	// with protect, write-protects them again in the same step. With protect, ranges outside the
	// asynchronous context are skipped by the kernel.
	template <typename Found>
	void Scan(uint64_t vaddr, uint64_t size, Found&& found, bool protect = true) const {
#if defined(PAGEMAP_SCAN)
		std::array<page_region, 512> regions {};
		auto                         start = vaddr;
		const auto                   end   = vaddr + size;
		while (start < end) {
			pm_scan_arg arg {};
			arg.size          = sizeof(arg);
			arg.flags         = protect ? PM_SCAN_WP_MATCHING : 0;
			arg.start         = start;
			arg.end           = end;
			arg.vec           = reinterpret_cast<uint64_t>(regions.data());
			arg.vec_len       = regions.size();
			arg.category_mask = PAGE_IS_WRITTEN;
			arg.return_mask   = PAGE_IS_WRITTEN;
			const auto count  = [&] {
				Common::KernelStats::Syscall kernel(Common::KernelStats::Kind::Scan);
				const auto result = ioctl(m_pagemap, PAGEMAP_SCAN, &arg);
				const auto kind   = result > 0 ? Common::KernelStats::Kind::ScanHit
				                               : Common::KernelStats::Kind::Scan;
				kernel.SetKind(kind);
				Common::KernelStats::AddUnits(
				    kind, (arg.walk_end > start ? arg.walk_end - start : 0) >> 12u);
				return result;
			}();
			EXIT_IF(count < 0);
			for (long i = 0; i < count; i++) {
				if (Graphics::MemDiag::Traced(regions[i].start, regions[i].end - regions[i].start)) {
					Graphics::MemDiag::Trace("scan-written", regions[i].start, regions[i].end - regions[i].start);
				}
				found(regions[i].start, regions[i].end);
			}
			// walk_end is where the kernel stopped: the end, or a full output vector.
			if (arg.walk_end <= start) {
				break;
			}
			start = arg.walk_end;
		}
#else
		(void)vaddr;
		(void)size;
		(void)found;
#endif
	}

	KYTY_CLASS_NO_COPY(GuestWriteProtection);

	[[nodiscard]] bool Enabled() const { return m_fd >= 0; }

	// False when the view cannot be write-protected; its watches then use mprotect.
	[[nodiscard]] bool Register(uint64_t vaddr, uint64_t size) const {
		uffdio_register range {};
		range.range.start = vaddr;
		range.range.len   = size;
		range.mode        = UFFDIO_REGISTER_MODE_WP;
		bool registered   = false;
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (g_test_fail_next_write_protect_register) {
			g_test_fail_next_write_protect_register = false;
			errno                                   = EBUSY;
		} else
#endif
		{
			Common::KernelStats::Syscall kernel(Common::KernelStats::Kind::UffdReg);
			registered = ioctl(m_fd, UFFDIO_REGISTER, &range) == 0;
		}
		if (registered && (range.ioctls & (uint64_t {1} << _UFFDIO_WRITEPROTECT)) == 0) {
			uffdio_range unregister {vaddr, size};
			(void)ioctl(m_fd, UFFDIO_UNREGISTER, &unregister);
			registered = false;
			errno      = EINVAL;
		}
		if (!registered) {
			LOGF("\t guest write tracking: mprotect for view 0x%016" PRIx64 " size 0x%016" PRIx64
			     " (userfaultfd registration failed: %d)\n",
			     vaddr, size, errno);
		}
		return registered;
	}

	[[nodiscard]] bool Protect(uint64_t vaddr, uint64_t size, bool write_protect,
	                           bool async = false) const {
		if (Graphics::MemDiag::Traced(vaddr, size)) {
			Graphics::MemDiag::Trace(write_protect ? "uffd-wp" : "uffd-unwp", vaddr, size, async ? 1 : 0);
		}
		const auto          page  = static_cast<uint64_t>(getpagesize());
		const auto          start = vaddr & ~(page - 1);
		uffdio_writeprotect range {};
		range.range.start = start;
		range.range.len   = ((vaddr + size + page - 1) & ~(page - 1)) - start;
		range.mode        = write_protect ? UFFDIO_WRITEPROTECT_MODE_WP : 0;
		Common::KernelStats::Syscall kernel(Common::KernelStats::Kind::UffdWp);
		return ioctl(async ? m_async_fd : m_fd, UFFDIO_WRITEPROTECT, &range) == 0;
	}

private:
	int m_fd       = -1;
	int m_async_fd = -1;
	int m_pagemap  = -1;
};
#endif

// KYTY_ASYNC_WP_VERIFY: stores through the backing alias change guest bytes without a write
// fault in any mode; the verifier skips them.
static bool                                       g_log_alias_writes = std::getenv("KYTY_ASYNC_WP_VERIFY") != nullptr;
static std::mutex                                 g_alias_write_mutex;
static std::vector<std::pair<uint64_t, uint64_t>> g_alias_writes;
// KYTY_ASYNC_WP_VERIFY: kept writes the GPU thread's last collection took; each must be among
// the writes that collection handled (RenderContext::VerifyAsyncWrites).
static std::vector<std::pair<uint64_t, uint64_t>> g_verify_kept_taken;

// Advances whenever guest bytes change without a store through a guest view or the backing
// (remaps, zeroing): RenderContext::IsCpLabel's proof does not cover those.
static std::atomic<uint64_t> g_remap_generation {0};

static void LogAliasWrite(uint64_t vaddr, uint64_t size) {
	if (Graphics::MemDiag::Traced(vaddr, size)) {
		Graphics::MemDiag::Trace("alias-write-or-remap", vaddr, size);
	}
	if (g_log_alias_writes) {
		std::lock_guard lock(g_alias_write_mutex);
		g_alias_writes.emplace_back(vaddr, vaddr + size);
	}
}

// Odd while a GuestBackingStore writer (map, unmap, zero, backing write) holds its lock, and
// advanced again when it lets go: a read through a translation taken at an even value, still
// current after the copy, saw no change. The backing alias stays mapped for the store's lifetime,
// so a read that races a change only has to be retried.
static std::atomic<uint64_t> g_translation_seq {0};

struct TranslationWrite {
	TranslationWrite() noexcept { g_translation_seq.fetch_add(1, std::memory_order_acq_rel); }
	~TranslationWrite() { g_translation_seq.fetch_add(1, std::memory_order_release); }
	TranslationWrite(const TranslationWrite&)            = delete;
	TranslationWrite& operator=(const TranslationWrite&) = delete;
};

// KYTY_FAST_GUEST_READ=0 always takes the locks; =verify also checks each cached translation
// against a locked lookup.
static int FastGuestReadMode() {
	static const int mode = [] {
		const char* value = std::getenv("KYTY_FAST_GUEST_READ");
		if (value == nullptr) {
			return 1;
		}
		return std::string_view(value) == "0" ? 0 : std::string_view(value) == "verify" ? 2 : 1;
	}();
	return mode;
}

class GuestBackingStore {
public:
	enum class FailureReason {
		None,
		FixedMapViewFailed,
		PlaceholderMapFailed,
		ProtectFailed,
		AddressRangeUnavailable,
	};

	explicit GuestBackingStore(uint64_t size): m_size(size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		m_handle = CreateFileMappingA(
		    INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_COMMIT,
		    static_cast<DWORD>(m_size >> 32u), static_cast<DWORD>(m_size & 0xffffffffu), nullptr);
		if (m_handle == nullptr) {
			EXIT("could not reserve %" PRIu64 " MB for guest direct memory: 0x%08" PRIx32
			     ". Windows commits\n"
			     "this up front, so the paging file is usually what needs to be larger.\n",
			     m_size / (1024U * 1024U), static_cast<uint32_t>(GetLastError()));
		}
		m_backing_base = static_cast<uint8_t*>(
		    MapViewOfFile(m_handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, m_size));
		if (m_backing_base == nullptr) {
			EXIT("direct-memory backing: writable alias failed: 0x%08" PRIx32 "\n",
			     static_cast<uint32_t>(GetLastError()));
		}
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#if defined(__APPLE__)
		{
			char shm_name[64];
			snprintf(shm_name, sizeof(shm_name), "/kyty-dm-%d-%llx", static_cast<int>(getpid()),
			         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(this)));
			m_fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
			if (m_fd >= 0) {
				shm_unlink(shm_name);
			}
		}
#else
		m_fd = static_cast<int>(syscall(SYS_memfd_create, "KytyDirectMemory", 0));
#endif
		if (m_fd < 0) {
			EXIT("direct-memory backing: shared fd creation failed: %d\n", errno);
		}
		if (ftruncate(m_fd, static_cast<off_t>(m_size)) != 0) {
			EXIT("direct-memory backing: ftruncate to %" PRIu64 " MB failed: %d\n",
			     m_size / (1024U * 1024U), errno);
		}
		auto* backing = mmap(nullptr, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
		if (backing == MAP_FAILED) {
			EXIT("direct-memory backing: writable alias failed: %d\n", errno);
		}
		m_backing_base = static_cast<uint8_t*>(backing);
#endif
	}

	~GuestBackingStore() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		for (const auto& [_, entry]: m_maps) {
			UnmapViewPreservePlaceholder(reinterpret_cast<void*>(entry.vaddr), entry.size);
		}
		UnmapViewOfFile(m_backing_base);
		CloseHandle(m_handle);
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		for (const auto& [_, entry]: m_maps) {
			EXIT_IF(
			    !UnmapViewPreservePlaceholder(reinterpret_cast<void*>(entry.vaddr), entry.size));
		}
		munmap(m_backing_base, m_size);
		close(m_fd);
#endif
	}

	KYTY_CLASS_NO_COPY(GuestBackingStore);

	static const char* GetFailureReasonName(FailureReason reason) {
		switch (reason) {
			case FailureReason::None: return "n/a";
			case FailureReason::FixedMapViewFailed: return "fixed-map-view-failed";
			case FailureReason::PlaceholderMapFailed: return "placeholder-map-failed";
			case FailureReason::ProtectFailed: return "protect-failed";
			case FailureReason::AddressRangeUnavailable: return "address-range-unavailable";
			default: return "unknown";
		}
	}

	[[nodiscard]] uint64_t GetBackingBase() const {
		return reinterpret_cast<uint64_t>(m_backing_base);
	}

	[[nodiscard]] uint64_t GetBackingSize() const { return m_size; }

	void ZeroRange(uint64_t backing_offset, uint64_t size) {
		std::lock_guard lock(m_mutex);
		const TranslationWrite translation_write;
		// Guest views of the zeroed range change too.
		g_remap_generation.fetch_add(1, std::memory_order_relaxed);
		for (const auto& [_, entry]: m_maps) {
			const auto begin = std::max(backing_offset, entry.backing_offset);
			const auto end   = std::min(backing_offset + size, entry.backing_offset + entry.size);
			if (begin < end) {
				LogAliasWrite(entry.vaddr + (begin - entry.backing_offset), end - begin);
			}
		}
		std::memset(m_backing_base + backing_offset, 0, size);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		g_test_backing_bytes_zeroed += size;
#endif
	}

	void WriteBacking(uint64_t vaddr, const void* data, uint64_t size) {
		LogAliasWrite(vaddr, size);
		// Skip unbacked gaps, mirroring sparse reads that zero-fill them.
		std::lock_guard lock(m_mutex);
		const TranslationWrite translation_write;
		const auto*     source = static_cast<const uint8_t*>(data);
		ForEachSparseRange(
		    m_maps, vaddr, size,
		    [&](const auto& entry, uint64_t start, uint64_t bytes) {
			    std::memcpy(m_backing_base + (entry.backing_offset + start - entry.vaddr),
			                source + (start - vaddr), bytes);
		    },
		    [](uint64_t, uint64_t) {});
	}

	// One aligned 4- or 8-byte store (a label: the guest may see it and reuse the label at
	// once, so the bytes must not be written twice as memcpy may).
	bool StoreLabel(uint64_t vaddr, uint64_t value, uint64_t size) {
		if ((size != 4 && size != 8) || (vaddr & (size - 1)) != 0) {
			return false;
		}
		LogAliasWrite(vaddr, size);
		std::shared_lock lock(m_mutex);
		auto             it = m_maps.upper_bound(vaddr);
		if (it == m_maps.begin()) {
			return false;
		}
		--it;
		const auto& entry = it->second;
		if (vaddr < entry.vaddr || vaddr + size > entry.vaddr + entry.size) {
			return false;
		}
		auto* target = m_backing_base + (entry.backing_offset + vaddr - entry.vaddr);
		if (size == 4) {
			*reinterpret_cast<volatile uint32_t*>(target) = static_cast<uint32_t>(value);
		} else {
			*reinterpret_cast<volatile uint64_t*>(target) = value;
		}
		return true;
	}

	// Fills [vaddr, vaddr + size) with a 32-bit value through the backing alias (as the guest
	// mapping's std::fill did).
	// False (nothing written) unless backing mappings hold all of it.
	bool FillDwords(uint64_t vaddr, uint64_t size, uint32_t value) {
		if ((vaddr & 3u) != 0 || (size & 3u) != 0) {
			return false;
		}
		std::shared_lock lock(m_mutex);
		bool             complete = true;
		ForEachSparseRange(
		    m_maps, vaddr, size, [](const MapEntry&, uint64_t, uint64_t) {},
		    [&](uint64_t, uint64_t) { complete = false; });
		if (!complete) {
			return false;
		}
		LogAliasWrite(vaddr, size);
		ForEachSparseRange(
		    m_maps, vaddr, size,
		    [&](const MapEntry& entry, uint64_t start, uint64_t bytes) {
			    auto* target = reinterpret_cast<uint32_t*>(
			        m_backing_base + (entry.backing_offset + start - entry.vaddr));
			    std::fill(target, target + bytes / sizeof(uint32_t), value);
		    },
		    [](uint64_t, uint64_t) {});
		return true;
	}

	// Copies [vaddr, vaddr + size) without locks when one backing mapping holds all of it, through
	// a translation this thread looked up earlier. False: take the locked path.
	bool TryReadFast(uint64_t vaddr, void* data, uint64_t size) {
		const int mode = FastGuestReadMode();
		if (mode == 0 || size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (g_test_backing_read != nullptr) {
			return false; // the test counts reads on the locked path
		}
#endif
		struct Translation {
			uint64_t       seq   = 1; // odd: empty
			uint64_t       begin = 0;
			uint64_t       end   = 0;
			const uint8_t* host  = nullptr; // backing byte of `begin`
		};
		static thread_local std::array<Translation, 4> cache;
		static thread_local uint32_t                   next = 0;
		const auto seq = g_translation_seq.load(std::memory_order_acquire);
		if ((seq & 1u) != 0) {
			return false;
		}
		const Translation* found = nullptr;
		for (const auto& entry: cache) {
			if (entry.seq == seq && vaddr >= entry.begin && vaddr + size <= entry.end) {
				found = &entry;
				break;
			}
		}
		if (found == nullptr) {
			std::shared_lock lock(m_mutex);
			// Writers change the sequence only under the exclusive lock.
			if (g_translation_seq.load(std::memory_order_relaxed) != seq) {
				return false;
			}
			auto it = m_maps.upper_bound(vaddr);
			if (it == m_maps.begin()) {
				return false;
			}
			--it;
			const auto& entry = it->second;
			if (vaddr < entry.vaddr || vaddr + size > entry.vaddr + entry.size) {
				return false;
			}
			auto& slot = cache[next++ % cache.size()];
			slot       = {seq, entry.vaddr, entry.vaddr + entry.size,
			              m_backing_base + entry.backing_offset};
			found      = &slot;
		} else if (mode == 2) {
			std::shared_lock lock(m_mutex);
			if (g_translation_seq.load(std::memory_order_relaxed) == seq) {
				auto it = m_maps.upper_bound(vaddr);
				EXIT_IF(it == m_maps.begin());
				--it;
				const auto& entry = it->second;
				if (entry.vaddr != found->begin || entry.vaddr + entry.size != found->end ||
				    m_backing_base + entry.backing_offset != found->host) {
					EXIT("KYTY_FAST_GUEST_READ=verify: stale translation for 0x%016" PRIx64 "\n",
					     vaddr);
				}
			}
		}
		std::memcpy(data, found->host + (vaddr - found->begin), size);
		std::atomic_thread_fence(std::memory_order_acquire);
		return g_translation_seq.load(std::memory_order_relaxed) == seq;
	}

	bool TryReadBacking(uint64_t vaddr, void* data, uint64_t size) {
		if (data == nullptr || size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		if (TryReadFast(vaddr, data, size)) {
			return true;
		}

		std::shared_lock lock(m_mutex);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (g_test_backing_read != nullptr) {
			g_test_backing_read(vaddr, size);
		}
#endif
		const auto end   = vaddr + size;
		auto       first = m_maps.upper_bound(vaddr);
		if (first == m_maps.begin()) {
			return false;
		}
		--first;
		auto current = vaddr;
		for (auto mapping = first; current < end; ++mapping) {
			if (mapping == m_maps.end() || current < mapping->second.vaddr ||
			    current >= mapping->second.vaddr + mapping->second.size) {
				return false;
			}
			current = std::min(end, mapping->second.vaddr + mapping->second.size);
		}

		auto* destination = static_cast<uint8_t*>(data);
		current = vaddr;
		for (auto mapping = first; current < end; ++mapping) {
			const auto& entry  = mapping->second;
			const auto  bytes  = std::min(end, entry.vaddr + entry.size) - current;
			const auto  offset = entry.backing_offset + current - entry.vaddr;
			std::memcpy(destination + (current - vaddr), m_backing_base + offset, bytes);
			current += bytes;
		}
		return true;
	}

	template <typename Gap>
	void ReadSparseBacking(uint64_t vaddr, void* data, uint64_t size, Gap&& gap) {
		std::shared_lock lock(m_mutex);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (g_test_backing_read != nullptr) {
			g_test_backing_read(vaddr, size);
		}
#endif
		CopySparseRange(m_maps, vaddr, data, size,
		                [this](const auto& entry, uint64_t start) {
			                return m_backing_base + (entry.backing_offset + start - entry.vaddr);
		                },
		                gap);
	}

	bool ProtectViews(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		std::shared_lock lock(m_mutex);
		bool             ok = true;
		ForEachSparseRange(
		    m_maps, vaddr, size,
		    [&](const auto&, uint64_t start, uint64_t bytes) {
			    ok = ok && VirtualMemory::Protect(start, bytes, mode);
		    },
		    [&](uint64_t, uint64_t) { ok = false; });
		return ok;
	}

	bool MapFixed(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	              VirtualMemory::Mode mode, FailureReason* failure_reason = nullptr) {
		// A remap changes the bytes behind the addresses without a store.
		g_remap_generation.fetch_add(1, std::memory_order_relaxed);
		LogAliasWrite(vaddr, size);
		bool write_protected = false;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (!ReplacePlaceholderView(vaddr, size, backing_offset, mode, failure_reason)) {
			return false;
		}
		std::lock_guard lock(m_mutex);
		const TranslationWrite translation_write;
#else
		std::lock_guard lock(m_mutex);
		const TranslationWrite translation_write;
		if (!MapView(vaddr, size, backing_offset, mode, &write_protected, failure_reason)) {
			return false;
		}
		RemoveMappingsUnlocked(vaddr, size);
#endif
		m_maps.emplace(vaddr, MapEntry {vaddr, size, backing_offset, mode, write_protected});
		// Again once the view is in: a new alias voids labels proven against the old views.
		g_remap_generation.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	bool Unmap(uint64_t vaddr, uint64_t size) {
		const auto            end = vaddr + size;
		std::vector<MapEntry> targets;
		{
			std::lock_guard lock(m_mutex);
			const TranslationWrite translation_write;
			auto            current = vaddr;
			while (current < end) {
				const auto* entry = FindContainingUnlocked(current);
				if (entry == nullptr) {
					return false;
				}
				const auto part_size = std::min(end, entry->vaddr + entry->size) - current;
				targets.push_back({current, part_size,
				                   entry->backing_offset + current - entry->vaddr, entry->mode});
				current += part_size;
			}
		}

		size_t removed  = 0;
		auto   rollback = [&] {
			while (removed != 0) {
				const auto& previous = targets[--removed];
				EXIT_IF(!MapFixed(previous.vaddr, previous.size, previous.backing_offset,
				                  previous.mode));
			}
		};
		for (const auto& entry: targets) {
			if (!UnmapOne(entry.vaddr, entry.size)) {
				rollback();
				return false;
			}
			++removed;
		}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		// Each unmapped file view becomes a separate Windows placeholder. The address-space owner
		// records this contiguous guest span as one free range, so normalize fragmented backing
		// views into one placeholder before publishing it there.
		if (removed > 1 && !CoalescePlaceholders(vaddr, size)) {
			rollback();
			return false;
		}
#endif

		return true;
	}

	bool UnmapOne(uint64_t vaddr, uint64_t size) {
		g_remap_generation.fetch_add(1, std::memory_order_relaxed);
		LogAliasWrite(vaddr, size);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (g_test_backing_store_unmaps_before_failure == 0) {
			g_test_backing_store_unmaps_before_failure = UINT32_MAX;
			return false;
		}
		if (g_test_backing_store_unmaps_before_failure != UINT32_MAX) {
			g_test_backing_store_unmaps_before_failure--;
		}
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		MapEntry old {};
		{
			std::lock_guard lock(m_mutex);
			const TranslationWrite translation_write;
			old = m_maps.extract(std::prev(m_maps.upper_bound(vaddr))).mapped();
		}
		if (!UnmapViewPreservePlaceholder(reinterpret_cast<void*>(old.vaddr), old.size)) {
			std::lock_guard lock(m_mutex);
			const TranslationWrite translation_write;
			m_maps.emplace(old.vaddr, old);
			return false;
		}

		const uint64_t left_size  = vaddr - old.vaddr;
		const uint64_t right_addr = vaddr + size;
		const uint64_t right_size = old.vaddr + old.size - right_addr;
		bool           ok         = true;
		if (left_size != 0) {
			ok = SplitPlaceholder(old.vaddr, left_size) && ok;
		}
		if (right_size != 0) {
			ok = SplitPlaceholder(vaddr, size) && ok;
		}
		if (left_size != 0) {
			ok = MapFixed(old.vaddr, left_size, old.backing_offset, old.mode) && ok;
		}
		if (right_size != 0) {
			ok = MapFixed(right_addr, right_size, old.backing_offset + (right_addr - old.vaddr),
			              old.mode) &&
			     ok;
		}
		if (ok) {
			return true;
		}

		if (left_size != 0 && Contains(old.vaddr, left_size)) {
			EXIT_IF(!UnmapOne(old.vaddr, left_size));
		}
		if (right_size != 0 && Contains(right_addr, right_size)) {
			EXIT_IF(!UnmapOne(right_addr, right_size));
		}
		EXIT_IF(!CoalescePlaceholders(old.vaddr, old.size));
		EXIT_IF(!MapFixed(old.vaddr, old.size, old.backing_offset, old.mode));
		return false;
#else
		// Replace only the requested range so concurrent accesses and GPU watch
		// protections on the surviving mappings remain intact.
		if (!UnmapViewPreservePlaceholder(reinterpret_cast<void*>(vaddr), size)) {
			return false;
		}

		std::lock_guard lock(m_mutex);
		const TranslationWrite translation_write;
		RemoveMappingsUnlocked(vaddr, size);

		return true;
#endif
	}

	[[nodiscard]] bool WriteProtectsViews() const {
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
		return m_write_protection.Enabled();
#else
		return false;
#endif
	}

	// Applies a page-tracker protection to part of the views. On a write-protected view, the
	// view protection changes only with access watches; write watches stay in the page tables.
	bool ProtectTracked(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode,
	                    bool access_changed) {
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
		if (m_write_protection.Enabled() &&
		    (mode == VirtualMemory::Mode::Read || mode == VirtualMemory::Mode::ReadWrite)) {
			std::shared_lock lock(m_mutex);
			const auto       end    = vaddr + size;
			auto             cursor = vaddr;
			auto             it     = m_maps.upper_bound(vaddr);
			if (it != m_maps.begin()) {
				--it;
			}
			const auto protect = [&](uint64_t start, uint64_t bytes, bool write_protected) {
				if (!write_protected) {
					return VirtualMemory::Protect(start, bytes, mode);
				}
				// Write-protect before the view becomes writable, so no write lands unwatched.
				if (access_changed && mode == VirtualMemory::Mode::Read) {
					// Re-protecting would erase the written state of earlier stores to these
					// still-watched pages; hand them to the next scan first.
					KeepAsyncWritesUnlocked(start, bytes);
				}
				return ProtectWritesUnlocked(start, bytes, mode == VirtualMemory::Mode::Read) &&
				       (!access_changed ||
				        VirtualMemory::Protect(start, bytes, VirtualMemory::Mode::ReadWrite));
			};
			for (; it != m_maps.end() && it->second.vaddr < end; ++it) {
				const auto& entry = it->second;
				const auto  start = std::max(cursor, entry.vaddr);
				const auto  stop  = std::min(end, entry.vaddr + entry.size);
				if (stop <= start) {
					continue;
				}
				if ((cursor < start && !protect(cursor, start - cursor, false)) ||
				    !protect(start, stop - start, entry.write_protected)) {
					return false;
				}
				cursor = stop;
			}
			return cursor >= end || protect(cursor, end - cursor, false);
		}
#else
		(void)access_changed;
#endif
		return VirtualMemory::Protect(vaddr, size, mode);
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
	static constexpr uint64_t ASYNC_CHUNK_BITS  = 21; // 2 MiB
	static constexpr uint64_t ASYNC_CHUNK_SIZE  = uint64_t {1} << ASYNC_CHUNK_BITS;
	static constexpr uint64_t ASYNC_CHUNK_LIMIT = uint64_t {1} << 40u; // lower guest range
	static constexpr size_t   ASYNC_CHUNK_COUNT = ASYNC_CHUNK_LIMIT >> ASYNC_CHUNK_BITS;

	[[nodiscard]] bool AsyncWriteProtection() const { return m_write_protection.AsyncEnabled(); }

	// Lock-free: flags only change on the GPU thread, which is the only caller.
	[[nodiscard]] bool IsAsync(uint64_t vaddr, uint64_t size) const noexcept {
		if (m_async_count.load(std::memory_order_relaxed) == 0 || size == 0) {
			return false;
		}
		const auto last = (vaddr + size - 1) >> ASYNC_CHUNK_BITS;
		for (auto chunk = vaddr >> ASYNC_CHUNK_BITS; chunk <= last && chunk < ASYNC_CHUNK_COUNT;
		     chunk++) {
			if (m_async_chunks[chunk].load(std::memory_order_relaxed) != 0) {
				return true;
			}
		}
		return false;
	}

	// Moves the write-protected views of a 2 MiB chunk to asynchronous write protection. The
	// next ScanAsyncWrites reports all of its pages as written, which covers writes that land
	// while the chunk changes context.
	bool PromoteChunk(uint64_t chunk) {
		if (!m_write_protection.AsyncEnabled() || chunk >= ASYNC_CHUNK_COUNT) {
			return false;
		}
		std::lock_guard lock(m_mutex);
		const TranslationWrite translation_write;
		if (m_async_chunks[chunk].load(std::memory_order_relaxed) != 0) {
			return false;
		}
		const auto begin = chunk << ASYNC_CHUNK_BITS;
		const auto end   = begin + ASYNC_CHUNK_SIZE;
		bool       moved = false;
		// KYTY_ASYNC_WP_DRYRUN: track the chunk as if asynchronous but keep its views synchronous
		// (to check KYTY_ASYNC_WP_VERIFY itself against the faulting path).
		static const bool dry_run = std::getenv("KYTY_ASYNC_WP_DRYRUN") != nullptr;
		ForEachSparseRange(
		    m_maps, begin, ASYNC_CHUNK_SIZE,
		    [&](const MapEntry& entry, uint64_t start, uint64_t bytes) {
			    if (entry.write_protected) {
				    moved = dry_run || m_write_protection.MakeAsync(start, bytes) || moved;
			    }
		    },
		    [](uint64_t, uint64_t) {});
		(void)end;
		if (!moved) {
			return false;
		}
		// Published under the unique lock: protections and new views route by this flag.
		m_async_chunks[chunk].store(1, std::memory_order_relaxed);
		m_async_count.fetch_add(1, std::memory_order_relaxed);
		m_async_list.insert(std::upper_bound(m_async_list.begin(), m_async_list.end(), chunk),
		                    chunk);
		return true;
	}

	// Whether one page lies in a write-protected view of an asynchronous chunk (so its guest
	// stores leave a mark PAGEMAP_SCAN reports).
	[[nodiscard]] bool AsyncPageTrackedUnlocked(uint64_t page) const {
		static const bool dry_run = std::getenv("KYTY_ASYNC_WP_DRYRUN") != nullptr;
		if (dry_run || !IsAsync(page, 0x1000)) {
			return false;
		}
		bool tracked = true;
		ForEachSparseRange(
		    m_maps, page, 0x1000,
		    [&](const MapEntry& entry, uint64_t, uint64_t) { tracked = tracked && entry.write_protected; },
		    [&](uint64_t, uint64_t) { tracked = false; });
		return tracked;
	}

	// Write-protects one asynchronously tracked page now (false if it is not one) and returns
	// a token for AsyncPageUntouched. Stores found on it go to the next ScanAsyncWrites.
	bool ProtectAsyncPage(uint64_t page, uint64_t& token) {
		std::shared_lock lock(m_mutex);
		if (!AsyncPageTrackedUnlocked(page)) {
			return false;
		}
		std::lock_guard kept_lock(m_kept_mutex);
		// A store through another guest view of the same bytes leaves this view's page clean,
		// so no proof for aliased pages. (A later alias advances the remap generation.)
		const auto generation = g_remap_generation.load(std::memory_order_relaxed);
		auto&      alias      = m_alias_memo[(page >> 12u) % m_alias_memo.size()];
		if (alias.page != page || alias.generation != generation) {
			alias = {page, generation, AliasedUnlocked(page)};
		}
		if (alias.aliased) {
			return false;
		}
		// When the page still holds a current token, its marks were cleared when that token was
		// made, so no scan: stores since then stay marked, the next peek sees them and collects
		// (conservative), and the next scan reports them. KYTY_LABEL_SCAN_SKIP=0 scans always.
		static const bool skip = [] {
			const char* value = std::getenv("KYTY_LABEL_SCAN_SKIP");
			return value == nullptr || value[0] != '0';
		}();
		if (skip) {
			const auto& slot = m_page_seq[(page >> 12u) % m_page_seq.size()];
			if (slot.page == page && slot.seq > m_seq_floor) {
				token = slot.seq;
				return true;
			}
		}
		m_write_protection.Scan(page, 0x1000, [&](uint64_t begin, uint64_t end) {
			HideWritesLocked(begin, end, true);
		});
		token = PageTokenLocked(page);
		return true;
	}

	// Whether no guest view has stored to the page since ProtectAsyncPage gave the token.
	[[nodiscard]] bool AsyncPageUntouched(uint64_t page, uint64_t token) {
		std::unique_lock<std::mutex> serial;
		if (SerialSubmitScans()) {
			serial = std::unique_lock(m_submit_scan_mutex);
		}
		std::shared_lock lock(m_mutex);
		if (!AsyncPageTrackedUnlocked(page)) {
			return false;
		}
		// Held across both checks: a re-protection between them would hide a store.
		std::lock_guard kept_lock(m_kept_mutex);
		const auto&     slot = m_page_seq[(page >> 12u) % m_page_seq.size()];
		if (slot.page != page || slot.seq != token || token <= m_seq_floor) {
			return false;
		}
		bool written = false;
		m_write_protection.Scan(page, 0x1000, [&](uint64_t, uint64_t) { written = true; }, false);
		return !written;
	}

	// KYTY_ASYNC_WP_VERIFY: pages written since the last ScanAsyncWrites, without consuming them.
	void PeekAsyncWrites(std::vector<std::pair<uint64_t, uint64_t>>& written) {
		std::shared_lock lock(m_mutex);
		for (const auto chunk: m_async_list) {
			m_write_protection.Scan(
			    chunk << ASYNC_CHUNK_BITS, ASYNC_CHUNK_SIZE,
			    [&](uint64_t begin, uint64_t end) { written.emplace_back(begin, end); }, false);
		}
		// Kept writes are pending report too: the next collection takes them. (A submit scan
		// clears marks and keeps them in one hold of m_kept_mutex.)
		lock.unlock();
		WaitSerialSubmitScan();
		std::lock_guard kept_lock(m_kept_mutex);
		written.insert(written.end(), m_kept_writes.begin(), m_kept_writes.end());
	}

	void AsyncRanges(std::vector<std::pair<uint64_t, uint64_t>>& ranges) {
		std::shared_lock lock(m_mutex);
		for (const auto chunk: m_async_list) {
			ranges.emplace_back(chunk << ASYNC_CHUNK_BITS, (chunk + 1) << ASYNC_CHUNK_BITS);
		}
	}

	[[nodiscard]] size_t AsyncChunkCount() {
		std::shared_lock lock(m_mutex);
		return m_async_list.size();
	}

	// Appends the page runs of asynchronous chunks written since the last scan, and
	// re-protects them. The caller handles them after the lock is released. Chunks the filter
	// rejects (nothing there needs its writes) are left as they are: a page that gains a write
	// watcher is write-protected then, so stores before that need no report.
	// Kept writes are taken after the scan: a store a submit scan took from the page tables is
	// kept in the same hold of m_kept_mutex (ScanAsyncWritesAtSubmit), so the first collection
	// that ends after it reports it.
	void ScanAsyncWrites(std::vector<std::pair<uint64_t, uint64_t>>& written,
	                     bool (*wanted)(uint64_t begin, uint64_t end) = nullptr) {
		ScanAsyncChunks(written, wanted);
		TakeKeptWrites(written);
	}

	// Appends the kept writes (re-protections and submit scans).
	void TakeKeptWrites(std::vector<std::pair<uint64_t, uint64_t>>& written) {
		WaitSerialSubmitScan();
		std::lock_guard kept_lock(m_kept_mutex);
		// KYTY_ASYNC_WP_VERIFY_DROP_KEPT=<n> (test only): silently drops every n-th kept write, to
		// show the verifier reports such a loss.
		static const uint64_t drop_every = [] {
			const char* value = std::getenv("KYTY_ASYNC_WP_VERIFY_DROP_KEPT");
			return value != nullptr && g_log_alias_writes ? std::strtoull(value, nullptr, 10) : 0;
		}();
		static uint64_t taken = 0;
		for (const auto& entry: m_kept_writes) {
			if (drop_every != 0 && ++taken % drop_every == 0) {
				continue;
			}
			written.push_back(entry);
			if (g_log_alias_writes) {
				g_verify_kept_taken.push_back(entry);
			}
		}
		m_kept_writes.clear();
	}

	// KYTY_SUBMIT_SCAN: the collection for a submission's start, done by the submitting guest
	// thread before it enqueues, so the next collection on the GPU thread reports what it finds,
	// whichever submission that collection is for. Two passes, so that no GPU-thread reader of
	// written marks or label tokens ever waits for it:
	//  1. find the written pages, leaving their marks (no lock the GPU thread waits on long);
	//  2. in one hold of m_kept_mutex: advance those pages' tokens, then re-protect exactly those
	//     pages and keep what that finds. A mark is only cleared after its page's token moved, and
	//     cleared marks become kept writes atomically, for every reader that holds m_kept_mutex
	//     (AsyncPageUntouched, TakeKeptWrites, PeekAsyncWrites). Stores to other pages keep their
	//     marks for a later scan.
	void ScanAsyncWritesAtSubmit(bool (*wanted)(uint64_t begin, uint64_t end)) {
		static thread_local std::vector<std::pair<uint64_t, uint64_t>> found;
		found.clear();
		if (SerialSubmitScans()) {
			// #90's scheme: one pass under a mutex the GPU thread's readers wait for.
			std::lock_guard submit_lock(m_submit_scan_mutex);
			ScanAsyncChunks(found, wanted);
			std::lock_guard kept_lock(m_kept_mutex);
			for (const auto& [begin, end]: found) {
				HideWritesLocked(begin, end, true);
			}
			return;
		}
		ScanAsyncChunks(found, wanted, false);
		if (found.empty()) {
			return;
		}
		std::shared_lock lock(m_mutex);
		std::lock_guard  kept_lock(m_kept_mutex);
		for (const auto& [begin, end]: found) {
			HideWritesLocked(begin, end, false);
		}
		for (const auto& [begin, end]: found) {
			m_write_protection.Scan(begin, end - begin, [&](uint64_t written_begin, uint64_t written_end) {
				m_kept_writes.emplace_back(written_begin, written_end);
			});
		}
	}

	// KYTY_SUBMIT_SCAN_SERIAL=1: the earlier scheme (for A/B).
	static bool SerialSubmitScans() {
		static const bool serial = [] {
			const char* value = std::getenv("KYTY_SUBMIT_SCAN_SERIAL");
			return value != nullptr && value[0] == '1';
		}();
		return serial;
	}
	void WaitSerialSubmitScan() {
		if (SerialSubmitScans()) {
			std::lock_guard submit_lock(m_submit_scan_mutex);
		}
	}

	void ScanAsyncChunks(std::vector<std::pair<uint64_t, uint64_t>>& written,
	                     bool (*wanted)(uint64_t begin, uint64_t end), bool protect = true) {
		std::shared_lock lock(m_mutex);
		const auto want = [&](uint64_t chunk) {
			return wanted == nullptr ||
			       wanted(chunk << ASYNC_CHUNK_BITS, (chunk + 1) << ASYNC_CHUNK_BITS);
		};
		for (size_t i = 0; i < m_async_list.size();) {
			auto first = m_async_list[i];
			if (!want(first)) {
				i++;
				continue;
			}
			auto last = first;
			while (++i < m_async_list.size() && m_async_list[i] == last + 1 && want(last + 1)) {
				last++;
			}
			m_write_protection.Scan(first << ASYNC_CHUNK_BITS,
			                        (last - first + 1) << ASYNC_CHUNK_BITS,
			                        [&](uint64_t begin, uint64_t end) {
				                        if (!written.empty() && written.back().second == begin) {
					                        written.back().second = end;
				                        } else {
					                        written.emplace_back(begin, end);
				                        }
			                        },
			                        protect);
		}
	}
#endif

	bool Contains(uint64_t vaddr, uint64_t size) {
		std::shared_lock lock(m_mutex);
		const auto      end     = vaddr + size;
		auto            current = vaddr;

		while (current < end) {
			const auto* entry = FindContainingUnlocked(current);
			if (entry == nullptr) {
				return false;
			}
			current = std::min(end, entry->vaddr + entry->size);
		}

		return true;
	}

private:
	struct MapEntry {
		uint64_t            vaddr          = 0;
		uint64_t            size           = 0;
		uint64_t            backing_offset = 0;
		VirtualMemory::Mode mode           = VirtualMemory::Mode::NoAccess;
		// Registered for userfaultfd write protection; watches on other views use mprotect.
		bool write_protected = false;
	};

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
	template <typename Func>
	void ForEachAsyncChunk(uint64_t vaddr, uint64_t size, Func&& func) const {
		if (m_async_list.empty()) {
			return;
		}
		const auto end = vaddr + size;
		for (auto start = vaddr; start < end;) {
			const auto chunk = start >> ASYNC_CHUNK_BITS;
			const auto stop  = std::min(end, (chunk + 1) << ASYNC_CHUNK_BITS);
			if (chunk < ASYNC_CHUNK_COUNT && m_async_chunks[chunk].load(std::memory_order_relaxed) != 0) {
				func(start, stop - start);
			}
			start = stop;
		}
	}

	// Under m_kept_mutex: [begin, end) was written and is write-protected again; report keeps it
	// for the next ScanAsyncWrites.
	void HideWritesLocked(uint64_t begin, uint64_t end, bool report) const {
		if (report) {
			m_kept_writes.emplace_back(begin, end);
		}
		if (end - begin > 64 * 0x1000) {
			m_seq_floor = ++m_seq;
			return;
		}
		for (auto page = begin; page < end; page += 0x1000) {
			m_page_seq[(page >> 12u) % m_page_seq.size()] = {page, ++m_seq};
		}
	}

	// Whether another view maps the bytes behind this page (under m_mutex).
	[[nodiscard]] bool AliasedUnlocked(uint64_t page) const {
		auto it = m_maps.upper_bound(page);
		if (it == m_maps.begin()) {
			return true;
		}
		--it;
		const auto& self = it->second;
		if (page < self.vaddr || page + 0x1000 > self.vaddr + self.size) {
			return true;
		}
		const auto offset = self.backing_offset + (page - self.vaddr);
		for (const auto& [_, entry]: m_maps) {
			if (&entry != &self && entry.backing_offset < offset + 0x1000 &&
			    offset < entry.backing_offset + entry.size) {
				return true;
			}
		}
		return false;
	}

	// Under m_kept_mutex: the page's current token.
	uint64_t PageTokenLocked(uint64_t page) const {
		auto& slot = m_page_seq[(page >> 12u) % m_page_seq.size()];
		if (slot.page != page || slot.seq <= m_seq_floor) {
			slot = {page, ++m_seq};
		}
		return slot.seq;
	}

	void KeepAsyncWritesUnlocked(uint64_t vaddr, uint64_t size) const {
		ForEachAsyncChunk(vaddr, size, [&](uint64_t start, uint64_t bytes) {
			std::lock_guard lock(m_kept_mutex);
			m_write_protection.Scan(start, bytes, [&](uint64_t begin, uint64_t end) {
				HideWritesLocked(begin, end, true);
			});
		});
	}

	// uffd write protection of a registered view range, split by context.
	bool ProtectWritesUnlocked(uint64_t vaddr, uint64_t size, bool write_protect) const {
		static const bool dry_run = std::getenv("KYTY_ASYNC_WP_DRYRUN") != nullptr;
		if (m_async_list.empty() || dry_run) {
			return m_write_protection.Protect(vaddr, size, write_protect);
		}
		const auto end = vaddr + size;
		for (auto start = vaddr; start < end;) {
			const auto chunk = start >> ASYNC_CHUNK_BITS;
			const auto stop  = std::min(end, (chunk + 1) << ASYNC_CHUNK_BITS);
			const bool async = chunk < ASYNC_CHUNK_COUNT &&
			                   m_async_chunks[chunk].load(std::memory_order_relaxed) != 0;
			// In an asynchronous chunk a page stays write-protected after its last watcher leaves:
			// a store there costs no signal, and the protection keeps "not written since" provable
			// (RenderContext::IsCpLabel).
			if (async && write_protect) {
				// Re-protecting would erase written state: the range can include pages already
				// watched (the page manager batches across them), so their stores are kept for
				// the next collection, and page tokens advance.
				std::lock_guard kept_lock(m_kept_mutex);
				m_write_protection.Scan(start, stop - start, [&](uint64_t begin, uint64_t end) {
					HideWritesLocked(begin, end, true);
				});
			} else if (!async && !m_write_protection.Protect(start, stop - start, write_protect, async)) {
				return false;
			}
			start = stop;
		}
		return true;
	}
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	void RemoveMappingsUnlocked(uint64_t vaddr, uint64_t size) {
		const auto end = vaddr + size;
		auto       it = m_maps.upper_bound(vaddr);
		if (it != m_maps.begin()) {
			--it;
		}
		while (it != m_maps.end() && it->first < end) {
			const auto old     = it->second;
			const auto old_end = old.vaddr + old.size;
			if (old_end <= vaddr) {
				++it;
				continue;
			}
			it = m_maps.erase(it);
			if (old.vaddr < vaddr) {
				m_maps.emplace(old.vaddr,
				               MapEntry {old.vaddr, vaddr - old.vaddr, old.backing_offset, old.mode,
				                         old.write_protected});
			}
			if (end < old_end) {
				m_maps.emplace(end,
				               MapEntry {end, old_end - end, old.backing_offset + end - old.vaddr,
				                         old.mode, old.write_protected});
			}
		}
	}
#endif

	const MapEntry* FindContainingUnlocked(uint64_t vaddr) const {
		auto next = m_maps.upper_bound(vaddr);
		if (next == m_maps.begin()) {
			return nullptr;
		}
		const auto& entry = std::prev(next)->second;
		return vaddr < entry.vaddr + entry.size ? &entry : nullptr;
	}

	static void SetFailure(FailureReason* out, FailureReason reason) {
		if (out != nullptr) {
			*out = reason;
		}
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	static DWORD GetProtect(VirtualMemory::Mode mode) {
		switch (mode) {
			case VirtualMemory::Mode::Read: return PAGE_READONLY;
			case VirtualMemory::Mode::Write:
			case VirtualMemory::Mode::ReadWrite: return PAGE_READWRITE;
			case VirtualMemory::Mode::Execute: return PAGE_EXECUTE;
			case VirtualMemory::Mode::ExecuteRead: return PAGE_EXECUTE_READ;
			case VirtualMemory::Mode::ExecuteWrite:
			case VirtualMemory::Mode::ExecuteReadWrite: return PAGE_EXECUTE_READWRITE;
			case VirtualMemory::Mode::NoAccess:
			default: return PAGE_NOACCESS;
		}
	}

	static bool ApplyProtection(void* ptr, uint64_t size, VirtualMemory::Mode mode) {
		DWORD old_protect = 0;
		return VirtualProtect(ptr, size, GetProtect(mode), &old_protect) != 0;
	}

	static bool SplitPlaceholder(uint64_t vaddr, uint64_t size) {
		return VirtualFree(reinterpret_cast<void*>(vaddr), size,
		                   MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != 0;
	}

	bool ReplacePlaceholderView(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	                            VirtualMemory::Mode mode, FailureReason* failure_reason) {
		const DWORD map_protect =
		    mode == VirtualMemory::Mode::NoAccess ? PAGE_READWRITE : GetProtect(mode);
		void* ptr =
		    MapViewOfFile3(m_handle, GetCurrentProcess(), reinterpret_cast<void*>(vaddr),
		                   backing_offset, size, MEM_REPLACE_PLACEHOLDER, map_protect, nullptr, 0);
		if (ptr == nullptr || reinterpret_cast<uint64_t>(ptr) != vaddr) {
			if (ptr != nullptr) {
				UnmapViewOfFile2(GetCurrentProcess(), ptr, MEM_PRESERVE_PLACEHOLDER);
			}
			SetFailure(failure_reason, FailureReason::PlaceholderMapFailed);
			return false;
		}

		if (mode == VirtualMemory::Mode::NoAccess && !ApplyProtection(ptr, size, mode)) {
			UnmapViewOfFile2(GetCurrentProcess(), ptr, MEM_PRESERVE_PLACEHOLDER);
			SetFailure(failure_reason, FailureReason::ProtectFailed);
			return false;
		}
		return true;
	}

	static bool UnmapViewPreservePlaceholder(void* vaddr, uint64_t /*size*/) {
		return UnmapViewOfFile2(GetCurrentProcess(), vaddr, MEM_PRESERVE_PLACEHOLDER) != 0;
	}

	HANDLE m_handle = nullptr;
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	static int GetProtect(VirtualMemory::Mode mode) {
		switch (mode) {
			case VirtualMemory::Mode::Read: return PROT_READ;
			case VirtualMemory::Mode::Write:
			case VirtualMemory::Mode::ReadWrite: return PROT_READ | PROT_WRITE;
			case VirtualMemory::Mode::Execute: return PROT_EXEC;
			case VirtualMemory::Mode::ExecuteRead: return PROT_EXEC | PROT_READ;
			case VirtualMemory::Mode::ExecuteWrite:
			case VirtualMemory::Mode::ExecuteReadWrite: return PROT_EXEC | PROT_READ | PROT_WRITE;
			case VirtualMemory::Mode::NoAccess:
			default: return PROT_NONE;
		}
	}

	bool MapView(uint64_t vaddr, uint64_t size, uint64_t backing_offset, VirtualMemory::Mode mode,
	             bool* write_protected, FailureReason* failure_reason) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (const auto callback = g_test_before_backing_map) {
			g_test_before_backing_map = nullptr;
			callback(vaddr, size);
		}
#endif
		if (mmap(reinterpret_cast<void*>(vaddr), size, GetProtect(mode), MAP_SHARED | MAP_FIXED,
		         m_fd, static_cast<off_t>(backing_offset)) == MAP_FAILED) {
			SetFailure(failure_reason, FailureReason::FixedMapViewFailed);
			return false;
		}
#if !defined(__APPLE__)
		*write_protected = m_write_protection.Enabled() && m_write_protection.Register(vaddr, size);
		if (*write_protected) {
			static const bool dry_run = std::getenv("KYTY_ASYNC_WP_DRYRUN") != nullptr;
			ForEachAsyncChunk(vaddr, size, [&](uint64_t start, uint64_t bytes) {
				// MakeAsync succeeds or stops the emulator: a failed move would lose protection.
				if (!dry_run) {
					(void)m_write_protection.MakeAsync(start, bytes);
				}
			});
		}
#else
		*write_protected = false;
#endif
		return true;
	}

	// Replace the file view atomically instead of exposing an unmapped hole in the
	// process-lifetime guest reservation.
	static bool UnmapViewPreservePlaceholder(void* vaddr, uint64_t size) {
		return mmap(vaddr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) !=
		       MAP_FAILED;
	}

	int m_fd = -1;
#if !defined(__APPLE__)
	GuestWriteProtection m_write_protection;
	// Chunks under asynchronous write protection; written under the unique lock.
	std::unique_ptr<std::atomic<uint8_t>[]> m_async_chunks =
	    std::make_unique<std::atomic<uint8_t>[]>(ASYNC_CHUNK_COUNT);
	std::vector<uint64_t> m_async_list;
	std::atomic<size_t>   m_async_count {0};
	// Writes found before a re-protection, reported by the next ScanAsyncWrites.
	mutable std::mutex                                 m_kept_mutex;
	mutable std::vector<std::pair<uint64_t, uint64_t>> m_kept_writes;
	// Per-page sequence numbers (by hash, under m_kept_mutex) that advance whenever a page's
	// written state is cleared outside ScanAsyncWrites: such a re-protection hides a store from
	// a later "written since?" check, so it voids the page's tokens (AsyncPageUntouched).
	struct PageSeq {
		uint64_t page = 0;
		uint64_t seq  = 0;
	};
	mutable std::array<PageSeq, 4096> m_page_seq {};
	mutable uint64_t                  m_seq       = 0;
	mutable uint64_t                  m_seq_floor = 0; // tokens at or below are void
	struct AliasMemo {
		uint64_t page       = 0;
		uint64_t generation = 0;
		bool     aliased    = true;
	};
	std::array<AliasMemo, 1024> m_alias_memo {}; // under m_kept_mutex
	std::mutex                  m_submit_scan_mutex; // KYTY_SUBMIT_SCAN_SERIAL only
#endif
#endif

	uint8_t*                     m_backing_base = nullptr;
	uint64_t                     m_size         = 0;
	std::shared_mutex            m_mutex;
	std::map<uint64_t, MapEntry> m_maps;
};

class GuestAddressSpace {
public:
	explicit GuestAddressSpace(uint64_t backing_size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		SYSTEM_INFO info {};
		GetSystemInfo(&info);
		m_granularity = info.dwAllocationGranularity;
#endif
		ReserveGuestRegions();
		m_backing = new GuestBackingStore(backing_size);
	}

	~GuestAddressSpace() {
		delete m_backing;
		m_backing = nullptr;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		for (const auto& [start, size]: m_owned) {
			const auto end     = start + size;
			auto       current = start;
			while (current < end) {
				MEMORY_BASIC_INFORMATION info {};
				EXIT_IF(VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof(info)) ==
				        0);
				const auto region_start = reinterpret_cast<uint64_t>(info.BaseAddress);
				const auto region_end   = region_start + static_cast<uint64_t>(info.RegionSize);
				if (info.State == MEM_COMMIT) {
					EXIT_IF(VirtualFree(reinterpret_cast<void*>(region_start), info.RegionSize,
					                    MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0);
					continue;
				}
				if (info.State == MEM_RESERVE) {
					EXIT_IF(VirtualFree(info.AllocationBase, 0, MEM_RELEASE) == 0);
				}
				current = region_end;
			}
		}
#else
		for (const auto& [start, size]: m_owned) {
			munmap(reinterpret_cast<void*>(start), size);
		}
#endif
	}

	KYTY_CLASS_NO_COPY(GuestAddressSpace);

	[[nodiscard]] uint64_t GetGranularity() const { return m_granularity; }
	[[nodiscard]] uint64_t GetBackingBase() const { return m_backing->GetBackingBase(); }
	[[nodiscard]] uint64_t GetBackingSize() const { return m_backing->GetBackingSize(); }
	[[nodiscard]] bool     WriteProtectsViews() const { return m_backing->WriteProtectsViews(); }

	void ZeroBacking(uint64_t backing_offset, uint64_t size) {
		m_backing->ZeroRange(backing_offset, size);
	}

	[[nodiscard]] bool Owns(uint64_t vaddr, uint64_t size) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		std::shared_lock lock(m_mutex);
		return std::any_of(m_owned.begin(), m_owned.end(), [vaddr, size](const auto& range) {
			return vaddr >= range.first && vaddr + size <= range.first + range.second;
		});
	}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	[[nodiscard]] bool OverlapsOwned(uint64_t vaddr, uint64_t size) {
		std::shared_lock lock(m_mutex);
		return std::any_of(m_owned.begin(), m_owned.end(), [vaddr, size](const auto& range) {
			return VirtualRangesOverlap(vaddr, size, range.first, range.second);
		});
	}
#endif

	// Writes the backed bytes of a range that holds no private mappings.
	bool TryWriteBacking(uint64_t vaddr, const void* data, uint64_t size) {
		if (!Owns(vaddr, size)) {
			return false;
		}
		std::shared_lock lock(m_mutex);
		if (OverlapsPrivateUnlocked(vaddr, size)) {
			return false;
		}
		m_backing->WriteBacking(vaddr, data, size);
		return true;
	}

	bool TryStoreLabelBacking(uint64_t vaddr, uint64_t value, uint64_t size) {
		if (!Owns(vaddr, size)) {
			return false;
		}
		std::shared_lock lock(m_mutex);
		return !OverlapsPrivateUnlocked(vaddr, size) && m_backing->StoreLabel(vaddr, value, size);
	}

	bool TryFillBacking(uint64_t vaddr, uint64_t size, uint32_t value) {
		if (!Owns(vaddr, size)) {
			return false;
		}
		std::shared_lock lock(m_mutex);
		return !OverlapsPrivateUnlocked(vaddr, size) && m_backing->FillDwords(vaddr, size, value);
	}

	bool TryReadBacking(uint64_t vaddr, void* data, uint64_t size) {
		return m_backing->TryReadBacking(vaddr, data, size);
	}

	template <bool copy_private>
	bool CopySparseMemory(uint64_t vaddr, void* data, uint64_t size) {
		// One backing mapping holds the range: owned, not private, no gaps.
		if (data != nullptr && m_backing->TryReadFast(vaddr, data, size)) {
			return true;
		}
		if (data == nullptr || !Owns(vaddr, size)) {
			return false;
		}
		std::shared_lock lock(m_mutex);
		if constexpr (!copy_private) {
			if (OverlapsPrivateUnlocked(vaddr, size)) {
				return false;
			}
		}
		m_backing->ReadSparseBacking(
		    vaddr, data, size, [&](uint64_t start, uint8_t* destination, uint64_t bytes) {
			    const auto zero = [](uint64_t, uint8_t* output, uint64_t length) {
				    std::memset(output, 0, length);
			    };
			    if constexpr (copy_private) {
				    // Backing gaps can contain private mappings or reservations only.
				    CopySparseRange(
				        m_mapped, start, destination, bytes,
				        [](const auto&, uint64_t address) {
					        return reinterpret_cast<const void*>(address);
				        },
				        zero);
			    } else {
				    zero(start, destination, bytes);
			    }
		    });
		return true;
	}

	bool BackingContains(uint64_t vaddr, uint64_t size) { return m_backing->Contains(vaddr, size); }

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
	[[nodiscard]] GuestBackingStore& Backing() { return *m_backing; }
#endif

	bool Protect(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		std::lock_guard lock(m_mutex);
		return ProtectMappedUnlocked(vaddr, size, mode);
	}

	bool ProtectTransient(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode,
	                      bool access_changed) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		// Tracker protections only read m_mapped; the page tracker orders them per page.
		std::shared_lock lock(m_mutex);
		return ForEachMappedPartUnlocked(
		    vaddr, size, [&](const MappedRegion& region, uint64_t start, uint64_t bytes) {
			    return region.type == MappingType::Backing
			               ? m_backing->ProtectTracked(start, bytes, mode, access_changed)
			               : VirtualMemory::Protect(start, bytes, mode);
		    });
	}

	bool MapBacking(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	                VirtualMemory::Mode               mode,
	                GuestBackingStore::FailureReason* failure_reason = nullptr) {
		std::lock_guard lock(m_mutex);
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		const bool replacing = ContainsMappedUnlocked(vaddr, size, MappingType::Backing);
#else
		constexpr bool replacing = false;
#endif
		if (!replacing && !ConsumeUnlocked(vaddr, size)) {
			if (failure_reason != nullptr) {
				*failure_reason = GuestBackingStore::FailureReason::AddressRangeUnavailable;
			}
			return false;
		}
		const auto host_mode = VirtualMemory::IsExecute(mode)
		                           ? VirtualMemory::Mode::ExecuteReadWrite
		                           : VirtualMemory::Mode::ReadWrite;
		if (m_backing->MapFixed(vaddr, size, backing_offset, host_mode, failure_reason)) {
			if (!replacing) {
				AddMappedUnlocked(vaddr, size, MappingType::Backing);
			}
			return true;
		}
		if (!replacing) {
			AddFreeUnlocked(vaddr, size);
		}
		return false;
	}

	bool UnmapBacking(uint64_t vaddr, uint64_t size) {
		std::lock_guard lock(m_mutex);
		if (!ContainsMappedUnlocked(vaddr, size, MappingType::Backing)) {
			return false;
		}
		if (!m_backing->Unmap(vaddr, size)) {
			return false;
		}
		RemoveMappedUnlocked(vaddr, size);
		AddFreeUnlocked(vaddr, size);
		return true;
	}

	uint64_t FindFreeAligned(uint64_t search_start, uint64_t search_end, uint64_t size,
	                         uint64_t alignment) {
		alignment = std::max<uint64_t>(alignment, PageSize());
		std::shared_lock lock(m_mutex);
		return FindFreeAlignedUnlocked(search_start, search_end, size, alignment);
	}

	bool ContainsFree(uint64_t vaddr, uint64_t size) {
		std::shared_lock lock(m_mutex);
		return FindContaining(vaddr, size) != m_free.end();
	}

	bool Commit(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		std::lock_guard lock(m_mutex);
		if (!ConsumeUnlocked(vaddr, size)) {
			return false;
		}
		const auto host_mode = VirtualMemory::IsExecute(mode)
		                           ? VirtualMemory::Mode::ExecuteReadWrite
		                           : VirtualMemory::Mode::ReadWrite;

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		auto* ptr = VirtualAlloc2(GetCurrentProcess(), reinterpret_cast<void*>(vaddr), size,
		                          MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
		                          GetProtect(host_mode), nullptr, 0);
		if (ptr == nullptr || reinterpret_cast<uint64_t>(ptr) != vaddr) {
			if (ptr != nullptr) {
				VirtualFree(ptr, 0, MEM_RELEASE);
			}
			AddFreeUnlocked(vaddr, size);
			return false;
		}
		AddMappedUnlocked(vaddr, size, MappingType::Private);
		return true;
#else
		if (VirtualMemory::Protect(vaddr, size, host_mode)) {
			AddMappedUnlocked(vaddr, size, MappingType::Private);
			return true;
		}
		AddFreeUnlocked(vaddr, size);
		return false;
#endif
	}

	bool ReleaseCommitted(uint64_t vaddr, uint64_t size) {
		if (!IsPageAligned(vaddr) || !IsPageAligned(size)) {
			return false;
		}
		std::lock_guard lock(m_mutex);
		if (!ContainsMappedUnlocked(vaddr, size, MappingType::Private)) {
			return false;
		}
		const auto end     = vaddr + size;
		auto       current = vaddr;
		std::vector<std::pair<uint64_t, uint64_t>> regions;
		while (current < end) {
			const auto it       = FindMappedContainingUnlocked(current);
			const auto part_end = std::min(end, it->second.vaddr + it->second.size);
			regions.emplace_back(current, part_end - current);
			current = part_end;
		}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		for (const auto& [region_vaddr, region_size]: regions) {
			EXIT_IF(VirtualFree(reinterpret_cast<void*>(region_vaddr), region_size,
			                    MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0);
		}
		// ReleaseCommitted also publishes the aggregate span as one logical free range.
		EXIT_IF(regions.size() > 1 && !CoalescePlaceholders(vaddr, size));
#else
		for (const auto& [region_vaddr, region_size]: regions) {
			RestorePlaceholder(region_vaddr, region_size);
		}
#endif
		RemoveMappedUnlocked(vaddr, size);
		AddFreeUnlocked(vaddr, size);
		return true;
	}

	bool SelfTest() {
		auto addr = FindFreeAligned(HOST_SYSTEM_MANAGED_MIN, HOST_SYSTEM_MANAGED_MAX + 1u,
		                            PageSize(), PageSize());
		if (addr == 0) {
			addr = FindFreeAligned(HOST_USER_MIN, HOST_USER_MAX + 1u, PageSize(), PageSize());
		}
		if (addr == 0) {
			LOGF_COLOR(Log::Color::Red,
			           "\t guest address-space self-test: 16 KiB range unavailable\n");
			return false;
		}
		if (!Commit(addr, PageSize(), VirtualMemory::Mode::ReadWrite)) {
			LOGF_COLOR(Log::Color::Red, "\t guest address-space self-test: commit failed\n");
			return false;
		}

		auto* ptr = reinterpret_cast<uint64_t*>(addr);
		*ptr      = 0x4b59545950484f4cull; // "KYTYPHOL"

		if (!ReleaseCommitted(addr, PageSize())) {
			LOGF_COLOR(Log::Color::Red,
			           "\t guest address-space self-test: release-committed failed\n");
			return false;
		}

		LOGF_COLOR(Log::Color::Green, "\t guest address-space self-test: ok\n");
		return true;
	}

private:
	enum class MappingType { Backing, Private };

	struct MappedRegion {
		uint64_t    vaddr = 0;
		uint64_t    size  = 0;
		MappingType type  = MappingType::Private;
	};

	static constexpr uint64_t PageSize() { return 0x4000; }

	static uint64_t AlignUp(uint64_t value, uint64_t alignment) {
		const auto remainder = value % alignment;
		return remainder != 0 ? value + (alignment - remainder) : value;
	}

	static uint64_t AlignDown(uint64_t value, uint64_t alignment) {
		return value & ~(alignment - 1u);
	}

	[[nodiscard]] static bool IsPageAligned(uint64_t value) {
		return (value & (PageSize() - 1u)) == 0;
	}

	[[nodiscard]] bool IsGranularityAligned(uint64_t value) const {
		return (value & (m_granularity - 1u)) == 0;
	}

	// Calls func(region, start, size) on each mapped part of the range until it returns false.
	template <typename Func>
	bool ForEachMappedPartUnlocked(uint64_t vaddr, uint64_t size, Func&& func) {
		const auto end = vaddr + size;
		auto       it  = m_mapped.upper_bound(vaddr);
		if (it != m_mapped.begin()) {
			--it;
		}
		for (; it != m_mapped.end() && it->second.vaddr < end; ++it) {
			const auto& region      = it->second;
			const auto  range_start = std::max(vaddr, region.vaddr);
			const auto  range_end   = std::min(end, region.vaddr + region.size);
			if (range_start < range_end && !func(region, range_start, range_end - range_start)) {
				return false;
			}
		}
		return true;
	}

	bool ProtectMappedUnlocked(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		return ForEachMappedPartUnlocked(
		    vaddr, size, [&](const MappedRegion& region, uint64_t start, uint64_t bytes) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			    // Rollback can split a backing view without changing the owner interval.
			    // Windows protection calls must stay within each actual view.
			    if (region.type == MappingType::Backing) {
				    return m_backing->ProtectViews(start, bytes, mode);
			    }
#else
			    (void)region;
#endif
			    return VirtualMemory::Protect(start, bytes, mode);
		    });
	}

	bool ConsumeUnlocked(uint64_t vaddr, uint64_t size) {
		const auto it = FindContaining(vaddr, size);
		if (it == m_free.end() || !SplitUnlocked(it, vaddr, size)) {
			return false;
		}
		m_free.erase(vaddr);
		return true;
	}

	std::map<uint64_t, MappedRegion>::iterator FindMappedContainingUnlocked(uint64_t vaddr) {
		auto next = m_mapped.upper_bound(vaddr);
		if (next == m_mapped.begin()) {
			return m_mapped.end();
		}
		auto it = std::prev(next);
		return vaddr < it->second.vaddr + it->second.size ? it : m_mapped.end();
	}

	bool ContainsMappedUnlocked(uint64_t vaddr, uint64_t size, MappingType type) {
		const auto end     = vaddr + size;
		auto       current = vaddr;
		while (current < end) {
			const auto it = FindMappedContainingUnlocked(current);
			if (it == m_mapped.end() || it->second.type != type) {
				return false;
			}
			current = std::min(end, it->second.vaddr + it->second.size);
		}
		return true;
	}

	[[nodiscard]] bool OverlapsPrivateUnlocked(uint64_t vaddr, uint64_t size) const {
		auto mapping = m_mapped.upper_bound(vaddr);
		if (mapping != m_mapped.begin()) {
			--mapping;
		}
		for (; mapping != m_mapped.end() && mapping->second.vaddr < vaddr + size; ++mapping) {
			const auto& region = mapping->second;
			if (region.type == MappingType::Private && region.vaddr + region.size > vaddr) {
				return true;
			}
		}
		return false;
	}

	void AddMappedUnlocked(uint64_t vaddr, uint64_t size, MappingType type) {
		m_mapped.emplace(vaddr, MappedRegion {vaddr, size, type});
	}

	void RemoveMappedUnlocked(uint64_t vaddr, uint64_t size) {
		const auto end     = vaddr + size;
		auto       current = vaddr;
		while (current < end) {
			const auto it           = FindMappedContainingUnlocked(current);
			const auto original     = it->second;
			const auto original_end = original.vaddr + original.size;
			const auto part_end     = std::min(end, original_end);
			m_mapped.erase(it);
			if (original.vaddr < current) {
				AddMappedUnlocked(original.vaddr, current - original.vaddr, original.type);
			}
			if (part_end < original_end) {
				AddMappedUnlocked(part_end, original_end - part_end, original.type);
			}
			current = part_end;
		}
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	static DWORD GetProtect(VirtualMemory::Mode mode) {
		switch (mode) {
			case VirtualMemory::Mode::Read: return PAGE_READONLY;
			case VirtualMemory::Mode::Write:
			case VirtualMemory::Mode::ReadWrite: return PAGE_READWRITE;
			case VirtualMemory::Mode::Execute: return PAGE_EXECUTE;
			case VirtualMemory::Mode::ExecuteRead: return PAGE_EXECUTE_READ;
			case VirtualMemory::Mode::ExecuteWrite:
			case VirtualMemory::Mode::ExecuteReadWrite: return PAGE_EXECUTE_READWRITE;
			case VirtualMemory::Mode::NoAccess:
			default: return PAGE_NOACCESS;
		}
	}
#endif

	void ReserveGuestRegions() {
		const std::array<std::pair<uint64_t, uint64_t>, 3> regions {{
		    {HOST_SYSTEM_MANAGED_MIN, HOST_SYSTEM_RESERVED_MAX + 1u},
		    {HOST_USER_MIN, HOST_USER_MAX + 1u},
		    {kExtendedMemoryBase, kExtendedMemoryBase + kExtendedMemorySize},
		}};

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		constexpr uint64_t MinimumRegionSize = 0x1000000;
		const auto         process           = GetCurrentProcess();
		for (const auto& [guest_start, guest_end]: regions) {
			auto next_addr = guest_start;
			while (next_addr < guest_end) {
				MEMORY_BASIC_INFORMATION info {};
				if (VirtualQuery(reinterpret_cast<const void*>(next_addr), &info, sizeof(info)) == 0) {
					EXIT("failed to inspect guest address space at 0x%016" PRIx64 "\n", next_addr);
				}

				const auto region_base = reinterpret_cast<uint64_t>(info.BaseAddress);
				const auto region_end = std::min<uint64_t>(
				    guest_end, region_base + static_cast<uint64_t>(info.RegionSize));
				const auto reserve_start =
				    AlignUp(std::max(region_base, guest_start), m_granularity);
				const auto reserve_end = AlignDown(region_end, m_granularity);

				if (info.State == MEM_FREE && reserve_end > reserve_start &&
				    reserve_end - reserve_start > MinimumRegionSize) {
					auto* ptr = VirtualAlloc2(
					    process, reinterpret_cast<void*>(reserve_start), reserve_end - reserve_start,
					    MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
					if (reinterpret_cast<uint64_t>(ptr) != reserve_start) {
						EXIT("failed to reserve guest address space at 0x%016" PRIx64
						     ", size 0x%016" PRIx64 "\n",
						     reserve_start, reserve_end - reserve_start);
					}
					AddFreeUnlocked(reserve_start, reserve_end - reserve_start);
					m_owned.emplace_back(reserve_start, reserve_end - reserve_start);
				}

				next_addr = region_end;
			}
		}
#else
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
		std::vector<std::pair<uint64_t, uint64_t>> occupied;
		FILE*                                      maps = fopen("/proc/self/maps", "r");
		EXIT_IF(maps == nullptr);

		char line[512];
		while (fgets(line, sizeof(line), maps) != nullptr) {
			unsigned long long mapping_start = 0;
			unsigned long long mapping_end   = 0;
			if (sscanf(line, "%llx-%llx", &mapping_start, &mapping_end) == 2) {
				occupied.emplace_back(static_cast<uint64_t>(mapping_start),
				                      static_cast<uint64_t>(mapping_end));
			}
		}
		fclose(maps);

		auto reserve_range = [this](uint64_t start, uint64_t end) {
			start = AlignUp(start, PageSize());
			end   = AlignDown(end, PageSize());
			if (end <= start) {
				return;
			}

			const auto size = end - start;
			int        flags = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE;
#if defined(KYTY_LINKED_GUEST_ADDRESS_SPACE)
			flags |= MAP_FIXED;
#elif defined(MAP_FIXED_NOREPLACE)
			flags |= MAP_FIXED_NOREPLACE;
#endif

			void* ptr = mmap(reinterpret_cast<void*>(start), size, PROT_NONE, flags, -1, 0);
			if (ptr == MAP_FAILED || reinterpret_cast<uint64_t>(ptr) != start) {
				if (ptr != MAP_FAILED) {
					munmap(ptr, size);
				}
				return;
			}

			AddFreeUnlocked(start, size);
			m_owned.emplace_back(start, size);
		};

		for (const auto& [region_start, region_end]: regions) {
			auto current = region_start;

			for (const auto& [mapping_start, mapping_end]: occupied) {
				if (mapping_end <= current) {
					continue;
				}
				if (mapping_start >= region_end) {
					break;
				}

				if (mapping_start > current) {
					reserve_range(current, std::min(mapping_start, region_end));
				}

				current = std::max(current, mapping_end);
				if (current >= region_end) {
					break;
				}
			}

			if (current < region_end) {
				reserve_range(current, region_end);
			}
		}
#else
		for (const auto& [start, end]: regions) {
			int flags = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE;
#if defined(KYTY_LINKED_GUEST_ADDRESS_SPACE)
			flags |= MAP_FIXED;
#elif defined(MAP_FIXED_NOREPLACE)
			flags |= MAP_FIXED_NOREPLACE;
#endif
			void* ptr = mmap(reinterpret_cast<void*>(start), end - start, PROT_NONE, flags, -1, 0);
			if (ptr == MAP_FAILED || reinterpret_cast<uint64_t>(ptr) != start) {
				if (ptr != MAP_FAILED) {
					munmap(ptr, end - start);
				}
				EXIT("failed to reserve guest address space at 0x%016" PRIx64
				     ", size 0x%016" PRIx64 "\n",
				     start, end - start);
			}
			AddFreeUnlocked(start, end - start);
			m_owned.emplace_back(start, end - start);
		}
#endif
#endif
	}

	uint64_t FindFreeAlignedUnlocked(uint64_t search_start, uint64_t search_end, uint64_t size,
	                                 uint64_t alignment) const {
		// Include the predecessor because search_start can lie inside a free range.
		auto it = m_free.upper_bound(search_start);
		if (it != m_free.begin()) {
			--it;
		}
		for (; it != m_free.end() && it->first < search_end; ++it) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
			g_test_free_range_visits.fetch_add(1, std::memory_order_relaxed);
#endif
			const auto& [base, range_size] = *it;
			const auto candidate           = AlignUp(std::max(base, search_start), alignment);
			if (candidate < search_end && size <= search_end - candidate &&
			    GuestFreeRangeContains(base, range_size, candidate, size)) {
				return candidate;
			}
		}
		return 0;
	}

	std::map<uint64_t, uint64_t>::iterator FindContaining(uint64_t vaddr, uint64_t size) {
		auto next = m_free.upper_bound(vaddr);
		if (next == m_free.begin()) {
			return m_free.end();
		}
		auto it = std::prev(next);
		return GuestFreeRangeContains(it->first, it->second, vaddr, size) ? it : m_free.end();
	}

	bool SplitUnlocked(std::map<uint64_t, uint64_t>::iterator it, uint64_t vaddr, uint64_t size) {
		const auto base = it->first;
		const auto end  = it->first + it->second;
		if (base != vaddr) {
			const auto left_size = vaddr - base;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			if (VirtualFree(reinterpret_cast<void*>(base), left_size,
			                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0) {
				LOGF_COLOR(Log::Color::Red,
				           "\t guest placeholder split failed: base=0x%016" PRIx64
				           " size=0x%016" PRIx64 " requested=0x%016" PRIx64
				           " request_size=0x%016" PRIx64 " error=0x%08" PRIx32 "\n",
				           base, end - base, vaddr, size, static_cast<uint32_t>(GetLastError()));
				return false;
			}
#endif
			it->second = left_size;
			it         = m_free.emplace(vaddr, end - vaddr).first;
		}

		if (it->second != size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			if (VirtualFree(reinterpret_cast<void*>(it->first), size,
			                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0) {
				LOGF_COLOR(Log::Color::Red,
				           "\t guest placeholder split failed: base=0x%016" PRIx64
				           " size=0x%016" PRIx64 " requested=0x%016" PRIx64
				           " request_size=0x%016" PRIx64 " error=0x%08" PRIx32 "\n",
				           it->first, it->second, vaddr, size,
				           static_cast<uint32_t>(GetLastError()));
				return false;
			}
#endif
			const auto right_start = it->first + size;
			const auto right_size  = it->second - size;
			it->second             = size;
			m_free.emplace(right_start, right_size);
		}

		return true;
	}

	void AddFreeUnlocked(uint64_t vaddr, uint64_t size) {
		auto [it, inserted] = m_free.emplace(vaddr, size);
		if (!inserted) {
			it->second = size;
		}

		auto base = it->first;
		auto end  = it->first + it->second;

		auto first = it;
		while (first != m_free.begin()) {
			auto prev = std::prev(first);
			if (prev->first + prev->second != base) {
				break;
			}
			base  = prev->first;
			first = prev;
		}

		auto last = std::next(it);
		while (last != m_free.end() && end == last->first) {
			end  = last->first + last->second;
			last = std::next(last);
		}

		if (base != it->first || end != it->first + it->second) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			if (CoalescePlaceholders(base, end - base)) {
				m_free.erase(first, last);
				m_free.emplace(base, end - base);
			}
#else
			m_free.erase(first, last);
			m_free.emplace(base, end - base);
#endif
		}
	}

	void RestorePlaceholder(uint64_t vaddr, uint64_t size) {
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
		// The address remains owned for the process lifetime; MAP_FIXED only replaces one of our
		// mappings and never acquires a new guest address.
		void* ptr = mmap(reinterpret_cast<void*>(vaddr), size, PROT_NONE,
		                 MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
		EXIT_IF(ptr == MAP_FAILED);
#else
		(void)vaddr;
		(void)size;
#endif
	}

	std::shared_mutex                          m_mutex;
	std::map<uint64_t, uint64_t>               m_free;
	std::map<uint64_t, MappedRegion>            m_mapped;
	std::vector<std::pair<uint64_t, uint64_t>> m_owned;
	uint64_t                                   m_granularity = 0x10000;
	GuestBackingStore*                         m_backing     = nullptr;
};
