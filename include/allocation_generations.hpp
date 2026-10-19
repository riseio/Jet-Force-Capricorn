#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace jfc {

class AllocationGenerations {
public:
    struct Range { uint32_t begin, end; uint64_t generation; };
    static constexpr uint32_t MemorySize = 0x800000;
    uint64_t serial = 0;
    std::vector<Range> ranges;

    uint64_t at(uint32_t address) const {
        auto it = std::upper_bound(ranges.begin(), ranges.end(), address,
            [](uint32_t key, const Range &range) { return key < range.begin; });
        if (it == ranges.begin()) return 0;
        --it;
        return address < it->end ? it->generation : 0;
    }

    void replace(uint32_t begin, uint32_t size, bool allocated) {
        if (!size || begin >= MemorySize || size > MemorySize - begin)
            throw std::runtime_error("Allocation generation range is outside RDRAM");
        if (allocated && serial == UINT64_MAX)
            throw std::overflow_error("Allocation generation exhausted");
        const uint32_t end = begin + size;
        auto first = std::lower_bound(ranges.begin(), ranges.end(), begin,
            [](const Range &range, uint32_t key) { return range.end <= key; });
        auto last = first;
        while (last != ranges.end() && last->begin < end) ++last;
        Range replacement[3];
        size_t count = 0;
        if (first != last && first->begin < begin) replacement[count++] = {first->begin, begin, first->generation};
        if (allocated) replacement[count++] = {begin, end, serial + 1};
        if (first != last && (last - 1)->end > end) replacement[count++] = {end, (last - 1)->end, (last - 1)->generation};

        const auto offset = first - ranges.begin();
        const auto removed = last - first;
        if (ranges.size() + count > ranges.capacity())
            ranges.reserve(std::max(ranges.size() + count, ranges.capacity() * 2));
        first = ranges.erase(ranges.begin() + offset, ranges.begin() + offset + removed);
        ranges.insert(first, replacement, replacement + count);
        if (allocated) ++serial;
    }

    void validate() const {
        if (serial == UINT64_MAX || ranges.size() > MemorySize)
            throw std::runtime_error("Invalid saved allocation generation count");
        uint32_t end = 0;
        for (const auto &range : ranges) {
            if (range.begin < end || range.end <= range.begin || range.end > MemorySize ||
                !range.generation || range.generation > serial)
                throw std::runtime_error("Invalid saved allocation generation interval");
            end = range.end;
        }
    }
};
}
