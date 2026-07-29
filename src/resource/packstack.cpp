#include "resource/packstack.h"

#include "core/log.h"
#include "resource/painters.h"

namespace hr::resource {
namespace {

class BuiltinProvider final : public Provider {
 public:
  const std::string& name() const override { return name_; }

  bool has(const ResourceId& id) const override { return findPainter(id) != nullptr; }

  std::optional<TextureInfo> info(const ResourceId& id) const override {
    if (!findPainter(id)) return std::nullopt;
    return TextureInfo {kPainterTile, kPainterTile, 1, 0.0};
  }

  Image load(const ResourceId& id) const override {
    const PainterEntry* entry = findPainter(id);
    if (!entry) return {};
    return paintTile(*entry);
  }

 private:
  std::string name_ = "built-in";
};

}  // namespace

std::unique_ptr<Provider> makeBuiltinProvider() { return std::make_unique<BuiltinProvider>(); }

void PackStack::push(std::unique_ptr<Provider> provider) {
  if (!provider) return;
  log::info("packstack: added provider \"%s\"", provider->name().c_str());
  providers_.push_back(std::move(provider));
}

void PackStack::clear() { providers_.clear(); }

bool PackStack::has(const ResourceId& id) const {
  for (const auto& p : providers_) {
    if (p->has(id)) return true;
  }
  return false;
}

std::optional<TextureInfo> PackStack::info(const ResourceId& id) const {
  for (const auto& p : providers_) {
    if (auto i = p->info(id)) return i;
  }
  return std::nullopt;
}

Image PackStack::load(const ResourceId& id) const {
  for (const auto& p : providers_) {
    if (!p->has(id)) continue;
    Image img = p->load(id);
    // An empty result is treated as a failed load and falls through, so a corrupt
    // file in a pack degrades to the layer below rather than to a blank tile.
    if (!img.empty()) return img;
    log::warn("packstack: provider \"%s\" failed to load %s", p->name().c_str(),
              id.str().c_str());
  }
  return {};
}

std::vector<ResourceId> PackStack::list(std::string_view prefix) const {
  // Only the built-in painters can be enumerated today. A folder or zip provider
  // would extend Provider with its own listing; the atlas does not enumerate the
  // stack in practice — it asks for exactly the ids the block table references.
  std::vector<ResourceId> out;
  for (const PainterEntry& e : builtinPainters()) {
    if (e.id.path().rfind(prefix, 0) == 0) out.push_back(e.id);
  }
  return out;
}

PackStack& defaultStack() {
  static PackStack stack = [] {
    PackStack s;
    s.push(makeBuiltinProvider());
    return s;
  }();
  return stack;
}

}  // namespace hr::resource
