#include "ui/text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "core/log.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace hr::ui {
namespace {

// One shared atlas page. 4 MB of R8 holds every glyph the interface uses across
// every size several times over; there is no eviction because there is nothing to
// evict — the set of (face, size, codepoint) triples the stylesheet can produce is
// fixed and small.
constexpr int kAtlasSize = 2048;
constexpr int kGlyphPadding = 1;

struct FaceSpec {
  platform::FontFamily family;
  int weight;
  bool italic;
};

// The stylesheet's faces. Weight 800 is deliberate: CSS resolves it up to Black,
// which is what the browser draws the menu title with.
constexpr FaceSpec kFaceSpecs[] = {
    {platform::FontFamily::Sans, 400, false},
    {platform::FontFamily::Sans, 600, false},
    {platform::FontFamily::Sans, 700, false},
    {platform::FontFamily::Sans, 800, false},
    {platform::FontFamily::Sans, 400, true},
    {platform::FontFamily::Mono, 400, false},
};
static_assert(std::size(kFaceSpecs) == static_cast<std::size_t>(FontId::Count),
              "face table must cover every FontId");

std::vector<unsigned char> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size <= 0) return {};
  in.seekg(0, std::ios::beg);
  std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
  in.read(reinterpret_cast<char*>(bytes.data()), size);
  return bytes;
}

std::uint32_t toUpperCp(std::uint32_t cp) {
  // text-transform: uppercase is only applied to ASCII labels here.
  if (cp >= 'a' && cp <= 'z') return cp - 'a' + 'A';
  return cp;
}

}  // namespace

// A loaded TTF, plus its per-size glyph cache.
struct Text::Face {
  std::string path;
  std::vector<unsigned char> bytes;
  stbtt_fontinfo info {};
  bool loaded = false;
  int ascentU = 0, descentU = 0, lineGapU = 0;  // font units

  // Key packs the integer pixel size and the glyph index, so one map serves every
  // size a face is drawn at.
  std::unordered_map<std::uint64_t, Glyph> glyphs;

  static std::uint64_t key(int glyphIndex, int pixelSize) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(pixelSize)) << 32) |
           static_cast<std::uint32_t>(glyphIndex);
  }
};

std::uint32_t decodeUtf8(const std::string& s, std::size_t& i) {
  if (i >= s.size()) return 0;
  const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
  const unsigned char c = byte(i);
  if (c < 0x80) {
    ++i;
    return c;
  }
  int extra = 0;
  std::uint32_t cp = 0;
  if ((c & 0xE0) == 0xC0) {
    extra = 1;
    cp = c & 0x1Fu;
  } else if ((c & 0xF0) == 0xE0) {
    extra = 2;
    cp = c & 0x0Fu;
  } else if ((c & 0xF8) == 0xF0) {
    extra = 3;
    cp = c & 0x07u;
  } else {
    ++i;
    return 0xFFFDu;
  }
  if (i + static_cast<std::size_t>(extra) >= s.size()) {
    ++i;
    return 0xFFFDu;
  }
  for (int k = 1; k <= extra; ++k) {
    const unsigned char cc = byte(i + k);
    if ((cc & 0xC0) != 0x80) {
      ++i;
      return 0xFFFDu;
    }
    cp = (cp << 6) | (cc & 0x3Fu);
  }
  i += extra + 1;
  return cp;
}

void appendUtf8(std::string& s, std::uint32_t cp) {
  if (cp < 0x80) {
    s.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::size_t stepUtf8(const std::string& s, std::size_t byteIndex, int delta) {
  std::size_t i = std::min(byteIndex, s.size());
  while (delta > 0 && i < s.size()) {
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    --delta;
  }
  while (delta < 0 && i > 0) {
    --i;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    ++delta;
  }
  return i;
}

// Face is defined above, so the implicit member destructors can see a complete type.
// Declaring these out of line is what keeps every other translation unit from needing
// stb_truetype's 5000 lines.
Text::Text() = default;
Text::~Text() = default;

bool Text::init() {
  faces_.resize(static_cast<std::size_t>(FontId::Count));
  for (std::size_t i = 0; i < faces_.size(); ++i) {
    const FaceSpec& spec = kFaceSpecs[i];
    auto f = std::make_unique<Face>();
    f->path = platform::findFont({spec.family, spec.weight, spec.italic});
    if (!f->path.empty()) {
      f->bytes = readFile(f->path);
      if (!f->bytes.empty() &&
          stbtt_InitFont(&f->info, f->bytes.data(), stbtt_GetFontOffsetForIndex(f->bytes.data(), 0))) {
        f->loaded = true;
        stbtt_GetFontVMetrics(&f->info, &f->ascentU, &f->descentU, &f->lineGapU);
        missingFonts_ = false;
      } else {
        log::warn("font failed to parse: %s", f->path.c_str());
      }
    }
    faces_[i] = std::move(f);
  }

  if (missingFonts_) {
    log::error("no usable interface font found; searched:");
    for (const std::string& dir : platform::fontDirectories()) log::error("  %s", dir.c_str());
    return false;
  }
  // The interface still has to draw if only some weights exist, so a face that
  // failed borrows the regular one rather than dropping its text.
  for (std::size_t i = 0; i < faces_.size(); ++i) {
    if (faces_[i]->loaded) continue;
    log::warn("interface font missing for face %zu; substituting the regular weight", i);
  }

  for (const std::string& path : platform::fallbackFonts()) {
    auto f = std::make_unique<Face>();
    f->path = path;
    f->bytes = readFile(path);
    if (f->bytes.empty()) continue;
    if (!stbtt_InitFont(&f->info, f->bytes.data(),
                        stbtt_GetFontOffsetForIndex(f->bytes.data(), 0))) {
      continue;
    }
    f->loaded = true;
    stbtt_GetFontVMetrics(&f->info, &f->ascentU, &f->descentU, &f->lineGapU);
    fallbackFaces_.push_back(std::move(f));
  }

  atlas_.resize(kAtlasSize, kAtlasSize, Rgba {0, 0, 0, 0});
  glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  // R8 coverage. GL_RED with a swizzle would work too, but ui.frag samples .r
  // directly so there is nothing to swizzle.
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kAtlasSize, kAtlasSize, 0, GL_RED, GL_UNSIGNED_BYTE,
               nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  log::info("interface fonts: %s", faces_[0]->path.c_str());
  return true;
}

void Text::destroy() {
  if (texture_) glDeleteTextures(1, &texture_);
  texture_ = 0;
  faces_.clear();
  fallbackFaces_.clear();
  atlas_ = Image {};
}

void Text::beginFrame(float deviceScale) {
  deviceScale_ = deviceScale > 0.0f ? deviceScale : 1.0f;
}

Text::Face& Text::face(FontId id) {
  const std::size_t i = static_cast<std::size_t>(id);
  Face& f = *faces_[i];
  if (f.loaded) return f;
  return *faces_[static_cast<std::size_t>(FontId::Sans)];
}

const std::string& Text::facePath(FontId id) const {
  const std::size_t i = static_cast<std::size_t>(id);
  if (i < faces_.size() && faces_[i]->loaded) return faces_[i]->path;
  return emptyPath_;
}

Text::Face* Text::faceFor(FontId id, std::uint32_t codepoint, int& glyphIndexOut) {
  Face& primary = face(id);
  int index = stbtt_FindGlyphIndex(&primary.info, static_cast<int>(codepoint));
  if (index != 0) {
    glyphIndexOut = index;
    return &primary;
  }
  for (auto& f : fallbackFaces_) {
    index = stbtt_FindGlyphIndex(&f->info, static_cast<int>(codepoint));
    if (index != 0) {
      glyphIndexOut = index;
      return f.get();
    }
  }
  // Nothing covers it. Return the primary with index 0 so the advance is still the
  // face's .notdef width and the run keeps its spacing.
  glyphIndexOut = 0;
  return &primary;
}

bool Text::packGlyph(int w, int h, int& x, int& y) {
  if (w <= 0 || h <= 0) {
    x = y = 0;
    return true;
  }
  if (shelfX_ + w + kGlyphPadding > kAtlasSize) {
    shelfX_ = 0;
    shelfY_ += shelfHeight_ + kGlyphPadding;
    shelfHeight_ = 0;
  }
  if (shelfY_ + h + kGlyphPadding > kAtlasSize) {
    if (!atlasFull_) {
      log::warn("glyph atlas full at %dx%d; some text will not draw", kAtlasSize, kAtlasSize);
      atlasFull_ = true;
    }
    return false;
  }
  x = shelfX_;
  y = shelfY_;
  shelfX_ += w + kGlyphPadding;
  shelfHeight_ = std::max(shelfHeight_, h);
  return true;
}

const Text::Glyph* Text::glyph(Face& f, int glyphIndex, int pixelSize) {
  const std::uint64_t key = Face::key(glyphIndex, pixelSize);
  auto it = f.glyphs.find(key);
  if (it != f.glyphs.end()) return it->second.valid ? &it->second : nullptr;

  const float sc = stbtt_ScaleForMappingEmToPixels(&f.info, static_cast<float>(pixelSize));
  int advU = 0, lsbU = 0;
  stbtt_GetGlyphHMetrics(&f.info, glyphIndex, &advU, &lsbU);

  Glyph g;
  g.advance = advU * sc;

  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  stbtt_GetGlyphBitmapBox(&f.info, glyphIndex, sc, sc, &x0, &y0, &x1, &y1);
  const int w = x1 - x0;
  const int h = y1 - y0;
  if (w > 0 && h > 0) {
    int ax = 0, ay = 0;
    if (!packGlyph(w, h, ax, ay)) {
      f.glyphs.emplace(key, g);  // invalid: no bitmap, advance still usable
      return nullptr;
    }
    std::vector<unsigned char> bitmap(static_cast<std::size_t>(w) * h);
    stbtt_MakeGlyphBitmap(&f.info, bitmap.data(), w, h, w, sc, sc, glyphIndex);
    for (int row = 0; row < h; ++row) {
      for (int col = 0; col < w; ++col) {
        const unsigned char a = bitmap[static_cast<std::size_t>(row) * w + col];
        atlas_.set(ax + col, ay + row, Rgba {a, a, a, a});
      }
    }
    g.x = ax;
    g.y = ay;
    g.w = w;
    g.h = h;
    g.bearingX = x0;
    g.bearingY = y0;
    g.valid = true;
    atlasDirty_ = true;
  }
  auto [inserted, ok] = f.glyphs.emplace(key, g);
  (void)ok;
  return inserted->second.valid ? &inserted->second : nullptr;
}

void Text::uploadDirty() {
  if (!atlasDirty_) return;
  atlasDirty_ = false;
  // The Image stores RGBA; the texture wants one channel. Repacking the whole page
  // costs 4 MB of walking, which only happens on a frame that added a glyph.
  static std::vector<unsigned char> single;
  single.resize(static_cast<std::size_t>(kAtlasSize) * kAtlasSize);
  const std::uint8_t* src = atlas_.data();
  for (std::size_t i = 0; i < single.size(); ++i) single[i] = src[i * 4];
  glBindTexture(GL_TEXTURE_2D, texture_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kAtlasSize, kAtlasSize, GL_RED, GL_UNSIGNED_BYTE,
                  single.data());
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glBindTexture(GL_TEXTURE_2D, 0);
}

// One shared walk, so measure() and draw() can never disagree about where a glyph
// sits — the class of bug that makes centred text drift by a pixel per screen.
template <typename Emit>
float Text::run(const std::string& s, const TextStyle& style, float penX, Emit&& emit) {
  const int pixelSize = std::max(1, static_cast<int>(std::lround(style.size * deviceScale_)));
  const float spacing = style.letterSpacing * deviceScale_;

  float pen = penX;
  std::uint32_t prev = 0;
  Face* prevFace = nullptr;
  std::size_t i = 0;
  while (i < s.size()) {
    std::uint32_t cp = decodeUtf8(s, i);
    if (cp == 0) break;
    if (style.uppercase) cp = toUpperCp(cp);

    int glyphIndex = 0;
    Face* f = faceFor(style.font, cp, glyphIndex);
    const float sc = stbtt_ScaleForMappingEmToPixels(&f->info, static_cast<float>(pixelSize));

    // Kerning only applies within a face; a fallback glyph next to a primary one
    // has no pair to look up, which is what the browser does too.
    if (prev != 0 && f == prevFace) {
      pen += stbtt_GetCodepointKernAdvance(&f->info, static_cast<int>(prev),
                                           static_cast<int>(cp)) *
             sc;
    }

    const Glyph* g = glyph(*f, glyphIndex, pixelSize);
    if (g) {
      emit(*f, *g, pen);
      pen += g->advance;
    } else {
      int advU = 0, lsbU = 0;
      stbtt_GetGlyphHMetrics(&f->info, glyphIndex, &advU, &lsbU);
      pen += advU * sc;
    }
    pen += spacing;
    prev = cp;
    prevFace = f;
  }
  return pen - penX;
}

float Text::measure(const std::string& s, const TextStyle& style) {
  if (s.empty()) return 0.0f;
  const float device = run(s, style, 0.0f, [](Face&, const Glyph&, float) {});
  return device / deviceScale_;
}

TextMetrics Text::metrics(const TextStyle& style) {
  Face& f = face(style.font);
  const int pixelSize = std::max(1, static_cast<int>(std::lround(style.size * deviceScale_)));
  const float sc = stbtt_ScaleForMappingEmToPixels(&f.info, static_cast<float>(pixelSize));
  TextMetrics m;
  m.ascent = f.ascentU * sc / deviceScale_;
  m.descent = -f.descentU * sc / deviceScale_;
  m.lineHeight = (f.ascentU - f.descentU + f.lineGapU) * sc / deviceScale_;
  return m;
}

float Text::draw(Ui2D& ui, float x, float baselineY, const std::string& s,
                 const TextStyle& style) {
  if (s.empty()) return 0.0f;
  uploadDirty();
  ui.setFontTexture(texture_);

  // Glyph quads are emitted in device pixels: rounding the pen to a whole device
  // pixel is what keeps the rasterised coverage from being resampled by the
  // texture filter, which is the difference between crisp 12px text and mush.
  const float baseDevX = std::round(x * deviceScale_);
  const float baseDevY = std::round(baselineY * deviceScale_);
  constexpr float inv = 1.0f / static_cast<float>(kAtlasSize);

  const auto emitRun = [&](float offX, float offY, Rgba color) {
    run(s, style, baseDevX + offX, [&](Face&, const Glyph& g, float pen) {
      const Rect r {std::round(pen) + static_cast<float>(g.bearingX),
                    baseDevY + offY + static_cast<float>(g.bearingY),
                    static_cast<float>(g.w), static_cast<float>(g.h)};
      ui.glyphQuad(r, g.x * inv, g.y * inv, (g.x + g.w) * inv, (g.y + g.h) * inv, color, color,
                   0.0f, 0.0f);
    });
  };

  for (int i = 0; i < style.shadowCount; ++i) {
    const TextStyle::Shadow& sh = style.shadows[i];
    if (sh.color.a == 0) continue;
    emitRun(sh.dx * deviceScale_, sh.dy * deviceScale_, sh.color);
  }
  emitRun(0.0f, 0.0f, style.color);

  TextStyle probe = style;
  probe.shadowCount = 0;
  return measure(s, probe);
}

float Text::drawInBox(Ui2D& ui, const Rect& box, const std::string& s, const TextStyle& style,
                      TextAlign align) {
  const TextMetrics m = metrics(style);
  const float w = measure(s, style);
  float x = box.x;
  if (align == TextAlign::Center) x = box.x + (box.w - w) * 0.5f;
  else if (align == TextAlign::Right) x = box.right() - w;
  // Centre the line box, then sit the baseline inside it — the same result as a
  // single-line flex item with align-items: center.
  const float lineTop = box.y + (box.h - m.lineHeight) * 0.5f;
  const float baseline = lineTop + (m.lineHeight - (m.ascent + m.descent)) * 0.5f + m.ascent;
  return draw(ui, x, baseline, s, style);
}

std::vector<std::string> Text::wrap(const std::string& s, const TextStyle& style,
                                    float maxWidth) {
  std::vector<std::string> lines;
  std::string line;
  std::size_t i = 0;
  while (i <= s.size()) {
    // Take the next word plus the whitespace that follows it.
    const std::size_t start = i;
    while (i < s.size() && s[i] != ' ' && s[i] != '\n') ++i;
    std::string word = s.substr(start, i - start);
    const bool newline = i < s.size() && s[i] == '\n';
    if (i < s.size()) ++i;

    std::string candidate = line.empty() ? word : line + " " + word;
    if (!line.empty() && measure(candidate, style) > maxWidth) {
      lines.push_back(line);
      line = word;
    } else {
      line = candidate;
    }
    if (newline) {
      lines.push_back(line);
      line.clear();
    }
    if (start >= s.size()) break;
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}

Image Text::rasterise(const std::string& s, const TextStyle& style, float deviceScale,
                      int* baselineOut) {
  const float savedScale = deviceScale_;
  deviceScale_ = deviceScale > 0.0f ? deviceScale : 1.0f;

  const int pixelSize = std::max(1, static_cast<int>(std::lround(style.size * deviceScale_)));
  Face& primary = face(style.font);
  const float sc = stbtt_ScaleForMappingEmToPixels(&primary.info, static_cast<float>(pixelSize));
  const int ascent = static_cast<int>(std::ceil(primary.ascentU * sc));
  const int descent = static_cast<int>(std::ceil(-primary.descentU * sc));

  // Two passes: measure the ink box, then draw into it. The glyph cache is not used
  // here — a one-off title at 54px would otherwise occupy a chunk of the atlas for
  // the rest of the session.
  int minX = 1 << 30, maxX = -(1 << 30), minY = 1 << 30, maxY = -(1 << 30);
  struct Placed {
    Face* face;
    int glyphIndex;
    int penX;
    int x0, y0, x1, y1;
  };
  std::vector<Placed> placed;

  float pen = 0.0f;
  std::uint32_t prev = 0;
  Face* prevFace = nullptr;
  std::size_t i = 0;
  while (i < s.size()) {
    std::uint32_t cp = decodeUtf8(s, i);
    if (cp == 0) break;
    if (style.uppercase) cp = toUpperCp(cp);
    int glyphIndex = 0;
    Face* f = faceFor(style.font, cp, glyphIndex);
    const float fsc = stbtt_ScaleForMappingEmToPixels(&f->info, static_cast<float>(pixelSize));
    if (prev != 0 && f == prevFace) {
      pen += stbtt_GetCodepointKernAdvance(&f->info, static_cast<int>(prev),
                                           static_cast<int>(cp)) *
             fsc;
    }
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&f->info, glyphIndex, fsc, fsc, &x0, &y0, &x1, &y1);
    const int px = static_cast<int>(std::round(pen));
    if (x1 > x0 && y1 > y0) {
      placed.push_back({f, glyphIndex, px, x0, y0, x1, y1});
      minX = std::min(minX, px + x0);
      maxX = std::max(maxX, px + x1);
      minY = std::min(minY, y0);
      maxY = std::max(maxY, y1);
    }
    int advU = 0, lsbU = 0;
    stbtt_GetGlyphHMetrics(&f->info, glyphIndex, &advU, &lsbU);
    pen += advU * fsc + style.letterSpacing * deviceScale_;
    prev = cp;
    prevFace = f;
  }

  deviceScale_ = savedScale;
  if (placed.empty()) {
    if (baselineOut) *baselineOut = ascent;
    return Image {};
  }
  (void)descent;

  const int w = maxX - minX;
  const int h = maxY - minY;
  Image out(w, h, Rgba {0, 0, 0, 0});
  std::vector<unsigned char> bitmap;
  for (const Placed& p : placed) {
    const float fsc = stbtt_ScaleForMappingEmToPixels(&p.face->info, static_cast<float>(pixelSize));
    const int gw = p.x1 - p.x0;
    const int gh = p.y1 - p.y0;
    bitmap.assign(static_cast<std::size_t>(gw) * gh, 0);
    stbtt_MakeGlyphBitmap(&p.face->info, bitmap.data(), gw, gh, gw, fsc, fsc, p.glyphIndex);
    for (int row = 0; row < gh; ++row) {
      for (int col = 0; col < gw; ++col) {
        const unsigned char a = bitmap[static_cast<std::size_t>(row) * gw + col];
        if (a == 0) continue;
        const int dx = p.penX + p.x0 - minX + col;
        const int dy = p.y0 - minY + row;
        const Rgba prevPx = out.get(dx, dy);
        if (a > prevPx.a) out.set(dx, dy, Rgba {255, 255, 255, a});
      }
    }
  }
  if (baselineOut) *baselineOut = -minY;
  return out;
}

}  // namespace hr::ui
