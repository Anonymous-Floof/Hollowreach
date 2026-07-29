#include "resource/identifier.h"

namespace hr {

ResourceId::ResourceId(std::string_view spec) {
  const std::size_t colon = spec.find(':');
  if (colon == std::string_view::npos) {
    path_ = std::string(spec);
    return;
  }

  std::string_view head = spec.substr(0, colon);
  std::string_view tail = spec.substr(colon + 1);

  // The web build wrote item sprites as "item:<key>" (js/render/itemmesh.js:52),
  // which reads as a namespace but is really a folder. Translate rather than
  // inventing an `item` namespace that a resource pack would never use.
  if (head == "item") {
    path_ = "item/" + std::string(tail);
    return;
  }

  ns_ = std::string(head);
  path_ = std::string(tail);
}

}  // namespace hr
