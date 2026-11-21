// The order of triangles around a shared edge, decided exactly.
#ifndef KERFKITE_SOLID_RADIAL_HPP
#define KERFKITE_SOLID_RADIAL_HPP

#include <algorithm>
#include <vector>

namespace kerfkite {
namespace solid {
namespace detail {

// Sorts `order` (indices into `third`) counter-clockwise about the directed edge u -> v (right-hand
// rule), starting from order[0], where third[i] is the vertex of triangle i opposite the edge.
// P provides orient3d(a, b, c, d) and orient2d(a, b, c, axis_u, axis_v) on point ids.
// False when two triangles lie in the same half-plane, which a valid arrangement never has.
template <class P>
bool radial_sort(const P &predicates, int u, int v, const std::vector<int> &third, std::vector<int> &order) {
    const size_t count = order.size();
    if (count < 3) return true;
    const int ref = third[static_cast<size_t>(order[0])];
    int au = 1, av = 2, ref_side = 0;
    for (int k = 0; k < 3 && !ref_side; ++k) {
        au = (k + 1) % 3;
        av = (k + 2) % 3;
        ref_side = predicates.orient2d(u, v, ref, au, av);
    }
    if (!ref_side) return false;  // a zero-area triangle
    // 1: within the first half turn, 2: exactly opposite the reference, 3: within the second half turn.
    std::vector<int> cls(third.size(), 0);
    bool clash = false;
    for (size_t i = 1; i < count; ++i) {
        const int p = third[static_cast<size_t>(order[i])];
        const int s = predicates.orient3d(u, v, ref, p);
        int c;
        if (s > 0) c = 1;
        else if (s < 0) c = 3;
        else if (predicates.orient2d(u, v, p, au, av) == ref_side) {
            c = 0;
            clash = true;
        } else c = 2;
        cls[static_cast<size_t>(order[i])] = c;
    }
    if (clash) return false;
    std::sort(order.begin() + 1, order.end(), [&](int a, int b) {
        const int ca = cls[static_cast<size_t>(a)], cb = cls[static_cast<size_t>(b)];
        if (ca != cb) return ca < cb;
        if (ca == 2) return false;
        // Within an open half turn: a comes first when b is counter-clockwise of it.
        return predicates.orient3d(u, v, third[static_cast<size_t>(a)], third[static_cast<size_t>(b)]) > 0;
    });
    for (size_t i = 2; i < count; ++i) {
        const int a = order[i - 1], b = order[i];
        if (cls[static_cast<size_t>(a)] != cls[static_cast<size_t>(b)]) continue;
        if (cls[static_cast<size_t>(a)] == 2) return false;
        if (predicates.orient3d(u, v, third[static_cast<size_t>(a)], third[static_cast<size_t>(b)]) == 0) return false;
    }
    return true;
}

}  // namespace detail
}  // namespace solid
}  // namespace kerfkite

#endif
