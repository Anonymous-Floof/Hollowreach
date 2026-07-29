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

PainterFn painterFor(IconKind kind) {
  switch (kind) {
    case IconKind::Stick: return paintStick;
    case IconKind::Pick: return paintPick;
    case IconKind::Axe: return paintAxe;
    case IconKind::Shovel: return paintShovel;
    case IconKind::Sword: return paintSword;
    case IconKind::Ingot: return paintIngot;
    case IconKind::Nugget: return paintNugget;
    case IconKind::Lump: return paintLump;
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
};

struct FoodSpec {
  const char* key;
  const char* name;
  std::uint32_t color;
  IconKind icon;
  int food;
  bool risky;
};

// js/game/items.js:44-50. `food` is in hunger POINTS on the 20-point bar, so a
// cooked meal visibly fills pips; the values echo Minecraft's.
constexpr FoodSpec kFoods[] = {
    {"pork_raw", "Raw Porkchop", 0xe08a90u, IconKind::Meat, 3, false},
    {"pork_cooked", "Cooked Porkchop", 0xb06a3cu, IconKind::Meat, 8, false},
    {"beef_raw", "Raw Beef", 0xc4525au, IconKind::Steak, 3, false},
    {"beef_cooked", "Steak", 0x8a4a2cu, IconKind::Steak, 8, false},
    {"rotten_flesh", "Rotten Flesh", 0x7a8c4eu, IconKind::Flesh, 2, true},
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
};

constexpr IconKind kArmorIcons[] = {IconKind::ArmorHelmet, IconKind::ArmorChest,
                                    IconKind::ArmorLegs, IconKind::ArmorBoots};

const char* toolTypeName(ToolKind k) {
  switch (k) {
    case ToolKind::Pick: return "pick";
    case ToolKind::Axe: return "axe";
    case ToolKind::Shovel: return "shovel";
    case ToolKind::Sword: return "sword";
    case ToolKind::None: break;
  }
  return "tool";
}

}  // namespace

// js/game/items.js:76-83. `speed` is shared across pick/axe/shovel/sword at a
// tier so they all progress consistently.
const std::array<ToolMaterial, 6>& toolMaterials() {
  static const std::array<ToolMaterial, 6> kTable {{
      {"wood", "Wooden", world::tier::kWood, "planks", 0xb08a52u, 1.7f, 60},
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
    ItemDef def;
    def.key = b.key;
    def.name = b.name;
    def.type = ItemType::Block;
    def.icon = IconKind::Block;
    def.blockId = b.id;
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
    } else {
      std::snprintf(buf, sizeof buf, "food: +%d hunger (%g pips)", it->food, it->food / 2.0);
    }
    out.lines.emplace_back(buf);
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
