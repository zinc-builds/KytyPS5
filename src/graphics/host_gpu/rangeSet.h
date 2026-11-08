#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_

#include "common/assert.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class RangeSet final {
public:
	void Add(uint64_t address, uint64_t size) {
		const auto end = End(address, size);
		auto       it  = m_ranges.lower_bound(address);
		if (it != m_ranges.begin() && std::prev(it)->second >= address) {
			it = std::prev(it);
		}
		if (it == m_ranges.end() || it->first > address) {
			it = m_ranges.emplace_hint(it, address, end);
		} else {
			if (it->second >= end) {
				return;
			}
			it->second = end;
		}
		auto next = std::next(it);
		while (next != m_ranges.end() && next->first <= it->second) {
			it->second = std::max(it->second, next->second);
			next       = m_ranges.erase(next);
		}
	}

	void Subtract(uint64_t address, uint64_t size) {
		const auto end = End(address, size);
		auto       it  = m_ranges.lower_bound(address);
		if (it != m_ranges.begin() && std::prev(it)->second > address) {
			it = std::prev(it);
		}
		while (it != m_ranges.end() && it->first < end) {
			const auto begin = it->first;
			const auto last  = it->second;
			it               = m_ranges.erase(it);
			if (begin < address) {
				m_ranges.emplace(begin, address);
			}
			if (last > end) {
				m_ranges.emplace(end, last);
				break;
			}
		}
	}

	template <typename Func>
	void ForEach(Func&& func) const {
		for (const auto& [begin, end]: m_ranges) {
			func(begin, end);
		}
	}

	[[nodiscard]] bool Intersects(uint64_t address, uint64_t size) const {
		const auto end = End(address, size);
		auto       it  = m_ranges.lower_bound(address);
		if (it != m_ranges.begin() && std::prev(it)->second > address) {
			return true;
		}
		return it != m_ranges.end() && it->first < end;
	}

	[[nodiscard]] bool Contains(uint64_t address, uint64_t size) const {
		const auto end = End(address, size);
		auto       it  = m_ranges.upper_bound(address);
		if (it == m_ranges.begin()) {
			return false;
		}
		--it;
		return it->first <= address && it->second >= end;
	}

	template <typename Func>
	void ForEachInRange(uint64_t address, uint64_t size, Func&& func) const {
		const auto end = End(address, size);
		auto       it  = m_ranges.upper_bound(address);
		if (it != m_ranges.begin()) {
			--it;
		}
		for (; it != m_ranges.end() && it->first < end; ++it) {
			const auto begin = std::max(address, it->first);
			const auto last  = std::min(end, it->second);
			if (begin < last) {
				func(begin, last);
			}
		}
	}

	[[nodiscard]] bool Empty() const { return m_ranges.empty(); }

private:
	static uint64_t End(uint64_t address, uint64_t size) {
		if (size == 0 || size > UINT64_MAX - address) {
			EXIT("invalid range-set address or size\n");
		}
		return address + size;
	}

	std::map<uint64_t, uint64_t> m_ranges;
};

// RangeSet's Add, Intersects and Contains over a sorted vector: lookups are a binary search over
// contiguous memory and Clear keeps the capacity, for sets rebuilt often and mostly queried or
// grown by ranges they already cover (the draw atomic-write batch).
class FlatRangeSet final {
public:
	void Add(uint64_t address, uint64_t size) {
		const auto end = End(address, size);
		// The first range starting after address, and the one before it if it reaches address.
		auto it = std::upper_bound(m_ranges.begin(), m_ranges.end(), address,
		                           [](uint64_t value, const Range& range) { return value < range.first; });
		if (it != m_ranges.begin() && std::prev(it)->second >= address) {
			it = std::prev(it);
			if (it->second >= end) {
				return;
			}
			it->second = end;
		} else {
			it = m_ranges.insert(it, {address, end});
		}
		// Absorb the ranges the grown one now reaches.
		auto last = std::next(it);
		while (last != m_ranges.end() && last->first <= it->second) {
			it->second = std::max(it->second, last->second);
			++last;
		}
		m_ranges.erase(std::next(it), last);
	}

	[[nodiscard]] bool Intersects(uint64_t address, uint64_t size) const {
		const auto end = End(address, size);
		auto it = std::upper_bound(m_ranges.begin(), m_ranges.end(), address,
		                           [](uint64_t value, const Range& range) { return value < range.first; });
		if (it != m_ranges.begin() && std::prev(it)->second > address) {
			return true;
		}
		return it != m_ranges.end() && it->first < end;
	}

	[[nodiscard]] bool Contains(uint64_t address, uint64_t size) const {
		const auto end = End(address, size);
		auto it = std::upper_bound(m_ranges.begin(), m_ranges.end(), address,
		                           [](uint64_t value, const Range& range) { return value < range.first; });
		return it != m_ranges.begin() && std::prev(it)->second >= end;
	}

	template <typename Func>
	void ForEach(Func&& func) const {
		for (const auto& [begin, end]: m_ranges) {
			func(begin, end);
		}
	}

	[[nodiscard]] bool Empty() const { return m_ranges.empty(); }
	void               Clear() { m_ranges.clear(); }

private:
	using Range = std::pair<uint64_t, uint64_t>; // [first, second)

	static uint64_t End(uint64_t address, uint64_t size) {
		if (size == 0 || size > UINT64_MAX - address) {
			EXIT("invalid range-set address or size\n");
		}
		return address + size;
	}

	std::vector<Range> m_ranges; // sorted, disjoint and not touching
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_
