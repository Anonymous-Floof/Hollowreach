// The recipe browser, ported from js/ui/recipebook.js.
//
// Reads the same recipe tables the crafting system does, so a new recipe appears here
// with no edit. Three things keep it usable as the list grows, all kept: category tabs,
// a fuzzy search, and family grouping — near-identical recipes (every stair, every
// pickaxe tier, a torch's two fuels) collapse into one card you cycle through with the
// ‹ › arrows.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "game/recipes.h"
#include "render/iconatlas.h"
#include "ui/dom.h"
#include "ui/text.h"
#include "ui/ui2d.h"
#include "ui/widgets.h"

namespace hr::ui {

class RecipeBook {
 public:
  void attach(const render::IconAtlas* icons) { icons_ = icons; }

  // Returns wherever the book was opened from — the world, or the crafting screen
  // that has the grid you were looking at.
  std::function<void()> onBack;
  // Clicking a recipe asks for it to be laid out in the grid the book was opened
  // from. A request, not an instruction: whether the station allows it and whether
  // the ingredients are there is decided by the crafting screen, which is the only
  // thing that knows either.
  std::function<void(const game::Recipe&)> onAutoFill;

  // Built lazily on first open, like the JS `built` flag: it walks every recipe and
  // groups it, which is not work to do at startup for a screen most sessions never open.
  void open();
  void close();

  void update(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens);
  void draw(Ui2D& ui, Text& text);

  bool searching() const { return search_.focused(); }
  void blurSearch(Input* input) { search_.setFocused(false, input); }

 private:
  // One recipe as the browser shows it: the input cells or chips, and the output.
  struct Entry {
    // Shaped: a grid of ingredient keys ("" for an empty cell) with its own width.
    std::vector<std::string> cells;
    int gridWidth = 0;
    // Shapeless: ingredient key and count pairs.
    std::vector<std::pair<std::string, int>> chips;
    std::string outKey;
    int outCount = 1;
    std::string name;
    // The recipe this row came from, for auto-fill. Null for a smelting row: a
    // forge has no grid to lay anything out in.
    const game::Recipe* recipe = nullptr;
  };
  struct Family {
    std::string key;
    std::string name;
    std::string category;
    std::vector<Entry> entries;
    int current = 0;
  };

  void buildData();
  void build(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens);
  void handle(const UiEvent& event, Text& text);
  void cardNode(const Family& family, int familyIndex);

  // Family and category keys, from familyKey()/categoryOf() in the JS.
  static std::string familyKeyFor(const std::string& outKey);
  static std::string categoryFor(const std::string& outKey);
  static std::string displayName(const std::string& key);
  // Query characters appearing in order somewhere in the string, case-insensitively.
  static bool fuzzy(const std::string& query, const std::string& subject);

  IconRef iconFor(const std::string& key) const;

  const render::IconAtlas* icons_ = nullptr;
  bool built_ = false;
  std::vector<Family> families_;
  std::string tab_ = "all";
  // Rows surviving the tab and the search, as indices into families_.
  std::vector<int> visible_;

  Doc doc_;
  // Rebuilt with the tree every frame: the icon a node shows, so a hover can name it,
  // and the shapeless chip counts, which the CSS positions absolutely over their chip.
  std::vector<std::string> iconKeys_;
  std::vector<std::pair<int, int>> chipCounts_;
  TextField search_;
  int hoveredTag_ = 0;
  int hoveredIndex_ = 0;
  // The icon under the cursor, for the shared tooltip.
  std::string hoverKey_;
  float mouseX_ = 0, mouseY_ = 0;
  double time_ = 0;
};

}  // namespace hr::ui
