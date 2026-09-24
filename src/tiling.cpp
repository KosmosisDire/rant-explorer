#include "tiling.hpp"

#include <algorithm>
#include <cmath>

namespace {

/* The weights of the two costs: how far a card is from its aspect, and how unequal the
   cards' areas are, so no card is starved to make another perfect. */
constexpr double STRETCH = 1, BALANCE = 1;

/* The cost of a card under the minimum size, before its shortfall adds more. */
constexpr double TOO_SMALL = 5, SHORTFALL = 50;

struct Item {
    Tile  tile;
    float min_w, min_h;
    int   index;
};

struct Layout {
    double               cost = INFINITY;
    std::vector<TileBox> boxes;   /* by the tiles' index */
};

/* One split into lines across a width by height area of content, gap_in apart inside a
   line and gap_across between lines. In a line every card shares a height, so each keeps
   its aspect exactly. The lines then fill the height, each giving way by its stiffness:
   line r scales by exp(p / stiff_r), with p found by bisection. */
Layout fit(const std::vector<std::vector<Item>>& lines, float W, float H, float gap_in, float gap_across)
{
    const size_t n = lines.size();
    std::vector<double> natural(n), stiff(n);
    for (size_t r = 0; r < n; r++) {
        double sum = 0, s = 0;
        for (const Item& it : lines[r]) {
            sum += it.tile.aspect;
            s   += it.tile.stiff;
        }
        natural[r] = (W - gap_in * (lines[r].size() - 1)) / sum;
        stiff[r]   = s / lines[r].size();
    }
    const double room = H - gap_across * (n - 1);
    Layout out;
    if (room <= 0) return out;
    auto total = [&](double p) {
        double t = 0;
        for (size_t r = 0; r < n; r++) t += natural[r] * std::exp(p / stiff[r]);
        return t;
    };
    double lo = -20, hi = 20;
    for (int k = 0; k < 60; k++) {
        const double mid = (lo + hi) / 2;
        (total(mid) > room ? hi : lo) = mid;
    }
    const double p = (lo + hi) / 2;

    out.cost = 0;
    size_t count = 0;
    for (const auto& line : lines) count += line.size();
    out.boxes.resize(count);
    std::vector<double> logs;
    double y = 0;
    for (size_t r = 0; r < n; r++) {
        const double h = natural[r] * std::exp(p / stiff[r]);
        double sum = 0;
        for (const Item& it : lines[r]) sum += it.tile.aspect;
        const double free = W - gap_in * (lines[r].size() - 1);
        double x = 0;
        for (const Item& it : lines[r]) {
            const double w = free * it.tile.aspect / sum;
            const double stretch = std::log((w / h) / it.tile.aspect);
            out.cost += STRETCH * it.tile.stiff * stretch * stretch * (w * h) / ((double)W * H);
            if (w < it.min_w) out.cost += TOO_SMALL + SHORTFALL * std::pow((it.min_w - w) / it.min_w, 2);
            if (h < it.min_h) out.cost += TOO_SMALL + SHORTFALL * std::pow((it.min_h - h) / it.min_h, 2);
            out.boxes[it.index] = TileBox{ (float)x, (float)y, (float)w, (float)h };
            logs.push_back(std::log(std::max(w * h, 1e-6)));
            x += w + gap_in;
        }
        y += h + gap_across;
    }
    double mean = 0, spread = 0;
    for (const double v : logs) mean += v;
    mean /= logs.size();
    for (const double v : logs) spread += (v - mean) * (v - mean);
    out.cost += BALANCE * spread / logs.size();
    return out;
}

/* The best of every split of the items, in order, into lines. */
Layout justify(const std::vector<Item>& items, float W, float H, float gap_in, float gap_across)
{
    Layout best;
    const size_t n = items.size();
    for (unsigned mask = 0; mask < (1u << (n - 1)); mask++) {
        std::vector<std::vector<Item>> lines(1);
        for (size_t i = 0; i < n; i++) {
            lines.back().push_back(items[i]);
            if (i + 1 < n && (mask >> i) & 1) lines.emplace_back();
        }
        Layout cand = fit(lines, W, H, gap_in, gap_across);
        if (cand.cost < best.cost) best = std::move(cand);
    }
    return best;
}

} /* namespace */

std::vector<TileBox> tile(const std::vector<Tile>& tiles, float width, float height, float gap,
                          float chrome_w, float chrome_h, float min_w, float min_h)
{
    /* Laid out as content: a card's chrome joins the gap after it, and the last card's
       comes off the area. */
    const float W = width - chrome_w, H = height - chrome_h;
    const float gap_x = gap + chrome_w, gap_y = gap + chrome_h;
    if (tiles.empty() || W <= 0 || H <= 0) return {};
    const float cw = std::max(1.f, min_w - chrome_w), ch = std::max(1.f, min_h - chrome_h);
    std::vector<Item> rows, cols;
    for (size_t i = 0; i < tiles.size() && i < (size_t)MAX_CARDS; i++) {
        Tile t = tiles[i];
        t.aspect = std::max(t.aspect, 0.05);
        rows.push_back(Item{ t, cw, ch, (int)i });
        /* columns are rows of the transpose */
        Tile turned = t;
        turned.aspect = 1 / t.aspect;
        cols.push_back(Item{ turned, ch, cw, (int)i });
    }
    Layout across = justify(rows, W, H, gap_x, gap_y);
    Layout down   = justify(cols, H, W, gap_y, gap_x);
    if (down.cost < across.cost) {
        for (TileBox& b : down.boxes) b = TileBox{ b.y, b.x, b.h, b.w };
        across = std::move(down);
    }
    for (TileBox& b : across.boxes) {
        b.w += chrome_w;
        b.h += chrome_h;
    }
    return across.boxes;
}
