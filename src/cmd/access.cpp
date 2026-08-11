#include "cmd/access.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "core/json.h"
#include "core/log.h"

namespace hr::cmd {
namespace {

bool equalsNoCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

// A row that records nothing is not worth keeping. Checked in one place so a
// pardon, a demotion and a whitelist removal all leave the file the same size they
// found it.
bool saysNothing(const AccessEntry& e) {
  return e.level == Level::Anyone && !e.banned && !e.allowed;
}

}  // namespace

AccessEntry* Access::find(const std::string& playerId, const std::string& name) {
  // An id match first and outright: a name can be changed between sessions, so
  // preferring the name would silently demote anyone who renamed themselves.
  if (!playerId.empty()) {
    for (AccessEntry& e : entries_) {
      if (!e.playerId.empty() && e.playerId == playerId) return &e;
    }
  }
  if (!name.empty()) {
    for (AccessEntry& e : entries_) {
      if (equalsNoCase(e.name, name)) return &e;
    }
  }
  return nullptr;
}

const AccessEntry* Access::find(const std::string& playerId, const std::string& name) const {
  return const_cast<Access*>(this)->find(playerId, name);
}

AccessEntry* Access::touch(const std::string& playerId, const std::string& name) {
  if (AccessEntry* found = find(playerId, name)) {
    // Fill in whichever half we did not have. This is what makes a ban typed
    // against a bare name start covering that person's id the first time they
    // connect under it.
    if (found->playerId.empty()) found->playerId = playerId;
    if (found->name.empty()) found->name = name;
    return found;
  }
  if (entries_.size() >= kMaxAccessEntries) return nullptr;
  AccessEntry entry;
  entry.playerId = playerId;
  entry.name = name;
  entries_.push_back(std::move(entry));
  return &entries_.back();
}

void Access::prune() {
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(), saysNothing), entries_.end());
}

bool Access::remember(const std::string& playerId, const std::string& name) {
  // Only for somebody already in the table. Everybody who ever connects getting a
  // line in the file would be a file that only grows, and a list of people about
  // whom nothing is recorded is not a list of anything.
  AccessEntry* found = find(playerId, name);
  if (!found) return false;
  bool changed = false;
  if (found->playerId.empty() && !playerId.empty()) {
    found->playerId = playerId;
    changed = true;
  }
  if (found->name.empty() && !name.empty()) {
    found->name = name;
    changed = true;
  }
  return changed;
}

Level Access::levelOf(const std::string& playerId, const std::string& name) const {
  const AccessEntry* e = find(playerId, name);
  return e ? e->level : Level::Anyone;
}

bool Access::banned(const std::string& playerId, const std::string& name) const {
  const AccessEntry* e = find(playerId, name);
  return e && e->banned;
}

bool Access::mayJoin(const std::string& playerId, const std::string& name,
                     std::string& reason) const {
  const AccessEntry* e = find(playerId, name);
  if (e && e->banned) {
    reason = e->reason.empty() ? "You are banned from this world"
                               : "Banned: " + e->reason;
    return false;
  }
  if (whitelist_) {
    // An operator is on the list by virtue of being an operator. Otherwise turning
    // the whitelist on would lock out the very people trusted to turn it off, and
    // the first thing anyone did with it would be to shut themselves out.
    const bool listed = e && (e->allowed || e->level >= Level::Operator);
    if (!listed) {
      reason = "This world is whitelisted";
      return false;
    }
  }
  return true;
}

bool Access::setLevel(const std::string& playerId, const std::string& name, Level level) {
  AccessEntry* e = touch(playerId, name);
  if (!e) return false;
  e->level = level;
  prune();
  return true;
}

bool Access::setBanned(const std::string& playerId, const std::string& name, bool on,
                       const std::string& reason) {
  AccessEntry* e = touch(playerId, name);
  if (!e) return false;
  e->banned = on;
  e->reason = on ? reason : std::string();
  prune();
  return true;
}

bool Access::setAllowed(const std::string& playerId, const std::string& name, bool on) {
  AccessEntry* e = touch(playerId, name);
  if (!e) return false;
  e->allowed = on;
  prune();
  return true;
}

// ---- persistence -------------------------------------------------------------

std::string Access::toJson() const {
  std::ostringstream out;
  out << "{\n";
  out << "  \"whitelist\": " << (whitelist_ ? "true" : "false") << ",\n";
  out << "  \"players\": [";
  bool first = true;
  for (const AccessEntry& e : entries_) {
    out << (first ? "\n" : ",\n");
    first = false;
    out << "    {";
    bool comma = false;
    const auto field = [&](const char* key, const std::string& value) {
      if (comma) out << ", ";
      comma = true;
      out << '"' << key << "\": \"" << json::escape(value) << '"';
    };
    if (!e.playerId.empty()) field("id", e.playerId);
    if (!e.name.empty()) field("name", e.name);
    if (e.level != Level::Anyone) field("level", levelName(e.level));
    if (e.banned) {
      if (comma) out << ", ";
      comma = true;
      out << "\"banned\": true";
      if (!e.reason.empty()) field("reason", e.reason);
    }
    if (e.allowed) {
      if (comma) out << ", ";
      comma = true;
      out << "\"allowed\": true";
    }
    out << "}";
  }
  out << (first ? "]\n" : "\n  ]\n");
  out << "}\n";
  return out.str();
}

bool Access::fromJson(const std::string& text, std::string* errorOut) {
  std::string error;
  const json::Value doc = json::parse(text, &error);
  if (!doc.isObject()) {
    if (errorOut) *errorOut = error.empty() ? "not an object" : error;
    return false;
  }
  std::vector<AccessEntry> parsed;
  const json::Value& players = doc["players"];
  for (std::size_t i = 0; i < players.size(); ++i) {
    if (parsed.size() >= kMaxAccessEntries) break;
    const json::Value& row = players.at(i);
    if (!row.isObject()) continue;
    AccessEntry entry;
    entry.playerId = row["id"].str();
    entry.name = row["name"].str();
    // An unknown level name reads as Anyone rather than failing the file. A typo in
    // a hand-edited list should cost that one promotion, not everybody's ban.
    Level level = Level::Anyone;
    if (levelFromName(row["level"].str(), level)) entry.level = level;
    entry.banned = row["banned"].flag();
    entry.allowed = row["allowed"].flag();
    entry.reason = row["reason"].str();
    // A row naming nobody cannot be matched against anyone, so it can only ever
    // grow the file.
    if (entry.playerId.empty() && entry.name.empty()) continue;
    if (saysNothing(entry)) continue;
    parsed.push_back(std::move(entry));
  }
  entries_ = std::move(parsed);
  whitelist_ = doc["whitelist"].flag();
  return true;
}

void Access::load(const std::string& path) {
  path_ = path;
  entries_.clear();
  whitelist_ = false;

  std::ifstream in(path, std::ios::binary);
  if (!in) return;  // a fresh install: nobody trusted, nobody barred
  std::stringstream buffer;
  buffer << in.rdbuf();

  std::string error;
  if (!fromJson(buffer.str(), &error)) {
    // Logged and ignored rather than fatal. Refusing to start because a hand-edited
    // list has a stray comma would lock a server operator out of their own machine,
    // and the safe reading of "we cannot tell who is trusted" is that nobody is.
    log::warn("access list at %s is malformed (%s) — starting with an empty one",
              path.c_str(), error.c_str());
  }
}

bool Access::save() const {
  if (path_.empty()) return false;
  std::ofstream out(path_, std::ios::binary | std::ios::trunc);
  if (!out) {
    log::warn("could not write %s", path_.c_str());
    return false;
  }
  out << toJson();
  return static_cast<bool>(out);
}

}  // namespace hr::cmd
