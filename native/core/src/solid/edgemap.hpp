// A table from the directed edges of a mesh to the triangle that has each, for code that changes a mesh
// in place. It is only ever asked about one edge at a time and never walked in its own order, so nothing
// a caller computes depends on how it is laid out.
#ifndef KERFKITE_SOLID_EDGEMAP_HPP
#define KERFKITE_SOLID_EDGEMAP_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kerfkite {
namespace solid {
namespace detail {

class EdgeMap {
public:
    // The key of the edge from vertex a to vertex b. Vertex numbers are far below 2^32 - 2, so no key is
    // one of the two values that mark a free slot.
    static uint64_t key(uint32_t a, uint32_t b) { return (static_cast<uint64_t>(a) << 32) | b; }

    void reserve(size_t edges) {
        size_t capacity = 16;
        while (capacity < 2 * edges + 2) capacity *= 2;
        if (capacity > slots_.size()) rebuild(capacity);
    }
    // The triangle of an edge, or null.
    const uint32_t *find(uint64_t k) const {
        if (slots_.empty()) return nullptr;
        const size_t mask = slots_.size() - 1;
        for (size_t s = hash(k) & mask;; s = (s + 1) & mask) {
            if (slots_[s].key == k) return &slots_[s].value;
            if (slots_[s].key == kEmpty) return nullptr;
        }
    }
    bool has(uint64_t k) const { return find(k) != nullptr; }
    // Sets the triangle of an edge, whether or not it had one.
    void set(uint64_t k, uint32_t value) {
        if (4 * (used_ + 1) > 3 * slots_.size()) rebuild(slots_.size() < 16 ? 16 : 2 * live_ + 2 > slots_.size() / 2 ? 2 * slots_.size() : slots_.size());
        const size_t mask = slots_.size() - 1;
        size_t free = slots_.size();
        for (size_t s = hash(k) & mask;; s = (s + 1) & mask) {
            if (slots_[s].key == k) {
                slots_[s].value = value;
                return;
            }
            if (slots_[s].key == kRemoved && free == slots_.size()) free = s;
            if (slots_[s].key == kEmpty) {
                if (free == slots_.size()) {
                    free = s;
                    ++used_;
                }
                slots_[free] = Slot{k, value};
                ++live_;
                return;
            }
        }
    }
    void erase(uint64_t k) {
        if (slots_.empty()) return;
        const size_t mask = slots_.size() - 1;
        for (size_t s = hash(k) & mask;; s = (s + 1) & mask) {
            if (slots_[s].key == k) {
                slots_[s].key = kRemoved;
                --live_;
                return;
            }
            if (slots_[s].key == kEmpty) return;
        }
    }

private:
    struct Slot {
        uint64_t key;
        uint32_t value;
    };
    static constexpr uint64_t kEmpty = ~uint64_t(0), kRemoved = ~uint64_t(0) - 1;
    std::vector<Slot> slots_;
    size_t used_ = 0;  // slots that are not empty: live or removed
    size_t live_ = 0;

    static size_t hash(uint64_t k) {
        k *= 0x9E3779B97F4A7C15ULL;
        return static_cast<size_t>(k ^ (k >> 29));
    }
    void rebuild(size_t capacity) {
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.assign(capacity, Slot{kEmpty, 0});
        used_ = live_ = 0;
        const size_t mask = capacity - 1;
        for (const Slot &slot : old) {
            if (slot.key == kEmpty || slot.key == kRemoved) continue;
            size_t s = hash(slot.key) & mask;
            while (slots_[s].key != kEmpty) s = (s + 1) & mask;
            slots_[s] = slot;
            ++used_;
            ++live_;
        }
    }
};

}  // namespace detail
}  // namespace solid
}  // namespace kerfkite

#endif
