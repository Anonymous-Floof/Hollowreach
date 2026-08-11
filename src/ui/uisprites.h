// Nine-slice sprites for the interface, supplied by resource packs.
//
// The token table (ui/theme.h) lets a pack decide what colour a panel is. This
// lets a pack decide what a panel is MADE of — carved stone, parchment, riveted
// iron — which is the difference between an interface somebody can recolour and
// one somebody can actually re-skin.
//
// **The built-in theme supplies none of these, and that is the design.** Every
// widget draws as a rounded rectangle with a border unless a pack has said
// otherwise, so this whole file costs one null check per painted node until
// somebody uses it. Shipping built-in sprites would mean shipping a second,
// parallel appearance for every widget and keeping the two agreeing forever.
//
// A pack supplies:
//
//     assets/<ns>/ui/sprites/panel.card.png     the image
//     assets/<ns>/ui/sprites/panel.card.json    { "slice": 8 }   (optional)
//
// where the name is a **slot** below. `slice` is the corner inset in source
// pixels and defaults to a quarter of the smaller side, which is right often
// enough that most sprites need no json at all.
//
// One GL texture per sprite rather than an atlas. Switching a texture flushes the
// batch, so a screen using four sprites costs four extra draw calls — against an
// interface that draws in well under thirty, that is not worth an atlas packer
// and the tile-bleed problem that comes with one.

#pragma once

#include <string>
#include <vector>

#include "core/gl.h"
#include "resource/pack.h"

namespace hr::ui {

// The widgets a pack may replace. Deliberately short: each of these is a shape
// that appears many times across many screens, so a pack that supplies four of
// them has re-skinned most of the interface. A slot per widget variant would be
// eighty images nobody will draw.
//
// Kept as an enum with a name table for the same reason the tokens are — so a
// misspelled slot in a pack is reported rather than silently ignored.
#define HR_UI_SPRITES(X)                                                            \
  X(PanelCard,     "panel.card")      /* a menu card, a dialog */                    \
  X(PanelInset,    "panel.inset")     /* a container inside one */                   \
  X(Button,        "button")                                                         \
  X(ButtonHover,   "button.hover")                                                   \
  X(ButtonPrimary, "button.primary")                                                 \
  X(Slot,          "slot")            /* an inventory cell */                        \
  X(SlotSelected,  "slot.selected")                                                  \
  X(Field,         "field")           /* a text input */                             \
  X(Overlay,       "overlay")         /* a panel over the live world */

enum class SpriteSlot : std::uint8_t {
#define HR_UI_X(id, name) id,
  HR_UI_SPRITES(HR_UI_X)
#undef HR_UI_X
  Count,
};

inline constexpr int kSpriteSlotCount = static_cast<int>(SpriteSlot::Count);

const char* nameOf(SpriteSlot slot);
bool spriteSlotByName(std::string_view name, SpriteSlot& out);

struct UiSprite {
  GLuint texture = 0;
  int width = 0, height = 0;
  float slice = 0;
  // The whole texture. Present so the nine-patch call reads the same as every
  // other textured draw, and so an atlas could be introduced later without
  // touching a single call site.
  float u0 = 0, v0 = 0, u1 = 1, v1 = 1;

  bool valid() const { return texture != 0 && width > 0 && height > 0; }
};

// The sprite for a slot, or nullptr when no pack supplies one — which is the
// ordinary case and the reason every caller must handle it.
const UiSprite* sprite(SpriteSlot slot);

// Replaces the whole set from the enabled packs, highest priority first. Frees
// the textures of the previous set.
//
// Needs a live GL context, so unlike the theme this cannot run on the headless
// dump paths; a caller without a context should simply not call it, and every
// sprite stays absent.
struct SpriteReport {
  int loaded = 0;
  std::vector<std::string> problems;
};
SpriteReport loadUiSprites(const std::vector<resource::PackInfo>& ordered);

// Drops every texture. Called at shutdown, and before the GL context goes.
void destroyUiSprites();

}  // namespace hr::ui
