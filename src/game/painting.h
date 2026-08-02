// A picture hung on a wall: the pixels themselves, attached to a block position.
//
// The one decision this whole feature turns on is that a painting stores its OWN
// pixels rather than the path of a screenshot. Naming a file is much cheaper and
// is wrong in three separate ways: a friend joining your world has never seen your
// screenshots folder, exporting a world would ship a picture frame full of nothing,
// and deleting a screenshot from the gallery would quietly blank a wall you had
// decorated months earlier. Carrying the picture makes all three work by
// construction and costs a fixed 48 KB per painting.
//
// That size is why the art is downscaled on the way in rather than stored at
// whatever the window happened to be: a 1080p screenshot is six megabytes, and a
// world with twenty of them on the walls would be a hundred and twenty megabytes
// to save and to send to a guest. 128 square is sharper than anything else in the
// game — the block textures are 16 — and is the whole picture at arm's length.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hr::game {

// Side of the square the art is stored at. Changing it changes the save format of
// the painting section, which is why it is a constant and not a per-painting field.
inline constexpr int kPaintingSize = 128;
inline constexpr std::size_t kPaintingBytes =
    static_cast<std::size_t>(kPaintingSize) * kPaintingSize * 3;

struct Painting {
  // kPaintingBytes of RGB, row-major from the top. Empty means a blank canvas:
  // hung but never given a picture, which is a perfectly good state to be in.
  std::vector<std::uint8_t> rgb;
  // Which gallery file it came from, for the picker's benefit only — it is a label,
  // never a lookup. Nothing reads pixels through it.
  std::string source;

  bool blank() const { return rgb.size() != kPaintingBytes; }
};

// Loads a PNG, centre-crops it to a square and box-filters it down to
// kPaintingSize. Centre-crop rather than letterbox because a painting is square
// and a screenshot is not: bars would be the most prominent thing on the wall.
// Returns false when the file cannot be read, leaving `out` untouched.
bool paintingFromPng(const std::string& path, Painting& out);

}  // namespace hr::game
