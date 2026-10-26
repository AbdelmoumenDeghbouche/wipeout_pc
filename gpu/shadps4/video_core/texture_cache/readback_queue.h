#pragma once

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <algorithm>
#include <deque>
#include <utility>

namespace VideoCore {

// Copies retire in submission order, including overlapping guest ranges.
template <typename Payload>
class ReadbackQueue {
public:
    struct Entry {
        std::uint64_t address, size, tick;
        Payload payload;
        bool valid = true;
        bool watched = true; ///< still protects its pages (see ReleaseOutside)
    };
    bool CanFit(std::uint64_t size) const {
        return entries.size() < max_entries && size <= MaxBytes && bytes <= MaxBytes - size;
    }
    /// gow3: how many copies may be pending. WipEout reads its auto-exposure chain back
    /// (about 42 tiny copies a frame); 64 filled up every other frame with the GPU a frame or
    /// two behind, and each time the whole GPU was waited for.
    void SetMaxEntries(std::size_t count) { max_entries = count; }
    bool Empty() const { return entries.empty(); }
    /// The tick of the oldest pending copy (not Empty()).
    std::uint64_t OldestTick() const { return entries.front().tick; }
    void Push(std::uint64_t address, std::uint64_t size, std::uint64_t tick, Payload payload) {
        assert(CanFit(size));
        entries.push_back({address, size, tick, std::move(payload)});
        bytes += size;
        Widen(entries.back());
    }
    std::uint64_t RequiredTick(std::uint64_t address, std::uint64_t size) const {
        if (!MayOverlap(address, size)) return 0;
        std::uint64_t tick = 0;
        for (const auto& entry : entries) {
            if (entry.valid && entry.watched && OverlapsPage(entry, address, size)) tick = entry.tick;
        }
        return tick;
    }
    template <typename Callback>
    void Cancel(std::uint64_t address, std::uint64_t size, Callback&& cancel) {
        if (!MayOverlap(address, size)) return;
        for (auto& entry : entries) {
            if (entry.valid && OverlapsPage(entry, address, size)) {
                cancel(entry);
                entry.valid = false;
            }
        }
    }
    /// gow3: a CPU access to [address, address + size) that shares pages with small copies but
    /// not their bytes stops waiting for them: each such copy is unwatched (`release` drops its
    /// page protection) and still published when the GPU is done. Returns the copies released.
    template <typename Release>
    unsigned ReleaseOutside(std::uint64_t address, std::uint64_t size, std::uint64_t max_size,
                            Release&& release) {
        unsigned released = 0;
        if (!MayOverlap(address, size)) return released;
        for (auto& entry : entries) {
            if (entry.valid && entry.watched && entry.size <= max_size &&
                OverlapsPage(entry, address, size) &&
                !(address < entry.address + entry.size && entry.address < address + size)) {
                release(entry);
                entry.watched = false;
                ++released;
            }
        }
        return released;
    }
    template <typename Ready, typename Complete>
    void Retire(Ready&& ready, Complete&& complete) {
        bool retired = false;
        while (!entries.empty() && ready(entries.front().tick)) {
            complete(entries.front());
            bytes -= entries.front().size;
            entries.pop_front();
            retired = true;
        }
        if (retired) {
            // The pages still covered: once per batch here, instead of a scan of every entry
            // at each buffer access outside them (RequiredTick runs on the draw recording thread).
            first_page = ~std::uint64_t{0};
            end_page = 0;
            for (const auto& entry : entries) Widen(entry);
        }
    }
    std::uint64_t Bytes() const { return bytes; }

private:
    /// Whether [address, address + size) shares a page with the range the entries cover.
    bool MayOverlap(std::uint64_t address, std::uint64_t size) const {
        return size && address < end_page && first_page < address + size;
    }
    void Widen(const Entry& entry) {
        first_page = std::min(first_page, entry.address & ~std::uint64_t{4095});
        end_page = std::max(end_page, (entry.address + entry.size + 4095) & ~std::uint64_t{4095});
    }
    static bool OverlapsPage(const Entry& entry, std::uint64_t address, std::uint64_t size) {
        if (!size) return false;
        const auto begin = entry.address & ~std::uint64_t{4095};
        const auto end = (entry.address + entry.size + 4095) & ~std::uint64_t{4095};
        return address < end && begin < address + size;
    }
    static constexpr std::uint64_t MaxBytes = 64ull << 20;
    std::deque<Entry> entries;
    std::uint64_t bytes = 0;
    std::size_t max_entries = 64;
    std::uint64_t first_page = ~std::uint64_t{0}, end_page = 0; ///< pages the entries cover
};
} // namespace VideoCore
