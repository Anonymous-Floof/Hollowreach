// The screenshot gallery's storage, ported from js/save/gallery.js.
//
// The web build kept captures as data URLs in two IndexedDB object stores, with a
// pre-baked JPEG thumbnail per item so that listing did not have to load the full
// images. None of that survives the port, and none of it needs to: the native build
// writes real PNG files into data/screenshots, so the directory IS the index and a
// thumbnail is a downscale of a file that is already there. That was already true
// when the gallery screen landed at M6 — what moves here is the directory walking
// itself, out of the interface layer and into the save layer where the rest of the
// on-disk state lives.
//
// What is genuinely gone, and stays gone until the panorama render exists: the
// second object store held six-face cube maps for the menu background, which is an
// F8 feature the renderer does not have yet.

#pragma once

#include <string>
#include <vector>

namespace hr::save::gallery {

struct Shot {
  std::string path;
  std::string name;
  double ageSeconds = 0;  // since it was written
};

// Every .png in data/screenshots, newest first.
std::vector<Shot> list();

bool erase(const std::string& path);

// Shows the file in the platform's file manager. The web build's "Save" button had
// to offer a download because its captures were locked inside a browser database;
// here the file is already the player's, so the button reveals it instead.
// Returns false where there is nothing to ask.
bool reveal(const std::string& path);

}  // namespace hr::save::gallery
