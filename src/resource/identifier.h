// Namespaced resource identifiers.
//
// The web build referred to textures by bare name — "turf_top", or "item:pick_copper"
// for the item sprites (js/render/texatlas.js). That proto-namespace becomes a real
// one here: `hollowreach:block/turf_top`, `hollowreach:item/pick_copper`.
//
// This exists for one reason: a future resource-pack loader. Minecraft's chain is
//
//     blockstate -> model -> texture variable (#side) -> texture file
//
// with `parent` inheritance, and a pack can intervene at any link. An identifier
// type plus the `#var` reference below is what keeps that chain expressible; a bare
// std::string of a texture name would collapse it. Nothing about resource packs is
// implemented — only the shape is here, so adding it later is additive.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace hr {

// The namespace built-in content lives under. A resource pack supplying
// `minecraft:` assets is mapped onto these through an alias table.
inline constexpr std::string_view kDefaultNamespace = "hollowreach";

class ResourceId {
 public:
  ResourceId() = default;

  // Accepts "path", "ns:path", and the legacy "item:key" spelling. A bare path
  // takes the default namespace, matching Minecraft's own shorthand.
  explicit ResourceId(std::string_view spec);

  ResourceId(std::string_view ns, std::string_view path) : ns_(ns), path_(path) {}

  const std::string& ns() const { return ns_; }
  const std::string& path() const { return path_; }

  bool empty() const { return path_.empty(); }

  // "hollowreach:block/turf_top"
  std::string str() const { return ns_ + ':' + path_; }

  // A new id with `prefix` prepended to the path, so a block definition can name
  // "turf_top" and the registry can resolve it to "block/turf_top" without every
  // definition repeating the folder.
  ResourceId withPathPrefix(std::string_view prefix) const {
    if (path_.rfind(prefix, 0) == 0) return *this;
    return ResourceId(ns_, std::string(prefix) + path_);
  }

  bool operator==(const ResourceId& other) const {
    return ns_ == other.ns_ && path_ == other.path_;
  }
  bool operator!=(const ResourceId& other) const { return !(*this == other); }
  bool operator<(const ResourceId& other) const {
    return ns_ != other.ns_ ? ns_ < other.ns_ : path_ < other.path_;
  }

 private:
  std::string ns_ {kDefaultNamespace};
  std::string path_;
};

// A model's texture slot: either a concrete resource, or a `#name` reference to
// another slot resolved through the model's parent chain. Minecraft's models are
// built on this indirection, so a pack can override `#side` for one variant
// without touching the others.
class TextureRef {
 public:
  TextureRef() = default;

  // "#side" becomes a variable reference; anything else is a resource id.
  explicit TextureRef(std::string_view spec) {
    if (!spec.empty() && spec.front() == '#') {
      variable_ = std::string(spec.substr(1));
    } else {
      id_ = ResourceId(spec);
    }
  }

  bool isVariable() const { return !variable_.empty(); }
  const std::string& variable() const { return variable_; }
  const ResourceId& id() const { return id_; }
  bool empty() const { return variable_.empty() && id_.empty(); }

  std::string str() const { return isVariable() ? "#" + variable_ : id_.str(); }

 private:
  std::string variable_;
  ResourceId id_;
};

}  // namespace hr

template <>
struct std::hash<hr::ResourceId> {
  std::size_t operator()(const hr::ResourceId& id) const noexcept {
    // FNV-1a over both parts with a separator, so ("a","bc") and ("ab","c") differ.
    std::size_t h = 1469598103934665603ull;
    auto mix = [&h](std::string_view s) {
      for (char c : s) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 1099511628211ull;
      }
    };
    mix(id.ns());
    mix(":");
    mix(id.path());
    return h;
  }
};
