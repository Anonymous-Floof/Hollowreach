#include "resource/painters.h"

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

PainterFn polishedTex(std::uint32_t base) {
  const Rgb3 c = hex(base);
  return [c](Image& img, int ox, int oy, Mulberry32& rng) {
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        const double k = 1 - y * 0.018 + (rng.next() * 2 - 1) * 0.04;
        px(img, ox, oy, x, y, c.r * k, c.g * k, c.b * k);
      }
    }
    for (int i = 0; i < T; ++i) {  // top highlight
      px(img, ox, oy, i, 0, c.r * 1.25, c.g * 1.25, c.b * 1.25);
    }
  };
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

// Wood doors and trapdoors: the wood's planks plus a frame, bevelled panels and
// hardware. The bevel — dark top and left, light bottom and right — makes the
// panels read as recessed rather than merely outlined.
PainterFn doorTex(std::uint32_t base, std::uint32_t line) {
  const Rgb3 b = hex(base), l = hex(line);
  // The nested plank fill is seeded from the base colour's *hex string* plus a
  // suffix, exactly as the JS concatenated it.
  char seedBuf[16];
  std::snprintf(seedBuf, sizeof(seedBuf), "#%06x", base);
  const std::string plankSeed = std::string(seedBuf) + "door";

  return [b, l, plankSeed](Image& img, int ox, int oy, Mulberry32& rng) {
    Mulberry32 inner = seeded(plankSeed);
    plankTexInto(img, ox, oy, inner, b, l);

    for (int i = 0; i < T; ++i) {
      px(img, ox, oy, 0, i, l.r, l.g, l.b);
      px(img, ox, oy, T - 1, i, l.r, l.g, l.b);
      px(img, ox, oy, i, 0, l.r, l.g, l.b);
      px(img, ox, oy, i, T - 1, l.r, l.g, l.b);
    }

    auto panel = [&](int y0, int y1) {
      for (int y = y0; y <= y1; ++y) {
        for (int x = 3; x <= 12; ++x) {
          const double j = (rng.next() * 2 - 1) * 8;
          px(img, ox, oy, x, y, b.r * 0.88 + j, b.g * 0.88 + j, b.b * 0.88 + j);
        }
      }
      for (int x = 3; x <= 12; ++x) {
        px(img, ox, oy, x, y0, b.r * 0.58, b.g * 0.58, b.b * 0.58);
        px(img, ox, oy, x, y1, b.r * 1.18, b.g * 1.18, b.b * 1.18);
      }
      for (int y = y0; y <= y1; ++y) {
        px(img, ox, oy, 3, y, b.r * 0.62, b.g * 0.62, b.b * 0.62);
        px(img, ox, oy, 12, y, b.r * 1.14, b.g * 1.14, b.b * 1.14);
      }
    };
    panel(2, 6);
    panel(9, 13);

    // Brass handle with a shadow pixel so it sits proud of the door.
    px(img, ox, oy, T - 5, 10, 240, 208, 110);
    px(img, ox, oy, T - 5, 11, 214, 178, 84);
    px(img, ox, oy, T - 4, 11, b.r * 0.5, b.g * 0.5, b.b * 0.5);
  };
}

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

// A scatter of small stones hugging the ground.
void pebblesTex(Image& img, int ox, int oy, Mulberry32& rng) {
  const Rgb3 cols[3] = {hex(0x8a8f96), hex(0x6f747b), hex(0xa2a7ad)};
  for (int i = 0; i < 5; ++i) {
    const int bx = 2 + static_cast<int>(rng.next() * (T - 5));
    const int by = T - 4 + static_cast<int>(rng.next() * 3);
    const int s = 2 + static_cast<int>(rng.next() * 2);
    const Rgb3 c = cols[static_cast<int>(rng.next() * 3)];
    for (int y = 0; y < s; ++y) {
      for (int x = 0; x < s + 1; ++x) {
        const double j = (rng.next() * 2 - 1) * 14;
        const int yy = by + y < T - 1 ? by + y : T - 1;
        px(img, ox, oy, (bx + x) % T, yy, c.r + j, c.g + j, c.b + j);
      }
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
  add("polished", [](Image& img, int ox, int oy, Mulberry32& rng) {
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        const double v = 132 - y * 2 + (rng.next() * 2 - 1) * 6;
        px(img, ox, oy, x, y, v, v + 2, v + 8);
      }
    }
    for (int i = 0; i < T; ++i) px(img, ox, oy, i, 0, 160, 164, 172);
  });
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
      px(img, ox, oy, i, 0, 200, 226, 232);
      px(img, ox, oy, i, T - 1, 190, 216, 224);
      px(img, ox, oy, 0, i, 200, 226, 232);
      px(img, ox, oy, T - 1, i, 190, 216, 224);
    }
    px(img, ox, oy, 1, 1, 236, 247, 251);
    px(img, ox, oy, 14, 1, 224, 240, 246);
    px(img, ox, oy, 1, 14, 224, 240, 246);
    px(img, ox, oy, 14, 14, 210, 232, 240);
    for (int i = 2; i < 8; ++i) px(img, ox, oy, i, i, 235, 246, 250);
    for (int i = 4; i < 8; ++i) px(img, ox, oy, i - 1, i + 2, 224, 240, 246);
    for (int i = 10; i < 13; ++i) px(img, ox, oy, i, i, 218, 236, 244);
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
  add("workbench_side", [](Image& img, int ox, int oy, Mulberry32& rng) {
    Mulberry32 inner = seeded("planks2");
    planksInto(img, ox, oy, inner);
    (void)rng;
    // Framed panel with a saw and a hammer hung on it.
    for (int i = 0; i < T; ++i) {
      px(img, ox, oy, i, 0, 84, 62, 36);
      px(img, ox, oy, i, 15, 74, 54, 30);
      px(img, ox, oy, 0, i, 84, 62, 36);
      px(img, ox, oy, 15, i, 84, 62, 36);
    }
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
  add("forge_side", [](Image& img, int ox, int oy, Mulberry32& rng) {
    // Mortared stone body with an arched, lintel-topped firebox. The fire is
    // layered bottom-up: coal bed, orange body, yellow tongues, hot core.
    noisy(img, ox, oy, hex(0x7a7e86), 12, rng);
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        const int row = y / 4, offset = row % 2 == 0 ? 0 : 4;
        if (y % 4 == 0 || (x + offset) % 8 == 0) px(img, ox, oy, x, y, 88, 90, 96);
      }
    }
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
  add("chest_side", [](Image& img, int ox, int oy, Mulberry32& rng) {
    // Horizontal boards, a deep lid seam, iron corner brackets, latch with keyhole.
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
  add("door", doorTex(0xb08a52, 0x6e5230));

  add("bed_head_top", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0xb5443a), 12, rng);  // red blanket base
    blobs(img, ox, oy, hex(0xa03c33), 6, rng, 1);
    for (int x = 0; x < T; ++x) {  // headboard rail
      px(img, ox, oy, x, 0, 124, 74, 42);
      px(img, ox, oy, x, 1, 96, 58, 34);
    }
    // Plump pillow: dim rounded edge, bright centre, a stitched highlight.
    for (int y = 2; y <= 6; ++y) {
      for (int x = 2; x <= 13; ++x) {
        const bool edge = y == 2 || y == 6 || x == 2 || x == 13;
        const double j = (rng.next() * 2 - 1) * 6;
        if (edge) {
          px(img, ox, oy, x, y, 202 + j, 200 + j, 190 + j);
        } else {
          px(img, ox, oy, x, y, 238 + j, 236 + j, 226 + j);
        }
      }
    }
    px(img, ox, oy, 4, 3, 250, 249, 242);
    px(img, ox, oy, 5, 3, 250, 249, 242);
    for (int x = 0; x < T; ++x) {  // blanket folded below the pillow
      px(img, ox, oy, x, 8, 150, 58, 50);
      px(img, ox, oy, x, 9, 128, 46, 40);
    }
  });
  add("bed_foot_top", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0xb5443a), 10, rng);  // red quilt
    for (int y = 0; y < T; ++y) {                // diagonal quilt stitching
      for (int x = 0; x < T; ++x) {
        if ((x + y) % 6 == 0 || (x - y + 32) % 6 == 0) px(img, ox, oy, x, y, 150, 58, 50);
      }
    }
    for (int x = 0; x < T; ++x) {  // tucked white sheet at the foot end
      px(img, ox, oy, x, 13, 128, 46, 40);
      px(img, ox, oy, x, 14, 222, 218, 206);
      px(img, ox, oy, x, 15, 192, 188, 176);
    }
  });
  add("bed_side", [](Image& img, int ox, int oy, Mulberry32& rng) {
    Mulberry32 inner = seeded("bedside");
    planksInto(img, ox, oy, inner);  // wood frame at the bottom
    // Draped blanket with a lit top edge and a shadowed hem.
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < T; ++x) {
        const double j = (rng.next() * 2 - 1) * 10;
        px(img, ox, oy, x, y, 181 + j, 68 + j, 58 + j);
      }
    }
    for (int x = 0; x < T; ++x) {
      px(img, ox, oy, x, 0, 205, 86, 74);
      px(img, ox, oy, x, 7, 138, 50, 44);
      px(img, ox, oy, x, 8, 224, 220, 208);  // white sheet peeking out
      px(img, ox, oy, x, 9, 128, 88, 48);    // frame rail
    }
    for (int y = 12; y < T; ++y) {  // shadowed gap between stout legs
      for (int x = 3; x <= 12; ++x) px(img, ox, oy, x, y, 34, 28, 22);
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

  add("pine_door", doorTex(0xc2a05a, 0x7a5e2e));
  add("dusk_door", doorTex(0x5a4634, 0x33271a));
  add("pine_trapdoor", trapdoorTex(0xc2a05a, 0x7a5e2e));
  add("dusk_trapdoor", trapdoorTex(0x5a4634, 0x33271a));
  add("birch_door", doorTex(0xd8c9a2, 0x8f8058));
  add("palm_door", doorTex(0xc9a06a, 0x7c5c32));
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

  // ---- soul anchor: night-dark stone shot through with a glowing teal core ----
  add("soul_anchor_top", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x2c2f3a), 10, rng);
    for (int y = 0; y < T; ++y) {
      for (int x = 0; x < T; ++x) {
        const double d = std::hypot(x - 7.5, y - 7.5);
        if (d < 2.4) {
          px(img, ox, oy, x, y, 150, 240, 226);  // hot core
        } else if (d < 4.2 && rng.next() < 0.8) {
          px(img, ox, oy, x, y, 74, 178, 168);
        } else if (std::abs(d - 6.2) < 0.7) {
          px(img, ox, oy, x, y, 52, 118, 116);  // faint ring
        }
      }
    }
    for (int i = 0; i < T; ++i) {
      px(img, ox, oy, i, 0, 60, 64, 78);
      px(img, ox, oy, 0, i, 60, 64, 78);
    }
  });
  add("soul_anchor_side", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x2c2f3a), 10, rng);
    for (int i = 0; i < T; ++i) {
      px(img, ox, oy, i, 0, 66, 70, 84);
      px(img, ox, oy, i, 15, 22, 24, 30);
    }
    for (int y = 3; y <= 13; ++y) {  // a rune-etched channel bleeding light
      px(img, ox, oy, 7, y, 74, 190, 178);
      px(img, ox, oy, 8, y, 96, 214, 200);
      if (y % 3 == 0) {
        px(img, ox, oy, 6, y, 58, 142, 136);
        px(img, ox, oy, 9, y, 58, 142, 136);
      }
    }
    px(img, ox, oy, 7, 2, 150, 240, 226);
    px(img, ox, oy, 8, 2, 150, 240, 226);
  });

  // ---- evil altar: the soul anchor's opposite number --------------------------
  //
  // Same construction as the anchor — near-black stone with something burning
  // inside it — turned from teal to ember, and caged. The block emits no light at
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
  add("stove_side", [](Image& img, int ox, int oy, Mulberry32& rng) {
    noisy(img, ox, oy, hex(0x54545c), 12, rng);
    for (int y = 9; y <= 13; ++y) {  // the firebox, glowing
      for (int x = 4; x <= 11; ++x) {
        const double f = rng.next();
        px(img, ox, oy, x, y, 200 + f * 40, 90 + f * 60, 30 + f * 30);
      }
    }
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

  return out;
}

}  // namespace

const std::vector<PainterEntry>& builtinPainters() {
  static const std::vector<PainterEntry> painters = buildPainters();
  return painters;
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
