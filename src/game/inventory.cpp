#include "game/inventory.h"

#include <algorithm>

#include "game/items.h"

namespace hr::game {

int Inventory::stackMax(const std::string& key) const {
  const ItemDef* it = getItem(key);
  return it ? it->maxStack : 64;
}

int Inventory::give(const std::string& key, int count, int dura) {
  const ItemDef* item = getItem(key);
  if (!item) return count;

  // A tool or armour piece handed over without a stated durability arrives new.
  if (dura < 0 && (item->type == ItemType::Tool || item->type == ItemType::Armor)) {
    dura = item->durability;
  }

  const int max = item->maxStack;
  if (max > 1) {
    // Top up partial stacks first, so picking up a drop does not fragment the bag.
    for (std::size_t i = 0; i < slots_.size() && count > 0; ++i) {
      ItemStack& s = slots_[i];
      if (s.empty() || s.key != key || s.count >= max) continue;
      const int add = std::min(max - s.count, count);
      s.count += add;
      count -= add;
    }
  }
  while (count > 0) {
    const int i = firstEmpty();
    if (i < 0) break;
    const int put = max > 1 ? std::min(max, count) : 1;
    slots_[i] = ItemStack {key, put, dura};
    count -= put;
  }
  return count;
}

int Inventory::firstEmpty() const {
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    if (slots_[i].empty()) return static_cast<int>(i);
  }
  return -1;
}

int Inventory::countOf(const std::string& key) const {
  int n = 0;
  for (const ItemStack& s : slots_) {
    if (!s.empty() && s.key == key) n += s.count;
  }
  return n;
}

bool Inventory::hasItems(const std::unordered_map<std::string, int>& need) const {
  for (const auto& [key, amount] : need) {
    if (countOf(key) < amount) return false;
  }
  return true;
}

void Inventory::removeItems(const std::unordered_map<std::string, int>& take) {
  for (const auto& [key, amount] : take) {
    int remaining = amount;
    for (std::size_t i = 0; i < slots_.size() && remaining > 0; ++i) {
      ItemStack& s = slots_[i];
      if (s.empty() || s.key != key) continue;
      const int n = std::min(s.count, remaining);
      s.count -= n;
      remaining -= n;
      if (s.count <= 0) s.clear();
    }
  }
}

void Inventory::consumeSelected() {
  ItemStack& s = selectedSlot();
  if (s.empty()) return;
  s.count -= 1;
  if (s.count <= 0) s.clear();
}

bool Inventory::damageSelectedTool(int amount) {
  ItemStack& s = selectedSlot();
  if (s.empty() || !s.wears()) return false;
  s.dura -= amount;
  if (s.dura <= 0) {
    s.clear();
    return true;
  }
  return false;
}

int Inventory::totalDefense() const {
  int d = 0;
  for (const ItemStack& s : armor_) {
    if (s.empty()) continue;
    if (const ItemDef* it = getItem(s.key)) d += it->defense;
  }
  return d;
}

void Inventory::damageArmor(int amount) {
  for (ItemStack& s : armor_) {
    if (s.empty() || !s.wears()) continue;
    s.dura -= amount;
    if (s.dura <= 0) s.clear();
  }
}

void Inventory::clear() {
  for (ItemStack& s : slots_) s.clear();
  for (ItemStack& s : armor_) s.clear();
  selected_ = 0;
}

}  // namespace hr::game
