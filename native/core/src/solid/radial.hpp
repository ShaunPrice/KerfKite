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
// False when two triangles lie in the same half-plane, which a valid arrangement never has.
//
// P answers three questions about the triangles, named by their index in `third`:
//   frame(u, v, third, i, au, av)  two coordinate axes in which triangle i has area, and its turn there;
//                                  0 when it has no area at all
//   side(u, v, p, au, av)          the turn of u, v and the point p in those axes
//   turn(u, v, third, i, j)        the sign of det[v-u; third[i]-u; third[j]-u]: positive when triangle
//                                  j lies counter-clockwise of triangle i by less than a half turn
// A P that knows the planes of the triangles can answer them without computing (see PoolPredicates);
// the answers are those of the determinants either way. plain_radial_frame() is the frame from the
// points alone.
template <class P>
int plain_radial_frame(const P &predicates, int u, int v, int ref, int &au, int &av) {
    int side = 0;
    for (int k = 0; k < 3 && !side; ++k) {
        au = (k + 1) % 3;
        av = (k + 2) % 3;
        side = predicates.orient2d(u, v, ref, au, av);
    }
    return side;
}

template <class P>
bool radial_sort(const P &predicates, int u, int v, const std::vector<int> &third, std::vector<int> &order) {
    const size_t count = order.size();
    if (count < 3) return true;
    int au = 1, av = 2;
    const int ref_side = predicates.frame(u, v, third, order[0], au, av);
    if (!ref_side) return false;  // a zero-area triangle
    // 1: within the first half turn, 2: exactly opposite the reference, 3: within the second half turn.
    std::vector<int> cls(third.size(), 0);
    bool clash = false;
    for (size_t i = 1; i < count; ++i) {
        const int s = predicates.turn(u, v, third, order[0], order[i]);
        int c;
        if (s > 0) c = 1;
        else if (s < 0) c = 3;
        else if (predicates.side(u, v, third[static_cast<size_t>(order[i])], au, av) == ref_side) {
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
        return predicates.turn(u, v, third, a, b) > 0;
    });
    for (size_t i = 2; i < count; ++i) {
        const int a = order[i - 1], b = order[i];
        if (cls[static_cast<size_t>(a)] != cls[static_cast<size_t>(b)]) continue;
        if (cls[static_cast<size_t>(a)] == 2) return false;
        if (predicates.turn(u, v, third, a, b) == 0) return false;
    }
    return true;
}

}  // namespace detail
}  // namespace solid
}  // namespace kerfkite

#endif
