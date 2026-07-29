// Text: font loading, the glyph atlas, measurement, and drawing.
//
// The browser did all of this. Replacing it means three things: rasterising glyphs
// (stb_truetype), caching them in one texture the batcher can sample, and matching
// the metrics closely enough that a ported `width: 340px` card still fits its label.
//
// Two deliberate simplifications against a real text engine, both invisible here:
// glyphs land on whole device pixels rather than being rasterised per subpixel
// offset, and shaping is a plain left-to-right run with kerning. Every string in
// this interface is Latin plus a handful of dingbats, so there is nothing to shape.
//
// Font *fallback* is not a simplification — it is required. The interface draws
// ♥ ♡ ◆ ◇ ● ☠ ◎ ✕ ➤ ‹ ›, and Segoe UI covers only some of those; the browser walked
// its family list for the rest, and so does this.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/gl.h"
#include "platform/fonts.h"
#include "resource/image.h"
#include "ui/theme.h"
#include "ui/ui2d.h"

namespace hr::ui {

// The faces the stylesheet actually asks for. Named rather than parameterised so a
// screen says `style.font = FontId::SansBold` and cannot ask for a face that has to
// be resolved at runtime.
enum class FontId : std::uint8_t {
  Sans,          // font-weight: 400
  SansSemibold,  // font-weight: 600  — buttons, slot counts, card titles
  SansBold,      // font-weight: 700  — world names, tooltip titles
  SansBlack,     // font-weight: 800  — resolves up to Segoe UI Black, per CSS
  SansItalic,    // .subtitle, .empty-note, .menu-sig
  Mono,          // "Consolas", monospace — .kbd and the F3 overlay
  Count,
};

struct TextStyle {
  FontId font = FontId::Sans;
  float size = 14.0f;        // CSS px
  float letterSpacing = 0;   // CSS letter-spacing, px
  Rgba color = color::text;
  bool uppercase = false;
  // text-shadow, applied before the glyph run. Up to two, matching the deepest use
  // in the stylesheet (.slot-count draws 1px offsets both ways).
  struct Shadow {
    float dx = 0, dy = 0, blur = 0;
    Rgba color = kTransparent;
  };
  Shadow shadows[2];
  int shadowCount = 0;

  TextStyle& withShadow(float dx, float dy, float blur, Rgba c) {
    if (shadowCount < 2) shadows[shadowCount++] = {dx, dy, blur, c};
    return *this;
  }
};

// Where a drawn string sits vertically. CSS positions text by its line box, which
// is what every ported layout value assumes.
struct TextMetrics {
  float ascent = 0;     // above the baseline, px
  float descent = 0;    // below the baseline, positive px
  float lineHeight = 0; // `line-height: normal` for this face and size
};

// Horizontal placement inside a box, matching text-align.
enum class TextAlign { Left, Center, Right };

class Text {
 public:
  Text();
  ~Text();
  Text(const Text&) = delete;
  Text& operator=(const Text&) = delete;

  bool init();
  void destroy();

  // Called once per frame with the ui scale, so glyphs are rasterised at the size
  // they will actually be sampled at rather than filtered up from layout pixels.
  void beginFrame(float deviceScale);

  GLuint texture() const { return texture_; }

  // Advance width of `s` in layout px, including letter-spacing (which CSS adds
  // after every character, the trailing one included).
  float measure(const std::string& s, const TextStyle& style);
  TextMetrics metrics(const TextStyle& style);

  // Draws with the baseline at `baselineY`. Returns the advance width.
  float draw(Ui2D& ui, float x, float baselineY, const std::string& s, const TextStyle& style);

  // Draws inside `box`, vertically centred on the line box the way a flex item with
  // `align-items: center` would be, and horizontally per `align`.
  float drawInBox(Ui2D& ui, const Rect& box, const std::string& s, const TextStyle& style,
                  TextAlign align = TextAlign::Left);

  // Greedy word wrap to `maxWidth`, splitting on spaces and keeping explicit
  // newlines. Used by the About panel and the multi-line notes.
  std::vector<std::string> wrap(const std::string& s, const TextStyle& style, float maxWidth);

  // CPU rasterisation of one line into an RGBA image: white with alpha = coverage,
  // tightly cropped, with the baseline position reported. This is how the menu
  // title is built — a fixed string with a three-stop gradient and two drop
  // shadows, which the plan calls for pre-rendering once rather than teaching the
  // shader about multi-stop gradients and blur.
  Image rasterise(const std::string& s, const TextStyle& style, float deviceScale,
                  int* baselineOut = nullptr);

  // True when no usable font file was found at all, so the caller can say so
  // rather than drawing an invisible interface.
  bool missingFonts() const { return missingFonts_; }
  const std::string& facePath(FontId id) const;

 private:
  struct Face;
  struct Glyph {
    // Position in the atlas, in atlas texels.
    int x = 0, y = 0, w = 0, h = 0;
    // Offset from the pen position (device px, y down from the baseline).
    int bearingX = 0, bearingY = 0;
    float advance = 0;  // device px
    bool valid = false;
  };

  Face& face(FontId id);
  // Resolves a codepoint to the face that has it, primary first then fallbacks.
  // Returns nullptr only when no loaded font covers it.
  Face* faceFor(FontId id, std::uint32_t codepoint, int& glyphIndexOut);
  const Glyph* glyph(Face& f, int glyphIndex, int pixelSize);
  bool packGlyph(int w, int h, int& x, int& y);
  void uploadDirty();

  // Shared walk used by measure() and draw(): decodes UTF-8, resolves faces,
  // applies kerning and letter-spacing, and calls `emit` per positioned glyph.
  template <typename Emit>
  float run(const std::string& s, const TextStyle& style, float penX, Emit&& emit);

  std::vector<std::unique_ptr<Face>> faces_;         // primary faces, indexed by FontId
  std::vector<std::unique_ptr<Face>> fallbackFaces_;

  Image atlas_;
  GLuint texture_ = 0;
  int shelfX_ = 0, shelfY_ = 0, shelfHeight_ = 0;
  bool atlasDirty_ = false;
  bool atlasFull_ = false;
  float deviceScale_ = 1.0f;
  bool missingFonts_ = true;
  std::string emptyPath_;
};

// Decodes one UTF-8 code point starting at `i`, advancing it. Invalid bytes decode
// to U+FFFD and consume one byte, so a malformed string cannot loop.
std::uint32_t decodeUtf8(const std::string& s, std::size_t& i);

// Appends `cp` as UTF-8. Used by the text fields, which edit by code point.
void appendUtf8(std::string& s, std::uint32_t cp);

// Byte index of the code point boundary `delta` code points from `byteIndex`.
std::size_t stepUtf8(const std::string& s, std::size_t byteIndex, int delta);

}  // namespace hr::ui
