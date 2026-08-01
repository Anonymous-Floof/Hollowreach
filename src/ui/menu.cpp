#include "ui/menu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "audio/sfx.h"
#include "core/log.h"
#include "core/prng.h"
#include "ui/imagefx.h"

namespace hr::ui {
namespace {

// Node tags. Values are arbitrary but stable, because the tween store keys off them.
enum : int {
  kTagNone = 0,
  kTagPlay = 10,
  kTagJoin,
  kTagGallery,
  kTagSettings,
  kTagAbout,
  kTagQuit,
  kTagBack,
  kTagNewWorld,
  kTagImport,
  kTagWorldRow,     // index = row
  kTagWorldPlay,
  kTagWorldExport,
  kTagWorldDelete,
  kTagCreate,
  kTagCancel,
  kTagNameField,
  kTagSeedField,
  kTagScrollAbout,
  kTagScrollWorlds,
  kTagJoinField,
  kTagJoinGo,
  kTagLanRow,        // index = row
  kTagScrollLan,

  kTagResume = 60,
  kTagMultiplayer,
  kTagPauseSettings,
  kTagPauseGallery,
  kTagPauseExport,
  kTagPauseQuit,
  kTagCopyInvite,
};

// The two feature and control tables from js/ui/menu.js:35-49, verbatim.
struct Feature {
  const char* title;
  const char* body;
};
// Kept in step with README.md deliberately: this is the same claim list, and a
// player reads whichever one is nearer to hand. See docs/RELEASING.md — updating
// both is a release step, not an afterthought.
constexpr Feature kFeatures[] = {
    {"Custom engine", "Its own OpenGL renderer, mesher and physics \xE2\x80\x94 no engine, "
                      "no game libraries."},
    {"Deferred lighting", "Coloured point lights, a directional sun with soft cast shadows, "
                          "SSAO and god-rays."},
    {"Volumetric skies", "Ray-marched cumulus with moving shadows, a full day/night cycle, "
                         "sun, moon and stars."},
    {"Living water", "A flowing-water automaton with reflections, currents and an "
                     "underwater view."},
    {"A world with depth", "192 blocks tall, a hundred of them underground: caves, "
                           "ravines, and ore in bands you have to dig for."},
    {"Things fall down", "Sand collapses, plants need ground under them, and a flood "
                         "washes away what it reaches."},
    {"Survive & build", "Mine, smelt at the Forge, climb the tool & armour tiers, farm "
                        "mobs, sleep, and build."},
    {"Play together", "Open a world to your network; friends on it see you, your edits "
                      "and each other."},
};

struct Control {
  const char* key;
  const char* action;
};
constexpr Control kControls[] = {
    {"WASD", "Move"},
    {"Mouse", "Look around"},
    {"Ctrl", "Sprint"},
    {"Space", "Jump \xC2\xB7 double-tap to fly"},
    {"L-click", "Break block"},
    {"R-click", "Place / use / eat"},
    {"M-click", "Pick block to hand"},
    // The en dash has to end its own literal: "\x939" would be read as one escape.
    {"1\xE2\x80\x93" "9 \xC2\xB7 Wheel", "Select hotbar"},
    {"E", "Inventory"},
    {"H", "Recipe book"},
    {"M", "Atlas map"},
    {"Q", "Drop item (Ctrl+Q: stack)"},
    {"F1", "Hide the interface"},
    {"F2", "Screenshot"},
    {"F3", "Debug overlay"},
    {"Esc", "Pause"},
};

constexpr const char* kTitleText = "HOLLOWREACH";
// .menu-glass .game-title
constexpr float kTitleSize = 54.0f;
constexpr float kTitleLetterSpacing = 10.0f;
// The title is rasterised at a fixed 2x so it stays crisp when the interface is scaled
// up, and is drawn as a texture at its CSS size.
constexpr float kTitleRaster = 2.0f;

std::string timeAgo(double seconds) {
  if (seconds <= 0) return "\xE2\x80\x94";
  if (seconds < 60) return "just now";
  char buffer[32];
  if (seconds < 3600) {
    std::snprintf(buffer, sizeof(buffer), "%dm ago", static_cast<int>(seconds / 60));
  } else if (seconds < 86400) {
    std::snprintf(buffer, sizeof(buffer), "%dh ago", static_cast<int>(seconds / 3600));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%dd ago", static_cast<int>(seconds / 86400));
  }
  return buffer;
}

}  // namespace

bool Menu::init(Text& text) { return buildTitleTexture(text); }

void Menu::destroy() {
  if (titleTexture_) glDeleteTextures(1, &titleTexture_);
  titleTexture_ = 0;
}

// .menu-glass .game-title:
//   background: linear-gradient(180deg, #eafff0 0%, #9fe3a6 52%, #5aa564 100%);
//   background-clip: text; -webkit-text-fill-color: transparent;
//   filter: drop-shadow(0 2px 0 #2f6238) drop-shadow(0 12px 22px rgba(0,0,0,0.6));
bool Menu::buildTitleTexture(Text& text) {
  TextStyle ts;
  ts.font = FontId::SansBlack;
  ts.size = kTitleSize;
  ts.letterSpacing = kTitleLetterSpacing;
  ts.color = color::white;

  Image glyphs = text.rasterise(kTitleText, ts, kTitleRaster);
  if (glyphs.empty()) {
    log::warn("menu title failed to rasterise");
    return true;  // not fatal; the menu simply has no title art
  }

  applyVerticalGradient(glyphs, {{0.0f, color::titleTop},
                                 {0.52f, color::titleMid},
                                 {1.0f, color::titleBottom}});

  // Two stacked drop-shadows. CSS applies them outside-in: the second filter blurs the
  // result of the first, but with a hard 0-blur first shadow the visible result is the
  // same as compositing both under the glyphs, which is what this does.
  const Image hard = dropShadowLayer(glyphs, 0.0f, color::primaryEdge);
  const Image soft = dropShadowLayer(glyphs, 22.0f * kTitleRaster, rgba(0, 0, 0, 0.6));

  // Room for the largest offset plus the soft blur's spread.
  const int padX = soft.width() / 2 - glyphs.width() / 2 + 4;
  const int padTop = 4;
  const int padBottom = static_cast<int>(std::ceil(12.0f * kTitleRaster)) + padX;
  Image out(glyphs.width() + padX * 2, glyphs.height() + padTop + padBottom,
            Rgba {0, 0, 0, 0});

  const int softX = padX - (soft.width() - glyphs.width()) / 2;
  const int softY = padTop + static_cast<int>(std::lround(12.0f * kTitleRaster)) -
                    (soft.height() - glyphs.height()) / 2;
  compositeOver(out, soft, softX, softY);
  const int hardX = padX - (hard.width() - glyphs.width()) / 2;
  const int hardY = padTop + static_cast<int>(std::lround(2.0f * kTitleRaster)) -
                    (hard.height() - glyphs.height()) / 2;
  compositeOver(out, hard, hardX, hardY);
  compositeOver(out, glyphs, padX, padTop);

  titleWidth_ = out.width();
  titleHeight_ = out.height();
  glGenTextures(1, &titleTexture_);
  glBindTexture(GL_TEXTURE_2D, titleTexture_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, out.width(), out.height(), 0, GL_RGBA,
               GL_UNSIGNED_BYTE, out.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  return true;
}

void Menu::setPage(MenuPage page) {
  page_ = page;
  if (page == MenuPage::Worlds) worldsLoaded_ = false;
  if (page == MenuPage::Join) {
    joinField_.clear();
    joinField_.placeholder = "HRW1\xE2\x80\xA6 or 192.168.1.7";
    joinField_.maxLength = 120;
    lanGames_.clear();
    lanRefresh_ = 0;
  }
  if (page == MenuPage::NewWorld) {
    nameField_.clear();
    nameField_.placeholder = "New World";
    nameField_.maxLength = 28;
    seedField_.clear();
    seedField_.placeholder = "random";
    seedField_.maxLength = 24;
  }
}

void Menu::blurFields(Input* input) {
  nameField_.setFocused(false, input);
  seedField_.setFocused(false, input);
  joinField_.setFocused(false, input);
}

void Menu::onShow() {
  setPage(MenuPage::Main);
  hoveredTag_ = 0;
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

void Menu::buildPageMain(const UiEvent& event, TweenStore& tweens) {
  struct Item {
    int tag;
    const char* label;
    widget::ButtonKind kind;
  };
  const Item items[] = {
      {kTagPlay, "Play", widget::ButtonKind::Primary},
      {kTagJoin, "Join a Friend", widget::ButtonKind::Normal},
      {kTagGallery, "Gallery", widget::ButtonKind::Normal},
      {kTagSettings, "Settings", widget::ButtonKind::Normal},
      {kTagAbout, "About", widget::ButtonKind::Normal},
      // Last, and the only Danger button here. Escape has always quit from this
      // screen; this is the same exit with a label on it, since nothing else says
      // the game can be left at all.
      {kTagQuit, "Quit", widget::ButtonKind::Danger},
  };
  for (const Item& item : items) {
    const bool hovered = hoveredTag_ == item.tag;
    Style s = widget::menuButton(hovered, item.kind);
    // #menu-root > .btn:hover { transform: translateX(3px) }
    s.translateX = tweens.toward(TweenStore::key(item.tag), hovered, 0.12) * 3.0f;
    const int node = main_.begin(s, item.tag);
    main_.label(item.label, widget::menuButtonText(hovered, item.kind));
    main_.end();
    (void)node;

    // The ::before accent bar, scaleY(0) -> scaleY(1) over .18s. Drawn as a custom node
    // so the bar's height can be animated independently of its slot in the layout.
    (void)event;
  }
}

void Menu::buildPageWorlds(const UiEvent& event, TweenStore& tweens) {
  if (!worldsLoaded_) {
    worlds_ = actions.listWorlds ? actions.listWorlds() : std::vector<WorldEntry> {};
    worldsLoaded_ = true;
  }

  // .world-list { max-height: 46vh; overflow-y: auto }
  Style list = Doc::column(0, Align::Stretch);
  list.scrollY = true;
  list.maxHeight = 0.46f * 900.0f;  // replaced below with the real viewport height
  main_.begin(list, kTagScrollWorlds);

  if (worlds_.empty()) {
    Style note;
    note.margin = Edges(16, 0);
    main_.label("No worlds yet \xE2\x80\x94 create one below.", widget::emptyNote(), note);
  }
  for (std::size_t i = 0; i < worlds_.size(); ++i) {
    const WorldEntry& w = worlds_[i];
    // .world-row { display:flex; justify-content:space-between; align-items:center }
    Style row = Doc::row(0, Justify::SpaceBetween, Align::Center);
    row.bg = color::panel2;
    row.border = color::edge;
    row.borderWidth = 2;
    row.radius = 9;
    row.padding = Edges(10, 14);
    row.margin = Edges(8, 0);
    main_.begin(row, kTagWorldRow, static_cast<int>(i));

    main_.begin(Doc::column(0, Align::Start));
    main_.label(w.name.empty() ? "Unnamed" : w.name, widget::worldName());
    char meta[96];
    std::snprintf(meta, sizeof(meta), "seed %u \xC2\xB7 saved %s", w.seed,
                  timeAgo(w.ageSeconds).c_str());
    main_.label(meta, widget::worldMeta());
    main_.end();

    main_.begin(Doc::row(10, Justify::End, Align::Center));
    struct RowButton {
      int tag;
      const char* label;
      widget::ButtonKind kind;
    };
    for (const RowButton& b : {RowButton {kTagWorldPlay, "Play", widget::ButtonKind::Normal},
                               RowButton {kTagWorldExport, "Export", widget::ButtonKind::Normal},
                               RowButton {kTagWorldDelete, "Delete",
                                          widget::ButtonKind::Danger}}) {
      const bool hovered = hoveredTag_ == b.tag && hoveredIndex_ == static_cast<int>(i);
      Style s = widget::btnSmall(hovered, false, b.kind);
      s.width = kAuto;
      main_.begin(s, b.tag, static_cast<int>(i));
      main_.label(b.label, widget::btnSmallText(hovered, b.kind));
      main_.end();
    }
    main_.end();
    main_.end();
  }
  main_.end();

  struct Action {
    int tag;
    const char* label;
    widget::ButtonKind kind;
  };
  for (const Action& a : {Action {kTagNewWorld, "Create New World", widget::ButtonKind::Primary},
                          // The browser opened a file picker here. A native build
                          // has no dialog to open without a dependency, so the
                          // exports folder is the drop box and the label says so.
                          Action {kTagImport, "Import from data/exports",
                                  widget::ButtonKind::Normal},
                          Action {kTagBack, "Back", widget::ButtonKind::Normal}}) {
    const bool hovered = hoveredTag_ == a.tag;
    Style s = widget::btn(hovered, false, a.kind);
    main_.begin(s, a.tag);
    main_.label(a.label, widget::btnText(hovered, a.kind));
    main_.end();
  }
  (void)event;
  (void)tweens;
}

void Menu::buildPageNewWorld(const UiEvent& event, TweenStore& tweens) {
  Style label;
  label.margin = Edges(10, 0, 0, 0);
  main_.label("World name", widget::fieldLabel(), label);
  main_.box(widget::textInput(nameField_.focused()), kTagNameField);
  main_.label("Seed (leave blank for random)", widget::fieldLabel(), label);
  main_.box(widget::textInput(seedField_.focused()), kTagSeedField);

  // .row.end { justify-content: flex-end; margin-top: 14px }
  Style row = Doc::row(10, Justify::End, Align::Center);
  row.margin = Edges(14, 0, 0, 0);
  main_.begin(row);
  struct Action {
    int tag;
    const char* label;
    widget::ButtonKind kind;
  };
  for (const Action& a : {Action {kTagCancel, "Cancel", widget::ButtonKind::Normal},
                          Action {kTagCreate, "Create", widget::ButtonKind::Primary}}) {
    const bool hovered = hoveredTag_ == a.tag;
    Style s = widget::btnSmall(hovered, false, a.kind);
    s.width = kAuto;
    main_.begin(s, a.tag);
    main_.label(a.label, widget::btnSmallText(hovered, a.kind));
    main_.end();
  }
  main_.end();
  (void)event;
  (void)tweens;
}

void Menu::buildPageJoin(const UiEvent& event, TweenStore& tweens) {
  // What the web build had here was a pair of textareas for pasting WebRTC
  // session descriptions back and forth. ENet needs an address, so the panel
  // becomes the two ways of getting one: games heard on this network, and a code
  // a friend sent you.
  Style heading;
  heading.margin = Edges(2, 0, 6, 0);
  main_.label("Games on this network", widget::fieldLabel(), heading);

  Style list = Doc::column(0, Align::Stretch);
  list.scrollY = true;
  list.maxHeight = 0.30f * 900.0f;
  main_.begin(list, kTagScrollLan);
  if (lanGames_.empty()) {
    Style note;
    note.margin = Edges(10, 0);
    main_.label("Listening\xE2\x80\xA6 a game opened to the LAN appears here within a second.",
                widget::emptyNote(), note);
  }
  for (std::size_t i = 0; i < lanGames_.size(); ++i) {
    const LanGame& g = lanGames_[i];
    Style row = Doc::row(0, Justify::SpaceBetween, Align::Center);
    row.bg = color::panel2;
    row.border = hoveredTag_ == kTagLanRow && hoveredIndex_ == static_cast<int>(i)
                     ? color::accent
                     : color::edge;
    row.borderWidth = 2;
    row.radius = 9;
    row.padding = Edges(9, 13);
    row.margin = Edges(6, 0);
    main_.begin(row, kTagLanRow, static_cast<int>(i));

    main_.begin(Doc::column(0, Align::Start));
    main_.label(g.worldName.empty() ? "Hollowreach" : g.worldName, widget::worldName());
    char meta[128];
    std::snprintf(meta, sizeof(meta), "%s \xC2\xB7 %s \xC2\xB7 %d/%d",
                  g.hostName.empty() ? "?" : g.hostName.c_str(), g.address.c_str(), g.players,
                  g.maxPlayers);
    main_.label(meta, widget::worldMeta());
    main_.end();

    const bool hovered = hoveredTag_ == kTagLanRow && hoveredIndex_ == static_cast<int>(i);
    Style go = widget::btnSmall(hovered, false, widget::ButtonKind::Normal);
    go.width = kAuto;
    main_.begin(go);
    main_.label("Join", widget::btnSmallText(hovered, widget::ButtonKind::Normal));
    main_.end();
    main_.end();
  }
  main_.end();

  Style label;
  label.margin = Edges(12, 0, 0, 0);
  main_.label("Or paste an invite code or address", widget::fieldLabel(), label);
  main_.box(widget::textInput(joinField_.focused()), kTagJoinField);

  Style row = Doc::row(10, Justify::End, Align::Center);
  row.margin = Edges(14, 0, 0, 0);
  main_.begin(row);
  struct Action {
    int tag;
    const char* label;
    widget::ButtonKind kind;
  };
  for (const Action& a : {Action {kTagBack, "Back", widget::ButtonKind::Normal},
                          Action {kTagJoinGo, "Connect", widget::ButtonKind::Primary}}) {
    const bool hovered = hoveredTag_ == a.tag;
    Style s = widget::btnSmall(hovered, false, a.kind);
    s.width = kAuto;
    main_.begin(s, a.tag);
    main_.label(a.label, widget::btnSmallText(hovered, a.kind));
    main_.end();
  }
  main_.end();
  (void)event;
  (void)tweens;
}

void Menu::buildPageAbout(const UiEvent& event, TweenStore& tweens) {
  // .about { width: min(470px, 82vw); max-height: 58vh; overflow-y: auto; line-height: 1.62 }
  Style about = Doc::column(0, Align::Stretch);
  about.scrollY = true;
  about.width = 470;
  about.maxHeight = 0.58f * 900.0f;
  about.padding = Edges(0, 8, 0, 0);
  main_.begin(about, kTagScrollAbout);

  Style para;
  para.margin = Edges(0, 0, 10, 0);
  para.maxWidth = 462;
  main_.label("Hollowreach is a vibecoded voxel sandbox built entirely from scratch "
              "\xE2\x80\x94 its own engine, procedural world, textures and models, with no "
              "game engine and no external libraries. It began in the browser and was "
              "rewritten in C++ to run as a native application.",
              widget::aboutLead(), para);

  Style heading;
  heading.margin = Edges(18, 0, 9, 0);
  main_.label("Features", widget::aboutHeading(), heading);

  // .feature-grid { grid-template-columns: 1fr 1fr; gap: 8px }
  Style featureGrid;
  featureGrid.display = Display::Grid;
  featureGrid.gridCols = 2;
  featureGrid.gap = 8;
  featureGrid.margin = Edges(2, 0, 4, 0);
  main_.begin(featureGrid);
  for (const Feature& f : kFeatures) {
    Style card = Doc::column(3, Align::Stretch);
    card.bg = rgba(255, 255, 255, 0.04);
    card.border = rgba(255, 255, 255, 0.07);
    card.borderWidth = 1;
    card.radius = 9;
    card.padding = Edges(9, 11);
    main_.begin(card);
    main_.label(f.title, widget::featTitle());
    Style body;
    body.maxWidth = 200;
    main_.label(f.body, widget::featBody(), body);
    main_.end();
  }
  main_.end();

  main_.label("Controls", widget::aboutHeading(), heading);

  // .controls-grid { grid-template-columns: auto 1fr; gap: 5px 12px; align-items: baseline }
  Style controlGrid;
  controlGrid.display = Display::Grid;
  controlGrid.gridCols = 2;
  controlGrid.gridFirstColAuto = true;
  controlGrid.gap = 12;
  controlGrid.rowGap = 5;
  controlGrid.align = Align::Start;
  main_.begin(controlGrid);
  for (const Control& c : kControls) {
    // .kbd — a small monospace key cap with a heavier bottom border.
    Style capCell = Doc::row(0, Justify::End, Align::Center);
    main_.begin(capCell);
    Style cap = Doc::row(0, Justify::Center, Align::Center);
    cap.bg = color::inputBg;
    cap.border = color::slot;
    cap.borderWidth = 1;
    cap.radius = 5;
    cap.padding = Edges(1, 7, 2, 7);
    main_.begin(cap);
    main_.label(c.key, widget::kbd());
    main_.end();
    main_.end();
    main_.label(c.action, widget::controlValue());
  }
  main_.end();

  main_.label("Your worlds", widget::aboutHeading(), heading);
  main_.label("Worlds save automatically beside the executable and can be exported to a "
              ".world file to back up or share with friends, then imported anywhere.",
              widget::aboutBody(), para);
  main_.end();

  const bool hovered = hoveredTag_ == kTagBack;
  Style s = widget::btn(hovered, false, widget::ButtonKind::Normal);
  main_.begin(s, kTagBack);
  main_.label("Back", widget::btnText(hovered, widget::ButtonKind::Normal));
  main_.end();
  (void)event;
  (void)tweens;
}

// ---------------------------------------------------------------------------
// Main menu card
// ---------------------------------------------------------------------------

void Menu::buildMainCard(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens,
                         const std::string& version) {
  main_.reset(&text);
  Style root = widget::screen();
  main_.begin(root);

  Style card = widget::glassCard();
  card.maxWidth = ui.width() * 0.92f;
  // @keyframes menuIn — .5s of opacity and a 14px rise with a slight scale.
  const float entrance = easeMenuIn(tweens.once(TweenStore::key(999), 0.5));
  card.opacity = entrance;
  card.translateY = (1.0f - entrance) * 14.0f;
  main_.begin(card);

  // .brand { text-align: center; margin-bottom: 20px }
  Style brand = Doc::column(0, Align::Center);
  brand.margin = Edges(0, 0, 20, 0);
  main_.begin(brand);
  if (titleTexture_) {
    IconRef ref;
    ref.texture = titleTexture_;
    // The title art is rasterised at 2x, so it draws at half its pixel size.
    const float w = static_cast<float>(titleWidth_) / kTitleRaster;
    const float h = static_cast<float>(titleHeight_) / kTitleRaster;
    // margin-left: 10px offsets the trailing letter-space so the word looks centred.
    Style titleBox;
    titleBox.margin = Edges(0, 0, 0, 10);
    main_.icon(ref, w, h, titleBox);
  } else {
    TextStyle fallback;
    fallback.font = FontId::SansBlack;
    fallback.size = kTitleSize;
    fallback.letterSpacing = kTitleLetterSpacing;
    fallback.color = color::titleMid;
    main_.label(kTitleText, fallback);
  }
  Style subtitle;
  subtitle.margin = Edges(9, 0, 0, 0);
  main_.label("a vibecoded voxel sandbox", widget::glassSubtitle(), subtitle);
  main_.end();

  switch (page_) {
    case MenuPage::Main: buildPageMain(event, tweens); break;
    case MenuPage::Worlds: buildPageWorlds(event, tweens); break;
    case MenuPage::NewWorld: buildPageNewWorld(event, tweens); break;
    case MenuPage::About: buildPageAbout(event, tweens); break;
    case MenuPage::Join: buildPageJoin(event, tweens); break;
  }

  main_.end();
  main_.end();

  // The scroll caps are viewport-relative in the CSS (46vh / 58vh); resolve them now
  // that the viewport height is known, then lay out.
  for (int i = 0; i < main_.count(); ++i) {
    Node& n = main_.node(i);
    if (n.tag == kTagScrollWorlds) n.style.maxHeight = ui.height() * 0.46f;
    if (n.tag == kTagScrollAbout) {
      n.style.maxHeight = ui.height() * 0.58f;
      n.style.width = std::min(470.0f, ui.width() * 0.82f);
    }
  }
  main_.layout({0, 0, ui.width(), ui.height()});
  (void)version;
}

void Menu::handleMain(const UiEvent& event, Text& text) {
  const int hit = main_.hitTest(event.mouseX, event.mouseY);
  // main.js:120 hung one listener on the document and ticked whenever the click
  // landed inside a <button>; this is that listener.
  if (event.leftClick && main_.clickedButton(hit)) audio::sfx::uiClick();
  hoveredTag_ = hit >= 0 ? main_.node(hit).tag : 0;
  hoveredIndex_ = hit >= 0 ? main_.node(hit).index : 0;

  // Scroll wheel over a scroll region.
  if (event.wheel != 0.0f) {
    for (int tag : {kTagScrollAbout, kTagScrollWorlds}) {
      const int node = main_.findTag(tag);
      if (node < 0) continue;
      if (!main_.node(node).rect.contains(event.mouseX, event.mouseY)) continue;
      main_.scroll(tag).offset += event.wheel * 48.0f;
    }
  }

  if (page_ == MenuPage::NewWorld) {
    // Click to focus, click outside to blur — the browser's own behaviour.
    if (event.leftClick) {
      Input* input = const_cast<Input*>(event.input);
      const bool onName = hoveredTag_ == kTagNameField;
      const bool onSeed = hoveredTag_ == kTagSeedField;
      nameField_.setFocused(onName, onName || onSeed ? input : input);
      seedField_.setFocused(onSeed, input);
      if (onName) {
        nameField_.placeCaretAt(text, main_.rectOf(kTagNameField).inset(12),
                                widget::body(), event.mouseX);
      }
      if (onSeed) {
        seedField_.placeCaretAt(text, main_.rectOf(kTagSeedField).inset(12),
                                widget::body(), event.mouseX);
      }
    }
    bool submitted = false;
    nameField_.handle(event, submitted);
    bool seedSubmitted = false;
    seedField_.handle(event, seedSubmitted);
    if (submitted || seedSubmitted) {
      if (actions.createWorld) {
        const std::string name = nameField_.text().empty() ? "New World" : nameField_.text();
        const std::uint32_t seed = seedField_.text().empty()
                                       ? randomSeed()
                                       : hashSeed(seedField_.text());
        actions.createWorld(name, seed);
      }
      return;
    }
  }

  if (page_ == MenuPage::Join) {
    if (event.leftClick) {
      Input* input = const_cast<Input*>(event.input);
      const bool onField = hoveredTag_ == kTagJoinField;
      joinField_.setFocused(onField, input);
      if (onField) {
        joinField_.placeCaretAt(text, main_.rectOf(kTagJoinField).inset(12), widget::body(),
                                event.mouseX);
      }
    }
    bool submitted = false;
    joinField_.handle(event, submitted);
    if (submitted) {
      if (actions.joinGame && !joinField_.text().empty()) actions.joinGame(joinField_.text());
      return;
    }
  }

  if (!event.leftClick) return;
  switch (hoveredTag_) {
    case kTagPlay: setPage(MenuPage::Worlds); break;
    case kTagJoin: setPage(MenuPage::Join); break;
    case kTagAbout: setPage(MenuPage::About); break;
    case kTagQuit:
      if (actions.quitGame) actions.quitGame();
      break;
    case kTagBack:
    case kTagCancel:
      setPage(page_ == MenuPage::NewWorld ? MenuPage::Worlds : MenuPage::Main);
      break;
    case kTagNewWorld: setPage(MenuPage::NewWorld); break;
    case kTagSettings:
      if (actions.openSettings) actions.openSettings();
      break;
    case kTagGallery:
      if (actions.openGallery) actions.openGallery();
      break;
    case kTagCreate: {
      if (!actions.createWorld) break;
      const std::string name = nameField_.text().empty() ? "New World" : nameField_.text();
      const std::uint32_t seed =
          seedField_.text().empty() ? randomSeed() : hashSeed(seedField_.text());
      actions.createWorld(name, seed);
      break;
    }
    case kTagWorldPlay:
      if (actions.loadWorld && hoveredIndex_ < static_cast<int>(worlds_.size())) {
        actions.loadWorld(worlds_[static_cast<std::size_t>(hoveredIndex_)].id);
      }
      break;
    case kTagJoinGo:
      if (actions.joinGame && !joinField_.text().empty()) actions.joinGame(joinField_.text());
      break;
    case kTagLanRow:
      if (actions.joinGame && hoveredIndex_ < static_cast<int>(lanGames_.size())) {
        const LanGame& g = lanGames_[static_cast<std::size_t>(hoveredIndex_)];
        actions.joinGame(g.address + ":" + std::to_string(g.port));
      }
      break;
    case kTagImport:
      if (actions.importWorlds) {
        actions.importWorlds();
        worldsLoaded_ = false;
      }
      break;
    case kTagWorldExport:
      if (actions.exportWorld && hoveredIndex_ < static_cast<int>(worlds_.size())) {
        actions.exportWorld(worlds_[static_cast<std::size_t>(hoveredIndex_)].id);
      }
      break;
    case kTagWorldDelete:
      if (actions.deleteWorld && hoveredIndex_ < static_cast<int>(worlds_.size())) {
        actions.deleteWorld(worlds_[static_cast<std::size_t>(hoveredIndex_)].id);
        worldsLoaded_ = false;
      }
      break;
    default: break;
  }
}

void Menu::updateMain(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens,
                      const std::string& version) {
  time_ += event.dt;
  // The LAN list is re-read four times a second while the page is open. The
  // listener itself is running continuously; this is only how often the panel
  // notices, which is the difference between a list that settles and one that
  // reshuffles under the cursor.
  if (page_ == MenuPage::Join) {
    lanRefresh_ -= event.dt;
    if (lanRefresh_ <= 0.0) {
      lanRefresh_ = 0.25;
      lanGames_ = actions.lanGames ? actions.lanGames() : std::vector<LanGame> {};
    }
  }
  // Build twice: the first pass gives the geometry the hit test needs, the second
  // applies the resulting hover state. Cheaper than a frame of hover lag.
  buildMainCard(ui, text, event, tweens, version);
  handleMain(event, text);
  buildMainCard(ui, text, event, tweens, version);
}

void Menu::drawMain(Ui2D& ui, Text& text) {
  const Rect full {0, 0, ui.width(), ui.height()};
  if (hasBackdrop_) {
    // .menu-screen::before — a vignette wash over the panorama, in two stacked layers.
    // radial-gradient(125% 95% at 50% 6%, rgba(8,12,18,0) 40%, rgba(6,9,14,0.55) 100%)
    ui.radialGradient(full, ui.width() * 0.5f, ui.height() * 0.06f, ui.width() * 1.25f,
                      ui.height() * 0.95f, 0.40f, 1.0f, rgba(8, 12, 18, 0.0),
                      rgba(6, 9, 14, 0.55));
    // linear-gradient(180deg, rgba(6,9,14,.30) 0%, transparent 24%, transparent 60%,
    //                 rgba(6,9,14,.66) 100%) — three bands, so three quads.
    ui.fillGradient({0, 0, ui.width(), ui.height() * 0.24f}, rgba(6, 9, 14, 0.30),
                    rgba(6, 9, 14, 0.0));
    ui.fillGradient({0, ui.height() * 0.60f, ui.width(), ui.height() * 0.40f},
                    rgba(6, 9, 14, 0.0), rgba(6, 9, 14, 0.66));
  } else {
    // #menu:not(.has-pano) — a calm static gradient instead of a blank void:
    // radial-gradient(circle at 50% 24%, #223042 0%, #0c1016 80%). The panorama is a
    // render feature that has not landed, so this is the path the menu takes today, and
    // it is the same path the browser takes with the panorama setting off.
    const float cx = ui.width() * 0.5f;
    const float cy = ui.height() * 0.24f;
    const float dx = std::max(cx, ui.width() - cx);
    const float dy = std::max(cy, ui.height() - cy);
    const float far = std::sqrt(dx * dx + dy * dy);
    ui.radialGradient(full, cx, cy, far, far, 0.0f, 0.80f, rgb(0x223042), rgb(0x0c1016));
  }

  // backdrop-filter: blur(10px) saturate(118%) on the card, clipped to its rounded
  // rect. Drawn before the card's own translucent gradient, which then tints it.
  const int cardNode = 1;  // the card is the root's only child
  if (backdrop_ != 0 && main_.count() > cardNode) {
    const Rect card = main_.node(cardNode).rect;
    if (!card.empty()) {
      ui.pushClip(card);
      ui.setTexture(backdrop_);
      // The blurred copy covers the whole viewport, so the card samples its own slice.
      ui.texturedRect(card, card.x / ui.width(), card.y / ui.height(),
                      card.right() / ui.width(), card.bottom() / ui.height(),
                      fade(color::white, main_.node(cardNode).opacity));
      ui.setTexture(0);
      ui.popClip();
    }
  }

  main_.paint(ui);

  // The ::before accent bar on each hovered menu button: a 3px column that scales from
  // the vertical centre, which is what transform-origin's default does to scaleY.
  for (int i = 0; i < main_.count(); ++i) {
    const Node& n = main_.node(i);
    if (n.tag < kTagPlay || n.tag > kTagAbout) continue;
    if (n.tag != hoveredTag_) continue;
    ui.fillRect({n.rect.x, n.rect.y, 3, n.rect.h}, color::accent);
  }

  // Text fields draw their own caret, so they are painted after the tree.
  const int nameNode = main_.findTag(kTagNameField);
  if (nameNode >= 0) {
    nameField_.draw(ui, text, main_.node(nameNode).rect.inset(12), widget::body(), time_);
  }
  const int seedNode = main_.findTag(kTagSeedField);
  if (seedNode >= 0) {
    seedField_.draw(ui, text, main_.node(seedNode).rect.inset(12), widget::body(), time_);
  }
  const int joinNode = main_.findTag(kTagJoinField);
  if (joinNode >= 0) {
    joinField_.draw(ui, text, main_.node(joinNode).rect.inset(12), widget::body(), time_);
  }

  // .menu-footer — the version tag and the signature, pinned to the bottom edge.
  const TextStyle versionStyle = widget::versionTag();
  const TextMetrics vm = text.metrics(versionStyle);
  const float footerY = ui.height() - 10 - vm.lineHeight;
  text.draw(ui, 16, footerY + vm.ascent, std::string("v") + HR_VERSION, versionStyle);
  const TextStyle sig = widget::menuSignature();
  const std::string signature = "Built from scratch \xE2\x80\x94 no engine, no libraries";
  text.draw(ui, ui.width() - 16 - text.measure(signature, sig), footerY + vm.ascent, signature,
            sig);
}

// ---------------------------------------------------------------------------
// Pause
// ---------------------------------------------------------------------------

void Menu::updatePause(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens,
                       bool hasWorld) {
  pauseHasWorld_ = hasWorld;
  const auto build = [&] {
    pause_.reset(&text);
    pause_.begin(widget::screen());
    Style card = widget::menuCard(false);
    pause_.begin(card);

    Style title;
    title.margin = Edges(0, 0, 18, 0);
    TextStyle h2 = widget::h2();
    pause_.begin([] {
      Style s = Doc::row(0, Justify::Center, Align::Center);
      s.margin = Edges(0, 0, 18, 0);
      return s;
    }());
    pause_.label("Paused", h2);
    pause_.end();
    (void)title;

    struct Item {
      int tag;
      const char* label;
      widget::ButtonKind kind;
    };
    // A guest cannot host, and a host's button offers the way out again. The label
    // is the state: there is no separate panel to open and nothing to read but
    // this and the line under it.
    const bool hosting = actions.isHosting && actions.isHosting();
    const bool guest = actions.isGuest && actions.isGuest();
    const char* mpLabel = guest ? "Leave the Host" : (hosting ? "Stop Hosting" : "Open to LAN");
    const Item items[] = {
        {kTagResume, "Resume", widget::ButtonKind::Primary},
        {kTagMultiplayer, mpLabel, widget::ButtonKind::Normal},
        {kTagPauseSettings, "Settings", widget::ButtonKind::Normal},
        {kTagPauseGallery, "Screenshots", widget::ButtonKind::Normal},
        {kTagPauseExport, "Export World", widget::ButtonKind::Normal},
        {kTagPauseQuit, "Save & Quit to Menu", widget::ButtonKind::Normal},
    };
    for (const Item& item : items) {
      const bool hovered = pauseHoveredTag_ == item.tag;
      Style s = widget::btn(hovered, false, item.kind);
      pause_.begin(s, item.tag);
      pause_.label(item.label, widget::btnText(hovered, item.kind));
      pause_.end();
    }
    // The invite code and who is connected, under the buttons. Empty in single
    // player, which is when there is nothing to say.
    const std::string status = actions.hostStatus ? actions.hostStatus() : std::string();
    if (!status.empty()) {
      Style note;
      note.margin = Edges(10, 0, 0, 0);
      note.maxWidth = 300;
      pause_.label(status, widget::emptyNote(), note);
      // Only while hosting: a guest has nothing to hand out, and the button would
      // copy an invite to a game they are not running.
      if (actions.isHosting && actions.isHosting()) {
        const bool hovered = pauseHoveredTag_ == kTagCopyInvite;
        Style s = widget::btn(hovered, false, widget::ButtonKind::Normal);
        s.margin = Edges(8, 0, 0, 0);
        pause_.begin(s, kTagCopyInvite);
        pause_.label("Copy Invite Code", widget::btnText(hovered, widget::ButtonKind::Normal));
        pause_.end();
      }
    }
    pause_.end();
    pause_.end();
    pause_.layout({0, 0, ui.width(), ui.height()});
  };

  build();
  handlePause(event);
  build();
  (void)tweens;
}

void Menu::handlePause(const UiEvent& event) {
  const int hit = pause_.hitTest(event.mouseX, event.mouseY);
  // main.js:120 hung one listener on the document and ticked whenever the click
  // landed inside a <button>; this is that listener.
  if (event.leftClick && pause_.clickedButton(hit)) audio::sfx::uiClick();
  pauseHoveredTag_ = hit >= 0 ? pause_.node(hit).tag : 0;
  if (!event.leftClick) return;
  switch (pauseHoveredTag_) {
    case kTagResume:
      if (actions.resume) actions.resume();
      break;
    case kTagPauseSettings:
      if (actions.openSettings) actions.openSettings();
      break;
    case kTagPauseGallery:
      if (actions.openGallery) actions.openGallery();
      break;
    case kTagMultiplayer:
      if (actions.toggleHosting) actions.toggleHosting();
      break;
    case kTagPauseExport:
      if (actions.exportWorld) actions.exportWorld(std::string());
      break;
    case kTagCopyInvite:
      if (actions.copyInvite) actions.copyInvite();
      break;
    case kTagPauseQuit:
      if (actions.saveAndQuit) actions.saveAndQuit();
      break;
    default: break;
  }
}

void Menu::drawPause(Ui2D& ui, Text& text) {
  // .screen — radial-gradient(circle at 50% 30%, #1d2733 0%, #0e1218 75%). The circle's
  // farthest-corner radius is what 100% means, and the second stop sits at 75% of it.
  const float cx = ui.width() * 0.5f;
  const float cy = ui.height() * 0.30f;
  const float far = std::sqrt(std::max(cx, ui.width() - cx) * std::max(cx, ui.width() - cx) +
                             std::max(cy, ui.height() - cy) * std::max(cy, ui.height() - cy));
  ui.radialGradient({0, 0, ui.width(), ui.height()}, cx, cy, far, far, 0.0f, 0.75f,
                    rgb(0x1d2733), rgb(0x0e1218));
  pause_.paint(ui);
  (void)text;
}

}  // namespace hr::ui
