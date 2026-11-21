// Sorting by a small whole-number key first: one counting pass puts the items in the order of their keys,
// and each run of items with one key is then sorted on its own. The result is the order std::sort gives
// with "key, then less" as the comparison, whenever that comparison leaves no two items equal; it is
// reached in a time that grows with the number of items, not with its logarithm as well.
#ifndef KERFKITE_SOLID_BUCKET_HPP
#define KERFKITE_SOLID_BUCKET_HPP

#include <algorithm>
#include <cstddef>
#include <vector>

namespace kerfkite {
namespace solid {
namespace detail {

// key(item) is below `range`; less(a, b) orders items of one key. `starts` and `scratch` are working
// space, kept by the caller between calls.
template <class T, class Key, class Less>
void sort_by_key(std::vector<T> &items, size_t range, Key key, Less less, std::vector<size_t> &starts, std::vector<T> &scratch) {
    const size_t n = items.size();
    if (n < 2) return;
    if (n < 48 || range > 8 * n + 1024) {
        // Too few items for the counting pass to pay, or keys too thinly spread.
        std::sort(items.begin(), items.end(), [&](const T &a, const T &b) {
            const size_t ka = key(a), kb = key(b);
            return ka != kb ? ka < kb : less(a, b);
        });
        return;
    }
    starts.assign(range + 1, 0);
    for (const T &item : items) ++starts[key(item) + 1];
    for (size_t k = 0; k < range; ++k) starts[k + 1] += starts[k];
    scratch.resize(n);
    for (const T &item : items) scratch[starts[key(item)]++] = item;
    // starts[k] is now the end of run k, which is the start of run k + 1.
    size_t first = 0;
    for (size_t k = 0; k < range && first < n; ++k) {
        const size_t last = starts[k];
        if (last - first > 1) std::sort(scratch.begin() + static_cast<std::ptrdiff_t>(first), scratch.begin() + static_cast<std::ptrdiff_t>(last), less);
        first = last;
    }
    items.swap(scratch);
}

}  // namespace detail
}  // namespace solid
}  // namespace kerfkite

#endif
