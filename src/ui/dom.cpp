#include "ui/dom.h"

#include <algorithm>
#include <cmath>

#include "ui/uisprites.h"

namespace hr::ui {
namespace {

float clampSize(float v, float min, float max) { return std::min(std::max(v, min), max); }

bool isRow(const Node& n) {
  return n.style.display == Display::Flex && n.style.dir == Dir::Row;
}

// Effective align-items for a child, honouring align-self.
Align alignFor(const Node& parent, const Node& child) {
  if (child.style.hasAlignSelf) return child.style.alignSelf;
  return parent.style.align;
}

}  // namespace

void Doc::reset(Text* text) {
  nodes_.clear();
  stack_.clear();
  text_ = text;
}

int Doc::begin(const Style& style, int tag, int index) {
  const int i = static_cast<int>(nodes_.size());
  Node n;
  n.style = style;
  n.tag = tag;
  n.index = index;
  if (!stack_.empty()) {
    const int p = stack_.back();
    n.parent = p;
    if (nodes_[p].lastChild < 0) {
      nodes_[p].firstChild = i;
    } else {
      nodes_[nodes_[p].lastChild].nextSibling = i;
    }
    nodes_[p].lastChild = i;
  }
  nodes_.push_back(std::move(n));
  stack_.push_back(i);
  return i;
}

void Doc::end() {
  if (!stack_.empty()) stack_.pop_back();
}

int Doc::box(const Style& style, int tag, int index) {
  const int i = begin(style, tag, index);
  end();
  return i;
}

int Doc::label(const std::string& s, const TextStyle& ts, const Style& style, int tag) {
  const int i = begin(style, tag);
  nodes_[i].content = Content::Text;
  nodes_[i].text = s;
  nodes_[i].textStyle = ts;
  end();
  return i;
}

int Doc::icon(const IconRef& ref, float w, float h, const Style& style, int tag, int index) {
  Style s = style;
  s.width = w;
  s.height = h;
  const int i = begin(s, tag, index);
  nodes_[i].content = Content::Icon;
  nodes_[i].icon = ref;
  end();
  return i;
}

int Doc::custom(const Style& style, int tag, int index) {
  const int i = begin(style, tag, index);
  nodes_[i].content = Content::Custom;
  end();
  return i;
}

Style Doc::row(float gap, Justify justify, Align align) {
  Style s;
  s.display = Display::Flex;
  s.dir = Dir::Row;
  s.justify = justify;
  s.align = align;
  s.gap = gap;
  return s;
}

Style Doc::column(float gap, Align align) {
  Style s;
  s.display = Display::Flex;
  s.dir = Dir::Column;
  s.align = align;
  s.gap = gap;
  return s;
}

// ---------------------------------------------------------------------------
// Pass 1: intrinsic sizes, bottom-up.
//
// `availableWidth` is only consulted where CSS genuinely needs it: wrapping rows,
// auto-fill grids, and text that must wrap. Everything else measures at max-content,
// which is what makes a card `min-width: 340px` grow to fit its longest button.
// ---------------------------------------------------------------------------
float Doc::measure(int i, float availableWidth) {
  Node& n = nodes_[i];
  const Style& s = n.style;

  const float boxAvail =
      std::max(0.0f, std::min(availableWidth, s.maxWidth) - s.padding.horizontal());
  const float innerAvail = s.width != kAuto ? std::max(0.0f, s.width - s.padding.horizontal())
                                            : boxAvail;

  float contentW = 0, contentH = 0;

  if (n.content == Content::Text && text_) {
    const TextMetrics m = text_->metrics(n.textStyle);
    n.baseline = m.ascent + (m.lineHeight - (m.ascent + m.descent)) * 0.5f;
    const float w = text_->measure(n.text, n.textStyle);
    if (s.ellipsis) {
      // Never grows past its box and never wraps; paint() does the truncation.
      contentW = std::min(w, innerAvail);
      contentH = m.lineHeight;
    } else if (w > innerAvail && s.maxWidth < kNone) {
      // Only a node with a width cap wraps; the rest are single-line labels, which
      // is how the stylesheet uses them.
      const std::vector<std::string> lines = text_->wrap(n.text, n.textStyle, innerAvail);
      contentW = 0;
      for (const std::string& line : lines) {
        contentW = std::max(contentW, text_->measure(line, n.textStyle));
      }
      contentH = m.lineHeight * static_cast<float>(std::max<std::size_t>(1, lines.size()));
    } else {
      contentW = w;
      contentH = m.lineHeight;
    }
  }

  if (s.display == Display::Grid) {
    // Measure children first so an `auto` first column and the row heights are known.
    int cols = s.gridCols;
    float colW = s.gridColWidth;
    std::vector<int> kids;
    for (int c = n.firstChild; c >= 0; c = nodes_[c].nextSibling) kids.push_back(c);

    const float colGap = s.gap;
    const float rowGap = s.rowGap != kAuto ? s.rowGap : s.gap;

    if (cols == 0) {
      // repeat(auto-fill, minmax(min, 1fr))
      const float min = std::max(1.0f, s.gridMinCol);
      cols = std::max(1, static_cast<int>(std::floor((innerAvail + colGap) / (min + colGap))));
      colW = (innerAvail - colGap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
    } else if (colW <= 0.0f) {
      colW = (innerAvail - colGap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
    }

    float firstColW = 0;
    for (std::size_t k = 0; k < kids.size(); ++k) {
      const float w = s.gridFirstColAuto && (static_cast<int>(k) % cols) == 0 ? innerAvail : colW;
      measure(kids[k], w);
      if (s.gridFirstColAuto && (static_cast<int>(k) % cols) == 0) {
        firstColW = std::max(firstColW, nodes_[kids[k]].measuredW);
      }
    }
    if (s.gridFirstColAuto) {
      // `grid-template-columns: auto 1fr` — column 0 is max-content, the rest split.
      const float rest = std::max(0.0f, innerAvail - firstColW - colGap);
      colW = cols > 1 ? rest / static_cast<float>(cols - 1) : rest;
      for (std::size_t k = 0; k < kids.size(); ++k) {
        if ((static_cast<int>(k) % cols) != 0) measure(kids[k], colW);
      }
      contentW = firstColW + colGap + colW * static_cast<float>(cols - 1);
    } else {
      contentW = std::max(contentW, colW * static_cast<float>(cols) +
                                        colGap * static_cast<float>(cols - 1));
    }

    const int rows =
        static_cast<int>((kids.size() + static_cast<std::size_t>(cols) - 1) / static_cast<std::size_t>(cols));
    float total = 0;
    for (int r = 0; r < rows; ++r) {
      float rowH = 0;
      for (int c = 0; c < cols; ++c) {
        const std::size_t k = static_cast<std::size_t>(r) * cols + c;
        if (k >= kids.size()) break;
        const Node& kid = nodes_[kids[k]];
        rowH = std::max(rowH, kid.measuredH + kid.style.margin.vertical());
      }
      total += rowH;
      if (r + 1 < rows) total += rowGap;
    }
    contentH = std::max(contentH, total);
  } else if (s.display == Display::Flex) {
    const bool horizontal = s.dir == Dir::Row;
    const float lineGap = s.rowGap != kAuto ? s.rowGap : s.gap;
    float maxLineMain = 0, totalCross = 0;
    float lineMain = 0, lineCross = 0;
    int inLine = 0, lineIndex = 0;
    for (int c = n.firstChild; c >= 0; c = nodes_[c].nextSibling) {
      Node& kid = nodes_[c];
      const float kidAvail = horizontal ? innerAvail : innerAvail - kid.style.margin.horizontal();
      measure(c, std::max(0.0f, kidAvail));
      const float kMain = horizontal ? kid.measuredW + kid.style.margin.horizontal()
                                     : kid.measuredH + kid.style.margin.vertical();
      const float kCross = horizontal ? kid.measuredH + kid.style.margin.vertical()
                                      : kid.measuredW + kid.style.margin.horizontal();
      const float withGap = inLine > 0 ? s.gap + kMain : kMain;
      if (horizontal && s.wrap && inLine > 0 && lineMain + withGap > innerAvail) {
        maxLineMain = std::max(maxLineMain, lineMain);
        totalCross += lineCross + (lineIndex > 0 ? lineGap : 0.0f);
        ++lineIndex;
        lineMain = kMain;
        lineCross = kCross;
        inLine = 1;
      } else {
        lineMain += withGap;
        lineCross = std::max(lineCross, kCross);
        ++inLine;
      }
    }
    maxLineMain = std::max(maxLineMain, lineMain);
    totalCross += lineCross + (lineIndex > 0 ? lineGap : 0.0f);
    if (horizontal) {
      contentW = std::max(contentW, maxLineMain);
      contentH = std::max(contentH, totalCross);
    } else {
      contentH = std::max(contentH, maxLineMain);
      contentW = std::max(contentW, totalCross);
    }
  } else {
    // Block: children stack, width is the widest.
    float total = 0;
    for (int c = n.firstChild; c >= 0; c = nodes_[c].nextSibling) {
      Node& kid = nodes_[c];
      measure(c, std::max(0.0f, innerAvail - kid.style.margin.horizontal()));
      total += kid.measuredH + kid.style.margin.vertical();
      contentW = std::max(contentW, kid.measuredW + kid.style.margin.horizontal());
    }
    contentH = std::max(contentH, total);
  }

  const float w = s.width != kAuto ? s.width : contentW + s.padding.horizontal();
  const float h = s.height != kAuto ? s.height : contentH + s.padding.vertical();
  n.measuredW = clampSize(w, s.minWidth, s.maxWidth);
  n.measuredH = clampSize(h, s.minHeight, s.maxHeight);
  return n.measuredW;
}

// ---------------------------------------------------------------------------
// Pass 2: placement, top-down.
// ---------------------------------------------------------------------------
void Doc::place(int i, Rect content) {
  Node& n = nodes_[i];
  const Style& s = n.style;
  if (s.display == Display::Grid) {
    placeGrid(i, content);
  } else if (s.display == Display::Flex) {
    placeFlex(i, content);
  } else {
    float y = content.y;
    for (int c = n.firstChild; c >= 0; c = nodes_[c].nextSibling) {
      Node& kid = nodes_[c];
      const Edges& m = kid.style.margin;
      float w = kid.style.width != kAuto ? kid.measuredW : content.w - m.horizontal();
      if (kid.style.width == kAuto && kid.style.maxWidth < kNone) {
        w = std::min(w, kid.style.maxWidth);
      }
      w = clampSize(w, kid.style.minWidth, kid.style.maxWidth);
      kid.rect = {content.x + m.left, y + m.top, w, kid.measuredH};
      Rect inner = {kid.rect.x + kid.style.padding.left, kid.rect.y + kid.style.padding.top,
                    std::max(0.0f, kid.rect.w - kid.style.padding.horizontal()),
                    std::max(0.0f, kid.rect.h - kid.style.padding.vertical())};
      place(c, inner);
      y += kid.measuredH + m.vertical();
    }
  }
}

void Doc::placeFlex(int i, const Rect& content) {
  Node& n = nodes_[i];
  const Style& s = n.style;
  const bool horizontal = s.dir == Dir::Row;
  const float mainSize = horizontal ? content.w : content.h;
  const float crossSize = horizontal ? content.h : content.w;
  const float rowGap = s.rowGap != kAuto ? s.rowGap : s.gap;

  std::vector<int> kids;
  for (int c = n.firstChild; c >= 0; c = nodes_[c].nextSibling) kids.push_back(c);
  if (kids.empty()) return;

  // Break into flex lines. Only rows wrap in this stylesheet.
  struct Line {
    int first = 0, count = 0;
    float main = 0, cross = 0;
  };
  std::vector<Line> lines;
  {
    Line line;
    line.first = 0;
    for (std::size_t k = 0; k < kids.size(); ++k) {
      const Node& kid = nodes_[kids[k]];
      const float kMain = horizontal ? kid.measuredW + kid.style.margin.horizontal()
                                     : kid.measuredH + kid.style.margin.vertical();
      const float kCross = horizontal ? kid.measuredH + kid.style.margin.vertical()
                                      : kid.measuredW + kid.style.margin.horizontal();
      const float withGap = line.count > 0 ? s.gap + kMain : kMain;
      if (horizontal && s.wrap && line.count > 0 && line.main + withGap > mainSize + 0.01f) {
        lines.push_back(line);
        line = Line {static_cast<int>(k), 1, kMain, kCross};
      } else {
        line.main += withGap;
        line.cross = std::max(line.cross, kCross);
        ++line.count;
      }
    }
    lines.push_back(line);
  }

  float crossCursor = horizontal ? content.y : content.x;
  for (const Line& line : lines) {
    // Distribute free space along the main axis.
    float free = mainSize - line.main;
    float growTotal = 0, shrinkTotal = 0;
    for (int k = line.first; k < line.first + line.count; ++k) {
      growTotal += nodes_[kids[k]].style.grow;
      shrinkTotal += nodes_[kids[k]].style.shrink;
    }

    std::vector<float> mainSizes(static_cast<std::size_t>(line.count));
    for (int k = 0; k < line.count; ++k) {
      const Node& kid = nodes_[kids[line.first + k]];
      mainSizes[k] = horizontal ? kid.measuredW : kid.measuredH;
    }
    if (free > 0 && growTotal > 0) {
      for (int k = 0; k < line.count; ++k) {
        const Node& kid = nodes_[kids[line.first + k]];
        if (kid.style.grow <= 0) continue;
        float add = free * (kid.style.grow / growTotal);
        const float cap = horizontal ? kid.style.maxWidth : kid.style.maxHeight;
        if (mainSizes[k] + add > cap) add = std::max(0.0f, cap - mainSizes[k]);
        mainSizes[k] += add;
      }
    } else if (free < 0 && shrinkTotal > 0 && !s.scrollY) {
      // A scroll region's children must keep their measured size — squashing them to
      // fit is exactly what `overflow-y: auto` exists to avoid.
      for (int k = 0; k < line.count; ++k) {
        const Node& kid = nodes_[kids[line.first + k]];
        if (kid.style.shrink <= 0) continue;
        float take = -free * (kid.style.shrink / shrinkTotal);
        const float floorSize = horizontal ? kid.style.minWidth : kid.style.minHeight;
        mainSizes[k] = std::max(floorSize, mainSizes[k] - take);
      }
    }

    float used = 0;
    for (int k = 0; k < line.count; ++k) {
      const Node& kid = nodes_[kids[line.first + k]];
      used += mainSizes[k] + (horizontal ? kid.style.margin.horizontal()
                                         : kid.style.margin.vertical());
    }
    used += s.gap * static_cast<float>(std::max(0, line.count - 1));

    float cursor = horizontal ? content.x : content.y;
    float spacing = s.gap;
    if (s.justify == Justify::Center) {
      cursor += (mainSize - used) * 0.5f;
    } else if (s.justify == Justify::End) {
      cursor += mainSize - used;
    } else if (s.justify == Justify::SpaceBetween && line.count > 1) {
      spacing = s.gap + (mainSize - used) / static_cast<float>(line.count - 1);
    }

    // Baseline alignment needs the tallest ascent on the line first.
    float maxAscent = 0;
    for (int k = 0; k < line.count; ++k) {
      maxAscent = std::max(maxAscent, nodes_[kids[line.first + k]].baseline);
    }

    for (int k = 0; k < line.count; ++k) {
      const int ci = kids[line.first + k];
      Node& kid = nodes_[ci];
      const Edges& m = kid.style.margin;
      const Align a = alignFor(n, kid);

      float kMain = mainSizes[k];
      float kCross = horizontal ? kid.measuredH : kid.measuredW;
      const float lineCross = (horizontal && s.wrap) ? line.cross : crossSize;
      if (a == Align::Stretch) {
        const bool explicitCross = horizontal ? kid.style.height != kAuto
                                              : kid.style.width != kAuto;
        if (!explicitCross) {
          kCross = std::max(0.0f, lineCross - (horizontal ? m.vertical() : m.horizontal()));
          const float cap = horizontal ? kid.style.maxHeight : kid.style.maxWidth;
          const float floorSize = horizontal ? kid.style.minHeight : kid.style.minWidth;
          kCross = clampSize(kCross, floorSize, cap);
        }
      }

      float crossOffset = 0;
      if (a == Align::Center) {
        crossOffset = (lineCross - (kCross + (horizontal ? m.vertical() : m.horizontal()))) * 0.5f;
      } else if (a == Align::End) {
        crossOffset = lineCross - (kCross + (horizontal ? m.vertical() : m.horizontal()));
      } else if (a == Align::Baseline && horizontal) {
        crossOffset = maxAscent - kid.baseline;
      }

      if (horizontal) {
        kid.rect = {cursor + m.left, crossCursor + crossOffset + m.top, kMain, kCross};
        cursor += kMain + m.horizontal() + spacing;
      } else {
        kid.rect = {crossCursor + crossOffset + m.left, cursor + m.top, kCross, kMain};
        cursor += kMain + m.vertical() + spacing;
      }

      Rect inner = {kid.rect.x + kid.style.padding.left, kid.rect.y + kid.style.padding.top,
                    std::max(0.0f, kid.rect.w - kid.style.padding.horizontal()),
                    std::max(0.0f, kid.rect.h - kid.style.padding.vertical())};
      place(ci, inner);
    }

    if (horizontal) {
      crossCursor += line.cross + rowGap;
    } else {
      crossCursor += line.cross + rowGap;
    }
  }
}

void Doc::placeGrid(int i, const Rect& content) {
  Node& n = nodes_[i];
  const Style& s = n.style;
  std::vector<int> kids;
  for (int c = n.firstChild; c >= 0; c = nodes_[c].nextSibling) kids.push_back(c);
  if (kids.empty()) return;

  const float colGap = s.gap;
  const float rowGap = s.rowGap != kAuto ? s.rowGap : s.gap;

  int cols = s.gridCols;
  float colW = s.gridColWidth;
  float firstColW = 0;
  if (cols == 0) {
    const float min = std::max(1.0f, s.gridMinCol);
    cols = std::max(1, static_cast<int>(std::floor((content.w + colGap) / (min + colGap))));
    cols = std::min(cols, static_cast<int>(kids.size()));
    cols = std::max(1, cols);
    colW = (content.w - colGap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
  } else if (s.gridFirstColAuto) {
    for (std::size_t k = 0; k < kids.size(); k += static_cast<std::size_t>(cols)) {
      firstColW = std::max(firstColW, nodes_[kids[k]].measuredW);
    }
    const float rest = std::max(0.0f, content.w - firstColW - colGap);
    colW = cols > 1 ? rest / static_cast<float>(cols - 1) : rest;
  } else if (colW <= 0.0f) {
    colW = (content.w - colGap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
  }

  const int rows = static_cast<int>((kids.size() + static_cast<std::size_t>(cols) - 1) /
                                    static_cast<std::size_t>(cols));
  float y = content.y;
  for (int r = 0; r < rows; ++r) {
    float rowH = 0;
    for (int c = 0; c < cols; ++c) {
      const std::size_t k = static_cast<std::size_t>(r) * cols + c;
      if (k >= kids.size()) break;
      const Node& kid = nodes_[kids[k]];
      rowH = std::max(rowH, kid.measuredH + kid.style.margin.vertical());
    }
    float x = content.x;
    for (int c = 0; c < cols; ++c) {
      const std::size_t k = static_cast<std::size_t>(r) * cols + c;
      if (k >= kids.size()) break;
      Node& kid = nodes_[kids[k]];
      const float trackW = (s.gridFirstColAuto && c == 0) ? firstColW : colW;
      const Edges& m = kid.style.margin;
      float w = kid.style.width != kAuto ? kid.measuredW : trackW - m.horizontal();
      float h = kid.style.height != kAuto ? kid.measuredH : kid.measuredH;
      // align-items applies per cell; baseline is treated as start, which is what
      // the two grids that ask for it look like in the browser at these sizes.
      const Align a = alignFor(n, kid);
      if (a == Align::Stretch && kid.style.height == kAuto) h = rowH - m.vertical();
      float cellY = y + m.top;
      if (a == Align::Center) cellY = y + (rowH - h - m.vertical()) * 0.5f + m.top;
      else if (a == Align::End) cellY = y + rowH - h - m.bottom;
      kid.rect = {x + m.left, cellY, w, h};
      Rect inner = {kid.rect.x + kid.style.padding.left, kid.rect.y + kid.style.padding.top,
                    std::max(0.0f, kid.rect.w - kid.style.padding.horizontal()),
                    std::max(0.0f, kid.rect.h - kid.style.padding.vertical())};
      place(kids[k], inner);
      x += trackW + colGap;
    }
    y += rowH + rowGap;
  }
}

void Doc::layout(const Rect& viewport) {
  if (nodes_.empty()) return;
  measure(0, viewport.w);

  Node& r = nodes_[0];
  // The root positions itself inside the viewport the way `.screen`'s flex centring
  // does, so a card ends up centred without the caller doing the arithmetic.
  float w = r.style.width != kAuto ? r.measuredW : viewport.w;
  float h = r.style.height != kAuto ? r.measuredH : viewport.h;
  w = clampSize(w, r.style.minWidth, r.style.maxWidth);
  h = clampSize(h, r.style.minHeight, r.style.maxHeight);
  r.rect = {viewport.x, viewport.y, w, h};
  Rect inner = {r.rect.x + r.style.padding.left, r.rect.y + r.style.padding.top,
                std::max(0.0f, r.rect.w - r.style.padding.horizontal()),
                std::max(0.0f, r.rect.h - r.style.padding.vertical())};
  place(0, inner);

  // Scroll regions shift their children and clamp their own offset. Done after
  // placement so contentHeight is the real laid-out height.
  for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
    Node& n = nodes_[i];
    if (!n.style.scrollY) continue;
    ScrollState& st = scroll(n.tag);
    float contentH = 0;
    for (int c = n.firstChild; c >= 0; c = nodes_[c].nextSibling) {
      contentH = std::max(contentH, nodes_[c].rect.bottom() + nodes_[c].style.margin.bottom -
                                        n.rect.y - n.style.padding.top);
    }
    st.contentHeight = contentH;
    st.viewHeight = std::max(0.0f, n.rect.h - n.style.padding.vertical());
    st.offset = clampSize(st.offset, 0.0f, std::max(0.0f, st.contentHeight - st.viewHeight));
    if (st.offset != 0.0f) {
      for (int c = n.firstChild; c >= 0; c = nodes_[c].nextSibling) {
        applyTranslate(c, 0.0f, -st.offset);
      }
    }
  }

  // transform: translate() on any node, applied to its whole subtree.
  for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
    const Style& s = nodes_[i].style;
    if (s.translateX != 0.0f || s.translateY != 0.0f) {
      applyTranslate(i, s.translateX, s.translateY);
    }
  }

  applyOpacity(0, 1.0f);
  applyClip(0, r.rect);
}

void Doc::applyTranslate(int i, float dx, float dy) {
  nodes_[i].rect.x += dx;
  nodes_[i].rect.y += dy;
  for (int c = nodes_[i].firstChild; c >= 0; c = nodes_[c].nextSibling) applyTranslate(c, dx, dy);
}

void Doc::applyOpacity(int i, float parentOpacity) {
  const float o = parentOpacity * nodes_[i].style.opacity;
  nodes_[i].opacity = o;
  for (int c = nodes_[i].firstChild; c >= 0; c = nodes_[c].nextSibling) applyOpacity(c, o);
}

void Doc::applyClip(int i, Rect current) {
  Node& n = nodes_[i];
  n.clipRect = current;
  n.clipped = true;
  Rect childClip = current;
  if (n.style.scrollY) {
    childClip = current.intersect({n.rect.x, n.rect.y, n.rect.w, n.rect.h});
  }
  for (int c = n.firstChild; c >= 0; c = nodes_[c].nextSibling) applyClip(c, childClip);
}

bool Doc::clickedButton(int node) const {
  for (int i = node; i >= 0; i = nodes_[i].parent) {
    if (nodes_[i].style.isButton) return true;
  }
  return false;
}

int Doc::hitTest(float x, float y) const {
  int best = -1;
  // Document order is paint order, so the last match is the topmost.
  for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
    const Node& n = nodes_[i];
    if (n.tag == 0) continue;
    if (!n.rect.contains(x, y)) continue;
    if (n.clipped && !n.clipRect.contains(x, y)) continue;
    if (n.opacity <= 0.01f) continue;
    best = i;
  }
  return best;
}

int Doc::findTag(int tag, int index) const {
  for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
    if (nodes_[i].tag == tag && nodes_[i].index == index) return i;
  }
  return -1;
}

Rect Doc::rectOf(int tag, int index) const {
  const int i = findTag(tag, index);
  return i >= 0 ? nodes_[i].rect : Rect {};
}

ScrollState& Doc::scroll(int tag) {
  for (auto& [key, state] : scrolls_) {
    if (key == tag) return state;
  }
  scrolls_.emplace_back(tag, ScrollState {});
  return scrolls_.back().second;
}

const ScrollState* Doc::scrollIfAny(int tag) const {
  for (const auto& [key, state] : scrolls_) {
    if (key == tag) return &state;
  }
  return nullptr;
}

void Doc::paintNode(Ui2D& ui, const Node& n) const {
  const Style& s = n.style;
  const float o = n.opacity;
  if (o <= 0.004f) return;
  if (n.rect.empty() && n.content != Content::Text) return;

  const auto tint = [o](Rgba c) { return o >= 0.999f ? c : fade(c, o); };

  if (s.shadow.color.a != 0) ui.shadow(n.rect, {s.shadow.dx, s.shadow.dy, s.shadow.blur,
                                                s.shadow.spread, tint(s.shadow.color)},
                                       s.radius);
  if (s.sprite != nullptr && s.sprite->valid()) {
    // Tinted by the node's own background, so one greyscale sprite can serve every
    // variant of a widget and still follow the theme — a pack ships `button.png`
    // once and the primary, danger and hover colours all come from the tokens.
    // A pack that wants its own colours ships a coloured sprite and sets the token
    // to white, which is the same mechanism read the other way round.
    ui.setTexture(s.sprite->texture);
    ui.ninePatch(n.rect, s.sprite->u0, s.sprite->v0, s.sprite->u1, s.sprite->v1,
                 s.sprite->width, s.sprite->height, s.sprite->slice,
                 tint(s.bg.a != 0 ? s.bg : kWhite));
    ui.setTexture(0);
  } else if (s.bg.a != 0 || (s.gradient && s.bg2.a != 0)) {
    if (s.gradient) {
      ui.fillGradient(n.rect, tint(s.bg), tint(s.bg2), s.radius);
    } else {
      ui.fillRect(n.rect, tint(s.bg), s.radius);
    }
  }
  if (s.insetHighlight.a != 0 && s.sprite == nullptr) {
    // inset 0 1px 0 rgba(...) is a one-pixel line just inside the top edge.
    ui.fillRect({n.rect.x + s.radius * 0.5f, n.rect.y + s.borderWidth,
                 std::max(0.0f, n.rect.w - s.radius), 1.0f},
                tint(s.insetHighlight));
  }
  if (s.borderWidth > 0 && s.border.a != 0 && s.sprite == nullptr) {
    ui.strokeRect(n.rect, tint(s.border), s.borderWidth, s.radius);
  }

  if (n.content == Content::Icon && n.icon.texture != 0) {
    ui.setTexture(n.icon.texture);
    ui.texturedRect(n.rect, n.icon.u0, n.icon.v0, n.icon.u1, n.icon.v1, tint(n.icon.tint));
    ui.setTexture(0);
  } else if (n.content == Content::Text && text_ && !n.text.empty()) {
    Rect inner = {n.rect.x + s.padding.left, n.rect.y + s.padding.top,
                  std::max(0.0f, n.rect.w - s.padding.horizontal()),
                  std::max(0.0f, n.rect.h - s.padding.vertical())};
    TextStyle ts = n.textStyle;
    ts.color = tint(ts.color);
    for (int i = 0; i < ts.shadowCount; ++i) ts.shadows[i].color = tint(ts.shadows[i].color);
    Text* t = const_cast<Text*>(text_);
    const TextMetrics m = t->metrics(ts);
    if (s.ellipsis) {
      std::string shown = n.text;
      if (t->measure(shown, ts) > inner.w) {
        const std::string dots = "…";  // a horizontal ellipsis
        const float dotsWidth = t->measure(dots, ts);
        // Drop code points from the end until the remainder plus the ellipsis fits.
        while (!shown.empty() && t->measure(shown, ts) + dotsWidth > inner.w) {
          shown.erase(stepUtf8(shown, shown.size(), -1));
        }
        shown += dots;
      }
      t->drawInBox(ui, inner, shown, ts, n.textAlign);
    } else if (inner.h > m.lineHeight * 1.5f) {
      // Multi-line: the measure pass already wrapped to this width.
      const std::vector<std::string> lines = t->wrap(n.text, ts, inner.w);
      float y = inner.y;
      for (const std::string& line : lines) {
        t->drawInBox(ui, {inner.x, y, inner.w, m.lineHeight}, line, ts, n.textAlign);
        y += m.lineHeight;
      }
    } else {
      t->drawInBox(ui, inner, n.text, ts, n.textAlign);
    }
  }
}

void Doc::paint(Ui2D& ui) const {
  if (nodes_.empty()) return;
  paintSubtree(ui, 0);
}

void Doc::paintSubtree(Ui2D& ui, int i) const {
  const Node& n = nodes_[i];
  paintNode(ui, n);
  const bool clip = n.style.scrollY;
  if (clip) ui.pushClip({n.rect.x, n.rect.y, n.rect.w, n.rect.h});
  for (int c = n.firstChild; c >= 0; c = nodes_[c].nextSibling) paintSubtree(ui, c);
  if (clip) {
    ui.popClip();
    // ::-webkit-scrollbar { width: 8px } with a rounded panel-2 thumb. Drawn after the
    // children and outside the clip so it sits over the content, as the browser's
    // overlay scrollbar does.
    if (const ScrollState* st = scrollIfAny(n.tag)) {
      if (st->contentHeight > st->viewHeight + 0.5f && st->viewHeight > 0) {
        const float track = n.rect.h;
        const float thumbH =
            std::max(24.0f, track * (st->viewHeight / st->contentHeight));
        const float travel = track - thumbH;
        const float t = st->contentHeight - st->viewHeight > 0
                            ? st->offset / (st->contentHeight - st->viewHeight)
                            : 0.0f;
        ui.fillRect({n.rect.right() - 8.0f, n.rect.y + travel * t, 8.0f, thumbH},
                    col(Role::PanelRaised), 4.0f);
      }
    }
  }
}

}  // namespace hr::ui
