// A bounding-box tree over grid boxes. The tree is built from one sort by Morton code with the item
// index as the tie-break, so its shape is the same with every standard library. Callers must still not
// let a result depend on the order in which a query reports items.
#ifndef KERFKITE_SOLID_BVH_HPP
#define KERFKITE_SOLID_BVH_HPP

#include <algorithm>
#include <cstdint>
#include <vector>

namespace kerfkite {
namespace solid {
namespace detail {

struct Box {
    int64_t lo[3], hi[3];
    bool overlaps(const Box &o) const {
        return lo[0] <= o.hi[0] && o.lo[0] <= hi[0] && lo[1] <= o.hi[1] && o.lo[1] <= hi[1] && lo[2] <= o.hi[2] && o.lo[2] <= hi[2];
    }
    void include(const Box &o) {
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], o.lo[k]);
            hi[k] = std::max(hi[k], o.hi[k]);
        }
    }
};

class Bvh {
public:
    struct Node {
        Box box;
        int left, right;   // children, or -1 in a leaf
        int first, count;  // items of a leaf
    };
    std::vector<Node> nodes;
    std::vector<int> items;

    void build(const std::vector<Box> &boxes) {
        nodes.clear();
        items.clear();
        const size_t n = boxes.size();
        if (!n) return;
        Box all = boxes[0];
        for (const Box &b : boxes) all.include(b);
        // 21 bits an axis of the doubled centre, relative to the whole.
        int shift = 0;
        for (int k = 0; k < 3; ++k)
            while (((2 * (all.hi[k] - all.lo[k])) >> shift) >= (int64_t(1) << 21)) ++shift;
        std::vector<std::pair<uint64_t, int>> keyed(n);
        for (size_t i = 0; i < n; ++i) {
            uint64_t code = 0;
            for (int k = 0; k < 3; ++k) {
                const uint64_t c = static_cast<uint64_t>((boxes[i].lo[k] + boxes[i].hi[k] - 2 * all.lo[k]) >> shift);
                code |= spread(c) << k;
            }
            keyed[i] = std::make_pair(code, static_cast<int>(i));
        }
        std::sort(keyed.begin(), keyed.end());
        items.resize(n);
        for (size_t i = 0; i < n; ++i) items[i] = keyed[i].second;
        nodes.reserve(n / 2 + 16);
        struct Job {
            int node, first, count;
        };
        std::vector<Job> stack;
        nodes.push_back(Node());
        stack.push_back(Job{0, 0, static_cast<int>(n)});
        while (!stack.empty()) {
            const Job job = stack.back();
            stack.pop_back();
            Box box = boxes[static_cast<size_t>(items[static_cast<size_t>(job.first)])];
            for (int i = 1; i < job.count; ++i) box.include(boxes[static_cast<size_t>(items[static_cast<size_t>(job.first + i)])]);
            Node node;
            node.box = box;
            node.left = node.right = -1;
            node.first = job.first;
            node.count = job.count;
            if (job.count > 4) {
                const int half = job.count / 2;
                node.left = static_cast<int>(nodes.size());
                node.right = node.left + 1;
                node.count = 0;
                nodes.push_back(Node());
                nodes.push_back(Node());
                stack.push_back(Job{node.left, job.first, half});
                stack.push_back(Job{node.right, job.first + half, job.count - half});
            }
            nodes[static_cast<size_t>(job.node)] = node;
        }
    }

    // Calls visit(item) for every item whose box overlaps `box`.
    template <class F>
    void query(const std::vector<Box> &boxes, const Box &box, F &&visit) const {
        if (nodes.empty()) return;
        int stack[96];
        int top = 0;
        stack[top++] = 0;
        while (top) {
            const Node &node = nodes[static_cast<size_t>(stack[--top])];
            if (!node.box.overlaps(box)) continue;
            if (node.left < 0) {
                for (int i = 0; i < node.count; ++i) {
                    const int item = items[static_cast<size_t>(node.first + i)];
                    if (boxes[static_cast<size_t>(item)].overlaps(box)) visit(item);
                }
            } else {
                stack[top++] = node.left;
                stack[top++] = node.right;
            }
        }
    }

private:
    static uint64_t spread(uint64_t x) {
        x &= 0x1fffff;
        x = (x | (x << 32)) & 0x1f00000000ffffULL;
        x = (x | (x << 16)) & 0x1f0000ff0000ffULL;
        x = (x | (x << 8)) & 0x100f00f00f00f00fULL;
        x = (x | (x << 4)) & 0x10c30c30c30c30c3ULL;
        x = (x | (x << 2)) & 0x1249249249249249ULL;
        return x;
    }
};

}  // namespace detail
}  // namespace solid
}  // namespace kerfkite

#endif
