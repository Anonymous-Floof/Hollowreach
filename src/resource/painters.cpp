#include "resource/painters.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

#include "core/jsmath.h"

namespace hr::resource {
namespace {

constexpr int T = kPainterTile;

// --- helpers, mirroring the JS ones ------------------------------------------

// clamp8(v) | 0 in the original: clamp into range, then truncate toward zero.
std::uint8_t clamp8(double v) {
  if (v < 0.0) return 0;
  if (v > 255.0) return 255;
  return static_cast<std::uint8_t>(v);
}

struct Rgb3 {
  double r = 0, g = 0, b = 0;
};

// hexToRgb("#rrggbb").
constexpr Rgb3 hex(std::uint32_t rgb) {
  return {static_cast<double>((rgb >> 16) & 255), static_cast<double>((rgb >> 8) & 255),
          static_cast<double>(rgb & 255)};
}

// One pixel. Alpha is a 0..1 fraction as in the CSS colour the JS built, and it
// *replaces* rather than blends: painters draw onto a transparent tile and never
// overdraw a translucent pixel, so replace reproduces Canvas2D's source-over onto
// transparency exactly — including the two painters that use a fractional alpha.
void px(Image& img, int ox, int oy, int x, int y, double r, double g, double b,
        double a = 1.0) {
  img.set(ox + x, oy + y,
          Rgba {clamp8(r), clamp8(g), clamp8(b),
                static_cast<std::uint8_t>(std::lround(a * 255.0))});
}

// Fills a whole tile with a base colour plus per-pixel brightness jitter.
void noisy(Image& img, int ox, int oy, Rgb3 base, double amt, Mulberry32& rng) {
  for (int y = 0; y < T; ++y) {
    for (int x = 0; x < T; ++x) {
      const double j = (rng.next() * 2 - 1) * amt;
      px(img, ox, oy, x, y, base.r + j, base.g + j, base.b + j);
    }
  }
}

// Scatters small blobs of colour: ore flecks, cobble, pebbles.
void blobs(Image& img, int ox, int oy, Rgb3 c, int count, Mulberry32& rng, int sizeMax = 2) {
  for (int i = 0; i < count; ++i) {
    const int bx = static_cast<int>(rng.next() * T);
    const int by = static_cast<int>(rng.next() * T);
    const int s = 1 + static_cast<int>(rng.next() * sizeMax);
    for (int y = 0; y < s; ++y) {
      for (int x = 0; x < s; ++x) {
        const double j = (rng.next() * 2 - 1) * 18;
        px(img, ox, oy, (bx + x) % T, (by + y) % T, c.r + j, c.g + j, c.b + j);
      }
    }
  }
}

// Iron L-brackets riveted into all four corners, shared by the chest tiles.
void chestBrackets(Image& img, int ox, int oy) {
  auto arm = [&](int cx, int cy, int dx, int dy) {
    for (int k = 0; k < 3; ++k) {
      px(img, ox, oy, cx + k * dx, cy, 136, 138, 146);
      px(img, ox, oy, cx, cy + k * dy, 136, 138, 146);
    }
    px(img, ox, oy, cx + dx, cy + dy, 108, 110, 118);  // rivet
  };
  arm(0, 0, 1, 1);
  arm(15, 0, -1, 1);
  arm(0, 15, 1, -1);
  arm(15, 15, -1, -1);
}

// The plank panel with a dark frame that every side of the workbench is built on.
void workbenchFrame(Image& img, int ox, int oy) {
  for (int i = 0; i < T; ++i) {
    px(img, ox, oy, i, 0, 84, 62, 36);
    px(img, ox, oy, i, 15, 74, 54, 30);
    px(img, ox, oy, 0, i, 84, 62, 36);
    px(img, ox, oy, 15, i, 84, 62, 36);
  }
}

// The forge's mortared stone, under every one of its four sides.
void forgeMasonry(Image& img, int ox, int oy, Mulberry32& rng) {
  noisy(img, ox, oy, hex(0x7a7e86), 12, rng);
  for (int y = 0; y < T; ++y) {
    for (int x = 0; x < T; ++x) {
      const int row = y / 4, offset = row % 2 == 0 ? 0 : 4;
      if (y % 4 == 0 || (x + offset) % 8 == 0) px(img, ox, oy, x, y, 88, 90, 96);
    }
  }
}

// A chest side: horizontal boards, the lid seam, a board join, corner brackets.
void chestSideInto(Image& img, int ox, int oy, Mulberry32& rng) {
  noisy(img, ox, oy, hex(0xa97e48), 10, rng);
  for (int x = 0; x < T; ++x) {
    px(img, ox, oy, x, 4, 92, 64, 32);    // lid seam
    px(img, ox, oy, x, 5, 158, 116, 64);  // lower lip catches light
    px(img, ox, oy, x, 10, 130, 94, 48);  // board join
  }
  for (int i = 0; i < T; ++i) {
    px(img, ox, oy, i, 0, 122, 88, 46);
    px(img, ox, oy, i, 15, 84, 58, 30);
    px(img, ox, oy, 0, i, 100, 70, 36);
    px(img, ox, oy, 15, i, 100, 70, 36);
  }
  chestBrackets(img, ox, oy);
}

// A fresh generator for a nested painter call. The original passed
// mulberry32(hashSeed(name)) so the inner texture is identical wherever it is
// reused, and crucially does *not* advance the outer sequence.
Mulberry32 seeded(std::string_view name) { return Mulberry32(hashSeed(name)); }

// --- parameterised families --------------------------------------------------
// Each mirrors a hand-written painter but takes colours, so a new stone or wood is
// just a colour pair.

void plankTexInto(Image& img, int ox, int oy, Mulberry32& rng, Rgb3 base, Rgb3 line) {
  noisy(img, ox, oy, base, 10, rng);
  for (int y = 0; y < T; y += 4) {
    for (int x = 0; x < T; ++x) px(img, ox, oy, x, y, line.r, line.g, line.b);
  }
  for (int y = 0; y < T; ++y) px(img, ox, oy, 7, y, line.r, line.g, line.b);
}

void planksInto(Image& img, int ox, int oy, Mulberry32& rng) {
  noisy(img, ox, oy, hex(0xb08a52), 10, rng);
  for (int y = 0; y < T; y += 4) {
    for (int x = 0; x < T; ++x) px(img, ox, oy, x, y, 138, 104, 60);
  }
  for (int y = 0; y < T; ++y) px(img, ox, oy, 7, y, 138, 104, 60);
}

void greystoneInto(Image& img, int ox, int oy, Mulberry32& rng) {
  noisy(img, ox, oy, hex(0x7d8189), 16, rng);
  blobs(img, ox, oy, hex(0x6b6f77), 8, rng);
}

PainterFn stoneTex(std::uint32_t base, std::uint32_t fleck) {
  return [base, fleck](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(base), 16, rng);
    blobs(img, ox, oy, hex(fleck), 8, rng);
  };
}

// Polished stone: a dressed slab with a bevel on all four edges, lit from the top
// left like every raised thing in the atlas.
//
// It used to be a vertical gradient with a highlight along the top row only, which
// looked fine on one block and wrong on a floor of them: every row of blocks got a
// bright line at one edge and a dark one at the other, and a paved area read as a
// field of stripes rather than as slabs. Four bevelled edges tile into a grid of
// slabs from any side.
void polishedInto(Image& img, int ox, int oy, Mulberry32& rng, Rgb3 c) {
  for (int y = 0; y < T; ++y) {
    for (int x = 0; x < T; ++x) {
      // A faint diagonal sheen across the face, then the per-pixel grain.
      const double sheen = 1.0 + (7.5 - (x + y) * 0.5) * 0.006;
      const double k = sheen + (rng.next() * 2 - 1) * 0.03;
      px(img, ox, oy, x, y, c.r * k, c.g * k, c.b * k);
    }
  }
  for (int i = 0; i < T; ++i) {
    px(img, ox, oy, i, 0, c.r * 1.22, c.g * 1.22, c.b * 1.22);          // top
    px(img, ox, oy, 0, i, c.r * 1.14, c.g * 1.14, c.b * 1.14);          // left
    px(img, ox, oy, i, T - 1, c.r * 0.72, c.g * 0.72, c.b * 0.72);      // bottom
    px(img, ox, oy, T - 1, i, c.r * 0.78, c.g * 0.78, c.b * 0.78);      // right
  }
  // The two corners where a light edge meets a dark one, split between them.
  px(img, ox, oy, T - 1, 0, c.r, c.g, c.b);
  px(img, ox, oy, 0, T - 1, c.r * 0.9, c.g * 0.9, c.b * 0.9);
}

PainterFn polishedTex(std::uint32_t base) {
  const Rgb3 c = hex(base);
  return [c](Image& img, int ox, int oy, Mulberry32& rng) { polishedInto(img, ox, oy, rng, c); };
}

PainterFn bricksTex(std::uint32_t base) {
  const Rgb3 c = hex(base);
  const Rgb3 mortar {c.r * 0.42, c.g * 0.42, c.b * 0.42};
  return [c, mortar](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, c, 8, rng);
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        const int row = y / 4;
        const int offset = row % 2 == 0 ? 0 : 4;
        if (y % 4 == 0 || (x + offset) % 8 == 0) {
          px(img, ox, oy, x, y, mortar.r, mortar.g, mortar.b);
        }
      }
    }
  };
}

PainterFn plankTex(std::uint32_t base, std::uint32_t line) {
  const Rgb3 b = hex(base), l = hex(line);
  return [b, l](Image& img, int ox, int oy, Mulberry32& rng) {
    plankTexInto(img, ox, oy, rng, b, l);
  };
}

PainterFn logTopTex(std::uint32_t base) {
  const Rgb3 c = hex(base);
  const Rgb3 ring {c.r * 0.74, c.g * 0.74, c.b * 0.74};
  return [c, ring](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, c, 10, rng);
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        const double d = std::hypot(x - 7.5, y - 7.5);
        if (static_cast<int>(std::floor(d)) % 2 == 0) {
          px(img, ox, oy, x, y, ring.r, ring.g, ring.b);
        }
      }
    }
  };
}

PainterFn logSideTex(std::uint32_t base) {
  const Rgb3 c = hex(base);
  const Rgb3 grain {c.r * 0.78, c.g * 0.78, c.b * 0.78};
  return [c, grain](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, c, 12, rng);
    for (int x = 1; x < T; x += 4) {
      for (int y = 0; y < T; ++y) {
        if (rng.next() < 0.85) px(img, ox, oy, x, y, grain.r, grain.g, grain.b);
      }
    }
  };
}

PainterFn leavesTex(std::uint32_t c1, std::uint32_t c2) {
  const Rgb3 a = hex(c1), b = hex(c2);
  return [a, b](Image& img, int ox, int oy, Mulberry32& rng) {
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        if (rng.next() < 0.16) continue;  // cutout gaps -> see-through canopy
        const Rgb3 c = rng.next() < 0.5 ? a : b;
        const double j = (rng.next() * 2 - 1) * 20;
        px(img, ox, oy, x, y, c.r + j, c.g + j, c.b + j);
      }
    }
  };
}

// Wood doors: ONE door painted across two tiles, the upper half over the lower.
//
// A door is two stacked cells, and until this both drew the same tile — a complete
// door, frame, two panels and a handle, squeezed into sixteen pixels — so every door
// in the world was two small doors stacked on top of each other, with two handles.
// Now each half paints its own rows of a single 16x32 design: vertical boards in a
// frame, a four-pane window in the upper half, a raised panel in the lower, and the
// handle at the height a hand would find it. The window is cut clean through: the
// door is already see-through-capable for its cutout, and a window you cannot see
// through is a painting of one.
//
// `rows` picks the half: 0 paints rows 0-15 of the design (the upper cell), 16 paints
// rows 16-31 (the lower).
PainterFn doorTex(std::uint32_t base, std::uint32_t line, int rows) {
  const Rgb3 b = hex(base), l = hex(line);
  return [b, l, rows](Image& img, int ox, int oy, Mulberry32& rng) {
    constexpr int kH = 2 * T;  // the whole door
    // Every rule below is written against the full 32-row door; this is the one
    // place that knows only half of it lands in this tile.
    auto put = [&](int x, int dy, double r, double g, double bl, double a = 1.0) {
      const int y = dy - rows;
      if (y >= 0 && y < T) px(img, ox, oy, x, y, r, g, bl, a);
    };
    auto shadeOf = [&](double k, double j = 0) {
      return Rgb3 {b.r * k + j, b.g * k + j, b.b * k + j};
    };

    // Boards: three vertical planks with dark seams, grain streaks down each.
    for (int y = 0; y < kH; ++y) {
      for (int x = 0; x < T; ++x) {
        double j = (rng.next() * 2 - 1) * 7;
        if ((x * 7 + y / 3) % 11 == 0) j -= 12;  // grain
        const Rgb3 c = shadeOf(1.0, j);
        put(x, y, c.r, c.g, c.b);
      }
    }
    for (int y = 0; y < kH; ++y) {
      put(5, y, l.r * 1.15, l.g * 1.15, l.b * 1.15);
      put(10, y, l.r * 1.15, l.g * 1.15, l.b * 1.15);
    }

    // The frame: stiles down both sides, rails across the top and the bottom and at
    // the waist, all a shade lighter than the boards they hold.
    auto frameRow = [&](int y) {
      for (int x = 0; x < T; ++x) {
        const Rgb3 c = shadeOf(1.08, (rng.next() * 2 - 1) * 5);
        put(x, y, c.r, c.g, c.b);
      }
    };
    for (int y = 0; y < kH; ++y) {
      for (int x : {1, 14}) {
        const Rgb3 c = shadeOf(1.08, (rng.next() * 2 - 1) * 5);
        put(x, y, c.r, c.g, c.b);
      }
    }
    frameRow(1);
    frameRow(15);
    frameRow(16);
    frameRow(30);
    // The outline: what makes a door read as a door against a wall of planks.
    for (int y = 0; y < kH; ++y) {
      put(0, y, l.r, l.g, l.b);
      put(T - 1, y, l.r, l.g, l.b);
    }
    for (int x = 0; x < T; ++x) {
      put(x, 0, l.r, l.g, l.b);
      put(x, kH - 1, l.r, l.g, l.b);
    }

    // Upper half: a window of four panes behind a cross of glazing bars. The panes
    // are open; the sill below them catches the light.
    for (int y = 4; y <= 11; ++y) {
      for (int x = 3; x <= 12; ++x) {
        const bool bar = x == 7 || x == 8 || y == 7 || y == 8;
        const bool rim = x == 3 || x == 12 || y == 4 || y == 11;
        if (rim) {
          put(x, y, l.r, l.g, l.b);
        } else if (bar) {
          const Rgb3 c = shadeOf(0.92);
          put(x, y, c.r, c.g, c.b);
        } else {
          put(x, y, 0, 0, 0, 0.0);
        }
      }
    }
    for (int x = 3; x <= 12; ++x) put(x, 12, b.r * 1.22, b.g * 1.22, b.b * 1.22);

    // Lower half: a raised panel, lit along its top and left, shadowed below and
    // to the right, so it stands proud of the boards rather than being drawn on.
    for (int y = 19; y <= 27; ++y) {
      for (int x = 3; x <= 12; ++x) {
        const Rgb3 c = shadeOf(1.02, (rng.next() * 2 - 1) * 5);
        put(x, y, c.r, c.g, c.b);
      }
    }
    for (int x = 3; x <= 12; ++x) {
      put(x, 19, b.r * 1.24, b.g * 1.24, b.b * 1.24);
      put(x, 27, b.r * 0.6, b.g * 0.6, b.b * 0.6);
    }
    for (int y = 19; y <= 27; ++y) {
      put(3, y, b.r * 1.16, b.g * 1.16, b.b * 1.16);
      put(12, y, b.r * 0.66, b.g * 0.66, b.b * 0.66);
    }

    // The handle: a brass knob with a backplate, at the waist rail where a hand
    // meets it, and a shadow pixel so it sits proud of the wood.
    put(12, 15, 96, 90, 84);
    put(12, 16, 240, 208, 110);
    put(12, 17, 214, 178, 84);
    put(13, 17, b.r * 0.5, b.g * 0.5, b.b * 0.5);
  };
}

// Trapdoors: the wood's planks in a frame with a cross-brace and iron studs.
PainterFn trapdoorTex(std::uint32_t base, std::uint32_t line) {
  const Rgb3 b = hex(base), l = hex(line);
  char seedBuf[16];
  std::snprintf(seedBuf, sizeof(seedBuf), "#%06x", base);
  const std::string plankSeed = std::string(seedBuf) + "trap";

  return [b, l, plankSeed](Image& img, int ox, int oy, Mulberry32& rng) {
    Mulberry32 inner = seeded(plankSeed);
    plankTexInto(img, ox, oy, inner, b, l);

    for (int i = 0; i < T; ++i) {
      px(img, ox, oy, i, 0, l.r, l.g, l.b);
      px(img, ox, oy, i, T - 1, l.r, l.g, l.b);
      px(img, ox, oy, 0, i, l.r, l.g, l.b);
      px(img, ox, oy, T - 1, i, l.r, l.g, l.b);
    }
    // Lit inner chamfer along the top and left of the frame.
    for (int i = 1; i < T - 1; ++i) {
      px(img, ox, oy, i, 1, b.r * 1.15, b.g * 1.15, b.b * 1.15);
      px(img, ox, oy, 1, i, b.r * 1.1, b.g * 1.1, b.b * 1.1);
    }
    // X cross-brace with iron studs where it meets the frame.
    for (int i = 2; i <= 13; ++i) {
      px(img, ox, oy, i, i, l.r, l.g, l.b);
      px(img, ox, oy, i, 15 - i, l.r, l.g, l.b);
    }
    px(img, ox, oy, 2, 2, 126, 128, 136);
    px(img, ox, oy, 13, 2, 126, 128, 136);
    px(img, ox, oy, 2, 13, 126, 128, 136);
    px(img, ox, oy, 13, 13, 126, 128, 136);
    (void)rng;  // the trapdoor's own detail is deterministic; only the plank fill rolls
  };
}

PainterFn oreTexture(std::uint32_t color, int count) {
  const Rgb3 c = hex(color);
  return [c, count](Image& img, int ox, int oy, Mulberry32& rng) {
    Mulberry32 inner = seeded("greystone");  // identical stone base under every ore
    greystoneInto(img, ox, oy, inner);
    blobs(img, ox, oy, c, count, rng, 2);
  };
}

// --- plants and greebles ------------------------------------------------------
// These paint onto the tile's transparent background leaving gaps, producing a
// cutout X billboard. y = 0 is the top of the tile, y = 15 the base at ground level.

// Wraps into [0, T) for the leaning blades, which can walk their x negative.
int wrapT(int x) { return ((x % T) + T) % T; }

struct BladeOpts {
  int count = 7;
  int minH = 6;
  int varH = 7;
  double arch = 0.22;  // chance a blade leans as it rises
};

PainterFn bladeTex(std::uint32_t cLo, std::uint32_t cHi, BladeOpts opts) {
  const Rgb3 lo = hex(cLo), hi = hex(cHi);
  return [lo, hi, opts](Image& img, int ox, int oy, Mulberry32& rng) {
    const int n = opts.count + static_cast<int>(rng.next() * 3);
    for (int i = 0; i < n; ++i) {
      int x = 1 + static_cast<int>(rng.next() * (T - 2));
      const int bh = opts.minH + static_cast<int>(rng.next() * opts.varH);
      const Rgb3 c = rng.next() < 0.5 ? lo : hi;
      for (int k = 0; k < bh; ++k) {
        const int y = T - 1 - k;
        if (y < 1) break;
        if (k > 2 && rng.next() < opts.arch) x += rng.next() < 0.5 ? -1 : 1;
        const double j = (rng.next() * 2 - 1) * 16;
        px(img, ox, oy, wrapT(x), y, c.r + j, c.g + j, c.b + j);
        if (k < bh - 1 && rng.next() < 0.35) {
          px(img, ox, oy, wrapT(x + 1), y, c.r + j - 10, c.g + j - 10, c.b + j - 10);
        }
      }
    }
  };
}

// A stem with a coloured bloom head on top.
PainterFn flowerTex(std::uint32_t stem, std::uint32_t petal, std::uint32_t centre) {
  const Rgb3 s = hex(stem), p = hex(petal), c = hex(centre);
  return [s, p, c](Image& img, int ox, int oy, Mulberry32& rng) {
    const int sx = 7 + static_cast<int>(rng.next() * 2);
    for (int y = 6; y < T; ++y) {
      const double j = (rng.next() * 2 - 1) * 10;
      px(img, ox, oy, sx, y, s.r + j, s.g + j, s.b + j);
    }
    px(img, ox, oy, sx - 1, 10, s.r, s.g, s.b);  // little leaves
    px(img, ox, oy, sx + 1, 12, s.r, s.g, s.b);
    // Bloom: a rough five-petal ring around (sx, 4).
    const int head[10][2] = {{sx, 1},     {sx - 1, 2}, {sx + 1, 2}, {sx - 2, 3}, {sx + 2, 3},
                             {sx - 2, 5}, {sx + 2, 5}, {sx - 1, 6}, {sx + 1, 6}, {sx, 7}};
    for (const auto& h : head) {
      const double j = (rng.next() * 2 - 1) * 14;
      px(img, ox, oy, h[0], h[1], p.r + j, p.g + j, p.b + j);
    }
    const int core[4][2] = {{sx, 3}, {sx, 4}, {sx - 1, 4}, {sx + 1, 4}};
    for (const auto& h : core) px(img, ox, oy, h[0], h[1], c.r, c.g, c.b);
  };
}

// Short stem plus a domed cap.
PainterFn mushroomTex(std::uint32_t cap, bool spotted) {
  const Rgb3 c = hex(cap);
  return [c, spotted](Image& img, int ox, int oy, Mulberry32& rng) {
    for (int y = 8; y < 13; ++y) {
      px(img, ox, oy, 7, y, 224, 216, 198);
      px(img, ox, oy, 8, y, 208, 198, 178);
    }
    for (int y = 5; y < 9; ++y) {
      for (int x = 4; x < 12; ++x) {
        if (y == 5 && (x < 6 || x > 9)) continue;
        const double j = (rng.next() * 2 - 1) * 10;
        px(img, ox, oy, x, y, c.r + j, c.g + j, c.b + j);
      }
    }
    if (spotted) {
      px(img, ox, oy, 6, 6, 236, 236, 226);
      px(img, ox, oy, 9, 7, 236, 236, 226);
      px(img, ox, oy, 8, 6, 236, 236, 226);
    }
  };
}

// A rounded leafy clump.
PainterFn bushTex(std::uint32_t c1, std::uint32_t c2) {
  const Rgb3 a = hex(c1), b = hex(c2);
  return [a, b](Image& img, int ox, int oy, Mulberry32& rng) {
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        if (std::hypot(x - 8.0, y - 8.5) > 7) continue;
        if (rng.next() < 0.14) continue;  // cutout gaps
        const Rgb3 c = rng.next() < 0.5 ? a : b;
        const double j = (rng.next() * 2 - 1) * 18;
        px(img, ox, oy, x, y, c.r + j, c.g + j, c.b + j);
      }
    }
    for (int y = 12; y < T; ++y) px(img, ox, oy, 7, y, 92, 68, 42);  // stem at the base
  };
}

// Bare brittle twigs.
void deadBushTex(Image& img, int ox, int oy, Mulberry32& rng) {
  const Rgb3 c = hex(0x8a6a3a);
  const int branches = 5 + static_cast<int>(rng.next() * 3);
  for (int i = 0; i < branches; ++i) {
    int x = 5 + static_cast<int>(rng.next() * 6);
    int y = T - 1;
    const int h = 7 + static_cast<int>(rng.next() * 6);
    const int dir = rng.next() < 0.5 ? -1 : 1;
    for (int k = 0; k < h; ++k) {
      if (y < 2) break;
      const double j = (rng.next() * 2 - 1) * 14;
      px(img, ox, oy, wrapT(x), y, c.r + j, c.g + j, c.b + j);
      --y;
      if (rng.next() < 0.5) x += dir;
    }
  }
}

// Pebbles are small boxes now (RenderKind::Pebbles), so this is the STONE they are
// made of rather than a picture of a few of them: water-worn grey, mottled, with a
// lighter crown where the light catches the top. Every face of every stone samples
// its own few texels of it by position, so the mottling is what tells the stones
// apart. Opaque, since a box has no use for a cutout.
void pebblesTex(Image& img, int ox, int oy, Mulberry32& rng) {
  noisy(img, ox, oy, hex(0x8a8f96), 14, rng);
  blobs(img, ox, oy, hex(0x6f747b), 10, rng, 2);
  blobs(img, ox, oy, hex(0xa9aeb4), 8, rng, 2);
  // Rows 12-15 are the sides of the stones (they are one or two texels tall and sit
  // on the floor); a darker band there is the shadow under each one's crown.
  for (int y = 13; y < T; ++y) {
    for (int x = 0; x < T; ++x) {
      const Rgba p = img.get(ox + x, oy + y);
      px(img, ox, oy, x, y, p.r * 0.84, p.g * 0.84, p.b * 0.84);
    }
  }
}

// Papyrus: two tiles sharing the SAME stem columns so stacked segments read as
// continuous reeds. `papyrus_stem` runs every stem the full tile height and is
// used for segments with more papyrus above; `papyrus` carries the stems up to
// feathered umbels at the tips.
constexpr int kPapyrusStems[4][2] = {{4, 3}, {7, 1}, {10, 4}, {12, 6}};  // x, crown y

void papyrusStemPx(Image& img, int ox, int oy, Mulberry32& rng, int sx, int y) {
  const double g = 120 + (rng.next() * 2 - 1) * 16;
  px(img, ox, oy, sx, y, 106, g + 30, 66);
  if ((y + sx) % 5 == 0) px(img, ox, oy, sx, y, 84, 118, 52);  // stem node ring
  if (rng.next() < 0.2) px(img, ox, oy, sx + 1, y, 88, g + 12, 54);
}

// --- crops --------------------------------------------------------------------
//
// Eighteen crops at four growth stages is seventy-two tiles. Drawn one at a time
// that is days of pixel-pushing that would still come out less consistent than this;
// drawn as five FAMILIES that take their colours and a stage, a new crop is a table
// row and every crop in a family grows the same way.
//
// `stage` runs 0..kCropStages-1. Two rules the families all obey, because both are
// things a player reads at a glance from standing height:
//
//   * Stage 0 must say "something is planted here" and nothing more, or an empty
//     field and a sown one look identical and nobody can tell what they have done.
//   * Only the LAST stage looks harvestable. If stage 2 and stage 3 read alike,
//     players harvest early, lose the yield, and never find out why.

// Vertical stalks that head out at the top: wheat, barley, rice, maize.
PainterFn cropStalkTex(int stage, std::uint32_t lo, std::uint32_t hi, std::uint32_t head) {
  const Rgb3 a = hex(lo), b = hex(hi), h = hex(head);
  return [a, b, h, stage](Image& img, int ox, int oy, Mulberry32& rng) {
    const int n = 4 + stage;             // fills in as it grows
    const int height = 4 + stage * 3;    // 4, 7, 10, 13 of the 16 rows
    for (int i = 0; i < n; ++i) {
      const int x = 1 + static_cast<int>(rng.next() * (T - 2));
      const int bh = height - static_cast<int>(rng.next() * 2);
      for (int k = 0; k < bh; ++k) {
        const int y = T - 1 - k;
        if (y < 1) break;
        const Rgb3 c = rng.next() < 0.5 ? a : b;
        const double j = (rng.next() * 2 - 1) * 14;
        px(img, ox, oy, wrapT(x), y, c.r + j, c.g + j, c.b + j);
      }
      if (stage == kCropStages - 1) {
        // The grain head. This is the entire "ready to cut" signal.
        const int top = T - 1 - bh;
        for (int k = 0; k < 3; ++k) {
          const int y = top - k;
          if (y < 0) break;
          const double j = (rng.next() * 2 - 1) * 12;
          px(img, ox, oy, wrapT(x), y, h.r + j, h.g + j, h.b + j);
          if (rng.next() < 0.6) px(img, ox, oy, wrapT(x + 1), y, h.r, h.g, h.b);
        }
      }
    }
  };
}

// A leafy tuft over a buried root: carrot, potato, onion, beetroot, garlic. The root
// is underground, so ripeness shows as the crown shouldering out of the soil.
PainterFn cropLeafyTex(int stage, std::uint32_t leafLo, std::uint32_t leafHi,
                       std::uint32_t root) {
  const Rgb3 a = hex(leafLo), b = hex(leafHi), r = hex(root);
  return [a, b, r, stage](Image& img, int ox, int oy, Mulberry32& rng) {
    const int n = 3 + stage * 2;
    const int height = 3 + stage * 2;  // 3, 5, 7, 9
    for (int i = 0; i < n; ++i) {
      int x = 2 + static_cast<int>(rng.next() * (T - 4));
      const int bh = height - static_cast<int>(rng.next() * 2);
      for (int k = 0; k < bh; ++k) {
        const int y = T - 1 - k;
        if (y < 1) break;
        if (k > 1 && rng.next() < 0.35) x += rng.next() < 0.5 ? -1 : 1;  // splay out
        const Rgb3 c = rng.next() < 0.5 ? a : b;
        const double j = (rng.next() * 2 - 1) * 16;
        px(img, ox, oy, wrapT(x), y, c.r + j, c.g + j, c.b + j);
      }
    }
    if (stage == kCropStages - 1) {
      // The crown of the root breaking the surface.
      for (int x = 6; x <= 9; ++x) {
        const double j = (rng.next() * 2 - 1) * 10;
        px(img, ox, oy, x, T - 1, r.r + j, r.g + j, r.b + j);
        if (x >= 7 && x <= 8) px(img, ox, oy, x, T - 2, r.r, r.g, r.b);
      }
    }
  };
}

// A trailing vine that sets fruit on the ground: pumpkin, melon, tomato, chili.
PainterFn cropVineTex(int stage, std::uint32_t vine, std::uint32_t fruit) {
  const Rgb3 v = hex(vine), f = hex(fruit);
  return [v, f, stage](Image& img, int ox, int oy, Mulberry32& rng) {
    const int spread = 3 + stage * 2;
    for (int i = 0; i < 3 + stage; ++i) {
      int x = 8 + static_cast<int>((rng.next() * 2 - 1) * spread);
      int y = T - 1;
      const int len = 3 + stage + static_cast<int>(rng.next() * 3);
      for (int k = 0; k < len; ++k) {
        if (y < 4) break;
        const double j = (rng.next() * 2 - 1) * 14;
        px(img, ox, oy, wrapT(x), y, v.r + j, v.g + j, v.b + j);
        if (rng.next() < 0.4) px(img, ox, oy, wrapT(x + 1), y, v.r - 12, v.g - 12, v.b - 12);
        --y;
        if (rng.next() < 0.5) x += rng.next() < 0.5 ? -1 : 1;
      }
    }
    // The fruit swells over the last two stages rather than appearing from nothing,
    // so "nearly ready" is legible as well as "ready".
    if (stage >= kCropStages - 2) {
      const int rad = stage == kCropStages - 1 ? 3 : 2;
      const int cx = 8, cy = T - 1 - rad;
      for (int yy = cy - rad; yy <= cy + rad; ++yy) {
        for (int xx = cx - rad; xx <= cx + rad; ++xx) {
          if (yy < 0 || yy >= T) continue;
          const double d = std::hypot(xx - cx, yy - cy);
          if (d > rad) continue;
          const double j = (rng.next() * 2 - 1) * 12 - (d > rad - 1 ? 18 : 0);
          px(img, ox, oy, wrapT(xx), yy, f.r + j, f.g + j, f.b + j);
        }
      }
    }
  };
}

// A low bush that berries up: strawberry, blueberry, grapes.
PainterFn cropBerryTex(int stage, std::uint32_t leaf, std::uint32_t berry) {
  const Rgb3 l = hex(leaf), b = hex(berry);
  return [l, b, stage](Image& img, int ox, int oy, Mulberry32& rng) {
    const double rad = 2.5 + stage * 1.6;
    const double cy = T - 1.5;
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        if (std::hypot(x - 8.0, y - cy) > rad) continue;
        if (rng.next() < 0.18) continue;  // cutout gaps, as the shrub does
        const double j = (rng.next() * 2 - 1) * 18;
        px(img, ox, oy, x, y, l.r + j, l.g + j, l.b + j);
      }
    }
    if (stage >= kCropStages - 2) {
      const int berries = stage == kCropStages - 1 ? 6 : 2;
      for (int i = 0; i < berries; ++i) {
        const int bx = 4 + static_cast<int>(rng.next() * 9);
        const int by = static_cast<int>(cy - rng.next() * rad);
        if (by < 0 || by >= T) continue;
        px(img, ox, oy, bx, by, b.r, b.g, b.b);
        if (rng.next() < 0.5 && bx + 1 < T) {
          px(img, ox, oy, bx + 1, by, b.r - 16, b.g - 16, b.b - 16);
        }
      }
    }
  };
}

// An upright plant hung with pods: chili, soybean.
//
// This exists because both of them used to borrow another family and came out
// unreadable: a chili drawn as round red fruit on a vine was indistinguishable from
// a tomato, and a soybean drawn as a leafy head was indistinguishable from a
// cabbage. Two crops a player cannot tell apart are, in practice, one crop.
PainterFn cropPodTex(int stage, std::uint32_t leaf, std::uint32_t pod) {
  const Rgb3 l = hex(leaf), p = hex(pod);
  return [l, p, stage](Image& img, int ox, int oy, Mulberry32& rng) {
    const int height = 5 + stage * 3;
    for (int i = 0; i < 3 + stage; ++i) {
      const int x = 3 + static_cast<int>(rng.next() * (T - 6));
      const int bh = height - static_cast<int>(rng.next() * 3);
      for (int k = 0; k < bh; ++k) {
        const int y = T - 1 - k;
        if (y < 1) break;
        const double j = (rng.next() * 2 - 1) * 15;
        px(img, ox, oy, wrapT(x), y, l.r + j, l.g + j, l.b + j);
        if (rng.next() < 0.3) px(img, ox, oy, wrapT(x + 1), y, l.r - 14, l.g - 14, l.b - 14);
      }
    }
    // Pods hang vertically, which is the whole point: a tall thin mark reads as a
    // pod at a glance where a round one reads as fruit.
    if (stage >= kCropStages - 2) {
      const int pods = stage == kCropStages - 1 ? 4 : 2;
      for (int i = 0; i < pods; ++i) {
        const int x = 3 + static_cast<int>(rng.next() * (T - 6));
        const int top = T - 2 - static_cast<int>(rng.next() * (height - 3));
        for (int k = 0; k < 4; ++k) {
          const int y = top + k;
          if (y < 0 || y >= T) continue;
          const double j = (rng.next() * 2 - 1) * 10;
          px(img, ox, oy, wrapT(x), y, p.r + j, p.g + j, p.b + j);
          if (k > 0 && k < 3) {
            px(img, ox, oy, wrapT(x + 1), y, p.r - 22, p.g - 22, p.b - 22);
          }
        }
      }
    }
  };
}

// A tight head: cabbage.
PainterFn cropHeadTex(int stage, std::uint32_t outer, std::uint32_t inner) {
  const Rgb3 o = hex(outer), n = hex(inner);
  return [o, n, stage](Image& img, int ox, int oy, Mulberry32& rng) {
    const double rad = 2.0 + stage * 1.7;
    const double cy = T - 1.0 - rad * 0.7;
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        const double d = std::hypot(x - 8.0, y - cy);
        if (d > rad) continue;
        // The heart of the head lightens as it firms up, which is what separates a
        // loose stage-2 rosette from a solid stage-3 one.
        const bool heart = d < rad * 0.45 && stage == kCropStages - 1;
        const Rgb3 c = heart ? n : o;
        const double j = (rng.next() * 2 - 1) * 16;
        px(img, ox, oy, x, y, c.r + j, c.g + j, c.b + j);
      }
    }
    for (int y = static_cast<int>(cy + rad); y < T; ++y) {
      px(img, ox, oy, 7, y, 86, 104, 52);  // the stem down to the soil
      px(img, ox, oy, 8, y, 74, 92, 44);
    }
  };
}

// --- registry ----------------------------------------------------------------

std::vector<PainterEntry> buildPainters() {
  std::vector<PainterEntry> out;
  out.reserve(120);

  auto add = [&out](std::string name, PainterFn fn) {
    ResourceId id(std::string("block/") + name);
    out.push_back({id, std::move(name), std::move(fn)});
  };

  // ---- terrain ----
  add("bedrock", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x2a2c30), 26, rng);
    blobs(img, ox, oy, hex(0x15161a), 18, rng);
  });
  add("greystone", greystoneInto);
  add("cobbled", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x73767d), 10, rng);
    blobs(img, ox, oy, hex(0x5a5d63), 10, rng, 3);
    blobs(img, ox, oy, hex(0x909499), 8, rng, 2);
  });
  add("loam", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x6b4b32), 20, rng);
    blobs(img, ox, oy, hex(0x52391f), 10, rng);
  });
  add("turf_top", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x5d9b41), 22, rng);
    blobs(img, ox, oy, hex(0x6fb14d), 14, rng);
    blobs(img, ox, oy, hex(0x4c8636), 10, rng);
  });
  add("turf_side", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x6b4b32), 20, rng);
    const Rgb3 grass = hex(0x5d9b41);
    for (int x = 0; x < T; ++x) {
      const int h = 3 + (rng.next() < 0.5 ? 1 : 0);
      for (int y = 0; y < h; ++y) {
        const double j = (rng.next() * 2 - 1) * 18;
        px(img, ox, oy, x, y, grass.r + j, grass.g + j, grass.b + j);
      }
    }
  });
  add("sand", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0xdccea2), 14, rng);
    blobs(img, ox, oy, hex(0xcdbd8a), 8, rng);
  });
  add("sandstone", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0xd8c896), 8, rng);
    for (int y = 3; y < T; y += 5) {
      for (int x = 0; x < T; ++x) px(img, ox, oy, x, y, 180, 162, 116);
    }
  });
  add("shingle", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x8a8073), 16, rng);
    blobs(img, ox, oy, hex(0x6f665b), 12, rng, 2);
    blobs(img, ox, oy, hex(0xa39a8b), 8, rng, 1);
  });

  // ---- wood ----
  add("log_top", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x9c7748), 10, rng);
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        // The original wrote this as (x - cx + 0.5) with cx = 8, i.e. the same
        // (7.5, 7.5) centre the parameterised logTopTex uses.
        const double d = std::hypot(x - 7.5, y - 7.5);
        if (static_cast<int>(std::floor(d)) % 2 == 0) px(img, ox, oy, x, y, 120, 92, 56);
      }
    }
  });
  add("log_side", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x7d5e38), 12, rng);
    for (int x = 1; x < T; x += 4) {
      for (int y = 0; y < T; ++y) {
        if (rng.next() < 0.85) px(img, ox, oy, x, y, 92, 68, 40);
      }
    }
  });
  add("leaves", [](Image& img, int ox, int oy, Mulberry32& rng) {
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        if (rng.next() < 0.16) continue;  // transparent gaps -> cutout canopy
        const Rgb3 c = rng.next() < 0.5 ? hex(0x3f7a32) : hex(0x356b2a);
        const double j = (rng.next() * 2 - 1) * 20;
        px(img, ox, oy, x, y, c.r + j, c.g + j, c.b + j);
      }
    }
  });
  add("planks", planksInto);

  // ---- built ----
  add("bricks", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x8a8d93), 8, rng);
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        const int row = y / 4;
        const int offset = row % 2 == 0 ? 0 : 4;
        if (y % 4 == 0 || (x + offset) % 8 == 0) px(img, ox, oy, x, y, 60, 62, 66);
      }
    }
  });
  add("polished", polishedTex(0x8a8c94));
  add("wool", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0xececec), 8, rng);
    blobs(img, ox, oy, hex(0xdadada), 16, rng, 2);
    blobs(img, ox, oy, hex(0xffffff), 10, rng, 1);
  });
  add("glass", [](Image& img, int ox, int oy, Mulberry32& rng) {
    // Transparent centre so it reads as see-through, a light frame with corner
    // rivets, and a pair of diagonal glints.
    (void)rng;
    for (int i = 0; i < T; ++i) {
      px(img, ox, oy, i, 0, 219, 219, 219);
      px(img, ox, oy, i, T - 1, 210, 210, 210);
      px(img, ox, oy, 0, i, 219, 219, 219);
      px(img, ox, oy, T - 1, i, 210, 210, 210);
    }
    px(img, ox, oy, 1, 1, 245, 245, 245);
    px(img, ox, oy, 14, 1, 237, 237, 237);
    px(img, ox, oy, 1, 14, 237, 237, 237);
    px(img, ox, oy, 14, 14, 227, 227, 227);
    for (int i = 2; i < 8; ++i) px(img, ox, oy, i, i, 244, 244, 244);
    for (int i = 4; i < 8; ++i) px(img, ox, oy, i - 1, i + 2, 237, 237, 237);
    for (int i = 10; i < 13; ++i) px(img, ox, oy, i, i, 233, 233, 233);
  });
  add("water", [](Image& img, int ox, int oy, Mulberry32& rng) {
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        const double j = (rng.next() * 2 - 1) * 12 + std::sin((x + y) * 0.8) * 6;
        // The 0.72 alpha only has to clear the shader's 0.5 cutout test; the
        // visible translucency comes from the water pass's constant 0.85.
        px(img, ox, oy, x, y, 40 + j, 90 + j, 170 + j, 0.72);
      }
    }
  });
  add("canvas", [](Image& img, int ox, int oy, Mulberry32& rng) {
    // The frame, and the only part of a painting the atlas ever holds: a grained
    // wooden border around a pale primed centre. The centre is what shows through
    // on a blank canvas and what the picture is drawn over when there is one, so it
    // is deliberately plain rather than textured.
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        const bool border = x < 1 || y < 1 || x >= T - 1 || y >= T - 1;
        if (border) {
          const int g = static_cast<int>(rng.next() * 22.0);
          px(img, ox, oy, x, y, 96 + g, 66 + g, 38 + g);
        } else {
          const int g = static_cast<int>(rng.next() * 10.0);
          px(img, ox, oy, x, y, 214 + g, 206 + g, 190 + g);
        }
      }
    }
    // A bevel: light along the top-left of the frame, shadow along the inside.
    for (int i = 1; i < T - 1; ++i) {
      px(img, ox, oy, i, 1, 176, 168, 152);
      px(img, ox, oy, 1, i, 176, 168, 152);
    }
  });
  add("torch", [](Image& img, int ox, int oy, Mulberry32& rng) {
    // Transparent background; a grained stick with a wrap, a charred head, and a
    // layered flame: ember base, orange body, yellow, white-hot core.
    (void)rng;
    for (int y = 7; y < T; ++y) {
      px(img, ox, oy, 7, y, 128, 94, 54);
      px(img, ox, oy, 8, y, 100, 72, 40);
    }
    px(img, ox, oy, 7, 9, 156, 118, 68);
    px(img, ox, oy, 8, 9, 76, 54, 30);  // binding wrap
    px(img, ox, oy, 7, 13, 110, 80, 46);  // grain nick
    px(img, ox, oy, 7, 6, 56, 44, 34);
    px(img, ox, oy, 8, 6, 42, 32, 26);  // charred head
    px(img, ox, oy, 7, 5, 232, 106, 28);
    px(img, ox, oy, 8, 5, 214, 90, 24);  // embers
    px(img, ox, oy, 6, 4, 242, 138, 34);
    px(img, ox, oy, 9, 4, 234, 124, 30);  // flame body
    px(img, ox, oy, 7, 4, 252, 184, 58);
    px(img, ox, oy, 8, 4, 250, 170, 50);
    px(img, ox, oy, 6, 3, 248, 166, 46);
    px(img, ox, oy, 9, 3, 240, 148, 38);
    px(img, ox, oy, 7, 3, 255, 226, 120);
    px(img, ox, oy, 8, 3, 255, 212, 98);
    px(img, ox, oy, 7, 2, 255, 244, 190);
    px(img, ox, oy, 8, 2, 255, 234, 158);  // hot core
    px(img, ox, oy, 8, 1, 255, 208, 108);  // licking tip
  });

  add("workbench_top", [](Image& img, int ox, int oy, Mulberry32& rng) {
    Mulberry32 inner = seeded("planks");
    planksInto(img, ox, oy, inner);
    (void)rng;
    // Banded edge frame, a carved 3x3 crafting grid, iron corner pins.
    for (int i = 0; i < T; ++i) {
      px(img, ox, oy, i, 0, 132, 100, 58);
      px(img, ox, oy, i, 15, 92, 68, 38);
      px(img, ox, oy, 0, i, 118, 88, 48);
      px(img, ox, oy, 15, i, 100, 74, 42);
    }
    auto groove = [&](int x, int y) { px(img, ox, oy, x, y, 88, 64, 36); };
    for (int i = 3; i <= 12; ++i) {
      groove(i, 3); groove(i, 12); groove(3, i); groove(12, i);  // grid frame
      groove(i, 6); groove(i, 9); groove(6, i); groove(9, i);    // cell dividers
    }
    for (int i = 4; i <= 11; ++i) {
      if (i != 6 && i != 9) px(img, ox, oy, i, 4, 196, 158, 100);  // carve catches light
    }
    px(img, ox, oy, 1, 1, 122, 124, 132);
    px(img, ox, oy, 14, 1, 122, 124, 132);
    px(img, ox, oy, 1, 14, 122, 124, 132);
    px(img, ox, oy, 14, 14, 122, 124, 132);
  });
  // The workbench has a front (the tool rack), two sides and a back since it
  // started facing whoever places it. All three share one framed plank panel.
  add("workbench_front", [](Image& img, int ox, int oy, Mulberry32& rng) {
    Mulberry32 inner = seeded("planks2");
    planksInto(img, ox, oy, inner);
    (void)rng;
    workbenchFrame(img, ox, oy);
    // A saw and a hammer hung on the panel.
    for (int x = 2; x <= 8; ++x) {  // saw: bright blade, toothed underside
      px(img, ox, oy, x, 4, 190, 194, 200);
      px(img, ox, oy, x, 5, 158, 162, 170);
      if (x % 2 == 0) px(img, ox, oy, x, 6, 150, 154, 162);
    }
    for (int y = 3; y <= 5; ++y) {  // wooden grip
      px(img, ox, oy, 9, y, 116, 84, 46);
      px(img, ox, oy, 10, y, 92, 66, 36);
    }
    for (int x = 9; x <= 13; ++x) {  // hammer head
      px(img, ox, oy, x, 9, 128, 132, 140);
      px(img, ox, oy, x, 10, 100, 104, 112);
    }
    px(img, ox, oy, 13, 9, 156, 160, 168);
    for (int y = 11; y <= 14; ++y) {  // shaft
      px(img, ox, oy, 10, y, 128, 94, 54);
      px(img, ox, oy, 11, y, 104, 74, 42);
    }
  });
  // A side: a drawer under the top, with a pull, and a shelf rail below.
  add("workbench_side", [](Image& img, int ox, int oy, Mulberry32& rng) {
    Mulberry32 inner = seeded("planks3");
    planksInto(img, ox, oy, inner);
    (void)rng;
    workbenchFrame(img, ox, oy);
    for (int y = 3; y <= 7; ++y) {
      for (int x = 3; x <= 12; ++x) {
        const bool edge = y == 3 || y == 7 || x == 3 || x == 12;
        if (edge) px(img, ox, oy, x, y, y == 3 || x == 3 ? 176 : 96, y == 3 || x == 3 ? 136 : 70,
                     y == 3 || x == 3 ? 82 : 40);
      }
    }
    px(img, ox, oy, 7, 5, 128, 130, 138);  // the pull
    px(img, ox, oy, 8, 5, 104, 106, 114);
    for (int x = 1; x <= 14; ++x) px(img, ox, oy, x, 11, 92, 68, 38);  // shelf rail
  });
  // The back: the bare panel, braced.
  add("workbench_back", [](Image& img, int ox, int oy, Mulberry32& rng) {
    Mulberry32 inner = seeded("planks4");
    planksInto(img, ox, oy, inner);
    (void)rng;
    workbenchFrame(img, ox, oy);
    for (int i = 2; i <= 13; ++i) {
      px(img, ox, oy, i, 15 - i, 104, 76, 42);
      px(img, ox, oy, i, 16 - i, 150, 112, 64);
    }
  });
  add("forge_top", [](Image& img, int ox, int oy, Mulberry32& rng) {
    // Mortared stone body, recessed vent glowing through iron grate bars.
    noisy(img, ox, oy, hex(0x7a7e86), 12, rng);
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        const int row = y / 4, offset = row % 2 == 0 ? 0 : 4;
        if (y % 4 == 0 || (x + offset) % 8 == 0) px(img, ox, oy, x, y, 88, 90, 96);
      }
    }
    for (int y = 4; y <= 11; ++y) {
      for (int x = 4; x <= 11; ++x) px(img, ox, oy, x, y, 24, 20, 18);
    }
    for (int y = 5; y <= 10; ++y) {
      for (int x = 5; x <= 10; ++x) {
        double heat = 1 - std::hypot(x - 7.5, y - 7.5) / 4.2;
        if (heat < 0) heat = 0;
        if (rng.next() < 0.45 + heat * 0.5) {
          px(img, ox, oy, x, y, 190 + heat * 65, 70 + heat * 120, 16 + heat * 40);
        }
      }
    }
    for (int gx : {6, 9}) {
      for (int y = 4; y <= 11; ++y) px(img, ox, oy, gx, y, 68, 70, 76);
    }
    for (int gy : {6, 9}) {
      for (int x = 4; x <= 11; ++x) px(img, ox, oy, x, gy, 62, 64, 70);
    }
  });
  // The forge's front is the firebox; its sides are banded masonry and its back a
  // flue hatch, so from behind or beside it you can see which way it faces.
  add("forge_front", [](Image& img, int ox, int oy, Mulberry32& rng) {
    // Mortared stone body with an arched, lintel-topped firebox. The fire is
    // layered bottom-up: coal bed, orange body, yellow tongues, hot core.
    forgeMasonry(img, ox, oy, rng);
    for (int x = 4; x <= 11; ++x) px(img, ox, oy, x, 5, 74, 76, 82);  // iron lintel
    px(img, ox, oy, 3, 5, 60, 62, 68);
    px(img, ox, oy, 12, 5, 60, 62, 68);
    for (int y = 6; y <= 14; ++y) {
      for (int x = 4; x <= 11; ++x) {
        if (y == 6 && (x < 6 || x > 9)) continue;  // arched corners
        px(img, ox, oy, x, y, 16, 13, 12);
      }
    }
    for (int x = 5; x <= 10; ++x) {
      const double r = 118 + rng.next() * 60;
      px(img, ox, oy, x, 14, r, 28, 14);
    }
    for (int x = 5; x <= 10; ++x) {
      if (rng.next() < 0.9) {
        const double g = 88 + rng.next() * 34;
        px(img, ox, oy, x, 13, 224, g, 20);
      }
    }
    for (int x = 5; x <= 10; ++x) {
      if (rng.next() < 0.75) {
        const double g = 138 + rng.next() * 32;
        px(img, ox, oy, x, 12, 246, g, 30);
      }
    }
    for (int x = 6; x <= 9; ++x) {
      if (rng.next() < 0.7) px(img, ox, oy, x, 11, 252, 190, 60);
    }
    for (int x = 6; x <= 9; ++x) {
      if (rng.next() < 0.45) px(img, ox, oy, x, 10, 255, 226, 120);
    }
    px(img, ox, oy, 7, 10, 255, 240, 170);
    px(img, ox, oy, 8, 11, 255, 236, 156);
    px(img, ox, oy, 6, 8, 250, 176, 60);  // stray spark
  });
  add("forge_side", [](Image& img, int ox, int oy, Mulberry32& rng) {
    forgeMasonry(img, ox, oy, rng);
    // Iron bands binding the corners, and soot climbing from where the fire is.
    for (int y = 0; y < T; ++y) {
      px(img, ox, oy, 0, y, 70, 72, 78);
      px(img, ox, oy, 15, y, 58, 60, 66);
    }
    for (int x = 0; x < T; ++x) px(img, ox, oy, x, 12, 74, 76, 82);
    for (int y = 0; y < 6; ++y) {
      for (int x = 1; x < T - 1; ++x) {
        if (rng.next() < 0.18 * (6 - y) / 6.0) {
          const Rgba c = img.get(ox + x, oy + y);
          px(img, ox, oy, x, y, c.r * 0.6, c.g * 0.6, c.b * 0.6);
        }
      }
    }
  });
  add("forge_back", [](Image& img, int ox, int oy, Mulberry32& rng) {
    forgeMasonry(img, ox, oy, rng);
    // A small iron ash hatch low down, bolted shut.
    for (int y = 9; y <= 13; ++y) {
      for (int x = 5; x <= 10; ++x) {
        const bool rim = y == 9 || y == 13 || x == 5 || x == 10;
        px(img, ox, oy, x, y, rim ? 58 : 84, rim ? 60 : 86, rim ? 66 : 94);
      }
    }
    px(img, ox, oy, 6, 10, 120, 122, 130);
    px(img, ox, oy, 9, 10, 120, 122, 130);
    px(img, ox, oy, 6, 12, 120, 122, 130);
    px(img, ox, oy, 9, 12, 120, 122, 130);
  });
  add("chest_top", [](Image& img, int ox, int oy, Mulberry32& rng) {
    // Warm oak boards bound by an iron strap, brackets riveted at the corners.
    noisy(img, ox, oy, hex(0xab7f49), 10, rng);
    for (int yy : {5, 10}) {
      for (int x = 0; x < T; ++x) px(img, ox, oy, x, yy, 134, 98, 52);
    }
    for (int i = 0; i < T; ++i) {
      px(img, ox, oy, i, 0, 122, 88, 46);
      px(img, ox, oy, i, 15, 88, 62, 32);
      px(img, ox, oy, 0, i, 104, 74, 38);
      px(img, ox, oy, 15, i, 104, 74, 38);
    }
    for (int y = 0; y < T; ++y) {
      px(img, ox, oy, 7, y, 130, 132, 140);
      px(img, ox, oy, 8, y, 102, 104, 112);
    }
    chestBrackets(img, ox, oy);
  });
  // The chest's latch is its front, and the lid's hinges are on the back. It used to
  // carry the latch on all four sides, which meant it had no front at all.
  add("chest_front", [](Image& img, int ox, int oy, Mulberry32& rng) {
    chestSideInto(img, ox, oy, rng);
    for (int y = 2; y <= 6; ++y) {  // latch plate straddling the seam
      for (int x = 6; x <= 9; ++x) px(img, ox, oy, x, y, 128, 130, 138);
    }
    for (int y = 2; y <= 6; ++y) px(img, ox, oy, 6, y, 104, 106, 114);
    for (int x = 6; x <= 9; ++x) px(img, ox, oy, x, 6, 92, 94, 102);
    px(img, ox, oy, 7, 2, 170, 172, 180);
    px(img, ox, oy, 8, 2, 170, 172, 180);
    px(img, ox, oy, 7, 4, 42, 42, 48);  // keyhole
    px(img, ox, oy, 8, 4, 42, 42, 48);
    px(img, ox, oy, 7, 5, 42, 42, 48);
  });
  add("chest_side", [](Image& img, int ox, int oy, Mulberry32& rng) {
    chestSideInto(img, ox, oy, rng);
    for (int x = 5; x <= 10; ++x) {  // a carrying handle
      px(img, ox, oy, x, 8, 112, 114, 122);
    }
    px(img, ox, oy, 5, 9, 92, 94, 102);
    px(img, ox, oy, 10, 9, 92, 94, 102);
  });
  add("chest_back", [](Image& img, int ox, int oy, Mulberry32& rng) {
    chestSideInto(img, ox, oy, rng);
    for (int hx : {3, 11}) {  // two hinges across the lid seam
      for (int y = 2; y <= 6; ++y) {
        px(img, ox, oy, hx, y, 118, 120, 128);
        px(img, ox, oy, hx + 1, y, 96, 98, 106);
      }
      px(img, ox, oy, hx, 4, 150, 152, 160);
    }
  });
  add("ladder", [](Image& img, int ox, int oy, Mulberry32& rng) {
    // Transparent background: two rails, chunky rungs with an underside shadow,
    // and a nail where each rung meets a rail.
    (void)rng;
    auto rail = [&](int x) {
      for (int y = 0; y < T; ++y) {
        px(img, ox, oy, x, y, 150, 116, 66);
        px(img, ox, oy, x + 1, y, 118, 90, 50);
      }
    };
    rail(2);
    rail(12);
    for (int y = 1; y < T - 1; y += 4) {
      for (int x = 2; x < 14; ++x) {
        px(img, ox, oy, x, y, 160, 124, 72);
        px(img, ox, oy, x, y + 1, 122, 94, 52);
      }
      px(img, ox, oy, 3, y, 104, 80, 44);
      px(img, ox, oy, 12, y, 104, 80, 44);
    }
  });
  add("trapdoor", trapdoorTex(0xb08a52, 0x6e5230));
  add("door", doorTex(0xb08a52, 0x6e5230, T));
  add("door_upper", doorTex(0xb08a52, 0x6e5230, 0));

  // ---- the bed ----
  //
  // Four of these are the MATTRESS, the one part of the model a dye reaches (see
  // bedBoxes in world/shapes.cpp), and are painted as neutral light greys so the
  // shaders' multiply lands on something that can take a colour: white takes it
  // exactly, and the darker hems come out as shaded versions of it. The frame and
  // the pillow are parts of their own and are painted in their real colours,
  // because no dye ever touches them — which is the whole reason the bed stopped
  // being one box. Faces are textured by position now, so each tile is drawn with
  // the rows the model actually shows in mind.
  //
  // The head cell's blanket, with the tile's top edge toward the head. The pillow
  // sits on rows 2-6 and the headboard hides 0-1, so the turned-down sheet is drawn
  // just below the pillow, where it shows.
  add("bed_head_top", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0xdadada), 7, rng);
    for (int x = 0; x < T; ++x) {
      px(img, ox, oy, x, 7, 250, 250, 250);  // the fold, catching the light
      px(img, ox, oy, x, 8, 238, 238, 238);
      px(img, ox, oy, x, 9, 170, 170, 170);  // and its shadow on the blanket
    }
    for (int y = 10; y < T; ++y) {  // quilting below the fold, as on the foot tile
      for (int x = 0; x < T; ++x) {
        if ((x + y) % 6 == 0 || (x - y + 32) % 6 == 0) px(img, ox, oy, x, y, 196, 196, 196);
      }
    }
  });
  add("bed_foot_top", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0xdadada), 7, rng);
    for (int y = 0; y < T; ++y) {  // diagonal quilt stitching
      for (int x = 0; x < T; ++x) {
        if ((x + y) % 6 == 0 || (x - y + 32) % 6 == 0) px(img, ox, oy, x, y, 196, 196, 196);
      }
    }
  });
  // The mattress's side. Only rows 7-9 are ever on screen — the mattress is three
  // texels deep, from 6/16 to 9/16 — so those three carry the design: a lit top
  // edge where the blanket rolls over, the blanket, and a hem in shadow.
  add("bed_side", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0xd2d2d2), 7, rng);
    for (int x = 0; x < T; ++x) {
      px(img, ox, oy, x, 7, 240, 240, 240);
      px(img, ox, oy, x, 9, 150, 150, 150);
    }
  });
  add("bed_bottom", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x9a9a9a), 8, rng);
  });
  // The wooden head- and footboards and the frame between them. Corner posts down
  // both edges, so each board reads as framed and the frame rail's ends read as the
  // posts they meet.
  add("bed_frame", [](Image& img, int ox, int oy, Mulberry32& rng) {
    const Rgb3 wood = hex(0x8a5a34), post = hex(0x6a4226), cap = hex(0xa87448);
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        double j = (rng.next() * 2 - 1) * 8;
        if ((x * 5 + y / 4) % 9 == 0) j -= 14;  // grain running down the boards
        const bool isPost = x <= 1 || x >= T - 2;
        const Rgb3 c = isPost ? post : wood;
        px(img, ox, oy, x, y, c.r + j, c.g + j, c.b + j);
      }
    }
    for (int y = 0; y < T; ++y) {  // board seams
      px(img, ox, oy, 5, y, wood.r * 0.72, wood.g * 0.72, wood.b * 0.72);
      px(img, ox, oy, 10, y, wood.r * 0.72, wood.g * 0.72, wood.b * 0.72);
    }
    // A lighter cap along the top of the headboard (row 2, the highest the model
    // shows) and of the footboard (row 6), so both boards have a finished edge.
    for (int x = 0; x < T; ++x) {
      px(img, ox, oy, x, 2, cap.r, cap.g, cap.b);
      px(img, ox, oy, x, 6, cap.r, cap.g, cap.b);
    }
  });
  add("bed_pillow", [](Image& img, int ox, int oy, Mulberry32& rng) {
    // Soft white, darkening toward every edge so it looks stuffed rather than cut.
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        const double dx = std::abs(x - 7.5) / 7.5, dy = std::abs(y - 7.5) / 7.5;
        const double edge = std::max(dx, dy);
        const double v = 246 - edge * edge * 34 + (rng.next() * 2 - 1) * 4;
        px(img, ox, oy, x, y, v, v - 1, v - 6);
      }
    }
  });

  // ---- generated stone and wood families ----
  add("umberstone", stoneTex(0x8a6a4a, 0x6f543a));
  add("slatestone", stoneTex(0x54606e, 0x424c58));
  add("polished_umber", polishedTex(0x9a7a58));
  add("polished_slate", polishedTex(0x64707e));
  add("bricks_umber", bricksTex(0x8a6a4a));
  add("bricks_slate", bricksTex(0x54606e));
  add("pine_planks", plankTex(0xc2a05a, 0xa8843e));
  add("dusk_planks", plankTex(0x5a4634, 0x463224));
  add("pine_log_top", logTopTex(0xc2a766));
  add("pine_log_side", logSideTex(0xb8924a));
  add("dusk_log_top", logTopTex(0x6a5236));
  add("dusk_log_side", logSideTex(0x4a3a2c));
  add("pine_leaves", leavesTex(0x7a9a4a, 0x6a8a3e));
  add("dusk_leaves", leavesTex(0x3a5a3a, 0x2e4a2e));
  add("birch_planks", plankTex(0xd8c9a2, 0xb3a276));
  add("birch_log_top", logTopTex(0xd9cfae));
  // Birch bark: chalk-white with the characteristic dark horizontal scores.
  add("birch_log_side", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0xe4dfd2), 8, rng);
    for (int i = 0; i < 9; ++i) {
      const int y = static_cast<int>(rng.next() * T);
      const int x = static_cast<int>(rng.next() * T);
      const int w = 2 + static_cast<int>(rng.next() * 3);
      for (int k = 0; k < w; ++k) px(img, ox, oy, (x + k) % T, y, 52, 48, 42);
    }
  });
  add("birch_leaves", leavesTex(0x8fb055, 0x7a9c44));
  add("palm_planks", plankTex(0xc9a06a, 0xa37c46));
  add("palm_log_top", logTopTex(0xc2a06a));
  // Palm trunk: stacked frond-scar rings instead of vertical grain.
  add("palm_log_side", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0xa3855a), 10, rng);
    for (int y = 2; y < T; y += 4) {
      for (int x = 0; x < T; ++x) {
        px(img, ox, oy, x, y, 130, 104, 66);
        if (rng.next() < 0.5) px(img, ox, oy, x, y + 1, 148, 120, 78);
      }
    }
  });
  add("palm_leaves", leavesTex(0x4fae4a, 0x3f9440));

  add("pine_door", doorTex(0xc2a05a, 0x7a5e2e, T));
  add("pine_door_upper", doorTex(0xc2a05a, 0x7a5e2e, 0));
  add("dusk_door", doorTex(0x5a4634, 0x33271a, T));
  add("dusk_door_upper", doorTex(0x5a4634, 0x33271a, 0));
  add("pine_trapdoor", trapdoorTex(0xc2a05a, 0x7a5e2e));
  add("dusk_trapdoor", trapdoorTex(0x5a4634, 0x33271a));
  add("birch_door", doorTex(0xd8c9a2, 0x8f8058, T));
  add("birch_door_upper", doorTex(0xd8c9a2, 0x8f8058, 0));
  add("palm_door", doorTex(0xc9a06a, 0x7c5c32, T));
  add("palm_door_upper", doorTex(0xc9a06a, 0x7c5c32, 0));
  add("birch_trapdoor", trapdoorTex(0xd8c9a2, 0x8f8058));
  add("palm_trapdoor", trapdoorTex(0xc9a06a, 0x7c5c32));

  // ---- snow ----
  add("snow_top", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0xeef2f6), 6, rng);
    blobs(img, ox, oy, hex(0xdde6ee), 8, rng, 1);
  });
  add("snowturf_side", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x6b4b32), 20, rng);
    for (int x = 0; x < T; ++x) {
      const int h = 3 + (rng.next() < 0.5 ? 1 : 0);
      for (int y = 0; y < h; ++y) {
        const double j = (rng.next() * 2 - 1) * 8;
        px(img, ox, oy, x, y, 236 + j, 240 + j, 246 + j);
      }
    }
  });

  // ---- soul anchor: a dressed-stone hearth ------------------------------------
  //
  // Where you come home to, so it is built like home: dressed grey stone of the
  // same family as the polished blocks, bound with a copper band, with embers
  // banked in a hearth in its top and a small arched niche in each side where the
  // fire shows through. It used to be near-black glass with a teal core and a
  // channel of teal light, a material nothing else in the world is made of, and
  // it read as a portal from a different game. The glow stays — it is how you find
  // the thing in the dark — but warm, the colour a hearth is.
  add("soul_anchor_top", [](Image& img, int ox, int oy, Mulberry32& rng) {
    polishedInto(img, ox, oy, rng, hex(0x8a8c94));
    for (int y = 3; y <= 12; ++y) {  // the hearth: a dark recess...
      for (int x = 3; x <= 12; ++x) px(img, ox, oy, x, y, 34, 30, 28);
    }
    for (int i = 3; i <= 12; ++i) {  // ...with a lip, lit on the far side
      px(img, ox, oy, i, 3, 58, 54, 52);
      px(img, ox, oy, 3, i, 58, 54, 52);
      px(img, ox, oy, i, 12, 108, 110, 118);
      px(img, ox, oy, 12, i, 108, 110, 118);
    }
    for (int y = 5; y <= 10; ++y) {  // embers, hottest at the heart
      for (int x = 5; x <= 10; ++x) {
        const double heat = std::max(0.0, 1.0 - std::hypot(x - 7.5, y - 7.5) / 3.6);
        if (rng.next() < 0.35 + heat * 0.6) {
          px(img, ox, oy, x, y, 170 + heat * 85, 62 + heat * 130, 20 + heat * 50);
        } else {
          px(img, ox, oy, x, y, 52, 40, 34);  // a coal gone dark
        }
      }
    }
    for (int c : {1, 14}) {  // copper studs at the corners
      px(img, ox, oy, c, 1, 196, 122, 70);
      px(img, ox, oy, c, 14, 168, 98, 54);
      px(img, ox, oy, 1, c, 196, 122, 70);
      px(img, ox, oy, 14, c, 168, 98, 54);
    }
  });
  add("soul_anchor_side", [](Image& img, int ox, int oy, Mulberry32& rng) {
    polishedInto(img, ox, oy, rng, hex(0x8a8c94));
    for (int x = 0; x < T; ++x) {  // the copper band
      px(img, ox, oy, x, 3, 204, 130, 76);
      px(img, ox, oy, x, 4, 168, 98, 54);
    }
    for (int x = 1; x < T; x += 4) px(img, ox, oy, x, 3, 232, 170, 110);  // rivets
    // An arched niche with the fire showing at its foot.
    for (int y = 7; y <= 13; ++y) {
      for (int x = 6; x <= 9; ++x) {
        if (y == 7 && (x == 6 || x == 9)) continue;  // the arch
        px(img, ox, oy, x, y, 30, 26, 24);
      }
    }
    for (int x = 6; x <= 9; ++x) {
      const double f = rng.next();
      px(img, ox, oy, x, 13, 220 + f * 30, 120 + f * 60, 40 + f * 20);
    }
    px(img, ox, oy, 7, 12, 250, 190, 90);
    px(img, ox, oy, 8, 12, 236, 150, 60);
    for (int x = 5; x <= 10; ++x) px(img, ox, oy, x, 14, 108, 110, 118);  // the sill
  });

  // ---- evil altar: the soul anchor's opposite number --------------------------
  //
  // Near-black stone with something burning inside it, caged — the anchor's dark
  // mirror, and meant to look foreign where the anchor looks like home. The
  // anchor was once built this way too, in teal; the altar is now the only block
  // that is. It emits no light at
  // all (a lit altar would stop monsters spawning around it), so every bit of the
  // glow has to be painted: the bars are drawn lighter on the side facing the core
  // and darker away from it, which is what sells an interior light source on a
  // texture that never actually contributes one.
  add("evil_altar_side", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x16121a), 8, rng);
    // Interior glow, brightest at the middle and gone by the frame.
    for (int y = 1; y < T - 1; ++y) {
      for (int x = 1; x < T - 1; ++x) {
        const double d = std::hypot(x - 7.5, y - 7.5);
        if (d > 6.5) continue;
        const double f = 1.0 - d / 6.5;
        px(img, ox, oy, x, y, 22 + 150 * f * f, 18 + 44 * f * f, 26 + 30 * f * f);
      }
    }
    // The ember at the heart of it.
    static constexpr int kCore[][2] = {{7, 7}, {8, 7}, {7, 8}, {8, 8}};
    for (const auto& p : kCore) px(img, ox, oy, p[0], p[1], 248, 158, 96);
    px(img, ox, oy, 7, 6, 226, 96, 52);
    px(img, ox, oy, 8, 9, 226, 96, 52);
    // Cage: four bars, each with a lit edge on the side the ember is.
    for (const int bx : {2, 6, 9, 13}) {
      for (int y = 1; y < T - 1; ++y) {
        px(img, ox, oy, bx, y, 44, 38, 50);
        px(img, ox, oy, bx + (bx < 8 ? 1 : -1), y, 72, 58, 62);
      }
    }
    // Frame, so stacked altars read as separate blocks.
    for (int i = 0; i < T; ++i) {
      px(img, ox, oy, i, 0, 58, 50, 62);
      px(img, ox, oy, i, 15, 12, 10, 14);
      px(img, ox, oy, 0, i, 48, 42, 54);
      px(img, ox, oy, 15, i, 20, 17, 23);
    }
  });
  add("evil_altar_top", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x16121a), 8, rng);
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        const double d = std::hypot(x - 7.5, y - 7.5);
        if (d < 1.9) {
          px(img, ox, oy, x, y, 250, 176, 112);  // the shaft down into the fire
        } else if (d < 3.6 && rng.next() < 0.85) {
          px(img, ox, oy, x, y, 186, 62, 44);
        } else if (std::abs(d - 5.6) < 0.7) {
          px(img, ox, oy, x, y, 96, 30, 34);  // a scored ring around the mouth
        }
      }
    }
    // Four notches on the ring, at the compass points, so the top has a facing.
    static constexpr int kNotches[][2] = {{7, 1}, {8, 14}, {1, 8}, {14, 7}};
    for (const auto& p : kNotches) px(img, ox, oy, p[0], p[1], 150, 48, 46);
    for (int i = 0; i < T; ++i) {
      px(img, ox, oy, i, 0, 52, 44, 56);
      px(img, ox, oy, 0, i, 52, 44, 56);
    }
  });

  // ---- papyrus ----
  add("papyrus_stem", [](Image& img, int ox, int oy, Mulberry32& rng) {
    for (const auto& stem : kPapyrusStems) {
      for (int y = 0; y < T; ++y) papyrusStemPx(img, ox, oy, rng, stem[0], y);
    }
  });
  add("papyrus", [](Image& img, int ox, int oy, Mulberry32& rng) {
    for (const auto& stem : kPapyrusStems) {
      const int sx = stem[0], top = stem[1];
      for (int y = top; y < T; ++y) papyrusStemPx(img, ox, oy, rng, sx, y);
      // Umbel: a little starburst of lighter fronds at the tip.
      const int frond[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-2, 0},
                               {2, 0},   {-1, 0}, {1, 0},  {0, -2}};
      for (const auto& f : frond) {
        const int yy = top + f[1], xx = sx + f[0];
        if (yy >= 0 && xx >= 0 && xx < T) px(img, ox, oy, xx, yy, 150, 190, 96);
      }
    }
  });

  // ---- ores: greystone base plus coloured flecks ----
  add("ore_embercoal", oreTexture(0x1d1d22, 9));
  add("ore_copper", oreTexture(0xc8783a, 9));
  add("ore_ferralite", oreTexture(0xd9cdb8, 9));
  add("ore_sunbrass", oreTexture(0xe8c64a, 8));
  add("ore_aetherite", oreTexture(0x46d8c4, 8));
  add("ore_sparkstone", oreTexture(0xe0432f, 9));
  add("ore_azurite", oreTexture(0x2f6fe0, 9));
  add("ore_gloamite", oreTexture(0x8a52e8, 8));
  add("ore_verdanite", oreTexture(0x46b558, 9));

  // ---- plants and greebles ----
  add("tall_grass", bladeTex(0x4f9438, 0x5da844, {8, 7, 6, 0.22}));
  add("fern", bladeTex(0x3f7a4a, 0x4f8f52, {9, 8, 6, 0.4}));
  add("bush", bushTex(0x3f7a32, 0x356b2a));
  add("dead_shrub", deadBushTex);
  add("pebbles", pebblesTex);
  add("mushroom_red", mushroomTex(0xc23a2f, true));
  add("mushroom_brown", mushroomTex(0x9c7350, false));
  add("flower_poppy", flowerTex(0x3f7a32, 0xd23a34, 0x241a12));
  add("flower_daisy", flowerTex(0x3f7a32, 0xf0f0ea, 0xecc24a));
  add("flower_cornflower", flowerTex(0x3f7a32, 0x4a6fe0, 0x2a3f8a));
  add("flower_dandelion", flowerTex(0x3f7a32, 0xf2c53a, 0xc99a24));
  add("flower_violet", flowerTex(0x3f7a32, 0x9a5ac2, 0xf2c53a));
  // The Dye update's three, filling the gaps between the primaries the first five
  // cover. flowerTex is already parametric, so a new species is a row and two hex
  // colours rather than a sprite — the same bargain the crop painters made.
  add("flower_marigold", flowerTex(0x3f7a32, 0xe8862a, 0xb4551a));
  add("flower_fernflower", flowerTex(0x3f7a32, 0x4fae53, 0x2f7a38));
  // A dark bloom with a pale eye, or it reads as a hole in the ground rather than a
  // flower. Black dye has to come from something you can actually see growing.
  add("flower_nightcap", flowerTex(0x3a6b2e, 0x2a2333, 0x8d86a0));

  // ---- crops: eighteen, four stages each, from the five families above ----
  //
  // The colours live here and the block properties live in blocks.cpp, joined by
  // the key — which is how the flowers and plants above already work, and is why
  // there is no shared crop table: painters are in resource/ and blocks are in
  // world/, so one table read by both would point a lower layer at a higher one.
  //
  // The cost of the split is that the two lists can drift. testCropArt() in the
  // self-test walks every crop block and demands a painter for every stage, which
  // is a cheaper guard than the layering violation would have been.
  {
    enum Family { Stalk, Leafy, Vine, Berry, Head, Pod };
    struct CropTex {
      const char* key;
      Family family;
      std::uint32_t c1, c2, c3;  // family-dependent; c3 unused by Vine/Berry/Head
    };
    static constexpr CropTex kCrops[] = {
        // grains: stalk colour, highlight, ripe head
        {"wheat", Stalk, 0x9caf46, 0xb4c455, 0xe0c65a},
        {"barley", Stalk, 0x8fa055, 0xa8b566, 0xd9c78a},
        {"rice", Stalk, 0x7fae5a, 0x93c06a, 0xe8e4c8},
        {"maize", Stalk, 0x5f9440, 0x74a851, 0xf0c433},
        // roots: leaf, leaf highlight, the crown that shows when ripe
        {"carrot", Leafy, 0x3f8a3a, 0x4f9e46, 0xe07a28},
        {"potato", Leafy, 0x4a8a44, 0x5a9c52, 0xc9a468},
        {"onion", Leafy, 0x6a9c50, 0x7cae60, 0xd8c9a2},
        {"beetroot", Leafy, 0x7a4a52, 0x8f5a60, 0xa0243c},
        {"garlic", Leafy, 0x6f9a58, 0x82ac68, 0xeae2d2},
        // ground fruit: vine, fruit
        {"pumpkin", Vine, 0x4a8a3a, 0xe0821e, 0},
        // The melon's rind is a PALE yellow-green on purpose. At 0x6fae3a it was
        // within a shade of its own vine and the fruit simply could not be seen.
        {"melon", Vine, 0x4f9440, 0xc2d95e, 0},
        {"tomato", Vine, 0x4a8a44, 0xd8392c, 0},
        {"chili", Pod, 0x53923f, 0xd42f24, 0},
        // bushes: leaf, berry
        {"strawberry", Berry, 0x3f8a3a, 0xd8323c, 0},
        {"blueberry", Berry, 0x4a7a4a, 0x4a5ac2, 0},
        {"grapes", Berry, 0x5a8a3f, 0x7a4ab0, 0},
        // heads: outer leaf, firm heart
        {"cabbage", Head, 0x6faa5a, 0xc6dca0, 0},
        // Podded, not a head: as a Head it was a green ball beside the cabbage's
        // green ball, and the two were the same crop as far as anyone could tell.
        {"soybean", Pod, 0x8aa84a, 0xd8cf7a, 0},
    };
    for (const CropTex& c : kCrops) {
      for (int s = 0; s < kCropStages; ++s) {
        std::string name = std::string("crop_") + c.key + "_" + std::to_string(s);
        switch (c.family) {
          case Stalk: add(std::move(name), cropStalkTex(s, c.c1, c.c2, c.c3)); break;
          case Leafy: add(std::move(name), cropLeafyTex(s, c.c1, c.c2, c.c3)); break;
          case Vine: add(std::move(name), cropVineTex(s, c.c1, c.c2)); break;
          case Berry: add(std::move(name), cropBerryTex(s, c.c1, c.c2)); break;
          case Head: add(std::move(name), cropHeadTex(s, c.c1, c.c2)); break;
          case Pod: add(std::move(name), cropPodTex(s, c.c1, c.c2)); break;
        }
      }
    }
  }

  // Fertilised soil: the damp tile, darkened, with green flecks worked through it.
  // It has to be tellable from plain farmland at a glance and across a whole field,
  // which is why the flecks are scattered rather than a border.
  // Fertilised AND watered: the enriched tile taken darker still, so a field reads
  // at a glance as one of four states rather than two. The green flecks stay bright
  // against it, which is what keeps it saying "fertilised" and not merely "wet".
  add("farmland_rich_wet", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x2e2013), 12, rng);
    for (int i = 0; i < 26; ++i) {
      const int x = static_cast<int>(rng.next() * T);
      const int y = static_cast<int>(rng.next() * T);
      const double f = rng.next();
      px(img, ox, oy, x, y, 62 + f * 36, 102 + f * 42, 44 + f * 24);
    }
    for (int x = 0; x < T; ++x) {
      px(img, ox, oy, x, 4, 40, 28, 17);
      px(img, ox, oy, x, 11, 40, 28, 17);
    }
  });

  add("farmland_rich", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x4a3320), 14, rng);
    for (int i = 0; i < 26; ++i) {
      const int x = static_cast<int>(rng.next() * T);
      const int y = static_cast<int>(rng.next() * T);
      const double f = rng.next();
      px(img, ox, oy, x, y, 70 + f * 40, 112 + f * 46, 52 + f * 26);
    }
    for (int x = 0; x < T; ++x) {  // the furrows, as on plain farmland
      px(img, ox, oy, x, 4, 58, 42, 26);
      px(img, ox, oy, x, 11, 58, 42, 26);
    }
  });

  // ---- the kitchen ----
  add("cutting_board_top", [](Image& img, int ox, int oy, Mulberry32& rng) {
    plankTexInto(img, ox, oy, rng, hex(0xc2a068), hex(0x9c7c46));
    for (int i = 0; i < 7; ++i) {  // knife scars across the grain
      const int x = 2 + static_cast<int>(rng.next() * 12);
      const int y = 3 + static_cast<int>(rng.next() * 10);
      px(img, ox, oy, x, y, 226, 214, 186);
      if (rng.next() < 0.6) px(img, ox, oy, x + 1, y, 214, 200, 172);
    }
  });
  add("cutting_board_side", plankTex(0xa8874e, 0x86682f));

  add("stove_top", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x4a4a52), 12, rng);
    for (int i = 0; i < 2; ++i) {  // two hotplates
      const int cx = 4 + i * 7, cy = 8;
      for (int y = -3; y <= 3; ++y) {
        for (int x = -3; x <= 3; ++x) {
          if (x * x + y * y > 9) continue;
          px(img, ox, oy, cx + x, cy + y, 32, 30, 34);
        }
      }
    }
  });
  // The stove's front is its oven door: a glowing window and a handle. Its sides are
  // riveted plate and its back carries the flue.
  add("stove_front", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x54545c), 12, rng);
    for (int y = 4; y <= 14; ++y) {  // the door
      for (int x = 2; x <= 13; ++x) {
        const bool rim = y == 4 || y == 14 || x == 2 || x == 13;
        if (rim) px(img, ox, oy, x, y, y == 4 || x == 2 ? 112 : 46, y == 4 || x == 2 ? 112 : 46,
                    y == 4 || x == 2 ? 120 : 52);
      }
    }
    for (int y = 8; y <= 12; ++y) {  // the window onto the fire
      for (int x = 4; x <= 11; ++x) {
        const double f = rng.next();
        px(img, ox, oy, x, y, 200 + f * 40, 90 + f * 60, 30 + f * 30);
      }
    }
    for (int x = 5; x <= 10; ++x) px(img, ox, oy, x, 6, 150, 152, 160);  // the handle
    px(img, ox, oy, 5, 7, 96, 98, 106);
    px(img, ox, oy, 10, 7, 96, 98, 106);
    for (int x = 2; x <= 13; x += 3) px(img, ox, oy, x, 1, 38, 36, 40);  // vent slots
  });
  add("stove_side", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x54545c), 12, rng);
    for (int i = 0; i < T; ++i) {
      px(img, ox, oy, i, 0, 108, 108, 116);
      px(img, ox, oy, i, 15, 40, 40, 46);
    }
    for (int x : {2, 13}) {
      for (int y : {2, 13}) px(img, ox, oy, x, y, 132, 134, 142);  // rivets
    }
  });
  add("stove_back", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x54545c), 12, rng);
    for (int y = 0; y <= 10; ++y) {  // the flue, up the back and out
      for (int x = 6; x <= 9; ++x) {
        px(img, ox, oy, x, y, x == 6 ? 84 : 56, x == 6 ? 84 : 56, x == 6 ? 92 : 62);
      }
    }
    for (int x = 5; x <= 10; ++x) px(img, ox, oy, x, 10, 40, 40, 46);  // its collar
  });

  add("cooking_pot_top", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x3a3a42), 10, rng);
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        if (std::hypot(x - 7.5, y - 7.5) > 5.5) continue;
        const double f = rng.next();
        px(img, ox, oy, x, y, 150 + f * 30, 96 + f * 24, 46 + f * 18);  // stew
      }
    }
  });
  add("cooking_pot_side", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x3a3a42), 10, rng);
    for (int x = 1; x < T - 1; ++x) px(img, ox, oy, x, 3, 88, 88, 96);       // rim
    for (int x = 3; x < T - 3; ++x) px(img, ox, oy, x, T - 2, 70, 70, 78);   // foot
  });

  // Tilled soil. Two tiles: dry, and the darker damp one within reach of water.
  add("farmland", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x6b4f33), 16, rng);
    for (int i = 0; i < 3; ++i) {  // furrows
      const int y = 3 + i * 5;
      for (int x = 0; x < T; ++x) {
        const double j = (rng.next() * 2 - 1) * 8;
        px(img, ox, oy, x, y, 88 + j, 66 + j, 44 + j);
      }
    }
  });
  add("farmland_wet", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x452f1d), 14, rng);
    for (int i = 0; i < 3; ++i) {
      const int y = 3 + i * 5;
      for (int x = 0; x < T; ++x) {
        const double j = (rng.next() * 2 - 1) * 7;
        px(img, ox, oy, x, y, 60 + j, 42 + j, 26 + j);
      }
    }
  });

  // ---- mob surfaces ----
  //
  // Greyscale, and multiplied by each box's own colour in the entity shader, so one
  // tile serves every mob that wears that kind of surface: the sheep's wool and the
  // cow's hide are the same grey detail in different colours. They sit near white
  // (the mean is about 0.9) so a mob keeps the colour it was designed in and gains
  // the texture on top, rather than coming out darker. Mobs were flat colour until
  // these, the one thing in the world with no texture at all.
  //
  // Named under entity/ rather than block/ so a resource pack finds them where it
  // would look for a mob's skin.
  auto addEntity = [&out](std::string name, PainterFn fn) {
    ResourceId id(std::string("entity/") + name);
    out.push_back({id, "entity_" + name, std::move(fn)});
  };
  // Eyes, noses, horns: parts too small to show a texture, which still need a UV
  // that points at something.
  addEntity("plain", [](Image& img, int ox, int oy, Mulberry32&) {
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) px(img, ox, oy, x, y, 255, 255, 255);
    }
  });
  // Skin and short hair: a soft mottle.
  addEntity("hide", [](Image& img, int ox, int oy, Mulberry32& rng) {
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        const double v = 232 + (rng.next() * 2 - 1) * 14;
        px(img, ox, oy, x, y, v, v, v);
      }
    }
    for (int i = 0; i < 10; ++i) {  // a few darker hairs
      const int x = static_cast<int>(rng.next() * T), y = static_cast<int>(rng.next() * (T - 1));
      px(img, ox, oy, x, y, 206, 206, 206);
      px(img, ox, oy, x, y + 1, 214, 214, 214);
    }
  });
  // Fleece: tight curls, each a lit crown over a shadowed underside.
  addEntity("wool", [](Image& img, int ox, int oy, Mulberry32& rng) {
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        const double v = 226 + (rng.next() * 2 - 1) * 8;
        px(img, ox, oy, x, y, v, v, v);
      }
    }
    for (int i = 0; i < 22; ++i) {
      const int cx = static_cast<int>(rng.next() * T), cy = static_cast<int>(rng.next() * T);
      px(img, ox, oy, cx, cy, 255, 255, 255);
      px(img, ox, oy, (cx + 1) % T, cy, 246, 246, 246);
      px(img, ox, oy, cx, (cy + 1) % T, 196, 196, 196);
      px(img, ox, oy, (cx + 1) % T, (cy + 1) % T, 206, 206, 206);
    }
  });
  // Woven cloth for shirts and trousers: a fine twill.
  addEntity("cloth", [](Image& img, int ox, int oy, Mulberry32& rng) {
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        const bool thread = (x + y) % 3 == 0;
        const double v = (thread ? 214 : 238) + (rng.next() * 2 - 1) * 6;
        px(img, ox, oy, x, y, v, v, v);
      }
    }
  });
  // Planking for the boat: boards with seams, grain along their length.
  addEntity("grain", [](Image& img, int ox, int oy, Mulberry32& rng) {
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        double v = 234 + (rng.next() * 2 - 1) * 8;
        if ((y * 5 + x / 5) % 7 == 0) v -= 20;  // grain
        if (y % 4 == 3) v = 176;                // seam between boards
        px(img, ox, oy, x, y, v, v, v);
      }
    }
  });

  return out;
}

}  // namespace

const std::vector<PainterEntry>& builtinPainters() {
  static const std::vector<PainterEntry> painters = buildPainters();
  return painters;
}

std::vector<ResourceId> builtinEntityTextureIds() {
  std::vector<ResourceId> ids;
  for (const PainterEntry& e : builtinPainters()) {
    if (e.id.path().rfind("entity/", 0) == 0) ids.push_back(e.id);
  }
  return ids;
}

const PainterEntry* findPainter(const ResourceId& id) {
  static const std::unordered_map<ResourceId, const PainterEntry*> index = [] {
    std::unordered_map<ResourceId, const PainterEntry*> map;
    for (const PainterEntry& e : builtinPainters()) map.emplace(e.id, &e);
    return map;
  }();
  auto it = index.find(id);
  return it == index.end() ? nullptr : it->second;
}

Image paintTile(const PainterEntry& entry) {
  Image tile(kPainterTile, kPainterTile);
  // Seeded from the bare texture name, matching the web build's
  // painter(ctx, ox, oy, mulberry32(hashSeed(name))).
  Mulberry32 rng(hashSeed(entry.seedName));
  entry.fn(tile, 0, 0, rng);
  return tile;
}

}  // namespace hr::resource
