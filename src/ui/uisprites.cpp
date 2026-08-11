#include "ui/uisprites.h"

#include <algorithm>
#include <cstdio>

#include "core/json.h"
#include "core/log.h"
#include "resource/image.h"

namespace hr::ui {
namespace {

const char* const kSlotNames[] = {
#define HR_UI_X(id, name) name,
    HR_UI_SPRITES(HR_UI_X)
#undef HR_UI_X
};

static_assert(sizeof(kSlotNames) / sizeof(kSlotNames[0]) == kSpriteSlotCount,
              "sprite slot name table and enum disagree");

UiSprite g_sprites[kSpriteSlotCount];

GLuint upload(const Image& image) {
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width(), image.height(), 0, GL_RGBA,
               GL_UNSIGNED_BYTE, image.data());
  // Linear, and clamped. A nine-slice samples right up to the edge of each of its
  // nine regions, and GL_REPEAT there would wrap a corner's outermost texel round
  // to the opposite side of the sprite — one bright line along two edges of every
  // panel, which is exactly the sort of artefact that gets blamed on the pack.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  return texture;
}

// The largest sprite this build will upload, per side. A UI sprite is a small
// decorated box; a pack handing over an 8192-square panel is either a mistake or
// an attempt to spend a gigabyte of video memory on nine quads.
constexpr int kMaxSpriteSide = 1024;

}  // namespace

const char* nameOf(SpriteSlot slot) { return kSlotNames[static_cast<int>(slot)]; }

bool spriteSlotByName(std::string_view name, SpriteSlot& out) {
  for (int i = 0; i < kSpriteSlotCount; ++i) {
    if (name == kSlotNames[i]) {
      out = static_cast<SpriteSlot>(i);
      return true;
    }
  }
  return false;
}

const UiSprite* sprite(SpriteSlot slot) {
  const UiSprite& s = g_sprites[static_cast<int>(slot)];
  return s.valid() ? &s : nullptr;
}

void destroyUiSprites() {
  for (UiSprite& s : g_sprites) {
    if (s.texture) glDeleteTextures(1, &s.texture);
    s = UiSprite{};
  }
}

SpriteReport loadUiSprites(const std::vector<resource::PackInfo>& ordered) {
  destroyUiSprites();
  SpriteReport report;

  for (int i = 0; i < kSpriteSlotCount; ++i) {
    const SpriteSlot slot = static_cast<SpriteSlot>(i);
    const std::string name = nameOf(slot);

    // Highest priority first, and the first pack that has it wins — so unlike the
    // theme there is no layering here. A sprite is one image; there is nothing for
    // a lower pack to contribute to it once a higher pack has supplied one.
    for (const resource::PackInfo& pack : ordered) {
      if (!pack.usable()) continue;
      std::string pngPath;
      for (const std::string& ns : pack.namespaces) {
        const std::string candidate =
            resource::packFile(pack, "assets/" + ns + "/ui/sprites/" + name + ".png");
        if (candidate.empty()) continue;
        std::FILE* probe = std::fopen(candidate.c_str(), "rb");
        if (!probe) continue;
        std::fclose(probe);
        pngPath = candidate;
        break;
      }
      if (pngPath.empty()) continue;

      Image image;
      std::string error;
      if (!Image::loadPng(pngPath, image, &error)) {
        report.problems.push_back(pack.name + ": " + name + ".png could not be read (" +
                                  error + ")");
        break;
      }
      if (image.width() <= 0 || image.height() <= 0 || image.width() > kMaxSpriteSide ||
          image.height() > kMaxSpriteSide) {
        report.problems.push_back(pack.name + ": " + name + ".png is not a usable size");
        break;
      }

      UiSprite s;
      s.width = image.width();
      s.height = image.height();
      // A quarter of the smaller side. Right for the great majority of nine-slice
      // art, which is why the sidecar is optional — a pack that needs a different
      // inset says so, and one that does not needs no second file per sprite.
      s.slice = static_cast<float>(std::min(s.width, s.height)) * 0.25f;

      const std::string metaPath = pngPath.substr(0, pngPath.size() - 4) + ".json";
      std::string metaError;
      const json::Value meta = json::parseFile(metaPath, &metaError);
      if (meta.isObject()) {
        const double v = meta["slice"].num(static_cast<double>(s.slice));
        // Bounded against the image it belongs to. A slice past half the sprite
        // describes nine regions that overlap, which has no meaning to resolve.
        if (v > 0.0 && v <= std::min(s.width, s.height) * 0.5) {
          s.slice = static_cast<float>(v);
        } else {
          report.problems.push_back(pack.name + ": " + name +
                                    ".json has a slice too large for the image");
        }
      }

      s.texture = upload(image);
      g_sprites[i] = s;
      report.loaded++;
      log::info("ui sprite: %s from %s (%dx%d, slice %.0f)", name.c_str(), pack.id.c_str(),
                s.width, s.height, static_cast<double>(s.slice));
      break;
    }
  }

  return report;
}

}  // namespace hr::ui
