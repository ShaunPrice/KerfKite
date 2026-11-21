// Exact planar arrangement and triangulation: see planar.hpp.
//
// The triangulation is the classical incremental one: each point is inserted into the triangle (or on
// the edge) that contains it, then each required edge is recovered by flipping the edges that cross it
// (Sloan, "A fast algorithm for generating constrained Delaunay triangulations", 1993, without the
// Delaunay step). Because orientation tests are exact, no triangle without area is ever made.
#include "planar.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <stdexcept>

namespace kerfkite {
namespace solid {
namespace detail {

namespace {
[[noreturn]] void defect(const char *what) { throw std::logic_error(std::string("solid kernel: planar arrangement: ") + what); }
}  // namespace

Planar::Planar(Pool &pool, const Plane &carrier, int axis_u, int axis_v, int sign)
    : pool_(pool), carrier_(carrier), au_(axis_u), av_(axis_v), sign_(sign) {
    root[0] = root[1] = root[2] = -1;
}

// For p on the line through a and b: strictly between them.
bool Planar::between(int a, int b, int p) const {
    int axis = au_;
    if (pool_.compare(a, b, axis) == 0) axis = av_;
    const int c1 = pool_.compare(a, p, axis), c2 = pool_.compare(p, b, axis);
    return c1 == c2 && c1 != 0;
}

void Planar::split() {
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    const size_t n = segments.size();
    std::vector<std::vector<int>> on(n);
    struct Bounds {
        double ulo, uhi, vlo, vhi;
    };
    std::vector<Bounds> box(n);
    for (size_t i = 0; i < n; ++i) {
        const Pt &a = pool_.pts[static_cast<size_t>(segments[i].a)], &b = pool_.pts[static_cast<size_t>(segments[i].b)];
        box[i] = Bounds{std::min(a.c[au_].lo, b.c[au_].lo), std::max(a.c[au_].hi, b.c[au_].hi), std::min(a.c[av_].lo, b.c[av_].lo),
                        std::max(a.c[av_].hi, b.c[av_].hi)};
    }
    std::vector<int> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&](int x, int y) {
        return box[static_cast<size_t>(x)].ulo != box[static_cast<size_t>(y)].ulo ? box[static_cast<size_t>(x)].ulo < box[static_cast<size_t>(y)].ulo : x < y;
    });
    std::vector<int> created;
    for (size_t ii = 0; ii < n; ++ii) {
        const size_t i = static_cast<size_t>(order[ii]);
        const PlanarSegment &s = segments[i];
        if (s.a == s.b) continue;
        for (size_t jj = ii + 1; jj < n && box[static_cast<size_t>(order[jj])].ulo <= box[i].uhi; ++jj) {
            const size_t j = static_cast<size_t>(order[jj]);
            if (box[j].vlo > box[i].vhi || box[i].vlo > box[j].vhi) continue;
            const PlanarSegment &t = segments[j];
            if (t.a == t.b || (s.a == t.a && s.b == t.b) || (s.a == t.b && s.b == t.a)) continue;
            const int o1 = orient(s.a, s.b, t.a), o2 = orient(s.a, s.b, t.b);
            if (o1 == o2 && o1 != 0) continue;
            const int o3 = orient(t.a, t.b, s.a), o4 = orient(t.a, t.b, s.b);
            if (o3 == o4 && o3 != 0) continue;
            if (o1 * o2 < 0 && o3 * o4 < 0) {
                const int x = pool_.three_planes(carrier_, supports[static_cast<size_t>(s.support)], supports[static_cast<size_t>(t.support)]);
                if (x < 0) defect("crossing segments without a common point");
                on[i].push_back(x);
                on[j].push_back(x);
                created.push_back(x);
                continue;
            }
            if (o1 == 0 && t.a != s.a && t.a != s.b && between(s.a, s.b, t.a)) on[i].push_back(t.a);
            if (o2 == 0 && t.b != s.a && t.b != s.b && between(s.a, s.b, t.b)) on[i].push_back(t.b);
            if (o3 == 0 && s.a != t.a && s.a != t.b && between(t.a, t.b, s.a)) on[j].push_back(s.a);
            if (o4 == 0 && s.b != t.a && s.b != t.b && between(t.a, t.b, s.b)) on[j].push_back(s.b);
        }
    }
    // Points that are not the end of any segment may still lie on one.
    std::vector<int> ends;
    ends.reserve(2 * n);
    for (const PlanarSegment &s : segments) {
        ends.push_back(s.a);
        ends.push_back(s.b);
    }
    std::sort(ends.begin(), ends.end());
    for (const int p : points) {
        if (std::binary_search(ends.begin(), ends.end(), p)) continue;
        const Pt &q = pool_.pts[static_cast<size_t>(p)];
        for (size_t i = 0; i < n; ++i) {
            if (q.c[au_].lo > box[i].uhi || q.c[au_].hi < box[i].ulo || q.c[av_].lo > box[i].vhi || q.c[av_].hi < box[i].vlo) continue;
            const PlanarSegment &s = segments[i];
            if (s.a == s.b) continue;
            if (orient(s.a, s.b, p) == 0 && between(s.a, s.b, p)) on[i].push_back(p);
        }
    }
    points.insert(points.end(), created.begin(), created.end());
    pieces.clear();
    for (size_t i = 0; i < n; ++i) {
        const PlanarSegment &s = segments[i];
        if (s.a == s.b) continue;
        points.push_back(s.a);
        points.push_back(s.b);
        if (on[i].empty()) {
            pieces.push_back(s);
            continue;
        }
        std::vector<int> &list = on[i];
        int axis = au_;
        if (pool_.compare(s.a, s.b, axis) == 0) axis = av_;
        const int direction = pool_.compare(s.a, s.b, axis);
        std::sort(list.begin(), list.end(), [&](int x, int y) {
            if (x == y) return false;
            return pool_.compare(x, y, axis) == direction;  // in the order from a to b
        });
        list.erase(std::unique(list.begin(), list.end()), list.end());
        int previous = s.a;
        for (const int p : list) {
            pieces.push_back(PlanarSegment{previous, p, s.support, s.source});
            previous = p;
        }
        pieces.push_back(PlanarSegment{previous, s.b, s.support, s.source});
    }
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
}

void Planar::relink(int neighbour, int from, int to) {
    if (neighbour < 0) return;
    Tri &t = tris_[static_cast<size_t>(neighbour)];
    for (int k = 0; k < 3; ++k)
        if (t.n[k] == from) {
            t.n[k] = to;
            return;
        }
    defect("broken adjacency");
}

void Planar::split_face(int t, int p) {
    const Tri old = tris_[static_cast<size_t>(t)];
    const int t1 = static_cast<int>(tris_.size()), t2 = t1 + 1;
    tris_[static_cast<size_t>(t)] = Tri{{old.v[0], old.v[1], p}, {old.n[0], t1, t2}};
    tris_.push_back(Tri{{old.v[1], old.v[2], p}, {old.n[1], t2, t}});
    tris_.push_back(Tri{{old.v[2], old.v[0], p}, {old.n[2], t, t1}});
    relink(old.n[1], t, t1);
    relink(old.n[2], t, t2);
    vtri_[static_cast<size_t>(old.v[0])] = t;
    vtri_[static_cast<size_t>(old.v[1])] = t;
    vtri_[static_cast<size_t>(old.v[2])] = t1;
    vtri_[static_cast<size_t>(p)] = t;
}

void Planar::split_edge(int t, int i, int p) {
    const Tri old = tris_[static_cast<size_t>(t)];
    const int a = old.v[i], b = old.v[(i + 1) % 3], c = old.v[(i + 2) % 3];
    const int u = old.n[i], nb = old.n[(i + 1) % 3], nc = old.n[(i + 2) % 3];
    const int t2 = static_cast<int>(tris_.size());
    tris_.push_back(Tri{{p, b, c}, {-1, nb, t}});
    tris_[static_cast<size_t>(t)] = Tri{{a, p, c}, {-1, t2, nc}};
    relink(nb, t, t2);
    vtri_[static_cast<size_t>(a)] = t;
    vtri_[static_cast<size_t>(b)] = t2;
    vtri_[static_cast<size_t>(c)] = t;
    vtri_[static_cast<size_t>(p)] = t;
    if (u < 0) return;
    const Tri other = tris_[static_cast<size_t>(u)];
    int j = 0;
    while (j < 3 && other.n[j] != t) ++j;
    if (j == 3) defect("broken adjacency");
    const int d = other.v[(j + 2) % 3];
    const int across_ad = other.n[(j + 1) % 3], across_db = other.n[(j + 2) % 3];
    const int u2 = static_cast<int>(tris_.size());
    tris_[static_cast<size_t>(u)] = Tri{{b, p, d}, {t2, u2, across_db}};
    tris_.push_back(Tri{{p, a, d}, {t, across_ad, u}});
    relink(across_ad, u, u2);
    tris_[static_cast<size_t>(t)].n[0] = u2;
    tris_[static_cast<size_t>(t2)].n[0] = u;
    vtri_[static_cast<size_t>(d)] = u;
}

// Replaces the edge i of t, shared with its neighbour, by the other diagonal of the two triangles.
void Planar::flip(int t, int i) {
    const Tri old = tris_[static_cast<size_t>(t)];
    const int u = old.n[i];
    const Tri other = tris_[static_cast<size_t>(u)];
    int j = 0;
    while (j < 3 && other.n[j] != t) ++j;
    if (j == 3) defect("broken adjacency");
    const int a = old.v[i], b = old.v[(i + 1) % 3], c = old.v[(i + 2) % 3], d = other.v[(j + 2) % 3];
    const int nb = old.n[(i + 1) % 3], nc = old.n[(i + 2) % 3];
    const int across_ad = other.n[(j + 1) % 3], across_db = other.n[(j + 2) % 3];
    tris_[static_cast<size_t>(t)] = Tri{{a, d, c}, {across_ad, u, nc}};
    tris_[static_cast<size_t>(u)] = Tri{{d, b, c}, {across_db, nb, t}};
    relink(across_ad, u, t);
    relink(nb, t, u);
    vtri_[static_cast<size_t>(a)] = t;
    vtri_[static_cast<size_t>(d)] = t;
    vtri_[static_cast<size_t>(c)] = t;
    vtri_[static_cast<size_t>(b)] = u;
}

// The triangle that has the directed edge x -> y, found by turning about x.
bool Planar::find_edge(int x, int y, int &t, int &i) const {
    const int start = vtri_[static_cast<size_t>(x)];
    int current = start;
    const size_t limit = tris_.size() + 2;
    for (size_t step = 0; step < limit; ++step) {
        const Tri &tri = tris_[static_cast<size_t>(current)];
        const int k = tri.v[0] == x ? 0 : tri.v[1] == x ? 1 : 2;
        if (tri.v[(k + 1) % 3] == y) {
            t = current;
            i = k;
            return true;
        }
        current = tri.n[(k + 2) % 3];
        if (current < 0 || current == start) break;
    }
    if (current == start) return false;
    current = start;
    for (size_t step = 0; step < limit; ++step) {
        const Tri &tri = tris_[static_cast<size_t>(current)];
        const int k = tri.v[0] == x ? 0 : tri.v[1] == x ? 1 : 2;
        current = tri.n[k];
        if (current < 0 || current == start) break;
        const Tri &next = tris_[static_cast<size_t>(current)];
        const int m = next.v[0] == x ? 0 : next.v[1] == x ? 1 : 2;
        if (next.v[(m + 1) % 3] == y) {
            t = current;
            i = m;
            return true;
        }
    }
    return false;
}

// Every triangle at vertex x.
void Planar::around(int x, std::vector<int> &out) const {
    out.clear();
    const int start = vtri_[static_cast<size_t>(x)];
    int current = start;
    const size_t limit = tris_.size() + 2;
    for (size_t step = 0; step < limit; ++step) {
        out.push_back(current);
        const Tri &tri = tris_[static_cast<size_t>(current)];
        const int k = tri.v[0] == x ? 0 : tri.v[1] == x ? 1 : 2;
        const int next = tri.n[(k + 2) % 3];
        if (next == start) return;
        if (next < 0) break;
        current = next;
    }
    current = start;
    for (size_t step = 0; step < limit; ++step) {
        const Tri &tri = tris_[static_cast<size_t>(current)];
        const int k = tri.v[0] == x ? 0 : tri.v[1] == x ? 1 : 2;
        const int next = tri.n[k];
        if (next < 0 || next == start) return;
        out.push_back(next);
        current = next;
    }
}

void Planar::insert(int p) {
    int t = last_;
    int s[3] = {0, 0, 0};
    bool found = false;
    const size_t limit = 2 * tris_.size() + 16;
    for (size_t step = 0; step < limit && !found; ++step) {
        const Tri &tri = tris_[static_cast<size_t>(t)];
        found = true;
        for (int k = 0; k < 3; ++k) {
            s[k] = o(tri.v[k], tri.v[(k + 1) % 3], p);
            if (s[k] < 0) {
                if (tri.n[k] < 0) defect("a point outside the triangulated area");
                t = tri.n[k];
                found = false;
                break;
            }
        }
    }
    if (!found) {
        // The walk can circle in a triangulation that is not Delaunay: look everywhere.
        for (size_t c = 0; c < tris_.size() && !found; ++c) {
            const Tri &tri = tris_[c];
            found = true;
            for (int k = 0; k < 3 && found; ++k) {
                s[k] = o(tri.v[k], tri.v[(k + 1) % 3], p);
                found = s[k] >= 0;
            }
            if (found) t = static_cast<int>(c);
        }
        if (!found) defect("a point outside the triangulated area");
    }
    const int zeros = (s[0] == 0) + (s[1] == 0) + (s[2] == 0);
    if (zeros == 0) split_face(t, p);
    else if (zeros == 1) split_edge(t, s[0] == 0 ? 0 : s[1] == 0 ? 1 : 2, p);
    else defect("two points at one place");
    last_ = vtri_[static_cast<size_t>(p)];
}

void Planar::enforce(int first, int second) {
    std::vector<std::pair<int, int>> todo;
    std::vector<int> fan;
    todo.emplace_back(first, second);
    size_t guard = 0;
    while (!todo.empty()) {
        if (++guard > 64 * (tris_.size() + 16)) defect("an edge could not be recovered");
        const int a = todo.back().first, b = todo.back().second;
        todo.pop_back();
        int t, i;
        if (find_edge(a, b, t, i) || find_edge(b, a, t, i)) continue;
        // The triangle at a that the segment leaves through its far edge.
        int right = -1, left = -1, far = -1;  // the crossed edge runs right -> left; far is the triangle beyond it
        bool divided = false;
        around(a, fan);
        const int pa = pid_[static_cast<size_t>(a)], pb = pid_[static_cast<size_t>(b)];
        const int du = pool_.compare(pa, pb, au_), dv = pool_.compare(pa, pb, av_);
        for (const int current : fan) {
            const Tri &tri = tris_[static_cast<size_t>(current)];
            const int k = tri.v[0] == a ? 0 : tri.v[1] == a ? 1 : 2;
            const int p = tri.v[(k + 1) % 3], q = tri.v[(k + 2) % 3];
            const int sp = o(a, b, p), sq = o(a, b, q);
            // A vertex on the segment itself (same direction from a as b): a-p is already an edge.
            if (sp == 0 && pool_.compare(pa, pid_[static_cast<size_t>(p)], au_) == du && pool_.compare(pa, pid_[static_cast<size_t>(p)], av_) == dv) {
                todo.emplace_back(p, b);
                divided = true;
                break;
            }
            if (sq == 0 && pool_.compare(pa, pid_[static_cast<size_t>(q)], au_) == du && pool_.compare(pa, pid_[static_cast<size_t>(q)], av_) == dv) {
                todo.emplace_back(q, b);
                divided = true;
                break;
            }
            if (sp < 0 && sq > 0) {
                right = p;
                left = q;
                far = tri.n[(k + 1) % 3];
                break;
            }
        }
        if (divided) continue;
        if (right < 0 || far < 0) defect("an edge leaves the triangulated area");
        std::deque<std::pair<int, int>> crossed;
        crossed.emplace_back(right, left);
        bool restart = false;
        for (size_t step = 0;; ++step) {
            if (step > tris_.size() + 4) defect("an edge could not be traced");
            const Tri &tri = tris_[static_cast<size_t>(far)];
            const int k = tri.v[0] == left ? 0 : tri.v[1] == left ? 1 : 2;  // far has left -> right
            if (tri.v[(k + 1) % 3] != right) defect("broken adjacency");
            const int r = tri.v[(k + 2) % 3];
            if (r == b) break;
            const int s = o(a, b, r);
            if (s == 0) {
                // A vertex on the segment between its ends: recover the two halves instead.
                todo.emplace_back(a, r);
                todo.emplace_back(r, b);
                restart = true;
                break;
            }
            if (s < 0) {
                // r is on the right: the segment leaves through r -> left.
                far = tri.n[(k + 2) % 3];
                right = r;
            } else {
                far = tri.n[(k + 1) % 3];
                left = r;
            }
            if (far < 0) defect("an edge leaves the triangulated area");
            crossed.emplace_back(right, left);
        }
        if (restart) continue;
        size_t flips = 0;
        const size_t flip_limit = 64 * (crossed.size() + 4) * (crossed.size() + 4) + 4096;
        while (!crossed.empty()) {
            if (++flips > flip_limit) defect("edge flips did not finish");
            const std::pair<int, int> edge = crossed.front();
            crossed.pop_front();
            if (!find_edge(edge.first, edge.second, t, i)) defect("a crossed edge vanished");
            const Tri &tri = tris_[static_cast<size_t>(t)];
            const int x = tri.v[i], y = tri.v[(i + 1) % 3], c = tri.v[(i + 2) % 3];
            const int u = tri.n[i];
            if (u < 0) defect("a crossed edge on the outside");
            const Tri &other = tris_[static_cast<size_t>(u)];
            int j = 0;
            while (j < 3 && other.n[j] != t) ++j;
            if (j == 3) defect("broken adjacency");
            const int d = other.v[(j + 2) % 3];
            if (o(x, d, c) > 0 && o(d, y, c) > 0) {
                flip(t, i);
                if (c != a && c != b && d != a && d != b) {
                    const int sc = o(a, b, c), sd = o(a, b, d);
                    if (sc * sd < 0) crossed.emplace_back(c, d);
                }
            } else {
                crossed.push_back(edge);
            }
        }
    }
}

// Flips edges that are not required until no triangle pair would be better shaped the other way (the
// Delaunay rule: the two angles facing an edge sum to at most a half turn). Any triangulation is
// correct; this one avoids the needless slivers that rounding to the grid would later flatten or turn
// over. The rule is evaluated in doubles, in a fixed order of correctly rounded operations, so every
// platform makes the same flips; whether a flip is possible at all is still decided exactly.
void Planar::improve(const std::vector<std::pair<int, int>> &required) {
    std::vector<std::array<double, 2>> at(pid_.size());
    for (size_t k = 0; k < pid_.size(); ++k) {
        const Pt &p = pool_.pts[static_cast<size_t>(pid_[k])];
        at[k][0] = (p.c[au_].lo + p.c[au_].hi) / 2;
        at[k][1] = (p.c[av_].lo + p.c[av_].hi) / 2;
    }
    // The cotangent of the angle at c facing the edge x-y.
    const auto cotangent = [&](int x, int y, int c) {
        const double ax = at[static_cast<size_t>(x)][0] - at[static_cast<size_t>(c)][0], ay = at[static_cast<size_t>(x)][1] - at[static_cast<size_t>(c)][1];
        const double bx = at[static_cast<size_t>(y)][0] - at[static_cast<size_t>(c)][0], by = at[static_cast<size_t>(y)][1] - at[static_cast<size_t>(c)][1];
        const double dot = ax * bx + ay * by, cross = std::fabs(ax * by - ay * bx);
        const double scale = std::fabs(ax * bx) + std::fabs(ay * by) + cross;
        if (!(scale > 0)) return 0.0;
        return dot / (cross > 1e-12 * scale ? cross : 1e-12 * scale);
    };
    std::vector<std::pair<int, int>> stack;
    for (size_t t = 0; t < tris_.size(); ++t)
        for (int k = 0; k < 3; ++k)
            if (tris_[t].n[k] > static_cast<int>(t)) stack.emplace_back(tris_[t].v[k], tris_[t].v[(k + 1) % 3]);
    size_t flips = 0;
    const size_t limit = 32 * tris_.size() + 64;
    while (!stack.empty() && flips < limit) {
        const std::pair<int, int> edge = stack.back();
        stack.pop_back();
        int t, i;
        if (!find_edge(edge.first, edge.second, t, i)) continue;  // flipped away meanwhile
        const Tri &tri = tris_[static_cast<size_t>(t)];
        const int u = tri.n[i];
        if (u < 0) continue;
        const int x = tri.v[i], y = tri.v[(i + 1) % 3], c = tri.v[(i + 2) % 3];
        if (std::binary_search(required.begin(), required.end(), std::make_pair(std::min(x, y), std::max(x, y)))) continue;
        const Tri &other = tris_[static_cast<size_t>(u)];
        int j = 0;
        while (j < 3 && other.n[j] != t) ++j;
        if (j == 3) defect("broken adjacency");
        const int d = other.v[(j + 2) % 3];
        if (!(cotangent(x, y, c) + cotangent(x, y, d) < -1e-9)) continue;
        if (!(o(x, d, c) > 0 && o(d, y, c) > 0)) continue;
        flip(t, i);
        ++flips;
        stack.emplace_back(x, d);
        stack.emplace_back(d, y);
        stack.emplace_back(y, c);
        stack.emplace_back(c, x);
    }
}

void Planar::triangulate(int r0, int r1, int r2) {
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    pid_.clear();
    pid_.push_back(r0);
    pid_.push_back(r1);
    pid_.push_back(r2);
    std::vector<int> rest;
    for (const int p : points)
        if (p != r0 && p != r1 && p != r2) rest.push_back(p);
    // Neighbouring points one after another keep the search for the containing triangle short.
    std::sort(rest.begin(), rest.end(), [&](int x, int y) {
        const Pt &a = pool_.pts[static_cast<size_t>(x)], &b = pool_.pts[static_cast<size_t>(y)];
        if (a.c[au_].lo != b.c[au_].lo) return a.c[au_].lo < b.c[au_].lo;
        if (a.c[av_].lo != b.c[av_].lo) return a.c[av_].lo < b.c[av_].lo;
        return x < y;
    });
    pid_.insert(pid_.end(), rest.begin(), rest.end());
    std::vector<int> ids(pid_.size());
    for (size_t k = 0; k < ids.size(); ++k) ids[k] = static_cast<int>(k);
    std::sort(ids.begin(), ids.end(), [&](int x, int y) { return pid_[static_cast<size_t>(x)] < pid_[static_cast<size_t>(y)]; });
    const auto local = [&](int pool_id) {
        size_t lo = 0, hi = ids.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (pid_[static_cast<size_t>(ids[mid])] < pool_id) lo = mid + 1;
            else hi = mid;
        }
        if (lo == ids.size() || pid_[static_cast<size_t>(ids[lo])] != pool_id) defect("a segment end that is not a point");
        return ids[lo];
    };
    if (orient(r0, r1, r2) <= 0) defect("the outer triangle is not counter-clockwise");
    tris_.clear();
    tris_.reserve(2 * pid_.size() + 4);
    tris_.push_back(Tri{{0, 1, 2}, {-1, -1, -1}});
    vtri_.assign(pid_.size(), 0);
    last_ = 0;
    for (size_t p = 3; p < pid_.size(); ++p) insert(static_cast<int>(p));
    std::vector<std::pair<int, int>> wanted;
    wanted.reserve(pieces.size());
    for (const PlanarSegment &s : pieces) {
        const int a = local(s.a), b = local(s.b);
        wanted.emplace_back(std::min(a, b), std::max(a, b));
    }
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    for (const std::pair<int, int> &edge : wanted) enforce(edge.first, edge.second);
    improve(wanted);
    triangles.resize(tris_.size());
    neighbours.resize(tris_.size());
    for (size_t k = 0; k < tris_.size(); ++k) {
        for (size_t c = 0; c < 3; ++c) {
            triangles[k][c] = pid_[static_cast<size_t>(tris_[k].v[c])];
            neighbours[k][c] = tris_[k].n[c];
        }
    }
}

void Planar::triangulate_around() {
    double ulo = 0, uhi = 0, vlo = 0, vhi = 0;
    bool first = true;
    for (const int p : points) {
        const Pt &q = pool_.pts[static_cast<size_t>(p)];
        if (first || q.c[au_].lo < ulo) ulo = q.c[au_].lo;
        if (first || q.c[au_].hi > uhi) uhi = q.c[au_].hi;
        if (first || q.c[av_].lo < vlo) vlo = q.c[av_].lo;
        if (first || q.c[av_].hi > vhi) vhi = q.c[av_].hi;
        first = false;
    }
    if (first) defect("nothing to triangulate");
    const int64_t u0 = static_cast<int64_t>(std::floor(ulo)) - 1, u1 = static_cast<int64_t>(std::ceil(uhi)) + 1;
    const int64_t v0 = static_cast<int64_t>(std::floor(vlo)) - 1, v1 = static_cast<int64_t>(std::ceil(vhi)) + 1;
    const int64_t reach = (u1 - u0) + (v1 - v0) + 2;
    const int64_t corner[3][2] = {{u0 - reach, v0 - 1}, {u1 + reach, v0 - 1}, {(u0 + u1) / 2, v1 + reach}};
    for (int k = 0; k < 3; ++k) {
        int64_t xyz[3] = {0, 0, 0};
        xyz[au_] = corner[k][0];
        xyz[av_] = corner[k][1];
        root[k] = pool_.add_grid(xyz[0], xyz[1], xyz[2]);
    }
    if (orient(root[0], root[1], root[2]) < 0) std::swap(root[1], root[2]);
    triangulate(root[0], root[1], root[2]);
}

}  // namespace detail
}  // namespace solid
}  // namespace kerfkite
