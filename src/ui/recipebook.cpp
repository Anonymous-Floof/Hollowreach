#include "ui/recipebook.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "audio/sfx.h"
#include "game/crafting.h"
#include "game/items.h"
#include "world/blocks.h"

namespace hr::ui {
namespace {

enum : int {
  kTagSearch = 400,
  kTagTab = 401,     // index = tab
  kTagPrev = 402,    // index = family
  kTagNext = 403,
  kTagIcon = 404,    // index into the frame's icon-key table
  kTagCard = 407,    // index = family; the whole row, clicked to lay the recipe out
  kTagResults = 405,
  kTagBack = 406,
};

// js/ui/recipebook.js:12-21, verbatim.
struct FamilyName {
  const char* key;
  const char* name;
};
constexpr FamilyName kFamilyNames[] = {
    {"fam:stairs", "Stairs"},         {"fam:slabs", "Slabs"},
    {"fam:vslabs", "Vertical Slabs"}, {"fam:doors", "Doors"},
    {"fam:trapdoors", "Trapdoors"},   {"fam:bricks", "Bricks"},
    {"fam:polished", "Polished Stone"},
    {"fam:tool:pick", "Pickaxes"},    {"fam:tool:axe", "Axes"},
    {"fam:tool:shovel", "Shovels"},   {"fam:tool:sword", "Swords"},
    {"fam:armor:0", "Helmets"},       {"fam:armor:1", "Chestplates"},
    {"fam:armor:2", "Leggings"},      {"fam:armor:3", "Boots"},
};

struct TabDef {
  const char* id;
  const char* label;
};
constexpr TabDef kTabs[] = {
    {"all", "All"},         {"building", "Building"}, {"tools", "Tools"},
    {"armour", "Armour"},   {"materials", "Materials"}, {"smelting", "Smelting"},
};

const char* familyDisplayName(const std::string& key) {
  for (const FamilyName& f : kFamilyNames) {
    if (key == f.key) return f.name;
  }
  return nullptr;
}

bool startsWithWord(const std::string& s, const char* prefix) {
  const std::size_t n = std::strlen(prefix);
  if (s.size() < n) return false;
  if (s.compare(0, n, prefix) != 0) return false;
  return s.size() == n || s[n] == '_';
}

std::string toolFamilySuffix(game::ToolKind kind) {
  switch (kind) {
    case game::ToolKind::Pick: return "pick";
    case game::ToolKind::Axe: return "axe";
    case game::ToolKind::Shovel: return "shovel";
    case game::ToolKind::Sword: return "sword";
    case game::ToolKind::None: break;
  }
  return "none";
}

}  // namespace

std::string RecipeBook::familyKeyFor(const std::string& outKey) {
  const game::ItemDef* item = game::getItem(outKey);
  if (item && item->type == game::ItemType::Tool) {
    return "fam:tool:" + toolFamilySuffix(item->toolType);
  }
  if (item && item->type == game::ItemType::Armor) {
    return "fam:armor:" + std::to_string(item->armorSlot);
  }
  if (item && item->type == game::ItemType::Block) {
    const world::BlockDef& b = world::blocks().def(item->blockId);
    switch (b.render) {
      case world::RenderKind::Stair: return "fam:stairs";
      case world::RenderKind::Slab: return "fam:slabs";
      case world::RenderKind::VSlab: return "fam:vslabs";
      case world::RenderKind::Door: return "fam:doors";
      case world::RenderKind::Trapdoor: return "fam:trapdoors";
      default: break;
    }
  }
  // /^bricks(_|$)/ and /^polished(_|$)/ from the JS.
  if (startsWithWord(outKey, "bricks")) return "fam:bricks";
  if (startsWithWord(outKey, "polished")) return "fam:polished";
  return outKey;
}

std::string RecipeBook::categoryFor(const std::string& outKey) {
  const game::ItemDef* item = game::getItem(outKey);
  if (!item) return "materials";
  switch (item->type) {
    case game::ItemType::Tool: return "tools";
    case game::ItemType::Armor: return "armour";
    case game::ItemType::Block: return "building";
    default: return "materials";
  }
}

std::string RecipeBook::displayName(const std::string& key) {
  if (!key.empty() && key[0] == '#') {
    // "#planks" -> "Any Planks". Title case because this is a tooltip heading and
    // sits where an item's own name would.
    std::string s = key.substr(1);
    bool boundary = true;
    for (char& c : s) {
      if (c == '_') {
        c = ' ';
        boundary = true;
        continue;
      }
      if (boundary && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
      boundary = false;
    }
    return "Any " + s;
  }
  const game::ItemDef* item = game::getItem(key);
  return item ? item->name : key;
}

bool RecipeBook::fuzzy(const std::string& query, const std::string& subject) {
  if (query.empty()) return true;
  std::size_t i = 0;
  for (char c : subject) {
    if (i >= query.size()) break;
    if (std::tolower(static_cast<unsigned char>(c)) ==
        std::tolower(static_cast<unsigned char>(query[i]))) {
      ++i;
    }
  }
  return i == query.size();
}

// In a creative world this screen stops being a recipe browser and becomes an item
// picker: every item in the game, once each, laid out to be taken.
//
// The same screen rather than a new one, because everything a picker needs is
// already here and working — the category tabs, the fuzzy search, the icon atlas,
// the tooltip, and the click-to-take path the output icons already use. A second
// screen would have been a second copy of all of it, and the recipe book is the
// wrong thing to be looking at when you can simply have the item.
void RecipeBook::buildCreativeData() {
  families_.clear();
  for (const game::ItemDef& item : game::items().all()) {
    Entry e;
    e.outKey = item.key;
    // A stack, not one. Filling a hotbar slot at a time is what makes a creative
    // menu usable; a tool or a boat maxes at one anyway and the give clamps.
    e.outCount = std::min(64, std::max(1, item.maxStack));
    e.name = item.name;
    e.recipe = nullptr;
    // One item per family, so the ‹ › cycling never appears: in a picker every
    // variant is a separate thing you might want, not a variation on one recipe.
    Family f;
    f.key = "give:" + item.key;
    f.name = item.name;
    f.category = categoryFor(item.key);
    f.entries.push_back(std::move(e));
    families_.push_back(std::move(f));
  }
}

void RecipeBook::buildData() {
  if (creative_) {
    buildCreativeData();
    return;
  }
  families_.clear();
  const auto addEntry = [&](const std::string& familyKey, const std::string& category,
                            Entry entry) {
    for (Family& f : families_) {
      if (f.key == familyKey) {
        f.entries.push_back(std::move(entry));
        return;
      }
    }
    Family f;
    f.key = familyKey;
    const char* named = familyDisplayName(familyKey);
    f.name = named ? named : entry.name;
    f.category = category;
    f.entries.push_back(std::move(entry));
    families_.push_back(std::move(f));
  };

  for (const game::Recipe& r : game::recipeBook().recipes()) {
    Entry e;
    e.outKey = r.outKey;
    e.outCount = r.outCount;
    e.name = displayName(r.outKey);
    if (r.type == game::RecipeType::Shaped) {
      e.gridWidth = r.width;
      e.cells.assign(static_cast<std::size_t>(r.width * r.height), std::string {});
      for (const game::RecipeCell& c : r.cells) {
        e.cells[static_cast<std::size_t>(c.row * r.width + c.col)] = c.key;
      }
    } else {
      // One chip per SLOT, not one per ingredient with a count on it. The
      // shapeless matcher counts occupied cells rather than items, so the Atlas's
      // three paper are three separate slots — and a single chip reading "paper
      // x3" says the opposite, which is exactly how somebody stacks three paper
      // into one square and concludes the recipe is broken.
      for (const auto& [key, count] : r.ingredients) {
        for (int n = 0; n < std::max(1, count); ++n) e.chips.push_back({key, 1});
      }
    }
    e.recipe = &r;
    addEntry(familyKeyFor(r.outKey), categoryFor(r.outKey), std::move(e));
  }

  for (const game::SmeltingRecipe& s : game::recipeBook().smelting()) {
    Entry e;
    e.chips = {{s.in, 1}};
    e.outKey = s.out;
    e.outCount = 1;
    e.name = displayName(s.out);
    addEntry("smelt:" + s.out, "smelting", std::move(e));
  }
}

void RecipeBook::open() {
  if (!built_) {
    buildData();
    built_ = true;
  }
  search_.clear();
  search_.placeholder = creative_ ? "Search items\xE2\x80\xA6" : "Search recipes\xE2\x80\xA6";
  hoveredTag_ = 0;
}

void RecipeBook::close() { hoverKey_.clear(); }

IconRef RecipeBook::iconFor(const std::string& key) const {
  IconRef ref;
  if (!icons_) return ref;
  // A "#tag" ingredient shows its representative, which is what the JS tagRepr did.
  const std::string resolved = world::blocks().tagRepresentative(key);
  if (icons_->uvFor(resolved.empty() ? key : resolved, ref.u0, ref.v0, ref.u1, ref.v1)) {
    ref.texture = icons_->texture();
  }
  return ref;
}

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

void RecipeBook::cardNode(const Family& family, int familyIndex) {
  const Entry& e = family.entries[static_cast<std::size_t>(
      std::min<int>(family.current, static_cast<int>(family.entries.size()) - 1))];

  // A picker tile: the icon and the name, and the whole thing is the target. No
  // ingredients, no arrow — there is no recipe being shown, only a thing to take.
  if (creative_) {
    Style tile = Doc::row(8, Justify::Start, Align::Center);
    tile.bg = col(Role::PanelRaised);
    tile.border = col(Role::Edge);
    tile.borderWidth = 1;
    tile.radius = 8;
    tile.padding = Edges(6, 8);
    const bool hot = hoveredTag_ == kTagIcon && hoveredIndex_ < static_cast<int>(iconKeys_.size()) &&
                     iconKeys_[static_cast<std::size_t>(hoveredIndex_)] == e.outKey;
    if (hot) {
      tile.bg = col(Role::SlotFill);
      tile.border = col(Role::Accent);
    }
    doc_.begin(tile, kTagCard, familyIndex);
    doc_.icon(iconFor(e.outKey), px(Scalar::RecipeIconOut), px(Scalar::RecipeIconOut), {}, kTagIcon,
              static_cast<int>(iconKeys_.size()));
    iconKeys_.push_back(e.outKey);
    iconIsOutput_.push_back(true);
    outCounts_.push_back(e.outCount);
    Style nameBox;
    nameBox.maxWidth = 120;
    nameBox.ellipsis = true;
    nameBox.grow = 1;
    doc_.label(e.name, widget::rbName(), nameBox);
    doc_.end();
    return;
  }

  // .rb-card { display: flex; align-items: center; gap: 8px; padding: 8px 10px }
  Style card = Doc::row(8, Justify::Start, Align::Center);
  card.bg = col(Role::PanelRaised);
  card.border = col(Role::Edge);
  card.borderWidth = 1;
  card.radius = 8;
  card.padding = Edges(8, 10);
  // The whole row is the button, the way Minecraft's book works — there is nothing
  // else on a card to click, and hunting for a small target is the opposite of what
  // this is for.
  if (hoveredTag_ == kTagCard && hoveredIndex_ == familyIndex && e.recipe) {
    card.bg = col(Role::SlotFill);
    card.border = col(Role::Accent);
  }
  doc_.begin(card, kTagCard, familyIndex);

  // The inputs: a grid for a shaped recipe, chips for a shapeless one.
  if (!e.cells.empty()) {
    Style grid;
    grid.display = Display::Grid;
    grid.gridCols = e.gridWidth;
    grid.gridColWidth = px(Scalar::RecipeCell);
    grid.gap = 2;
    doc_.begin(grid);
    for (const std::string& cellKey : e.cells) {
      const bool empty = cellKey.empty();
      doc_.begin(widget::rbCell(empty));
      if (!empty) {
        doc_.icon(iconFor(cellKey), px(Scalar::RecipeIcon), px(Scalar::RecipeIcon), {}, kTagIcon,
                  static_cast<int>(iconKeys_.size()));
        iconKeys_.push_back(cellKey);
        iconIsOutput_.push_back(false);
      outCounts_.push_back(0);
        outCounts_.push_back(0);
      }
      doc_.end();
    }
    doc_.end();
  } else {
    // .rb-chips { display: flex; flex-wrap: wrap; gap: 4px }
    Style chips = Doc::row(4, Justify::Start, Align::Center);
    chips.wrap = true;
    chips.maxWidth = 90;
    doc_.begin(chips);
    for (const auto& [key, count] : e.chips) {
      doc_.icon(iconFor(key), px(Scalar::RecipeIcon), px(Scalar::RecipeIcon), {}, kTagIcon,
                static_cast<int>(iconKeys_.size()));
      iconKeys_.push_back(key);
      iconIsOutput_.push_back(false);
      if (count > 1) {
        // .rb-chip i — the count, bottom-right of the chip. Absolutely positioned in
        // the CSS, so it is drawn in draw() rather than laid out.
        chipCounts_.push_back({static_cast<int>(iconKeys_.size()) - 1, count});
      }
    }
    doc_.end();
  }

  TextStyle go;
  go.size = 16;
  go.color = col(Role::Muted);
  doc_.label("\xE2\x9E\xA4", go);  // ➤

  // .rb-out { display: flex; align-items: center; gap: 6px; min-width: 0 }
  Style out = Doc::row(6, Justify::Start, Align::Center);
  out.grow = 1;
  out.minWidth = 0;
  doc_.begin(out);
  doc_.icon(iconFor(e.outKey), px(Scalar::RecipeIconOut), px(Scalar::RecipeIconOut), {}, kTagIcon,
            static_cast<int>(iconKeys_.size()));
  iconKeys_.push_back(e.outKey);
  iconIsOutput_.push_back(true);
  outCounts_.push_back(e.outCount);

  Style meta = Doc::column(2, Align::Start);
  meta.minWidth = 0;
  doc_.begin(meta);
  std::string label = e.name;
  if (e.outCount > 1) label = std::to_string(e.outCount) + "\xC3\x97 " + e.name;
  // .rb-name { white-space: nowrap; overflow: hidden; text-overflow: ellipsis }
  Style nameBox;
  nameBox.maxWidth = 130;
  nameBox.ellipsis = true;
  doc_.label(label, widget::rbName(), nameBox);

  if (family.entries.size() > 1) {
    // .rb-cyc — ‹ 2/5 › in muted 11px.
    Style cyc = Doc::row(4, Justify::Start, Align::Center);
    doc_.begin(cyc);
    for (int tag : {kTagPrev, kTagNext}) {
      const bool hovered = hoveredTag_ == tag && hoveredIndex_ == familyIndex;
      doc_.begin(widget::rbArrow(hovered), tag, familyIndex);
      TextStyle ts;
      ts.size = 12;
      ts.color = hovered ? col(Role::AccentInk) : col(Role::Text);
      doc_.label(tag == kTagPrev ? "\xE2\x80\xB9" : "\xE2\x80\xBA", ts);
      doc_.end();
      if (tag == kTagPrev) {
        char index[24];
        std::snprintf(index, sizeof(index), "%d/%d", family.current + 1,
                      static_cast<int>(family.entries.size()));
        Style box;
        box.width = 30;
        doc_.label(index, widget::muted(11.0f), box);
        doc_.node(doc_.count() - 1).textAlign = TextAlign::Center;
      }
    }
    doc_.end();
  }
  doc_.end();
  doc_.end();
  doc_.end();
}

void RecipeBook::build(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens) {
  doc_.reset(&text);
  iconKeys_.clear();
  iconIsOutput_.clear();
  outCounts_.clear();
  chipCounts_.clear();

  doc_.begin(widget::screen());

  // .menu-card.wide.rb-window { width: 760px; max-height: 88vh }
  Style card = widget::menuCard(true);
  card.width = std::min(760.0f, ui.width() * 0.94f);
  card.maxHeight = ui.height() * 0.88f;
  card.display = Display::Flex;
  card.dir = Dir::Column;
  doc_.begin(card);

  // Title left, the way back right. The book is reached from the crafting screens
  // as well as from the world, and Escape alone is not a visible way out of it.
  Style titleRow = Doc::row(10, Justify::SpaceBetween, Align::Center);
  titleRow.margin = Edges(0, 0, 18, 0);
  doc_.begin(titleRow);
  doc_.label(creative_ ? "Creative Menu" : "Recipe Book", widget::h2());
  {
    const bool hovered = hoveredTag_ == kTagBack;
    Style s = widget::btnSmall(hovered, false, widget::ButtonKind::Normal);
    s.width = kAuto;
    s.margin = Edges(0);
    doc_.begin(s, kTagBack);
    doc_.label("Back \xC2\xB7 H", widget::btnSmallText(hovered, widget::ButtonKind::Normal));
    doc_.end();
  }
  doc_.end();

  // .rb-controls { display: flex; flex-wrap: wrap; gap: 8px; padding: 4px 0 10px }
  Style controls = Doc::row(8, Justify::Start, Align::Center);
  controls.wrap = true;
  controls.padding = Edges(4, 0, 10, 0);
  doc_.begin(controls);
  doc_.box(widget::searchInput(search_.focused()), kTagSearch);
  Style tabs = Doc::row(4, Justify::Start, Align::Center);
  tabs.wrap = true;
  doc_.begin(tabs);
  for (std::size_t i = 0; i < std::size(kTabs); ++i) {
    const bool active = tab_ == kTabs[i].id;
    const bool hovered = hoveredTag_ == kTagTab && hoveredIndex_ == static_cast<int>(i);
    doc_.begin(widget::rbTab(active, hovered), kTagTab, static_cast<int>(i));
    TextStyle ts;
    ts.size = 12;
    ts.color = active ? col(Role::AccentInk) : (hovered ? col(Role::Text) : col(Role::Muted));
    if (active) ts.font = FontId::SansBold;
    doc_.label(kTabs[i].label, ts);
    doc_.end();
  }
  doc_.end();
  doc_.end();

  // Filter: the tab, then the fuzzy query. A family whose own name does not match but
  // one of whose variants does jumps the card to that variant, exactly as the JS did.
  visible_.clear();
  const std::string query = search_.text();
  for (std::size_t i = 0; i < families_.size(); ++i) {
    Family& f = families_[i];
    if (tab_ != "all" && f.category != tab_) continue;
    if (!query.empty() && !fuzzy(query, f.name)) {
      int hit = -1;
      for (std::size_t k = 0; k < f.entries.size(); ++k) {
        if (fuzzy(query, f.entries[k].name)) {
          hit = static_cast<int>(k);
          break;
        }
      }
      if (hit < 0) continue;
      f.current = hit;
    }
    visible_.push_back(static_cast<int>(i));
  }

  // .rb-results { grid-template-columns: repeat(auto-fill, minmax(220px, 1fr)); gap: 8px }
  Style results = Doc::column(0, Align::Stretch);
  results.scrollY = true;
  results.grow = 1;
  results.maxHeight = ui.height() * 0.88f - 150.0f;
  doc_.begin(results, kTagResults);
  if (visible_.empty()) {
    Style empty;
    empty.padding = Edges(20, 4);
    doc_.label("No recipes match \xE2\x80\x9C" + query + "\xE2\x80\x9D.", widget::muted(14.0f),
               empty);
  } else {
    Style grid;
    grid.display = Display::Grid;
    grid.gridCols = 0;
    grid.gridMinCol = creative_ ? 170 : 220;
    grid.gap = 8;
    doc_.begin(grid);
    for (int index : visible_) cardNode(families_[static_cast<std::size_t>(index)], index);
    doc_.end();
  }
  doc_.end();

  Style foot;
  foot.margin = Edges(10, 0, 2, 0);
  foot.maxWidth = 700;
  doc_.label(creative_
                 ? "Click anything to take a stack of it. Tools and boats come one at a "
                   "time; anything that does not fit is dropped at your feet."
                 : "Tip: many recipes group \xE2\x80\x94 use the \xE2\x80\xB9 \xE2\x80\xBA "
                   "arrows to cycle materials/tiers. Forge fuels: coal, charcoal, and "
                   "anything wooden (logs, planks, tools, chests, boats, torches\xE2\x80\xA6).",
             widget::rbFoot(), foot);

  doc_.end();
  doc_.end();
  doc_.layout({0, 0, ui.width(), ui.height()});
  (void)event;
  (void)tweens;
}

void RecipeBook::handle(const UiEvent& event, Text& text) {
  const int hit = doc_.hitTest(event.mouseX, event.mouseY);
  // main.js:120 hung one listener on the document and ticked whenever the click
  // landed inside a <button>; this is that listener.
  if (event.leftClick && doc_.clickedButton(hit)) audio::sfx::uiClick();
  hoveredTag_ = hit >= 0 ? doc_.node(hit).tag : 0;
  hoveredIndex_ = hit >= 0 ? doc_.node(hit).index : 0;
  mouseX_ = event.mouseX;
  mouseY_ = event.mouseY;

  hoverKey_.clear();
  if (hoveredTag_ == kTagIcon && hoveredIndex_ < static_cast<int>(iconKeys_.size())) {
    hoverKey_ = iconKeys_[static_cast<std::size_t>(hoveredIndex_)];
  }

  if (event.wheel != 0.0f) {
    const int node = doc_.findTag(kTagResults);
    if (node >= 0 && doc_.node(node).rect.contains(event.mouseX, event.mouseY)) {
      doc_.scroll(kTagResults).offset += event.wheel * 48.0f;
    }
  }

  bool submitted = false;
  const bool changed = search_.handle(event, submitted);
  if (changed) doc_.scroll(kTagResults).offset = 0;

  if (!event.leftClick) return;
  Input* input = const_cast<Input*>(event.input);
  search_.setFocused(hoveredTag_ == kTagSearch, input);
  if (hoveredTag_ == kTagSearch) {
    search_.placeCaretAt(text, doc_.rectOf(kTagSearch).inset(10), widget::rbName(),
                         event.mouseX);
    return;
  }
  if (hoveredTag_ == kTagBack) {
    if (onBack) onBack();
    return;
  }
  // An output icon, in a world that allows it: have one.
  //
  // Ahead of the card handler below rather than inside it, because Doc::hitTest
  // returns the topmost node and an icon always shadows the card it sits on — so a
  // click on the result never reaches kTagCard at all. Ingredients are excluded:
  // half of them are "#tag" strings that no inventory would accept.
  if (hoveredTag_ == kTagIcon && hoveredIndex_ < static_cast<int>(iconKeys_.size()) &&
      iconIsOutput_[static_cast<std::size_t>(hoveredIndex_)]) {
    if (onGive && canGive && canGive()) {
      onGive(iconKeys_[static_cast<std::size_t>(hoveredIndex_)],
             outCounts_[static_cast<std::size_t>(hoveredIndex_)]);
      return;
    }
  }
  if (hoveredTag_ == kTagTab && hoveredIndex_ < static_cast<int>(std::size(kTabs))) {
    tab_ = kTabs[static_cast<std::size_t>(hoveredIndex_)].id;
    doc_.scroll(kTagResults).offset = 0;
    return;
  }
  if (hoveredTag_ == kTagPrev || hoveredTag_ == kTagNext) {
    if (hoveredIndex_ < static_cast<int>(families_.size())) {
      Family& f = families_[static_cast<std::size_t>(hoveredIndex_)];
      const int n = static_cast<int>(f.entries.size());
      const int delta = hoveredTag_ == kTagNext ? 1 : -1;
      f.current = ((f.current + delta) % n + n) % n;
    }
    return;
  }
  if (hoveredTag_ == kTagCard && hoveredIndex_ < static_cast<int>(families_.size())) {
    const Family& f = families_[static_cast<std::size_t>(hoveredIndex_)];
    const Entry& e = f.entries[static_cast<std::size_t>(
        std::min<int>(f.current, static_cast<int>(f.entries.size()) - 1))];
    // The variant on show is the one you get. Cycling with the arrows is how you
    // choose which wood or which tier, so clicking has to mean that one and not
    // the family's first.
    if (e.recipe && onAutoFill) onAutoFill(*e.recipe);
  }
}

void RecipeBook::update(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens) {
  time_ += event.dt;
  build(ui, text, event, tweens);
  handle(event, text);
  build(ui, text, event, tweens);
}

void RecipeBook::draw(Ui2D& ui, Text& text) {
  ui.fillRect({0, 0, ui.width(), ui.height()}, col(Role::WashScreen));
  doc_.paint(ui);

  // The chip counts, which the CSS positions absolutely over the bottom-right of a chip.
  for (const auto& [iconIndex, count] : chipCounts_) {
    const int node = doc_.findTag(kTagIcon, iconIndex);
    if (node < 0) continue;
    const Rect r = doc_.node(node).rect;
    TextStyle ts;
    ts.font = FontId::SansBlack;
    ts.size = 11;
    ts.color = kWhite;
    ts.withShadow(1, 1, 0, kBlack);
    const std::string label = std::to_string(count);
    const TextMetrics m = text.metrics(ts);
    text.draw(ui, r.right() + 2 - text.measure(label, ts), r.bottom() + 3 - m.descent, label,
              ts);
  }

  const int searchNode = doc_.findTag(kTagSearch);
  if (searchNode >= 0) {
    search_.draw(ui, text, doc_.node(searchNode).rect.inset(10), widget::rbName(), time_);
  }

  if (!hoverKey_.empty()) {
    const std::string resolved = world::blocks().tagRepresentative(hoverKey_);
    const std::string key = resolved.empty() ? hoverKey_ : resolved;
    const game::Tooltip tip = game::itemTooltip(key, -1, game::fuelValue(key));

    // An ingredient TAG is not the item it happens to resolve to. Naming it "Oak
    // Planks" is how a player in a pine forest concludes they cannot make a
    // pickaxe — which was a real bug in the recipe itself until recently, and
    // this label was still telling them it was true. Name the tag, then list
    // what satisfies it.
    if (hoverKey_.front() == '#') {
      std::vector<std::string> lines;
      if (const std::vector<std::string>* members = world::blocks().tag(hoverKey_.substr(1))) {
        std::string list;
        for (const std::string& m : *members) {
          const game::ItemDef* def = game::getItem(m);
          if (!def) continue;
          if (!list.empty()) list += ", ";
          list += def->name;
        }
        if (!list.empty()) lines.push_back(list);
      }
      widget::drawTooltip(ui, text, mouseX_, mouseY_, displayName(hoverKey_), lines);
      return;
    }
    widget::drawTooltip(ui, text, mouseX_, mouseY_, tip.name, tip.lines);
  }
}

}  // namespace hr::ui
