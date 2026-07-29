// The resource provider stack.
//
// An ordered list of providers, highest priority first, resolving a ResourceId to
// pixels. Today there is exactly one provider — the built-in procedural painters —
// and that is the whole point: the atlas builder already asks *the stack* rather
// than asking the painters directly, so adding folder and zip pack providers later
// is additive and touches nothing downstream.
//
// This mirrors Minecraft's resource-pack stack, including the part that matters
// most: a pack does not have to be complete. Whatever it does not supply falls
// through to the layer below, so a pack replacing only the grass textures works.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "resource/identifier.h"
#include "resource/image.h"

namespace hr::resource {

// What a provider knows about a texture before it is asked to produce pixels, so
// the atlas can decide its tile resolution without painting everything twice.
struct TextureInfo {
  int width = 0;
  int height = 0;
  // Vertically stacked animation frames, from a .mcmeta sidecar. Always 1 today.
  int frames = 1;
  double frameTimeSeconds = 0.0;
};

class Provider {
 public:
  virtual ~Provider() = default;

  // A short name for logs and the future pack-selection UI.
  virtual const std::string& name() const = 0;

  virtual bool has(const ResourceId& id) const = 0;
  virtual std::optional<TextureInfo> info(const ResourceId& id) const = 0;

  // Produces the texture at its native resolution. `id` is guaranteed to satisfy
  // has(); returning an empty Image is treated as a failure and falls through.
  virtual Image load(const ResourceId& id) const = 0;
};

// Wraps the compiled-in procedural painters.
std::unique_ptr<Provider> makeBuiltinProvider();

class PackStack {
 public:
  // Providers are consulted in insertion order, so add the highest priority first.
  void push(std::unique_ptr<Provider> provider);
  void clear();

  bool has(const ResourceId& id) const;
  std::optional<TextureInfo> info(const ResourceId& id) const;

  // Resolves through the stack. Returns an empty Image when nothing provides it;
  // the atlas turns that into its magenta "missing" tile so a typo in a texture
  // name is obvious in game rather than silently drawing the wrong thing.
  Image load(const ResourceId& id) const;

  // Every id any provider offers under `prefix`. Used to enumerate what needs to
  // go into the atlas.
  std::vector<ResourceId> list(std::string_view prefix) const;

  const std::vector<std::unique_ptr<Provider>>& providers() const { return providers_; }

 private:
  std::vector<std::unique_ptr<Provider>> providers_;
};

// The stack the game uses. Built at startup with the built-in provider, and where
// a future pack loader would insert user packs above it.
PackStack& defaultStack();

}  // namespace hr::resource
