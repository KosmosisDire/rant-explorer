/* Card tiling: cards in their order, split into justified rows or columns that fill the
   pane exactly, each card as near its desired aspect as the others allow. */
#ifndef TILING_HPP
#define TILING_HPP

#include <vector>

/* The most cards the pane shows at once. The search tries every split, 2^(n-1) of them. */
constexpr int MAX_CARDS = 10;

/* What a card wants: its width over its height, and how much it minds another. A picture
   is stiff, since stretching one only adds bars. */
struct Tile {
    double aspect = 2;
    double stiff  = 1;
};

struct TileBox {
    float x = 0, y = 0, w = 0, h = 0;
};

/* The boxes for the tiles in a width by height area, gap apart, in the tiles' order. The
   aspect is of a card's content: every card keeps chrome_w by chrome_h around it for its
   name and foot. A card under the minimum size costs so much that a layout gives one up
   only when every other does too. */
std::vector<TileBox> tile(const std::vector<Tile>& tiles, float width, float height, float gap,
                          float chrome_w, float chrome_h, float min_w, float min_h);

#endif /* TILING_HPP */
