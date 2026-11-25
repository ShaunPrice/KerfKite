// Adaptive clearing for relief roughing: at each level the stock is taken away along paths that keep the width
// of cut near a chosen engagement, instead of running straight through the stock at the cutter's full width.
//
// The method is this file's own and works on a grid.
//
// * The map is covered by square-ish cells of a fraction of the cutter. Each cell knows the height the cutter's
//   flat end must stay above over it: the highest surface in the cell plus the allowance, never above the stock
//   top (the same bound the raster contact uses: the maxima of the map's cells).
// * The cutter's centre moves between the cells' corners (vertices), to one of its eight neighbours at a time.
//   A vertex is allowed at a level when no cell near enough to be under the cutter anywhere on the way to a
//   neighbour needs more height: the footprint tested is the cutter's radius plus the diagonal of a cell and
//   the tolerance by which paths are later straightened. So every move between allowed vertices is clear, and
//   walls keep that much more than the allowance.
// * At a level the stock is the cells that may be cut there. For every vertex the number of stock cells under
//   the cutter is kept up to date as cells are cut, so the stock a move would take is known at once: after a
//   move the cutter has cut everything under it, so what the next move takes is what lies under its end.
// * The cutter enters a level down a helix in the widest room there is, or straight down where there is none.
//   From there it takes, move by move, the neighbour that keeps the stock taken near the engagement without
//   exceeding it over the last few moves, preferring to carry straight on. When no such move is left it goes
//   through what it has already cleared to the nearest place where one is. Only where stock can be reached in
//   no other way (a channel no wider than the cutter) does it cut at more than the engagement; that length is
//   reported.
// * Each path is straightened within the tolerance and carries its own depths.
#include <algorithm>
#include <cmath>
#include <deque>
#include <string>
#include <vector>

#include "kerfkite/errors.hpp"
#include "kerfkite/relief.hpp"

namespace kerfkite {

namespace {

using json = Json;

const double kPi = 3.14159265358979323846;
const double kEps = 1e-9;

struct Offset {
    int i, j;
};

struct Grid {
    int nx = 0, ny = 0;  // cells; vertices are (nx + 1) x (ny + 1)
    double gx = 0, gy = 0;
    int vertex(int i, int j) const { return j * (nx + 1) + i; }
    int cell(int i, int j) const { return j * nx + i; }
    bool has_vertex(int i, int j) const { return i >= 0 && j >= 0 && i <= nx && j <= ny; }
    bool has_cell(int i, int j) const { return i >= 0 && j >= 0 && i < nx && j < ny; }
};

// The cells whose centres lie within `radius` of a vertex, as offsets from the vertex to the cell.
std::vector<Offset> footprint(const Grid &g, double radius) {
    std::vector<Offset> out;
    const int kx = static_cast<int>(std::ceil(radius / g.gx)) + 1, ky = static_cast<int>(std::ceil(radius / g.gy)) + 1;
    for (int j = -ky; j < ky; ++j)
        for (int i = -kx; i < kx; ++i)
            if (std::hypot((i + 0.5) * g.gx, (j + 0.5) * g.gy) <= radius) out.push_back({i, j});
    return out;
}

struct Point {
    double x, y, z;
};

// A path straightened: the points of a run at one depth that lie within the tolerance of the line between the
// points kept are dropped. Points where the depth changes are all kept.
void straighten(const std::vector<Point> &in, double tolerance, std::vector<Point> &out) {
    out.clear();
    size_t from = 0;
    std::vector<std::pair<size_t, size_t>> stack;
    std::vector<char> keep(in.size(), 0);
    while (from < in.size()) {
        size_t to = from;
        while (to + 1 < in.size() && in[to + 1].z == in[from].z) ++to;
        keep[from] = keep[to] = 1;
        stack.clear();
        stack.emplace_back(from, to);
        while (!stack.empty()) {
            const auto span = stack.back();
            stack.pop_back();
            if (span.second <= span.first + 1) continue;
            const Point &a = in[span.first], &b = in[span.second];
            const double dx = b.x - a.x, dy = b.y - a.y, length = std::hypot(dx, dy);
            double worst = -1;
            size_t at = span.first;
            for (size_t k = span.first + 1; k < span.second; ++k) {
                double d;
                if (length < kEps) {
                    d = std::hypot(in[k].x - a.x, in[k].y - a.y);
                } else {
                    // The distance to the segment, not to the line through it: a path may double back.
                    const double t = std::min(1.0, std::max(0.0, ((in[k].x - a.x) * dx + (in[k].y - a.y) * dy) / (length * length)));
                    d = std::hypot(in[k].x - (a.x + t * dx), in[k].y - (a.y + t * dy));
                }
                if (d > worst) worst = d, at = k;
            }
            if (worst > tolerance) {
                keep[at] = 1;
                stack.emplace_back(span.first, at);
                stack.emplace_back(at, span.second);
            }
        }
        from = to + 1;
    }
    for (size_t k = 0; k < in.size(); ++k)
        if (keep[k]) out.push_back(in[k]);
}

}  // namespace

Json adaptive_roughing(const ReliefContact &map, double origin_x, double origin_y, double step, double engagement) {
    const double radius = map.diameter / 2;
    if (!(engagement >= 0.08 * map.diameter - kEps) || engagement > map.diameter + kEps)
        throw ValueError("The engagement for adaptive clearing must be between 8% and 100% of the cutter diameter");
    const double dx = map.width / map.cols, dy = map.height / map.rows;

    // How deep the roughing can go at most, to bound the levels before the grid is chosen.
    double floor = 0;
    for (const double m : map.maxima) floor = std::min(floor, std::min(0.0, m + map.allowance));
    if (floor > -kEps) throw ValueError("There is nothing to rough: the relief leaves the whole stock at its top");
    if (std::ceil(-floor / step - kEps) > 400) throw ValueError("That is more than 400 roughing levels; use a larger step down");
    const double most_levels = std::ceil(-floor / step - kEps);

    Grid g;
    {
        double size = std::min(map.diameter / 20, engagement / 2.5);
        const double vertices = std::min(250000.0, 6.0e6 / most_levels);
        const double coarse = std::sqrt(map.width * map.height / vertices);
        if (coarse > size) {
            if (coarse > engagement / 1.6)
                throw ValueError("The relief is too large to clear with that engagement: use a larger engagement, a larger step down or the level passes");
            size = coarse;
        }
        g.nx = std::max(1, static_cast<int>(std::ceil(map.width / size - kEps)));
        g.ny = std::max(1, static_cast<int>(std::ceil(map.height / size - kEps)));
        g.gx = map.width / g.nx;
        g.gy = map.height / g.ny;
    }
    const double diagonal = std::hypot(g.gx, g.gy), tolerance = 0.4 * std::min(g.gx, g.gy);
    const double extra = diagonal + tolerance;  // what walls keep beyond the allowance
    const size_t cells = static_cast<size_t>(g.nx) * g.ny, vertices = static_cast<size_t>(g.nx + 1) * (g.ny + 1);

    // The height the cutter's end must keep over each cell.
    std::vector<double> need(cells);
    for (int j = 0; j < g.ny; ++j) {
        const int r0 = std::max(0, static_cast<int>(std::floor((j * g.gy - kEps) / dy)));
        const int r1 = std::min(map.rows - 1, static_cast<int>(std::floor(((j + 1) * g.gy + kEps) / dy)));
        for (int i = 0; i < g.nx; ++i) {
            const int c0 = std::max(0, static_cast<int>(std::floor((i * g.gx - kEps) / dx)));
            const int c1 = std::min(map.cols - 1, static_cast<int>(std::floor(((i + 1) * g.gx + kEps) / dx)));
            double high = -map.depth;
            for (int r = r0; r <= r1; ++r)
                for (int c = c0; c <= c1; ++c) high = std::max(high, map.maxima[static_cast<size_t>(r) * map.cols + c]);
            need[g.cell(i, j)] = std::min(0.0, high + map.allowance);
        }
    }

    // The lowest the cutter may be at each vertex, with every move to a neighbour in mind.
    const std::vector<Offset> under = footprint(g, radius + extra), cut = footprint(g, radius);
    if (cut.empty()) throw ValueError("The roughing cutter is too small for this relief's size: use a larger cutter or the level passes");
    std::vector<double> contact(vertices);
    double lowest = 0;
    for (int j = 0; j <= g.ny; ++j)
        for (int i = 0; i <= g.nx; ++i) {
            double high = -map.depth - 1;
            for (const Offset &o : under)
                if (g.has_cell(i + o.i, j + o.j)) high = std::max(high, need[g.cell(i + o.i, j + o.j)]);
            contact[g.vertex(i, j)] = high;
            lowest = std::min(lowest, high);
        }
    if (lowest > -kEps) throw ValueError("There is nothing to rough: the cutter cannot go below the stock top anywhere with this allowance");
    const int levels = static_cast<int>(std::ceil(-lowest / step - kEps));

    static const int kDir[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    double length_of[8], limit_of[8];
    const double cell_area = g.gx * g.gy;
    for (int d = 0; d < 8; ++d) {
        length_of[d] = std::hypot(kDir[d][0] * g.gx, kDir[d][1] * g.gy);
        limit_of[d] = engagement * length_of[d] / cell_area;  // stock cells a move may take at the engagement
    }
    const double kBurst = 1.4;  // one move may take this much of its share, if the moves before it took less
    const int kWindow = 3;       // moves looked back over

    std::vector<char> stock(cells), allowed(vertices);
    std::vector<int> count(vertices);     // stock cells under the cutter at each vertex
    std::vector<float> room(vertices);    // distance to the nearest vertex that is not allowed, or to the edge
    std::vector<int> seen(vertices, 0), parent(vertices), queue;
    int stamp_id = 0;
    queue.reserve(vertices);

    json::Array paths, depth_rows;
    size_t points = 0;
    double forced_length = 0, cut_length = 0;
    long long helixes = 0, plunges = 0;
    std::vector<Point> path, straight;

    const auto emit = [&] {
        if (!path.empty()) {
            // A plunge with nothing to cut round it is still a cut: as a path it is its point twice.
            if (path.size() == 1) path.push_back(path[0]);
            straighten(path, tolerance, straight);
            json::Array xy, z;
            xy.reserve(straight.size());
            z.reserve(straight.size());
            for (const Point &p : straight) {
                xy.push_back(json(json::Array{json(origin_x + p.x), json(origin_y + p.y)}));
                z.push_back(json(p.z));
            }
            points += straight.size();
            if (points > 200000)
                throw ValueError("The adaptive passes exceed 200,000 points; use a larger engagement, a larger step down or a smaller relief");
            paths.push_back(json(std::move(xy)));
            depth_rows.push_back(json(std::move(z)));
        }
        path.clear();
    };

    for (int level = 1; level <= levels; ++level) {
        const double above = -step * (level - 1), here = std::max(-step * level, lowest);
        long long left = 0;
        for (size_t c = 0; c < cells; ++c) stock[c] = need[c] <= here + kEps;
        for (size_t v = 0; v < vertices; ++v) allowed[v] = contact[v] <= here + kEps;
        for (int j = 0; j <= g.ny; ++j)
            for (int i = 0; i <= g.nx; ++i) {
                int n = 0;
                if (allowed[g.vertex(i, j)])
                    for (const Offset &o : cut)
                        if (g.has_cell(i + o.i, j + o.j)) n += stock[g.cell(i + o.i, j + o.j)];
                count[g.vertex(i, j)] = n;
                left += n > 0;
            }
        if (!left) continue;
        // Room round each vertex, by two passes over the grid.
        {
            const float big = 1e30f, sx = static_cast<float>(g.gx), sy = static_cast<float>(g.gy), sd = static_cast<float>(diagonal);
            for (int j = 0; j <= g.ny; ++j)
                for (int i = 0; i <= g.nx; ++i) {
                    float d = allowed[g.vertex(i, j)] ? big : 0;
                    d = std::min(d, std::min(std::min((i + 1) * sx, (g.nx - i + 1) * sx), std::min((j + 1) * sy, (g.ny - j + 1) * sy)));
                    if (i > 0) d = std::min(d, room[g.vertex(i - 1, j)] + sx);
                    if (j > 0) {
                        d = std::min(d, room[g.vertex(i, j - 1)] + sy);
                        if (i > 0) d = std::min(d, room[g.vertex(i - 1, j - 1)] + sd);
                        if (i < g.nx) d = std::min(d, room[g.vertex(i + 1, j - 1)] + sd);
                    }
                    room[g.vertex(i, j)] = d;
                }
            for (int j = g.ny; j >= 0; --j)
                for (int i = g.nx; i >= 0; --i) {
                    float d = room[g.vertex(i, j)];
                    if (i < g.nx) d = std::min(d, room[g.vertex(i + 1, j)] + sx);
                    if (j < g.ny) {
                        d = std::min(d, room[g.vertex(i, j + 1)] + sy);
                        if (i < g.nx) d = std::min(d, room[g.vertex(i + 1, j + 1)] + sd);
                        if (i > 0) d = std::min(d, room[g.vertex(i - 1, j + 1)] + sd);
                    }
                    room[g.vertex(i, j)] = d;
                }
        }

        const auto take = [&](int ci, int cj) {
            const int c = g.cell(ci, cj);
            if (!stock[c]) return;
            stock[c] = 0;
            for (const Offset &o : cut)
                if (g.has_vertex(ci - o.i, cj - o.j)) --count[g.vertex(ci - o.i, cj - o.j)];
        };
        const auto stamp = [&](int i, int j) {
            if (!count[g.vertex(i, j)]) return;
            for (const Offset &o : cut)
                if (g.has_cell(i + o.i, j + o.j)) take(i + o.i, j + o.j);
        };
        const auto at = [&](int i, int j) { return Point{i * g.gx, j * g.gy, here}; };

        // The last moves: stock cells taken and length, newest last.
        std::deque<std::pair<double, double>> recent;
        const auto rested = [&] {
            recent.clear();
            for (int k = 0; k < kWindow; ++k) recent.emplace_back(0.0, std::min(g.gx, g.gy));
        };
        const auto fits = [&](int cells_taken, int d) {
            if (cells_taken <= 0 || cells_taken > kBurst * limit_of[d]) return false;
            double taken = cells_taken, travelled = length_of[d];
            for (const auto &move : recent) taken += move.first, travelled += move.second;
            return taken <= engagement * travelled / cell_area + 1e-6;
        };
        const auto moved = [&](double taken, int d) {
            recent.emplace_back(taken, length_of[d]);
            while (static_cast<int>(recent.size()) > kWindow) recent.pop_front();
        };

        for (;;) {
            // Where to enter: the allowed vertex with stock under it that has the most room.
            int ei = -1, ej = -1;
            float best_room = -1;
            for (int j = 0; j <= g.ny; ++j)
                for (int i = 0; i <= g.nx; ++i) {
                    const int v = g.vertex(i, j);
                    if (allowed[v] && count[v] > 0 && room[v] > best_room) best_room = room[v], ei = i, ej = j;
                }
            if (ei < 0) break;
            int pi = ei, pj = ej, heading = 2;
            // A helix of the largest radius that fits, down from the level above.
            int turns_radius = static_cast<int>(std::floor(std::min(0.75 * radius, static_cast<double>(best_room) - 2 * diagonal) / g.gx));
            double helix = 0;
            for (; turns_radius >= 1; turns_radius /= 2) {
                helix = turns_radius * g.gx;
                if (helix < 0.15 * radius) {
                    turns_radius = 0;
                    break;
                }
                bool clear = ei - turns_radius >= 0 && ei + turns_radius <= g.nx && ej * g.gy - helix >= 0 && ej * g.gy + helix <= map.height;
                const double reach = radius + helix + extra;
                const int kx = static_cast<int>(std::ceil(reach / g.gx)) + 1, ky = static_cast<int>(std::ceil(reach / g.gy)) + 1;
                for (int j = -ky; clear && j < ky; ++j)
                    for (int i = -kx; i < kx; ++i)
                        if (g.has_cell(ei + i, ej + j) && std::hypot((i + 0.5) * g.gx, (j + 0.5) * g.gy) <= reach && need[g.cell(ei + i, ej + j)] > here + kEps) {
                            clear = false;
                            break;
                        }
                if (clear) break;
            }
            if (turns_radius >= 1) {
                // Down at about five degrees, then once round at the level to leave its floor flat.
                const double around = 2 * kPi * helix, fall = around * std::tan(5 * kPi / 180);
                const int turns = std::max(1, static_cast<int>(std::ceil((above - here) / fall - kEps)));
                const int sides = std::max(16, std::min(72, static_cast<int>(std::ceil(around / (2 * std::min(g.gx, g.gy))))));
                const double cx = ei * g.gx, cy = ej * g.gy;
                for (int k = 0; k <= turns * sides; ++k) {
                    const double a = 2 * kPi * (k % sides) / sides;
                    path.push_back({cx + helix * std::cos(a), cy + helix * std::sin(a), above + (here - above) * k / (turns * sides)});
                }
                for (int k = 1; k <= sides; ++k) {
                    const double a = 2 * kPi * (k % sides) / sides;
                    path.push_back({cx + helix * std::cos(a), cy + helix * std::sin(a), here});
                }
                cut_length += (turns + 1) * around;
                const double reach = radius + helix;
                const int kx = static_cast<int>(std::ceil(reach / g.gx)) + 1, ky = static_cast<int>(std::ceil(reach / g.gy)) + 1;
                for (int j = -ky; j < ky; ++j)
                    for (int i = -kx; i < kx; ++i)
                        if (g.has_cell(ei + i, ej + j) && std::hypot((i + 0.5) * g.gx, (j + 0.5) * g.gy) <= reach) take(ei + i, ej + j);
                pi = ei + turns_radius;
                ++helixes;
            } else {
                path.push_back(at(pi, pj));
                stamp(pi, pj);
                ++plunges;
            }
            rested();

            for (;;) {
                // The best move to a neighbour: near the engagement, and straight on rather than round.
                int chosen = -1;
                double chosen_score = -1e30;
                for (int d = 0; d < 8; ++d) {
                    const int qi = pi + kDir[d][0], qj = pj + kDir[d][1];
                    if (!g.has_vertex(qi, qj)) continue;
                    const int q = g.vertex(qi, qj);
                    if (!allowed[q] || !fits(count[q], d)) continue;
                    const double share = count[q] / limit_of[d];
                    int turn = std::abs(d - heading);
                    if (turn > 4) turn = 8 - turn;
                    const double score = std::min(share, 1.0) - 0.3 * std::max(0.0, share - 1) - 0.14 * turn;
                    if (score > chosen_score) chosen_score = score, chosen = d;
                }
                if (chosen >= 0) {
                    pi += kDir[chosen][0];
                    pj += kDir[chosen][1];
                    moved(count[g.vertex(pi, pj)], chosen);
                    stamp(pi, pj);
                    path.push_back(at(pi, pj));
                    cut_length += length_of[chosen];
                    heading = chosen;
                    continue;
                }
                // Through what is cleared to the nearest place with such a move; failing that, to the nearest place
                // with stock beside it at all, which is then cut at more than the engagement.
                ++stamp_id;
                queue.clear();
                const int start = g.vertex(pi, pj);
                queue.push_back(start);
                seen[start] = stamp_id;
                parent[start] = -1;
                int target = -1, target_dir = -1, fallback = -1, fallback_dir = -1, fallback_count = 0;
                rested();
                for (size_t head = 0; head < queue.size() && target < 0; ++head) {
                    const int u = queue[head], ui = u % (g.nx + 1), uj = u / (g.nx + 1);
                    for (int d = 0; d < 8; ++d) {
                        const int qi = ui + kDir[d][0], qj = uj + kDir[d][1];
                        if (!g.has_vertex(qi, qj)) continue;
                        const int q = g.vertex(qi, qj);
                        if (!allowed[q]) continue;
                        if (count[q] > 0) {
                            if (fits(count[q], d)) {
                                target = u;
                                target_dir = d;
                                break;
                            }
                            if (fallback < 0 || (u == fallback && count[q] < fallback_count)) fallback = u, fallback_dir = d, fallback_count = count[q];
                        } else if (seen[q] != stamp_id) {
                            seen[q] = stamp_id;
                            parent[q] = u;
                            queue.push_back(q);
                        }
                    }
                }
                const bool forced = target < 0;
                if (forced) target = fallback, target_dir = fallback_dir;
                if (target < 0) break;  // nothing more can be reached from here
                // The way there, in order.
                std::vector<int> way;
                for (int v = target; v != start; v = parent[v]) way.push_back(v);
                double link = 0;
                {
                    int from = start;
                    for (size_t k = way.size(); k-- > 0;) {
                        link += std::hypot((way[k] % (g.nx + 1) - from % (g.nx + 1)) * g.gx, (way[k] / (g.nx + 1) - from / (g.nx + 1)) * g.gy);
                        from = way[k];
                    }
                }
                if (link > 4 * map.diameter) {
                    // Far: lift out and come down again there, into what is already cleared.
                    emit();
                    path.push_back(at(target % (g.nx + 1), target / (g.nx + 1)));
                } else {
                    for (size_t k = way.size(); k-- > 0;) path.push_back(at(way[k] % (g.nx + 1), way[k] / (g.nx + 1)));
                    cut_length += link;
                }
                pi = target % (g.nx + 1);
                pj = target / (g.nx + 1);
                heading = target_dir;
                if (forced) {
                    pi += kDir[target_dir][0];
                    pj += kDir[target_dir][1];
                    stamp(pi, pj);
                    path.push_back(at(pi, pj));
                    forced_length += length_of[target_dir];
                    cut_length += length_of[target_dir];
                    // Carry on along the channel while it stays too narrow for anything else.
                    for (;;) {
                        rested();
                        bool free_move = false;
                        int next = -1, next_count = 0;
                        for (int d = 0; d < 8; ++d) {
                            const int qi = pi + kDir[d][0], qj = pj + kDir[d][1];
                            if (!g.has_vertex(qi, qj)) continue;
                            const int q = g.vertex(qi, qj);
                            if (!allowed[q] || count[q] <= 0) continue;
                            if (fits(count[q], d)) free_move = true;
                            int turn = std::abs(d - heading);
                            if (turn > 4) turn = 8 - turn;
                            if (turn <= 1 && (next < 0 || count[q] < next_count)) next = d, next_count = count[q];
                        }
                        if (free_move || next < 0) break;
                        pi += kDir[next][0];
                        pj += kDir[next][1];
                        stamp(pi, pj);
                        path.push_back(at(pi, pj));
                        forced_length += length_of[next];
                        cut_length += length_of[next];
                        heading = next;
                    }
                }
            }
            emit();
        }
    }
    if (paths.empty()) throw ValueError("The roughing cutter cannot reach any stock with this allowance: use a smaller cutter or the level passes");

    return json{{"paths", json(std::move(paths))},
                {"path_depths", json(std::move(depth_rows))},
                {"levels", levels},
                {"step", step},
                {"lowest", lowest},
                {"strategy", "adaptive"},
                {"engagement", engagement},
                {"grid", json::array({g.gx, g.gy})},
                {"wall_extra", extra},
                {"cut_length", cut_length},
                {"forced_length", forced_length},
                {"helix_entries", helixes},
                {"plunge_entries", plunges}};
}

}  // namespace kerfkite
