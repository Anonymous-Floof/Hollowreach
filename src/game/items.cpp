#include "game/items.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace hr::game {
namespace {

constexpr int G = kSpriteSize;

// --- colour helpers ---------------------------------------------------------

// js/game/items.js:176 — multiply each channel, clamp at 255, truncate. The
// truncation is not incidental: `| 0` after a float multiply is what gives the
// palettes their exact step values, and rounding instead shifts entire sprites.
Rgba shade(std::uint32_t hex, double f) {
  const auto ch = [hex, f](int shift) {
    const double v = static_cast<double>((hex >> shift) & 255u) * f;
    return static_cast<std::uint8_t>(static_cast<int>(std::min(255.0, v)));
  };
  return {ch(16), ch(8), ch(0), 255};
}

constexpr Rgba solid(std::uint32_t hex) {
  return {static_cast<std::uint8_t>((hex >> 16) & 255u),
          static_cast<std::uint8_t>((hex >> 8) & 255u),
          static_cast<std::uint8_t>(hex & 255u), 255};
}

// Shared wood palette for handles, hafts and grips (js/game/items.js:209).
constexpr Rgba HW = solid(0xa8845au);
constexpr Rgba HM = solid(0x7e6038u);
constexpr Rgba HD = solid(0x553f24u);

// rgba(24,18,12,0.82). Canvas rounds the alpha to 209, and the extruder's cutoff
// is 128, so the rim is part of the model silhouette as well as the icon.
constexpr Rgba kOutline {24, 18, 12, 209};

// --- 16x16 grid helpers -----------------------------------------------------

void pset(SpriteGrid& g, int x, int y, Rgba c) {
  if (x >= 0 && x < G && y >= 0 && y < G) g[static_cast<std::size_t>(y) * G + x] = c;
}
void prow(SpriteGrid& g, int x0, int x1, int y, Rgba c) {
  for (int x = x0; x <= x1; ++x) pset(g, x, y, c);
}
void pcol(SpriteGrid& g, int x, int y0, int y1, Rgba c) {
  for (int y = y0; y <= y1; ++y) pset(g, x, y, c);
}
bool filledAt(const SpriteGrid& g, int x, int y) {
  return g[static_cast<std::size_t>(y) * G + x].a != 0;
}

// Vertical two-tone haft with a darker butt end.
void haft(SpriteGrid& g, int y0, int y1) {
  pcol(g, 7, y0, y1, HW);
  pcol(g, 8, y0, y1, HM);
  pset(g, 7, y1, HD);
  pset(g, 8, y1, HD);
}

// --- sprite painters --------------------------------------------------------
// One per IconKind, transcribed from js/game/items.js:220-467. These are the
// densest lines in the port: every literal here is a texel of finished art, and
// a single transposed coordinate changes an icon *and* the 3D model built from it.

void paintStick(SpriteGrid& g, std::uint32_t, const ItemDef&) {
  for (int i = 0; i < 8; ++i) {
    pset(g, 4 + i, 13 - i, HW);
    pset(g, 5 + i, 13 - i, HM);
  }
  pset(g, 7, 10, HD);  // knot
}

void paintPick(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba M = shade(col, 1.45), m = solid(col), d = shade(col, 0.62);
  haft(g, 3, 14);
  // crescent head: lit along the crown, thickened underneath
  static constexpr int kArc[][2] = {{2, 7},  {2, 6},  {2, 5},  {3, 4}, {4, 3},  {5, 2},
                                    {6, 2},  {7, 1},  {8, 1},  {9, 2}, {10, 2}, {11, 3},
                                    {12, 4}, {13, 5}, {13, 6}, {13, 7}};
  for (const auto& p : kArc) pset(g, p[0], p[1], p[1] <= 2 ? M : m);
  static constexpr int kUnder[][2] = {{3, 5}, {4, 4},  {5, 3},  {6, 3},
                                      {7, 2}, {8, 2},  {9, 3},  {10, 3},
                                      {11, 4}, {12, 5}};
  for (const auto& p : kUnder) pset(g, p[0], p[1], d);
}

void paintAxe(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba M = shade(col, 1.45), m = solid(col), d = shade(col, 0.62);
  haft(g, 4, 14);
  // bearded blade on the left of the haft, poll nub on the right
  prow(g, 5, 8, 1, m);
  prow(g, 3, 8, 2, m);
  prow(g, 2, 8, 3, m);
  prow(g, 2, 6, 4, m);
  prow(g, 2, 5, 5, m);
  prow(g, 3, 4, 6, d);
  pcol(g, 2, 3, 5, M);
  pset(g, 3, 2, M);  // cutting edge catches light
  pset(g, 9, 2, d);
  pset(g, 9, 3, d);  // poll
}

void paintShovel(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba M = shade(col, 1.45), m = solid(col), d = shade(col, 0.62);
  // spade blade up top, neck, then the haft
  prow(g, 6, 9, 0, M);
  prow(g, 5, 10, 1, m);
  prow(g, 5, 10, 2, m);
  prow(g, 5, 10, 3, m);
  prow(g, 6, 9, 4, m);
  prow(g, 7, 8, 5, m);
  pcol(g, 5, 1, 3, M);  // lit rim
  pcol(g, 10, 1, 3, d);
  pset(g, 9, 4, d);  // shaded rim
  haft(g, 6, 14);
}

void paintSword(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba M = shade(col, 1.45), m = solid(col), d = shade(col, 0.72);
  // Diagonal blade, three texels thick (lit edge / core / shaded edge). The rows
  // stack straight DOWN from the lit edge — offsetting them diagonally leaves
  // corner-touching gaps that read as a checkerboard once the sprite is extruded
  // and magnified as the held model.
  pset(g, 14, 1, M);
  pset(g, 14, 2, m);
  for (int i = 0; i < 9; ++i) {
    pset(g, 13 - i, 2 + i, M);
    pset(g, 13 - i, 3 + i, m);
    pset(g, 13 - i, 4 + i, d);
  }
  static constexpr int kGrip[][2] = {{2, 9}, {3, 10}, {4, 11}, {5, 12}, {6, 13}};
  for (const auto& p : kGrip) pset(g, p[0], p[1], HD);
  pset(g, 3, 10, HM);
  pset(g, 5, 12, HM);
  pset(g, 3, 12, HM);
  pset(g, 2, 13, HM);  // grip
  pset(g, 1, 14, HD);
  pset(g, 2, 14, HD);  // pommel
}

void paintIngot(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba M = shade(col, 1.4), m = solid(col), d = shade(col, 0.62);
  prow(g, 5, 12, 5, M);  // lit top face
  prow(g, 4, 13, 6, shade(col, 1.18));
  for (int y = 7; y <= 10; ++y) prow(g, 3, 12, y, m);
  pcol(g, 13, 7, 10, d);  // right end
  prow(g, 3, 13, 11, d);  // base shadow
  pset(g, 5, 8, M);
  pset(g, 6, 7, M);  // gleam
}

void paintNugget(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba M = shade(col, 1.4), m = solid(col), d = shade(col, 0.62);
  prow(g, 6, 9, 5, m);
  prow(g, 5, 11, 6, m);
  prow(g, 4, 11, 7, m);
  prow(g, 4, 12, 8, m);
  prow(g, 5, 12, 9, m);
  prow(g, 5, 11, 10, d);
  prow(g, 6, 10, 11, d);
  pset(g, 6, 6, M);
  pset(g, 7, 6, M);
  pset(g, 5, 7, M);
}

// A pinch of ground pigment: a small heap with a scatter of loose grains above it.
// Shaded from ItemDef::color like every other painter here, so eight dyes are eight
// table rows and no sprite work at all.
void paintDye(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba M = shade(col, 1.9), m = solid(col), d = shade(col, 0.5);
  // The heap.
  prow(g, 4, 11, 12, d);
  prow(g, 4, 11, 11, m);
  prow(g, 5, 10, 10, m);
  prow(g, 6, 9, 9, M);
  // Loose grains drifting off it, so a dye reads as powder rather than as a stone.
  pset(g, 5, 7, m);
  pset(g, 9, 6, m);
  pset(g, 7, 5, M);
  pset(g, 11, 8, d);
  pset(g, 3, 9, d);
}

// A wooden board with a thumb hole and six dabs of wet colour on it. The dabs are
// hard-coded rather than shaded from ItemDef::color, and deliberately: this is the
// one item in the game whose whole subject IS the colours, so a single-hue version
// of it would say nothing about what it does.
void paintPalette(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba M = shade(col, 1.5), m = solid(col), d = shade(col, 0.55);
  // The board: a rounded slab.
  prow(g, 3, 12, 4, M);
  for (int y = 5; y <= 10; ++y) prow(g, 2, 13, y, m);
  prow(g, 3, 12, 11, d);
  // The thumb hole. Painted very dark rather than cut out: the sprite is also
  // extruded into the held and dropped models, and a hole through the middle of one
  // texel of board reads as a gap in the mesh rather than as a thumb hole.
  const Rgba hole = shade(col, 0.22);
  pset(g, 4, 9, hole);
  pset(g, 5, 9, hole);
  pset(g, 4, 10, hole);
  pset(g, 5, 10, hole);
  // Six dabs, one per corner of the wheel. These are the only literal colours in
  // any painter here.
  pset(g, 4, 6, Rgba{0xd2, 0x3a, 0x34, 255});
  pset(g, 6, 5, Rgba{0xe8, 0x86, 0x2a, 255});
  pset(g, 8, 5, Rgba{0xf2, 0xc5, 0x3a, 255});
  pset(g, 10, 6, Rgba{0x4f, 0xae, 0x53, 255});
  pset(g, 11, 8, Rgba{0x4a, 0x6f, 0xe0, 255});
  pset(g, 9, 9, Rgba{0x9a, 0x5a, 0xc2, 255});
}

void paintLump(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba M = shade(col, 2.4), m = solid(col), d = shade(col, 0.45);
  prow(g, 6, 9, 4, m);
  prow(g, 5, 10, 5, m);
  prow(g, 4, 11, 6, m);
  prow(g, 4, 12, 7, m);
  prow(g, 3, 12, 8, m);
  prow(g, 4, 12, 9, m);
  prow(g, 4, 11, 10, m);
  prow(g, 5, 10, 11, d);
  prow(g, 6, 9, 12, d);
  pset(g, 6, 5, M);
  pset(g, 5, 6, M);
  pset(g, 6, 6, M);  // glinting facet
  pcol(g, 11, 7, 9, d);
  pset(g, 12, 8, d);  // fractured face
}

void paintGem(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba M = shade(col, 1.5), m = solid(col), d = shade(col, 0.6);
  prow(g, 5, 10, 3, M);  // table
  prow(g, 4, 11, 4, m);
  prow(g, 3, 12, 5, m);  // girdle (widest)
  prow(g, 4, 11, 6, m);
  prow(g, 5, 10, 7, m);
  prow(g, 6, 9, 8, m);
  prow(g, 7, 8, 9, m);
  prow(g, 7, 8, 10, d);  // culet point
  pset(g, 4, 4, M);
  pset(g, 3, 5, M);
  pset(g, 4, 6, M);
  pset(g, 5, 7, M);  // lit facets
  pset(g, 11, 4, d);
  pset(g, 12, 5, d);
  pset(g, 11, 6, d);
  pset(g, 10, 7, d);
  pset(g, 6, 4, solid(0xffffffu));  // sparkle
}

void paintShard(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba M = shade(col, 1.5), m = solid(col), d = shade(col, 0.55);
  pset(g, 7, 1, M);
  pcol(g, 6, 3, 12, M);
  pcol(g, 7, 2, 12, m);
  pcol(g, 8, 4, 12, d);  // tall spike
  pset(g, 10, 6, M);
  pcol(g, 10, 7, 12, m);
  pcol(g, 11, 8, 12, d);  // companion
  prow(g, 5, 12, 13, d);  // base rubble
}

void paintBoat(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba M = shade(col, 1.3), m = solid(col), d = shade(col, 0.6);
  prow(g, 1, 14, 8, M);  // gunwale
  prow(g, 2, 13, 9, m);
  prow(g, 3, 12, 10, m);
  prow(g, 4, 11, 11, m);
  prow(g, 5, 10, 12, d);  // keel
  prow(g, 5, 10, 9, d);   // shaded interior
  pset(g, 7, 9, HW);
  pset(g, 8, 9, HW);  // seat plank
}

void paintMeat(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba M = shade(col, 1.3), m = solid(col), d = shade(col, 0.72);
  // the chop: an oval with a shaded underside and a marbling streak
  for (int y = 2; y <= 12; ++y) {
    for (int x = 5; x <= 14; ++x) {
      const double a = (x - 9.7) / 4.9, b = (y - 7) / 5.1;
      const double v = a * a + b * b;
      if (v > 1) continue;
      pset(g, x, y, (v > 0.6 && (x > 10 || y > 8)) ? d : m);
    }
  }
  static constexpr int kMarble[][2] = {{8, 4}, {8, 5}, {9, 6}, {9, 7}, {10, 8}};
  for (const auto& p : kMarble) pset(g, p[0], p[1], M);
  // protruding bone with a knuckle
  const Rgba B = solid(0xefe6d2u), Bd = solid(0xcfc2a4u);
  pset(g, 6, 10, B);
  pset(g, 5, 11, B);
  pset(g, 6, 11, Bd);
  pset(g, 4, 12, B);
  pset(g, 5, 12, Bd);
  pset(g, 2, 11, B);
  pset(g, 3, 11, B);
  pset(g, 2, 12, B);
  pset(g, 3, 12, Bd);
  pset(g, 2, 13, B);
  pset(g, 3, 13, Bd);
}

void paintFlesh(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  // ragged, hole-riddled slab in sickly greens and browns
  const Rgba m = solid(col), d = shade(col, 0.66), b = solid(0x6e5a38u);
  for (int y = 3; y <= 13; ++y) {
    for (int x = 2; x <= 13; ++x) {
      if ((x == 2 || x == 13) && y % 3 != 1) continue;  // ragged sides
      if ((y == 3 || y == 13) && x % 3 == 0) continue;  // ragged ends
      if ((x * 3 + y * 5) % 11 == 0) continue;          // rot holes
      pset(g, x, y, (x * 7 + y * 3) % 9 < 3 ? b : ((x + y) % 4 == 0 ? d : m));
    }
  }
}

void paintPaper(SpriteGrid& g, std::uint32_t, const ItemDef&) {
  const Rgba P = solid(0xece7d4u), S = solid(0xcfc8aeu), L = solid(0x8f886eu);
  for (int y = 2; y <= 13; ++y) prow(g, 4, 11, y, P);
  for (int y = 2; y <= 13; ++y) pset(g, 11, y, S);  // shaded right edge
  prow(g, 4, 11, 13, S);
  pset(g, 11, 2, S);
  pset(g, 10, 2, S);
  pset(g, 11, 3, S);  // dog-eared corner
  for (int y : {5, 8, 11}) prow(g, 6, 9, y, L);  // faint script lines
}

void paintLeather(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba M = shade(col, 1.25), m = solid(col), d = shade(col, 0.62);
  // a tanned hide: irregular blob with darker crinkled edges
  for (int y = 3; y <= 12; ++y) {
    for (int x = 3; x <= 12; ++x) {
      if ((x == 3 || x == 12) && (y < 5 || y > 10)) continue;
      if ((y == 3 || y == 12) && (x < 5 || x > 10)) continue;
      pset(g, x, y, (x + y * 3) % 7 == 0 ? d : m);
    }
  }
  pset(g, 5, 4, M);
  pset(g, 6, 4, M);
  pset(g, 4, 6, M);  // worn sheen
  pset(g, 7, 8, d);
  pset(g, 9, 6, d);
  pset(g, 6, 10, d);  // crease marks
}

void paintSteak(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba M = shade(col, 1.35), m = solid(col), d = shade(col, 0.66);
  const Rgba F = solid(0xe8dcc0u);
  // a thick-cut slab with a fat rind along the top edge
  for (int y = 4; y <= 12; ++y) {
    for (int x = 3; x <= 13; ++x) {
      const double a = (x - 8) / 5.4, b = (y - 8) / 4.6;
      const double v = a * a + b * b;
      if (v > 1) continue;
      pset(g, x, y, (v > 0.62 && (x > 9 || y > 9)) ? d : m);
    }
  }
  static constexpr int kRind[][2] = {{5, 4}, {6, 4}, {7, 4}, {8, 4},
                                     {9, 4}, {4, 5}, {10, 4}};
  for (const auto& p : kRind) pset(g, p[0], p[1], F);
  static constexpr int kSear[][2] = {{6, 6}, {7, 7}, {8, 8}, {6, 9}};
  for (const auto& p : kSear) pset(g, p[0], p[1], M);
}

void paintBucket(SpriteGrid& g, std::uint32_t col, const ItemDef& item) {
  const Rgba M = shade(col, 1.35), m = solid(col), d = shade(col, 0.6);
  static constexpr int kHandle[][2] = {{5, 3}, {6, 2}, {7, 2}, {8, 2}, {9, 2}, {10, 3}};
  for (const auto& p : kHandle) pset(g, p[0], p[1], d);
  // tapering pail
  prow(g, 4, 11, 5, M);
  for (int y = 6; y <= 11; ++y) {
    const int inz = (y - 6) >> 1;
    prow(g, 4 + inz, 11 - inz, y, m);
  }
  prow(g, 6, 9, 12, d);
  pcol(g, 4, 6, 9, M);
  pcol(g, 11, 6, 9, d);
  // contents peeking over the rim
  if (item.hasFill) {
    prow(g, 5, 10, 5, solid(item.fill));
    prow(g, 5, 10, 4, shade(item.fill, 1.15));
  }
}

void paintAtlas(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba C = solid(col), Cd = shade(col, 0.62);
  const Rgba P = solid(0xe8e2ccu), A = solid(0x3f77d9u), Gold = solid(0xc8a23au);
  // a stout leather-bound tome, pages on the right, a compass-rose clasp
  for (int y = 2; y <= 13; ++y) prow(g, 3, 11, y, C);
  pcol(g, 3, 2, 13, Cd);  // spine
  for (int y = 3; y <= 12; ++y) {
    pset(g, 12, y, P);
    pset(g, 13, y, shade(0xe8e2ccu, 0.8));
  }  // page block
  prow(g, 3, 11, 13, Cd);
  pcol(g, 7, 2, 13, Gold);  // gilt band
  pset(g, 9, 6, Gold);
  pset(g, 9, 8, Gold);
  pset(g, 8, 7, Gold);
  pset(g, 10, 7, Gold);  // rose points
  pset(g, 9, 7, A);      // compass jewel
}

void paintWayshard(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba M = shade(col, 1.5), m = solid(col), d = shade(col, 0.55);
  const Rgba W = solid(0xf2ecffu);
  // a rising sliver with motes streaming skyward off its tip
  pset(g, 8, 2, W);
  pset(g, 6, 4, M);
  pset(g, 10, 5, M);  // motes
  for (int i = 0; i < 8; ++i) {
    pset(g, 7, 5 + i, M);
    pset(g, 8, 5 + i, m);
    pset(g, 9, 6 + i, d);
  }
  pset(g, 8, 4, M);
  prow(g, 6, 10, 13, d);  // base chips
}

void paintArmorHelmet(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba M = shade(col, 1.4), m = solid(col), d = shade(col, 0.62);
  prow(g, 5, 10, 3, M);  // crown highlight
  prow(g, 4, 11, 4, m);
  for (int y = 5; y <= 8; ++y) prow(g, 3, 12, y, m);
  prow(g, 5, 10, 8, d);  // brow shadow over the face
  for (int y = 9; y <= 11; ++y) {
    prow(g, 3, 4, y, m);
    prow(g, 11, 12, y, m);
  }  // cheek guards
  pcol(g, 3, 5, 9, M);
  pcol(g, 12, 5, 9, d);
}

void paintArmorChest(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba M = shade(col, 1.4), m = solid(col), d = shade(col, 0.62);
  prow(g, 2, 5, 3, m);
  prow(g, 10, 13, 3, m);  // shoulders beside the neck hole
  prow(g, 2, 5, 4, m);
  prow(g, 10, 13, 4, m);
  for (int y = 5; y <= 12; ++y) prow(g, 3, 12, y, m);
  pcol(g, 2, 5, 7, m);
  pcol(g, 13, 5, 7, m);  // sleeve stubs
  pcol(g, 3, 5, 11, M);
  pcol(g, 12, 5, 11, d);
  prow(g, 3, 12, 12, d);  // waist shadow
  pset(g, 6, 5, M);
  pset(g, 9, 5, d);  // collar rim
}

void paintArmorLegs(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba M = shade(col, 1.4), m = solid(col), d = shade(col, 0.62);
  prow(g, 3, 12, 3, M);  // belt highlight
  prow(g, 3, 12, 4, m);
  prow(g, 3, 12, 5, m);
  for (int y = 6; y <= 13; ++y) {
    prow(g, 3, 6, y, m);
    prow(g, 9, 12, y, m);
  }
  pcol(g, 6, 6, 13, d);
  pcol(g, 9, 6, 13, d);  // inner seams
  pcol(g, 3, 6, 12, M);
  prow(g, 3, 6, 13, d);
  prow(g, 9, 12, 13, d);  // hems
}

void paintArmorBoots(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba M = shade(col, 1.4), m = solid(col), d = shade(col, 0.62);
  for (int y = 7; y <= 9; ++y) {
    prow(g, 3, 6, y, m);
    prow(g, 10, 13, y, m);
  }
  prow(g, 3, 6, 7, M);
  prow(g, 10, 13, 7, M);  // cuff highlights
  for (int y = 10; y <= 12; ++y) {
    prow(g, 2, 7, y, m);
    prow(g, 9, 14, y, m);
  }
  prow(g, 2, 7, 12, d);
  prow(g, 9, 14, 12, d);  // soles
}

using PainterFn = void (*)(SpriteGrid&, std::uint32_t, const ItemDef&);

// --- produce and meals -------------------------------------------------------
//
// Nine painters cover eighteen crops and two dozen meals, because every one of them
// shades from ItemDef::color the way paintMeat already does. A new crop is a table
// row and a hex value; it does not need a new sprite drawn by hand, which is the
// only reason a content target this size is affordable at all.

// A tied bundle of stalks: wheat, barley, rice, maize.
void paintGrain(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba m = solid(col), d = shade(col, 0.74), M = shade(col, 1.25);
  for (int i = 0; i < 5; ++i) {
    const int x = 4 + i * 2;
    for (int y = 3; y <= 13; ++y) pset(g, x, y, (i % 2) ? m : M);
    pset(g, x, 2, M);  // the ear
    pset(g, x - 1, 3, d);
    pset(g, x + 1, 4, d);
  }
  prow(g, 3, 12, 11, shade(0x8a6a3a, 1.0));  // the binding twine
  prow(g, 3, 12, 12, shade(0x6f5430, 1.0));
}

// A tapered root with a leafy crown: carrot, potato, onion, beetroot, garlic.
void paintRoot(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba m = solid(col), d = shade(col, 0.72), M = shade(col, 1.22);
  const Rgba leaf = shade(0x4f9e46, 1.0), leafD = shade(0x3f8a3a, 1.0);
  for (int y = 5; y <= 14; ++y) {
    const int half = (14 - y) / 2 + 1;  // widest at the shoulder, pointed at the tip
    for (int x = 8 - half; x <= 8 + half; ++x) {
      pset(g, x, y, x <= 8 - half + 1 ? M : (x >= 8 + half ? d : m));
    }
  }
  pset(g, 7, 4, leaf);
  pset(g, 9, 4, leaf);
  pset(g, 8, 3, leafD);
  pset(g, 6, 3, leafD);
  pset(g, 10, 3, leaf);
  pset(g, 8, 2, leaf);
}

// A round fruit with a stem: pumpkin, melon, tomato.
void paintProduce(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba m = solid(col), d = shade(col, 0.70), M = shade(col, 1.24);
  for (int y = 4; y <= 14; ++y) {
    for (int x = 3; x <= 13; ++x) {
      const double a = (x - 8.0) / 5.2, b = (y - 9.0) / 5.2;
      const double v = a * a + b * b;
      if (v > 1.0) continue;
      pset(g, x, y, v < 0.25 && x < 8 ? M : (v > 0.62 && (x > 8 || y > 10) ? d : m));
    }
  }
  pset(g, 8, 3, shade(0x4a7a32, 1.0));  // stem
  pset(g, 8, 2, shade(0x3f6a2a, 1.0));
  pset(g, 9, 3, shade(0x5a8a3f, 1.0));
}

// A long pod: chili, soybean.
void paintPod(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba m = solid(col), d = shade(col, 0.72), M = shade(col, 1.26);
  for (int y = 4; y <= 13; ++y) {
    const int x = 6 + (y - 4) / 3;  // a gentle curve
    pset(g, x, y, M);
    pset(g, x + 1, y, m);
    pset(g, x + 2, y, d);
  }
  pset(g, 6, 3, shade(0x4a7a32, 1.0));
  pset(g, 7, 2, shade(0x3f6a2a, 1.0));
}

// A small cluster: strawberry, blueberry, grapes.
void paintBerry(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba m = solid(col), d = shade(col, 0.70), M = shade(col, 1.28);
  static constexpr int kBerries[][2] = {{6, 7}, {10, 7}, {8, 10}, {5, 11}, {11, 11}};
  for (const auto& b : kBerries) {
    for (int y = b[1] - 1; y <= b[1] + 1; ++y) {
      for (int x = b[0] - 1; x <= b[0] + 1; ++x) {
        if (x == b[0] - 1 && y == b[1] - 1) continue;  // round the corners off
        if (x == b[0] + 1 && y == b[1] + 1) continue;
        pset(g, x, y, (x == b[0] - 1 || y == b[1] - 1) ? M : (y == b[1] + 1 ? d : m));
      }
    }
  }
  pset(g, 8, 4, shade(0x4a7a32, 1.0));  // a sprig on top
  pset(g, 7, 5, shade(0x3f6a2a, 1.0));
  pset(g, 9, 5, shade(0x5a8a3f, 1.0));
}

// A leafy head: cabbage.
void paintLeafy(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba m = solid(col), d = shade(col, 0.72), M = shade(col, 1.30);
  for (int y = 3; y <= 14; ++y) {
    for (int x = 3; x <= 13; ++x) {
      const double a = (x - 8.0) / 5.4, b = (y - 8.5) / 5.8;
      if (a * a + b * b > 1.0) continue;
      pset(g, x, y, m);
    }
  }
  // Veins, which is what stops it reading as a plain green ball.
  for (int y = 5; y <= 12; ++y) pset(g, 8, y, M);
  pset(g, 6, 7, M);
  pset(g, 10, 7, M);
  pset(g, 5, 10, d);
  pset(g, 11, 10, d);
  prow(g, 6, 10, 14, d);
}

// A scatter of seed: the wild bootstrap.
void paintSeed(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba m = solid(col), d = shade(col, 0.74);
  static constexpr int kSeeds[][2] = {{5, 6}, {9, 5}, {7, 9}, {11, 8}, {4, 10},
                                      {9, 11}, {6, 12}, {12, 11}};
  for (const auto& s : kSeeds) {
    pset(g, s[0], s[1], m);
    pset(g, s[0] + 1, s[1], d);
    pset(g, s[0], s[1] + 1, d);
  }
}

// A filled bowl: soups and stews. The contents take the item's colour, the bowl is
// always the same fired clay, so twenty meals read as one family at a glance.
void paintBowl(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba m = solid(col), M = shade(col, 1.22);
  const Rgba clay = shade(0xa8703f, 1.0), clayD = shade(0x83562f, 1.0);
  const Rgba clayM = shade(0xc08a55, 1.0);
  prow(g, 4, 11, 7, M);  // the surface of the food
  prow(g, 3, 12, 8, m);
  pset(g, 5, 6, M);
  pset(g, 9, 6, m);
  prow(g, 2, 13, 9, clayM);  // rim
  prow(g, 2, 13, 10, clay);
  prow(g, 3, 12, 11, clay);
  prow(g, 4, 11, 12, clayD);
  prow(g, 6, 9, 13, clayD);
}

// A plated meal: roasts, pies, anything not swimming in stock.
void paintPlate(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba m = solid(col), M = shade(col, 1.24), d = shade(col, 0.74);
  const Rgba plate = shade(0xd8cfc0, 1.0), plateD = shade(0xa89f92, 1.0);
  prow(g, 2, 13, 11, plate);
  prow(g, 3, 12, 12, plateD);
  for (int y = 5; y <= 10; ++y) {
    for (int x = 4; x <= 11; ++x) {
      const double a = (x - 7.5) / 4.0, b = (y - 8.0) / 3.4;
      if (a * a + b * b > 1.0) continue;
      pset(g, x, y, y <= 6 ? M : (y >= 10 ? d : m));
    }
  }
}

// A haft with a flat blade turned down at the head.
void paintHoe(SpriteGrid& g, std::uint32_t col, const ItemDef&) {
  const Rgba m = solid(col), d = shade(col, 0.74), M = shade(col, 1.25);
  const Rgba haft = shade(0x9c7748, 1.0), haftD = shade(0x7c5c34, 1.0);

  // The haft climbs from the butt at the bottom right to the head at the top left.
  // The first version ran it to (4,12) while drawing the blade up at rows 3-5, so
  // the two never touched and the icon read as a stick with a bar floating beside
  // it. A tool has to look like one object.
  for (int i = 0; i < 8; ++i) {
    pset(g, 12 - i, 12 - i, haft);
    pset(g, 13 - i, 12 - i, haftD);
  }

  // The head: a blade jutting LEFT from the top of the haft, with its cutting edge
  // turned down. That right angle is the whole silhouette of a hoe, and it is what
  // stops this reading as an axe at sixteen pixels.
  prow(g, 2, 6, 4, M);  // the back of the blade
  prow(g, 2, 6, 5, m);  // its body, meeting the haft at x=5
  prow(g, 2, 4, 6, d);  // the edge, turned down toward the soil
}

PainterFn painterFor(IconKind kind) {
  switch (kind) {
    case IconKind::Hoe: return paintHoe;
    case IconKind::Grain: return paintGrain;
    case IconKind::Root: return paintRoot;
    case IconKind::Produce: return paintProduce;
    case IconKind::Pod: return paintPod;
    case IconKind::Berry: return paintBerry;
    case IconKind::Leafy: return paintLeafy;
    case IconKind::Seed: return paintSeed;
    case IconKind::Bowl: return paintBowl;
    case IconKind::Plate: return paintPlate;
    case IconKind::Stick: return paintStick;
    case IconKind::Pick: return paintPick;
    case IconKind::Axe: return paintAxe;
    case IconKind::Shovel: return paintShovel;
    case IconKind::Sword: return paintSword;
    case IconKind::Ingot: return paintIngot;
    case IconKind::Nugget: return paintNugget;
    case IconKind::Lump: return paintLump;
    case IconKind::Dye: return paintDye;
    case IconKind::Palette: return paintPalette;
    case IconKind::Gem: return paintGem;
    case IconKind::Shard: return paintShard;
    case IconKind::Boat: return paintBoat;
    case IconKind::Meat: return paintMeat;
    case IconKind::Flesh: return paintFlesh;
    case IconKind::Paper: return paintPaper;
    case IconKind::Leather: return paintLeather;
    case IconKind::Steak: return paintSteak;
    case IconKind::Bucket: return paintBucket;
    case IconKind::Atlas: return paintAtlas;
    case IconKind::Wayshard: return paintWayshard;
    case IconKind::ArmorHelmet: return paintArmorHelmet;
    case IconKind::ArmorChest: return paintArmorChest;
    case IconKind::ArmorLegs: return paintArmorLegs;
    case IconKind::ArmorBoots: return paintArmorBoots;
    case IconKind::Block:
    case IconKind::Fallback: return nullptr;
  }
  return nullptr;
}

// --- registry data ----------------------------------------------------------

struct MaterialSpec {
  const char* key;
  const char* name;
  std::uint32_t color;
  IconKind icon;
};

// js/game/items.js:20-37.
constexpr MaterialSpec kMaterials[] = {
    {"stick", "Stick", 0x9c7748u, IconKind::Stick},
    {"embercoal", "Coal", 0x1d1d22u, IconKind::Lump},
    {"charcoal", "Charcoal", 0x36322cu, IconKind::Lump},
    {"raw_copper", "Raw Copper", 0xa5612eu, IconKind::Nugget},
    {"copper_ingot", "Copper Ingot", 0xc8783au, IconKind::Ingot},
    {"raw_ferralite", "Raw Iron", 0xb9ad95u, IconKind::Nugget},
    {"ferralite_ingot", "Iron Ingot", 0xcfd2d6u, IconKind::Ingot},
    {"raw_sunbrass", "Raw Gold", 0xc9a838u, IconKind::Nugget},
    {"sunbrass_ingot", "Gold Ingot", 0xe8c64au, IconKind::Ingot},
    {"aetherite", "Diamond", 0x46d8c4u, IconKind::Gem},
    {"sparkstone", "Sparkstone", 0xe0432fu, IconKind::Shard},
    {"azurite", "Azurite", 0x2f6fe0u, IconKind::Shard},
    {"gloamite", "Gloamite", 0x8a52e8u, IconKind::Shard},
    {"verdanite", "Verdanite", 0x46b558u, IconKind::Shard},
    {"leather", "Leather", 0x9a6a3cu, IconKind::Leather},
    {"paper", "Paper", 0xece7d4u, IconKind::Paper},
    // Shaken out of tall grass and ferns. It exists so a player who has not yet
    // stumbled on a wild crop patch still has a way into farming at all; planting it
    // gives one of the common crops rather than a chosen one, which keeps finding
    // the real thing worth doing.
    {"wild_seeds", "Wild Seeds", 0xbfa96au, IconKind::Seed},
    // Every pot meal is served into one of these and gives it back when eaten, so a
    // handful of bowls is a one-off cost rather than a running one.
    {"bowl", "Bowl", 0xa8703fu, IconKind::Bowl},
    // Milled at the cutting board; the one thing the stove bakes into bread.
    {"flour", "Flour", 0xe8dcc0u, IconKind::Seed},
    // Compost. Verdanite is the growth ore, and rotten flesh is the one item in the
    // game whose only use was a gamble nobody takes — so this is also where it stops
    // being pure litter.
    {"fertiliser", "Fertiliser", 0x6f8f42u, IconKind::Lump},

    // --- The Dye update ------------------------------------------------------
    //
    // One per flower. Appended, like everything else here: an item's index IS its
    // cell in the icon atlas, so inserting one anywhere but the end repaints every
    // icon after it.
    //
    // These colours are not decoration. The palette charges one dye per application
    // and picks the one NEAREST the colour being mixed, so this table is the set of
    // anchors the whole 24-bit space is measured against — which is why they are
    // spread across the wheel rather than chosen to look nice beside each other.
    {"dye_red", "Red Dye", 0xd23a34u, IconKind::Dye},
    {"dye_orange", "Orange Dye", 0xe8862au, IconKind::Dye},
    {"dye_yellow", "Yellow Dye", 0xf2c53au, IconKind::Dye},
    {"dye_green", "Green Dye", 0x4fae53u, IconKind::Dye},
    {"dye_blue", "Blue Dye", 0x4a6fe0u, IconKind::Dye},
    {"dye_purple", "Purple Dye", 0x9a5ac2u, IconKind::Dye},
    {"dye_white", "White Dye", 0xf0f0eau, IconKind::Dye},
    {"dye_black", "Black Dye", 0x2a2333u, IconKind::Dye},
};

struct FoodSpec {
  const char* key;
  const char* name;
  std::uint32_t color;
  IconKind icon;
  int food;
  bool risky;
  float sat;
  NutritionGroup group;
  int quality;  // worth as a cooking INGREDIENT, not as a meal
};

// `food` is in hunger POINTS on the 20-point bar, and `sat` is the hidden buffer
// that decides how long it lasts.
//
// THE MEAT NUMBERS CAME DOWN ON PURPOSE. Cooked meat used to be 8 points — the best
// food in the game, off one punch of a cow and one forge slot. Nothing a kitchen
// could produce was going to beat that, so nobody would ever have cooked. It is 5
// now, with a saturation of 3, which still makes it the best thing you can eat
// *without* cooking and comfortably worse than anything you can eat with. Raw meat
// dropped 3 -> 2 for the same reason: it is an ingredient that you may eat in an
// emergency, not a meal.
//
// Rotten flesh feeds no group. It is calories, not nutrition, and a diet built on it
// should stay stuck at zero bonus health no matter how much you choke down.
constexpr FoodSpec kFoods[] = {
    {"pork_raw", "Raw Porkchop", 0xe08a90u, IconKind::Meat, 2, false, 0.5f,
     NutritionGroup::Protein, 3},
    {"pork_cooked", "Cooked Porkchop", 0xb06a3cu, IconKind::Meat, 5, false, 3.0f,
     NutritionGroup::Protein, 4},
    {"beef_raw", "Raw Beef", 0xc4525au, IconKind::Steak, 2, false, 0.5f,
     NutritionGroup::Protein, 3},
    {"beef_cooked", "Steak", 0x8a4a2cu, IconKind::Steak, 5, false, 3.0f,
     NutritionGroup::Protein, 4},
    {"rotten_flesh", "Rotten Flesh", 0x7a8c4eu, IconKind::Flesh, 2, true, 0.0f,
     NutritionGroup::None, 0},

    // --- crop produce --------------------------------------------------------
    //
    // Every one of these is edible and none of them is worth eating: one or two
    // points and almost no saturation, which is the "last option" the whole update
    // is built around. Their real value is `quality`, the number the cooking pot
    // sums to decide which tier of a dish comes out.
    //
    // The produce is also the SEED — using one on farmland plants it — so these are
    // the only crop items that exist. See blocks.cpp.
    {"wheat", "Wheat", 0xe0c65au, IconKind::Grain, 1, false, 0.5f, NutritionGroup::Grain, 2},
    {"barley", "Barley", 0xd9c78au, IconKind::Grain, 1, false, 0.5f, NutritionGroup::Grain, 2},
    {"rice", "Rice", 0xe8e4c8u, IconKind::Grain, 1, false, 0.5f, NutritionGroup::Grain, 2},
    {"maize", "Maize", 0xf0c433u, IconKind::Grain, 2, false, 1.0f, NutritionGroup::Grain, 3},
    {"carrot", "Carrot", 0xe07a28u, IconKind::Root, 2, false, 1.0f,
     NutritionGroup::Vegetable, 3},
    {"potato", "Potato", 0xc9a468u, IconKind::Root, 2, false, 1.0f,
     NutritionGroup::Vegetable, 3},
    {"onion", "Onion", 0xd8c9a2u, IconKind::Root, 1, false, 0.5f,
     NutritionGroup::Vegetable, 3},
    {"beetroot", "Beetroot", 0xa0243cu, IconKind::Root, 2, false, 1.0f,
     NutritionGroup::Vegetable, 2},
    // Garlic and chili are seasonings: barely worth eating, disproportionately worth
    // cooking with. That gap is the clearest statement the numbers can make that
    // some things are ingredients and not food.
    {"garlic", "Garlic", 0xeae2d2u, IconKind::Root, 1, false, 0.0f,
     NutritionGroup::Vegetable, 5},
    {"chili", "Chili", 0xd42f24u, IconKind::Pod, 1, false, 0.0f, NutritionGroup::Vegetable, 5},
    {"pumpkin", "Pumpkin", 0xe0821eu, IconKind::Produce, 2, false, 1.0f,
     NutritionGroup::Vegetable, 3},
    {"melon", "Melon", 0x6fae3au, IconKind::Produce, 2, false, 1.0f, NutritionGroup::Fruit, 3},
    {"tomato", "Tomato", 0xd8392cu, IconKind::Produce, 2, false, 1.0f,
     NutritionGroup::Vegetable, 4},
    {"strawberry", "Strawberry", 0xd8323cu, IconKind::Berry, 2, false, 1.0f,
     NutritionGroup::Fruit, 3},
    {"blueberry", "Blueberry", 0x4a5ac2u, IconKind::Berry, 2, false, 1.0f,
     NutritionGroup::Fruit, 3},
    {"grapes", "Grapes", 0x7a4ab0u, IconKind::Berry, 2, false, 1.0f, NutritionGroup::Fruit, 3},
    {"cabbage", "Cabbage", 0x8fc46au, IconKind::Leafy, 2, false, 1.0f,
     NutritionGroup::Vegetable, 3},
    {"soybean", "Soybeans", 0xd8cf7au, IconKind::Pod, 1, false, 0.5f,
     NutritionGroup::Protein, 4},

    // --- prepared at the cutting board ---------------------------------------
    //
    // Butchered meat is the point of the first two: one raw chop becomes two strips,
    // so processing meat makes it go FURTHER. With animal husbandry still to come,
    // that is the difference between meat being scarce and meat being a dead end.
    {"meat_strips", "Meat Strips", 0xc4707au, IconKind::Meat, 1, false, 0.5f,
     NutritionGroup::Protein, 4},
    {"cooked_strips", "Seared Strips", 0x9a5a34u, IconKind::Meat, 3, false, 2.0f,
     NutritionGroup::Protein, 5},

    // --- from the stove -------------------------------------------------------
    {"bread", "Bread", 0xd8a860u, IconKind::Plate, 5, false, 4.0f, NutritionGroup::Grain, 4},
    {"baked_potato", "Baked Potato", 0xd2a86au, IconKind::Plate, 4, false, 3.0f,
     NutritionGroup::Vegetable, 3},
    {"roast_vegetables", "Roast Vegetables", 0xc98a3cu, IconKind::Plate, 5, false, 4.0f,
     NutritionGroup::Vegetable, 4},
    {"grilled_maize", "Grilled Maize", 0xefc24au, IconKind::Plate, 4, false, 3.0f,
     NutritionGroup::Grain, 3},
    {"cheese", "Cheese", 0xf0cc5au, IconKind::Plate, 4, false, 4.0f, NutritionGroup::Dairy, 5},

    // --- meals from the pot ---------------------------------------------------
    //
    // These are the reason the whole update exists, so their numbers are meant to
    // look unreasonable next to a cooked chop: a full meal is most of a hunger bar
    // AND a saturation buffer that lasts, where the best uncooked thing in the game
    // is five points and three. Cooking is not a small optimisation, it is the way
    // you are supposed to eat.
    {"vegetable_soup", "Vegetable Soup", 0xc07a3au, IconKind::Bowl, 8, false, 8.0f,
     NutritionGroup::Vegetable, 4},
    {"hearty_stew", "Hearty Stew", 0x8a4a26u, IconKind::Bowl, 14, false, 18.0f,
     NutritionGroup::Protein, 6},
    {"pumpkin_soup", "Pumpkin Soup", 0xe0821eu, IconKind::Bowl, 9, false, 10.0f,
     NutritionGroup::Vegetable, 4},
    {"tomato_soup", "Tomato Soup", 0xc8342au, IconKind::Bowl, 9, false, 10.0f,
     NutritionGroup::Vegetable, 4},
    {"bean_stew", "Bean Stew", 0xa8894au, IconKind::Bowl, 11, false, 13.0f,
     NutritionGroup::Protein, 5},
    {"rice_bowl", "Rice Bowl", 0xe4dfc4u, IconKind::Bowl, 10, false, 11.0f,
     NutritionGroup::Grain, 4},
    {"porridge", "Porridge", 0xe0d2a4u, IconKind::Bowl, 10, false, 12.0f,
     NutritionGroup::Grain, 4},
    {"fruit_salad", "Fruit Salad", 0xd85a72u, IconKind::Bowl, 8, false, 9.0f,
     NutritionGroup::Fruit, 4},
    {"berry_compote", "Berry Compote", 0x8a3a92u, IconKind::Bowl, 7, false, 8.0f,
     NutritionGroup::Fruit, 4},
    {"garden_salad", "Garden Salad", 0x7ab04au, IconKind::Bowl, 7, false, 7.0f,
     NutritionGroup::Vegetable, 3},
    {"stuffed_pumpkin", "Stuffed Pumpkin", 0xd8801eu, IconKind::Plate, 13, false, 16.0f,
     NutritionGroup::Vegetable, 6},
    {"cheese_toastie", "Cheese Toastie", 0xe8b85au, IconKind::Plate, 9, false, 9.0f,
     NutritionGroup::Dairy, 4},
    {"garlic_bread", "Garlic Bread", 0xdcb46eu, IconKind::Plate, 7, false, 6.0f,
     NutritionGroup::Grain, 4},
    {"meat_pie", "Meat Pie", 0xb07a3au, IconKind::Plate, 15, false, 19.0f,
     NutritionGroup::Protein, 6},
    {"veg_pie", "Vegetable Pie", 0xc9a05au, IconKind::Plate, 12, false, 15.0f,
     NutritionGroup::Vegetable, 5},
};

struct ToolTypeSpec {
  const char* type;
  const char* name;
  ToolKind kind;
  IconKind icon;
};
constexpr ToolTypeSpec kToolTypes[] = {
    {"pick", "Pickaxe", ToolKind::Pick, IconKind::Pick},
    {"axe", "Axe", ToolKind::Axe, IconKind::Axe},
    {"shovel", "Shovel", ToolKind::Shovel, IconKind::Shovel},
    {"sword", "Sword", ToolKind::Sword, IconKind::Sword},
    // Appended, so the six hoes land at the end of the tool matrix. Item order only
    // decides icon-atlas cells (see ItemDef::index), but keeping additions at the
    // end is the habit that makes a golden diff readable.
    {"hoe", "Hoe", ToolKind::Hoe, IconKind::Hoe},
};

constexpr IconKind kArmorIcons[] = {IconKind::ArmorHelmet, IconKind::ArmorChest,
                                    IconKind::ArmorLegs, IconKind::ArmorBoots};

const char* toolTypeName(ToolKind k) {
  switch (k) {
    case ToolKind::Pick: return "pick";
    case ToolKind::Axe: return "axe";
    case ToolKind::Shovel: return "shovel";
    case ToolKind::Sword: return "sword";
    case ToolKind::Hoe: return "hoe";
    case ToolKind::None: break;
  }
  return "tool";
}

}  // namespace

// js/game/items.js:76-83. `speed` is shared across pick/axe/shovel/sword at a
// tier so they all progress consistently.
const std::array<ToolMaterial, 6>& toolMaterials() {
  static const std::array<ToolMaterial, 6> kTable {{
      // "#planks", not "planks": the literal is OAK planks, so a spawn in a pine,
      // birch, dusk or palm forest could not craft the first pickaxe at all while
      // a bed — which uses the tag — worked fine. Every wooden recipe takes the
      // tag now, so "wood is wood" holds everywhere it should.
      {"wood", "Wooden", world::tier::kWood, "#planks", 0xb08a52u, 1.7f, 60},
      {"stone", "Stone", world::tier::kStone, "cobbled", 0x7d8189u, 2.4f, 130},
      {"copper", "Copper", world::tier::kCopper, "copper_ingot", 0xc8783au, 3.2f, 200},
      {"ferralite", "Iron", world::tier::kFerralite, "ferralite_ingot", 0xcfd2d6u, 4.2f, 360},
      {"sunbrass", "Golden", world::tier::kSunbrass, "sunbrass_ingot", 0xe8c64au, 6.5f, 90},
      {"aetherite", "Diamond", world::tier::kAetherite, "aetherite", 0x46d8c4u, 5.2f, 820},
  }};
  return kTable;
}

const std::array<ArmorMaterial, 4>& armorMaterials() {
  static const std::array<ArmorMaterial, 4> kTable {{
      {"copper", "Copper", 0xc8783au, 1, 120},
      {"ferralite", "Iron", 0xcfd2d6u, 2, 240},
      {"sunbrass", "Golden", 0xe8c64au, 1, 80},
      {"aetherite", "Diamond", 0x46d8c4u, 3, 520},
  }};
  return kTable;
}

const std::array<ArmorPiece, 4>& armorPieces() {
  static const std::array<ArmorPiece, 4> kTable {{
      {"helmet", "Helm", 0, 1.0f},
      {"chest", "Chestguard", 1, 1.6f},
      {"legs", "Greaves", 2, 1.4f},
      {"boots", "Boots", 3, 0.8f},
  }};
  return kTable;
}

ItemRegistry::ItemRegistry() {
  const world::BlockRegistry& reg = world::blocks();
  items_.reserve(reg.count() + 64);

  auto add = [this](ItemDef def) -> ItemDef& {
    def.index = static_cast<int>(items_.size());
    byKey_.emplace(def.key, def.index);
    items_.push_back(std::move(def));
    return items_.back();
  };

  // ---- block items (everything placeable) ----
  // js/game/items.js:13-17. Air is not a thing, water is only ever a bucket, and
  // bedrock must not be obtainable.
  byBlock_.assign(reg.count(), -1);
  for (const world::BlockDef& b : reg.all()) {
    if (b.key == "air" || b.key == "water" || b.key == "bedrock") continue;
    // Crops get no block item. Their produce is what plants them (used on farmland),
    // so a `crop_wheat` item would be a second, worse way to do the same thing —
    // placeable on any wall, at any growth stage, with a name nobody asked for.
    if (b.cropStages > 0) continue;
    ItemDef def;
    def.key = b.key;
    def.name = b.name;
    def.type = ItemType::Block;
    def.icon = IconKind::Block;
    def.blockId = b.id;
    // Mirrored rather than restated. A block item and its block are the same thing
    // to a player, so "can this be dyed" has to have one answer — and the block
    // table is where it is already written.
    def.dyeable = b.dyeable;
    byBlock_[b.id] = static_cast<int>(items_.size());
    add(std::move(def));
  }

  // ---- materials ----
  for (const MaterialSpec& m : kMaterials) {
    ItemDef def;
    def.key = m.key;
    def.name = m.name;
    def.type = ItemType::Material;
    def.icon = m.icon;
    def.color = m.color;
    add(std::move(def));
  }

  // ---- foods (right-click to eat) ----
  for (const FoodSpec& f : kFoods) {
    ItemDef def;
    def.key = f.key;
    def.name = f.name;
    def.type = ItemType::Food;
    def.icon = f.icon;
    def.color = f.color;
    def.food = f.food;
    def.risky = f.risky;
    def.sat = f.sat;
    def.group = f.group;
    def.quality = f.quality;
    add(std::move(def));
  }

  // ---- usable items that aren't blocks ----
  {
    ItemDef def;
    def.key = "boat";
    def.name = "Oak Boat";
    def.type = ItemType::Boat;
    def.icon = IconKind::Boat;
    def.maxStack = 1;
    def.color = 0x8a6a3au;
    add(std::move(def));
  }
  // Buckets: the empty one scoops still water (or milks a cow); the filled ones
  // place or pour back. `fill` tints the icon's contents.
  {
    ItemDef def;
    def.key = "bucket";
    def.name = "Bucket";
    def.type = ItemType::Bucket;
    def.icon = IconKind::Bucket;
    def.maxStack = 16;
    def.color = 0xb8bcc4u;
    add(std::move(def));
  }
  {
    ItemDef def;
    def.key = "water_bucket";
    def.name = "Water Bucket";
    def.type = ItemType::Bucket;
    def.icon = IconKind::Bucket;
    def.maxStack = 1;
    def.color = 0xb8bcc4u;
    def.holds = "water";
    def.hasFill = true;
    def.fill = 0x3f77d9u;
    add(std::move(def));
  }
  {
    ItemDef def;
    def.key = "milk_bucket";
    def.name = "Milk Bucket";
    def.type = ItemType::Bucket;
    def.icon = IconKind::Bucket;
    def.maxStack = 1;
    def.color = 0xb8bcc4u;
    def.holds = "milk";
    def.hasFill = true;
    def.fill = 0xf0eee6u;
    add(std::move(def));
  }
  // The Atlas: carrying it unlocks the world map, waypoints and the minimap.
  {
    ItemDef def;
    def.key = "atlas";
    def.name = "Atlas";
    def.type = ItemType::Atlas;
    def.icon = IconKind::Atlas;
    def.maxStack = 1;
    def.color = 0x8a5a34u;
    add(std::move(def));
  }
  // Wayshard: a sliver of gloamite tuned to the open sky. Consumed on use.
  {
    ItemDef def;
    def.key = "wayshard";
    def.name = "Wayshard";
    def.type = ItemType::Warp;
    def.icon = IconKind::Wayshard;
    def.maxStack = 16;
    def.color = 0x9a6ae8u;
    add(std::move(def));
  }

  // ---- tools ----
  for (const ToolMaterial& m : toolMaterials()) {
    for (const ToolTypeSpec& t : kToolTypes) {
      ItemDef def;
      def.key = std::string(t.type) + "_" + m.id;
      def.name = std::string(m.name) + " " + t.name;
      def.type = ItemType::Tool;
      def.icon = t.icon;
      def.maxStack = 1;
      def.toolType = t.kind;
      def.tier = m.tier;
      def.speed = m.speed;
      def.durability = m.durability;
      def.color = m.color;
      add(std::move(def));
    }
  }

  // ---- armour (metals only) ----
  for (const ArmorMaterial& m : armorMaterials()) {
    int pieceIndex = 0;
    for (const ArmorPiece& p : armorPieces()) {
      ItemDef def;
      def.key = std::string(p.piece) + "_" + m.id;
      def.name = std::string(m.name) + " " + p.name;
      def.type = ItemType::Armor;
      def.icon = kArmorIcons[pieceIndex++];
      def.maxStack = 1;
      def.armorSlot = p.slot;
      // Math.max(1, Math.round(defense * mult)); JS rounds half up, and every
      // product here is well away from a .5 boundary.
      def.defense = std::max(1, static_cast<int>(std::floor(m.defense * p.mult + 0.5f)));
      def.durability = static_cast<int>(std::floor(m.durability * p.mult + 0.5f));
      def.color = m.color;
      add(std::move(def));
    }
  }

  // ---- colourable armour, appended after the whole plain matrix -------------
  //
  // A second full 4x4 rather than a `dyeable` flag on the sixteen above, because a
  // player who spends iron on a chestplate should not have made an undyed one by
  // accident — and because a plain iron chestplate LOOKS like iron, which is worth
  // keeping. Crafting the plain piece with a wool is what converts it.
  //
  // The colour is a neutral grey rather than the material's own. That is the whole
  // trick: the sprite painters shade from ItemDef::color, so a grey base produces a
  // greyscale piece whose highlights and shadows survive the multiply — the dye
  // lands on white and comes out at full strength, the outline stays near-black and
  // reads as an outline. Handing these m.color would tint every dye toward iron.
  //
  // Appended AFTER the plain matrix, never interleaved with it: an item's index is
  // its cell in the icon atlas.
  for (const ArmorMaterial& m : armorMaterials()) {
    int pieceIndex = 0;
    for (const ArmorPiece& p : armorPieces()) {
      ItemDef def;
      def.key = std::string("dyed_") + p.piece + "_" + m.id;
      def.name = std::string("Colourable ") + m.name + " " + p.name;
      def.type = ItemType::Armor;
      def.icon = kArmorIcons[pieceIndex++];
      def.maxStack = 1;
      def.armorSlot = p.slot;
      // Identical protection and wear to the plain piece. Dyeing is a cosmetic
      // choice, and a cosmetic choice that costs defence is a choice nobody makes.
      def.defense = std::max(1, static_cast<int>(std::floor(m.defense * p.mult + 0.5f)));
      def.durability = static_cast<int>(std::floor(m.durability * p.mult + 0.5f));
      def.color = 0xd6d6d6u;
      def.dyeable = true;
      add(std::move(def));
    }
  }

  // The palette itself, last of all. No durability: it is the tool that opens the
  // colour screen, and a colour screen that wears out and has to be re-crafted from
  // one of every flower in the world would make the whole feature a chore.
  {
    ItemDef def;
    def.key = "palette";
    def.name = "Dyer's Palette";
    def.type = ItemType::Palette;
    def.icon = IconKind::Palette;
    def.maxStack = 1;
    def.color = 0xa8763fu;
    add(std::move(def));
  }
}

const ItemRegistry& ItemRegistry::get() {
  static const ItemRegistry instance;
  return instance;
}

const ItemDef* ItemRegistry::find(std::string_view key) const {
  auto it = byKey_.find(std::string(key));
  return it == byKey_.end() ? nullptr : &items_[it->second];
}

int ItemRegistry::indexOf(std::string_view key) const {
  auto it = byKey_.find(std::string(key));
  return it == byKey_.end() ? -1 : it->second;
}

const ItemDef* ItemRegistry::forBlock(world::BlockId id) const {
  if (id >= byBlock_.size() || byBlock_[id] < 0) return nullptr;
  return &items_[byBlock_[id]];
}

int maxDurability(std::string_view key) {
  const ItemDef* it = getItem(key);
  return it ? it->durability : 0;
}
bool isTool(std::string_view key) {
  const ItemDef* it = getItem(key);
  return it && it->type == ItemType::Tool;
}
bool isArmor(std::string_view key) {
  const ItemDef* it = getItem(key);
  return it && it->type == ItemType::Armor;
}

Tooltip itemTooltip(std::string_view key, int durabilityOverride, float fuelSeconds) {
  Tooltip out;
  // An ingredient tag ("#planks") stands for a family, not an item.
  if (!key.empty() && key.front() == '#') {
    std::string label(key.substr(1));
    std::replace(label.begin(), label.end(), '_', ' ');
    out.name = "Any " + label;
    return out;
  }

  const ItemDef* it = getItem(key);
  if (!it) {
    out.name = std::string(key);
    return out;
  }
  out.name = it->name;

  char buf[128];
  if (it->type == ItemType::Tool) {
    std::snprintf(buf, sizeof buf, "%s \xC2\xB7 tier %d", toolTypeName(it->toolType), it->tier);
    out.lines.emplace_back(buf);
    // Swords are weapons: show attack damage, not dig speed.
    if (it->toolType == ToolKind::Sword) {
      std::snprintf(buf, sizeof buf, "attack: %d damage", 3 + it->tier);
    } else {
      std::snprintf(buf, sizeof buf, "mining speed \xC3\x97%g", static_cast<double>(it->speed));
    }
    out.lines.emplace_back(buf);
  }
  if (it->type == ItemType::Armor) {
    std::snprintf(buf, sizeof buf, "+%d defense", it->defense);
    out.lines.emplace_back(buf);
  }
  if (it->type == ItemType::Food) {
    if (it->risky) {
      std::snprintf(buf, sizeof buf, "food: a gamble (+%d or \xE2\x88\x92%d hunger)", it->food,
                    it->food);
      out.lines.emplace_back(buf);
    } else {
      std::snprintf(buf, sizeof buf, "food: +%d hunger (%g pips)", it->food, it->food / 2.0);
      out.lines.emplace_back(buf);
      // Saturation is the number that actually answers "how long will this hold
      // me", and it was invisible before there was anything to compare. Showing it
      // is most of what teaches a player that the stew is not merely bigger.
      std::snprintf(buf, sizeof buf, "saturation: %g", static_cast<double>(it->sat));
      out.lines.emplace_back(buf);
    }
    if (it->group != NutritionGroup::None) {
      std::snprintf(buf, sizeof buf, "diet: %s", nutritionName(it->group));
      out.lines.emplace_back(buf);
    }
    // Where it can be planted, for anything that is also a seed. This replaced a
    // toast that fired whenever somebody right-clicked bare ground holding food —
    // a tooltip answers the same question only when it is asked.
    if (world::blocks().cropForProduce(it->key) != 0) {
      out.lines.emplace_back("plant on tilled soil (use a hoe)");
    }
  }
  if (it->type == ItemType::Warp) out.lines.emplace_back("use: warp to the surface above you");
  if (it->type == ItemType::Atlas) {
    out.lines.emplace_back("carry it: world map (M) \xC2\xB7 minimap (N)");
  }
  if (it->key == "bucket") out.lines.emplace_back("scoops still water \xC2\xB7 milks cows");

  if (it->durability > 0) {
    const int shown = durabilityOverride >= 0 ? durabilityOverride : it->durability;
    std::snprintf(buf, sizeof buf, "%d/%d durability", shown, it->durability);
    out.lines.emplace_back(buf);
  }
  if (fuelSeconds > 0) {
    std::snprintf(buf, sizeof buf, "forge fuel \xC2\xB7 %gs", static_cast<double>(fuelSeconds));
    out.lines.emplace_back(buf);
  }
  return out;
}

SpriteGrid spriteGridFor(const ItemDef& item) {
  SpriteGrid g {};
  if (PainterFn paint = painterFor(item.icon)) {
    paint(g, item.color, item);
  } else if (item.icon != IconKind::Block) {
    // js/game/items.js:477 — an unpainted kind falls back to a plain square so a
    // new item is visible rather than invisible.
    for (int y = 5; y <= 10; ++y) prow(g, 5, 10, y, solid(item.color));
  }
  return g;
}

SpriteGrid outlined(const SpriteGrid& grid) {
  SpriteGrid out = grid;
  for (int y = 0; y < G; ++y) {
    for (int x = 0; x < G; ++x) {
      if (filledAt(grid, x, y)) continue;
      const bool touching = (x > 0 && filledAt(grid, x - 1, y)) ||
                            (x < G - 1 && filledAt(grid, x + 1, y)) ||
                            (y > 0 && filledAt(grid, x, y - 1)) ||
                            (y < G - 1 && filledAt(grid, x, y + 1));
      if (touching) out[static_cast<std::size_t>(y) * G + x] = kOutline;
    }
  }
  return out;
}

Image spriteImage(const ItemDef& item) {
  const SpriteGrid g = outlined(spriteGridFor(item));
  Image img(G, G);
  for (int y = 0; y < G; ++y) {
    for (int x = 0; x < G; ++x) img.set(x, y, g[static_cast<std::size_t>(y) * G + x]);
  }
  return img;
}

std::vector<ResourceId> collectItemTextureIds() {
  std::set<ResourceId> unique;
  for (const ItemDef& it : items().all()) {
    if (it.icon == IconKind::Block) continue;
    unique.insert(ResourceId("item/" + it.key));
  }
  return std::vector<ResourceId>(unique.begin(), unique.end());
}

namespace {

class ItemSpriteProvider final : public resource::Provider {
 public:
  const std::string& name() const override { return name_; }

  bool has(const ResourceId& id) const override { return lookup(id) != nullptr; }

  std::optional<resource::TextureInfo> info(const ResourceId& id) const override {
    if (!lookup(id)) return std::nullopt;
    return resource::TextureInfo {kSpriteSize, kSpriteSize, 1, 0.0};
  }

  Image load(const ResourceId& id) const override {
    const ItemDef* it = lookup(id);
    return it ? spriteImage(*it) : Image {};
  }

 private:
  static const ItemDef* lookup(const ResourceId& id) {
    if (id.ns() != kDefaultNamespace) return nullptr;
    const std::string& path = id.path();
    if (path.rfind("item/", 0) != 0) return nullptr;
    const ItemDef* it = getItem(std::string_view(path).substr(5));
    // A block item has no sprite: its tiles come from the block painters.
    return (it && it->icon != IconKind::Block) ? it : nullptr;
  }

  std::string name_ = "built-in item sprites";
};

}  // namespace

std::unique_ptr<resource::Provider> makeItemSpriteProvider() {
  return std::make_unique<ItemSpriteProvider>();
}

}  // namespace hr::game
