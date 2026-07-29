// Locating the fonts the interface draws with.
//
// css/style.css asks for `"Segoe UI", "Trebuchet MS", system-ui, sans-serif` and
// `"Consolas", monospace`. Both are Windows system fonts, so on the platform this
// port targets the browser was rasterising those exact files — which makes loading
// them from the system font directory the closest possible match, and better than
// the bundled Inter/JetBrains Mono the plan originally called for.
//
// Neither font is redistributable, so nothing is shipped: this resolves a family
// and weight to a file on disk, walking a fallback list per platform. The CSS
// font-matching rules are followed for weight, which matters — `font-weight: 800`
// on the menu title has no ExtraBold to match, so it resolves *up* to Segoe UI
// Black, and using Bold there would visibly thin the title.

#pragma once

#include <string>
#include <vector>

namespace hr::platform {

enum class FontFamily {
  Sans,  // "Segoe UI", "Trebuchet MS", system-ui, sans-serif
  Mono,  // "Consolas", monospace
};

struct FontRequest {
  FontFamily family = FontFamily::Sans;
  int weight = 400;  // CSS numeric weight
  bool italic = false;
};

// The file that best matches `req`, or empty when nothing suitable was found.
std::string findFont(const FontRequest& req);

// Files to consult, in order, for glyphs the primary face does not have. The
// browser does exactly this: a symbol like ♥ or ☠ falls through the family list
// until something covers it.
std::vector<std::string> fallbackFonts();

// Every directory searched, for the log line when a face is missing.
std::vector<std::string> fontDirectories();

}  // namespace hr::platform
