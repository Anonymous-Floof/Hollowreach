// The Dyer's Palette: the screen a palette opens.
//
// A hue ring, a saturation/value square inside it, a hex field, and one slot to put
// the thing being coloured in. Modelled on TimeWheel — a card, a dial you drag, and
// two buttons — because it is the same shape of screen and a second idiom for
// "drag a thing round a circle" would be a second set of bugs.
//
// What this class does NOT know: what a dye costs, whether the player can afford one,
// or what may be dyed at all. All of that is game/dyeing.h, so it can be asserted
// without a window. This file draws a wheel and reports where the mouse is.
//
// Favourites are held as two plain lists handed in from outside. The screen neither
// loads nor saves them — one list lives in the world file and one beside settings.json,
// and which is which is App's business, not the wheel's.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "game/inventory.h"
#include "render/iconatlas.h"
#include "ui/text.h"
#include "ui/ui2d.h"
#include "ui/widgets.h"

namespace hr::ui {

// HSV in 0..1, RGB packed 0x00RRGGBB. Public because the wheel's whole geometry is
// this conversion and it is the part worth testing without a window.
std::uint32_t hsvToRgb(float h, float s, float v);
void rgbToHsv(std::uint32_t rgb, float& h, float& s, float& v);
// "#4a6fe0", "4A6FE0", "#abc". Returns false and leaves `out` alone on anything else.
bool parseHex(const std::string& text, std::uint32_t& out);
std::string toHex(std::uint32_t rgb);

class PaletteUI {
 public:
  // `slot` is the screen's own single-item slot, owned by App so its contents survive
  // the screen being closed and reopened. `inv` pays for the dye.
  void attach(game::Inventory* inv, game::ItemStack* slot) {
    inv_ = inv;
    slot_ = slot;
  }
  void setIcons(const render::IconAtlas* icons) { icons_ = icons; }
  void open();
  void close();
  bool isOpen() const { return open_; }

  std::uint32_t colour() const { return hsvToRgb(h_, s_, v_); }

  void update(Ui2D& ui, Text& text, const UiEvent& event);
  void draw(Ui2D& ui, Text& text);

  // --- embedded in the inventory screen ---------------------------------------
  //
  // The palette is a mode of InventoryUI, because the one thing it needs is a slot
  // the bag can move something into and every gesture that fills a slot lives there.
  // These two are what that screen calls: paint everything into the box the layout
  // reserved, and take a click inside it. `slot` is the item being coloured and may
  // be null; `inv` pays for the dye.
  void drawInto(Ui2D& ui, Text& text, const Rect& box, const Rect& side,
                const game::Inventory* inv, const game::ItemStack* slot);
  // Returns true when the click belonged to the wheel, so the slot layer skips it.
  bool updateIn(const UiEvent& event, const Rect& box, const Rect& side,
                game::Inventory& inv, game::ItemStack* slot);

  // Typed characters and editing keys, routed here while the hex field has focus.
  bool wantsText() const { return editingHex_; }
  void onChar(unsigned int codepoint);
  void onKey(int key);

  // The two favourite lists, read and written straight through. App supplies them and
  // is what persists them.
  std::vector<std::uint32_t>* worldFavourites = nullptr;
  std::vector<std::uint32_t>* globalFavourites = nullptr;

  // Fired when a favourite list changed and wants writing to disk.
  std::function<void(bool global)> onFavouritesChanged;
  std::function<void()> onClose;

 private:
  struct Layout {
    Rect card;
    float cx = 0, cy = 0;      // wheel centre
    float outer = 0, inner = 0;  // hue ring
    Rect square;               // saturation / value
    Rect slot, preview, hex;
    Rect apply, done;
    Rect saveWorld, saveGlobal;
    Rect worldRow, globalRow;
  };
  Layout layout(Ui2D& ui) const;
  void applyNow();
  int favouriteAt(const Layout& lay, float mx, float my, bool& global) const;

  game::Inventory* inv_ = nullptr;
  game::ItemStack* slot_ = nullptr;
  const render::IconAtlas* icons_ = nullptr;

  bool open_ = false;
  float h_ = 0.0f, s_ = 0.8f, v_ = 0.9f;

  enum class Drag { None, Hue, Square };
  Drag drag_ = Drag::None;

  bool editingHex_ = false;
  std::string hexText_;
  int hover_ = 0;  // widget tag under the cursor
};

}  // namespace hr::ui
