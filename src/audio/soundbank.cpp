#include "audio/soundbank.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <unordered_set>

#include "audio/decode.h"
#include "core/json.h"
#include "core/log.h"

namespace fs = std::filesystem;

namespace hr::audio {
namespace {

// A pack cannot be allowed to decode itself into swap. 192 MB of float mono is
// about 17 minutes of 48 kHz audio, which is far more than any sound pack, and
// small enough that hitting it is a bug in the pack rather than a limit worth
// raising.
constexpr std::size_t kMaxBankBytes = 192u * 1024u * 1024u;
// A pack with a systematic mistake produces one complaint per entry. Forty is
// enough to see the pattern; the rest are counted, not stored.
constexpr std::size_t kMaxWarnings = 40;
// How far an unlisted file is probed for numbered variants. Six, not four,
// because vanilla's footsteps run stone1..stone6 and stopping at four would take
// two thirds of a pack's step variety without saying so.
constexpr int kMaxConventionVariants = 6;

// Windows refuses to open a path of 260 characters or more without opting in to
// long paths, which this build does not. Anything near it is reported as what it
// is rather than as a missing file.
constexpr std::size_t kPathLimitWarning = 250;

const char* const kExtensions[] = {".ogg", ".wav"};

// The material half of a block event, in the order audio/sfx.h declares them.
// `wool` rather than `cloth` and `dirt` rather than `ground`: the first is
// Minecraft's spelling and the second is not, but Minecraft folds dirt in with
// gravel and this game does not, so it gets its own name and a fallback.
const char* const kMaterials[] = {"stone", "ore",    "wood",   "dirt", "sand",
                                  "gravel", "grass", "leaves", "wool", "glass"};
const char* const kBlockActions[] = {"break", "hit", "place", "step"};

// Events with no material in them. Minecraft's spelling wherever it has one, so
// that a Minecraft pack supplies them without being edited.
const char* const kFixedEvents[] = {
    // blocks with a voice of their own
    "block.wooden_door.open", "block.wooden_door.close", "block.wooden_trapdoor.open",
    "block.wooden_trapdoor.close", "block.chest.open", "block.chest.close",
    "block.furnace.smelt",
    // the player
    "entity.player.hurt", "entity.player.death", "entity.player.big_fall",
    "entity.player.small_fall", "entity.player.splash", "entity.player.swim",
    "entity.player.bubbles", "entity.player.craft", "entity.player.attack.sweep",
    "entity.player.attack.strong", "entity.player.attack.crit", "entity.generic.eat",
    "entity.generic.burn", "entity.enderman.teleport", "entity.item.pickup",
    "entity.item.throw",
    // mobs
    "entity.sheep.ambient", "entity.sheep.hurt", "entity.sheep.death", "entity.pig.ambient",
    "entity.pig.hurt", "entity.pig.death", "entity.cow.ambient", "entity.cow.hurt",
    "entity.cow.death", "entity.zombie.ambient", "entity.zombie.hurt", "entity.zombie.death",
    // interface
    "ui.button.click", "ui.slot.click", "ui.screenshot",
};

struct Fallback {
  const char* from;
  const char* to;
};

// Hand-written and short. Every one of these exists so that a pack made for
// Minecraft covers a sound this game has and Minecraft does not, rather than
// leaving a hole a player would hear as "the pack only half works".
const Fallback kFallbacks[] = {
    // Minecraft has no ore sound type: ores use the stone set, which is exactly
    // what falling through to it produces.
    {"block.ore.break", "block.stone.break"},
    {"block.ore.hit", "block.stone.hit"},
    {"block.ore.place", "block.stone.place"},
    {"block.ore.step", "block.stone.step"},
    // Minecraft's SoundType.GROUND — plain dirt — is the gravel set.
    {"block.dirt.break", "block.gravel.break"},
    {"block.dirt.hit", "block.gravel.hit"},
    {"block.dirt.place", "block.gravel.place"},
    {"block.dirt.step", "block.gravel.step"},
    // ...and its leaves are the grass set.
    {"block.leaves.break", "block.grass.break"},
    {"block.leaves.hit", "block.grass.hit"},
    {"block.leaves.place", "block.grass.place"},
    {"block.leaves.step", "block.grass.step"},
    // A pack that does doors probably did not do trapdoors separately.
    {"block.wooden_trapdoor.open", "block.wooden_door.open"},
    {"block.wooden_trapdoor.close", "block.wooden_door.close"},
    // Wading is a quieter version of the same splash.
    {"entity.player.swim", "entity.player.splash"},
    // Throwing an item has no Minecraft event; picking one up does.
    {"entity.item.throw", "entity.item.pickup"},
    // Two interface sounds Minecraft does not name. Any pack with a click has
    // something reasonable to say here.
    {"ui.slot.click", "ui.button.click"},
    {"ui.screenshot", "ui.button.click"},
};

// "block.stone.break" -> "block/stone/break"
std::string eventToPath(std::string_view event) {
  std::string out(event);
  std::replace(out.begin(), out.end(), '.', '/');
  return out;
}

// ---------------------------------------------------------------------------
// Minecraft's own default sound paths.
//
// This table is the equivalent of the `sounds.json` Minecraft ships INSIDE its
// game jar, and without it most real Minecraft sound packs do almost nothing.
//
// The reason is not obvious until you take one apart. A pack's own sounds.json
// only lists the events it wants to change the *definition* of — weights, extra
// variants, a different file. To change how something SOUNDS, the usual and much
// simpler thing is to drop a replacement .ogg at the path vanilla already uses
// and ship no entry for it at all, because vanilla's table still supplies the
// mapping. One real pack tested here defines 140 events but ships 662 files: the
// other five hundred are exactly that case, and to us they were invisible.
//
// The paths are irregular and cannot be derived from a rule — they are what a
// decade of Minecraft's history left behind:
//
//     block.stone.break  -> dig/stone1..4          (the pre-1.8 folder, still used)
//     block.stone.hit    -> step/stone1..6          (mining ticks ARE the step sounds)
//     entity.item.pickup -> random/pop, pop2, pop3  (note: no pop1)
//     entity.player.hurt -> damage/hit1..3
//     entity.cow.ambient -> mob/cow/say1..4
//
// so a lookup table is the only honest form for it. Anything absent from a pack
// simply is not found, which costs a few stat calls and nothing else.
struct VanillaPaths {
  const char* event;
  // Probed as `base`, then `base1`..`base6`; see kMaxConventionVariants. That
  // covers both `step/stone1..6` and the `pop`/`pop2`/`pop3` spelling.
  const char* bases[3];
};

const VanillaPaths kVanillaPaths[] = {
    // Materials. Vanilla keeps break and place in dig/, and step and hit in
    // step/ — a block's mining tick really is its footstep sound.
    {"block.stone.break", {"dig/stone"}},
    {"block.stone.place", {"dig/stone"}},
    {"block.stone.hit", {"step/stone"}},
    {"block.stone.step", {"step/stone"}},
    // Ores use the stone set in Minecraft; spelled out rather than left to the
    // fallback table so a pack that ships only dig/stone still covers them.
    {"block.ore.break", {"dig/stone"}},
    {"block.ore.place", {"dig/stone"}},
    {"block.ore.hit", {"step/stone"}},
    {"block.ore.step", {"step/stone"}},
    {"block.wood.break", {"dig/wood"}},
    {"block.wood.place", {"dig/wood"}},
    {"block.wood.hit", {"step/wood"}},
    {"block.wood.step", {"step/wood"}},
    // Minecraft's SoundType.GROUND — plain dirt — is the gravel set.
    {"block.dirt.break", {"dig/gravel"}},
    {"block.dirt.place", {"dig/gravel"}},
    {"block.dirt.hit", {"step/gravel"}},
    {"block.dirt.step", {"step/gravel"}},
    {"block.gravel.break", {"dig/gravel"}},
    {"block.gravel.place", {"dig/gravel"}},
    {"block.gravel.hit", {"step/gravel"}},
    {"block.gravel.step", {"step/gravel"}},
    {"block.sand.break", {"dig/sand"}},
    {"block.sand.place", {"dig/sand"}},
    {"block.sand.hit", {"step/sand"}},
    {"block.sand.step", {"step/sand"}},
    {"block.grass.break", {"dig/grass"}},
    {"block.grass.place", {"dig/grass"}},
    {"block.grass.hit", {"step/grass"}},
    {"block.grass.step", {"step/grass"}},
    // Leaves are the grass set in vanilla too.
    {"block.leaves.break", {"dig/grass"}},
    {"block.leaves.place", {"dig/grass"}},
    {"block.leaves.hit", {"step/grass"}},
    {"block.leaves.step", {"step/grass"}},
    {"block.wool.break", {"dig/cloth"}},
    {"block.wool.place", {"dig/cloth"}},
    {"block.wool.hit", {"step/cloth"}},
    {"block.wool.step", {"step/cloth"}},
    // Glass shatters from random/, but steps and taps come from the usual pair.
    {"block.glass.break", {"random/glass", "dig/glass"}},
    {"block.glass.place", {"dig/glass"}},
    {"block.glass.hit", {"step/glass"}},
    {"block.glass.step", {"step/glass"}},

    // Doors and containers, which moved to the modern block/ layout.
    {"block.wooden_door.open", {"block/wooden_door/open", "random/door_open"}},
    {"block.wooden_door.close", {"block/wooden_door/close", "random/door_close"}},
    {"block.wooden_trapdoor.open", {"block/wooden_trapdoor/open"}},
    {"block.wooden_trapdoor.close", {"block/wooden_trapdoor/close"}},
    {"block.chest.open", {"block/chest/open", "random/chestopen"}},
    {"block.chest.close", {"block/chest/close", "random/chestclosed"}},

    // The player.
    {"entity.player.hurt", {"damage/hit"}},
    // Minecraft has no separate player death sound; it reuses the hurt set.
    {"entity.player.death", {"damage/hit"}},
    {"entity.player.big_fall", {"damage/fallbig"}},
    {"entity.player.small_fall", {"damage/fallsmall"}},
    {"entity.player.splash", {"liquid/splash"}},
    {"entity.player.swim", {"liquid/swim"}},
    {"entity.player.attack.crit", {"entity/player/attack/crit"}},
    {"entity.player.attack.strong", {"entity/player/attack/strong"}},
    {"entity.player.attack.sweep", {"entity/player/attack/sweep"}},
    {"entity.generic.eat", {"random/eat"}},
    {"entity.generic.burn", {"random/fizz"}},
    {"entity.enderman.teleport", {"mob/endermen/portal"}},
    {"entity.item.pickup", {"random/pop"}},
    {"entity.item.throw", {"random/bow"}},

    // Mobs. These are why a Minecraft pack can give this game farm animals that
    // the built-in synthesiser only approximates.
    {"entity.cow.ambient", {"mob/cow/say"}},
    {"entity.cow.hurt", {"mob/cow/hurt"}},
    {"entity.cow.death", {"mob/cow/hurt"}},
    {"entity.pig.ambient", {"mob/pig/say"}},
    {"entity.pig.hurt", {"mob/pig/say"}},
    {"entity.pig.death", {"mob/pig/death"}},
    {"entity.sheep.ambient", {"mob/sheep/say"}},
    {"entity.sheep.hurt", {"mob/sheep/say"}},
    {"entity.sheep.death", {"mob/sheep/say"}},
    {"entity.zombie.ambient", {"mob/zombie/say"}},
    {"entity.zombie.hurt", {"mob/zombie/hurt"}},
    {"entity.zombie.death", {"mob/zombie/death"}},

    {"ui.button.click", {"random/click"}},
    {"ui.slot.click", {"random/click"}},
};

}  // namespace

const std::vector<std::string>& soundEventCatalogue() {
  static const std::vector<std::string> kAll = [] {
    std::vector<std::string> out;
    for (const char* material : kMaterials) {
      for (const char* action : kBlockActions) {
        out.push_back(std::string("block.") + material + "." + action);
      }
    }
    for (const char* fixed : kFixedEvents) out.emplace_back(fixed);
    std::sort(out.begin(), out.end());
    return out;
  }();
  return kAll;
}

std::string_view soundEventFallback(std::string_view event) {
  for (const Fallback& f : kFallbacks) {
    if (event == f.from) return f.to;
  }
  return {};
}

namespace {

// Whether this game can ever play `event`.
//
// A Minecraft pack describes Minecraft's whole sound set — anvils, pistons,
// villagers, the wither — and this game has none of those. Loading them anyway
// cost three things: memory for clips that can never be heard (a 662-file pack
// decoded four times more audio than it needed), a "184 of 78 replaced" summary
// that was arithmetic on two different denominators, and a warning for every
// file a pack was missing for an event we do not have — which buried the ones
// that mattered under complaints about `block.wool.fall`.
bool playable(const std::string& event) {
  static const std::unordered_set<std::string> kPlayable = [] {
    std::unordered_set<std::string> out;
    for (const std::string& e : soundEventCatalogue()) {
      out.insert(e);
      // A fallback target must be loadable even when it is not itself something
      // the game fires directly — every event in this build's catalogue happens
      // to be both, but that is not something to rely on silently.
      const std::string_view to = soundEventFallback(e);
      if (!to.empty()) out.insert(std::string(to));
    }
    return out;
  }();
  return kPlayable.count(event) > 0;
}

}  // namespace

// ---------------------------------------------------------------------------

SoundBank& sounds() {
  static SoundBank bank;
  return bank;
}

void SoundBank::warn(const std::string& line) {
  // Capped in the log as well as on the screen, and counted off warningCount_
  // rather than off warnings_.size() — the vector stops growing at the cap, so
  // testing its size would print the "and more" line once per warning from then
  // on, which is the noise this is trying to stop.
  ++warningCount_;
  if (warningCount_ <= static_cast<int>(kMaxWarnings)) {
    log::warn("packs: %s", line.c_str());
    warnings_.push_back(line);
  } else if (warningCount_ == static_cast<int>(kMaxWarnings) + 1) {
    log::warn("packs: ...and more of the same; the rest are not logged");
  }
}

void SoundBank::clear() {
  // The arena is deliberately NOT cleared. A voice on the audio thread may still
  // be reading a clip, and there is no handshake with that thread short of
  // stopping the device. Keeping the decoded bytes costs memory that is already
  // bounded; freeing them costs a use-after-free in the mixer.
  events_.clear();
  warnings_.clear();
  warningCount_ = 0;
  stats_ = Stats{};
}

bool SoundBank::has(std::string_view event) const {
  const auto it = events_.find(std::string(event));
  return it != events_.end() && !it->second.empty();
}

const SoundClip* SoundBank::loadClip(const std::string& path) {
  const auto existing = arena_.find(path);
  if (existing != arena_.end()) return existing->second.get();

  // Measured against the arena, not against stats_.bytes. The arena is what holds
  // memory — it never frees — while stats_ describes only what the current
  // selection references, and a cap read off that would let the arena grow without
  // limit as packs are toggled.
  if (arenaBytes_ >= kMaxBankBytes) {
    warn("sound memory limit reached, skipping " + path);
    return nullptr;
  }

  DecodedAudio decoded;
  std::string error;
  if (!decodeAudioFile(path, decoded, &error) || decoded.empty()) {
    warn(error.empty() ? ("could not decode " + path) : error);
    return nullptr;
  }

  auto clip = std::make_unique<SoundClip>();
  clip->mono = std::move(decoded.mono);
  clip->sampleRate = decoded.sampleRate;
  clip->source = path;
  const SoundClip* raw = clip.get();
  arenaBytes_ += raw->mono.size() * sizeof(float);
  arena_.emplace(path, std::move(clip));
  return raw;
}

void SoundBank::addPack(const resource::PackInfo& pack) {
  // Which events this pack has already spoken for, so the convention probe below
  // does not also fire for an event sounds.json already described.
  std::vector<std::string> described;

  // Namespaces are applied in reverse search order for the same reason packs are:
  // `hollowreach` is searched first and therefore has to be written last, so that
  // a pack carrying both can use `replace` in assets/hollowreach/ to override what
  // its own assets/minecraft/ supplied.
  for (auto nsIt = pack.namespaces.rbegin(); nsIt != pack.namespaces.rend(); ++nsIt) {
    const std::string& ns = *nsIt;
    const std::string soundsRoot = "assets/" + ns + "/sounds/";
    const std::string metaRel = "assets/" + ns + "/sounds.json";
    const std::string metaPath = resource::packFile(pack, metaRel);
    if (metaPath.empty()) continue;

    std::error_code ec;
    if (fs::is_regular_file(metaPath, ec)) {
      std::string error;
      const json::Value doc = json::parseFile(metaPath, &error);
      if (!error.empty()) {
        warn(pack.id + ": " + error);
      } else if (!doc.isObject()) {
        warn(pack.id + ": sounds.json is not an object of events");
      } else {
        for (const auto& [eventName, entry] : doc.fields()) {
          if (eventName.empty()) continue;
          // Skipped before its files are touched, so a Minecraft pack's several
          // hundred events this game has no use for cost nothing at all.
          if (!playable(eventName)) continue;
          described.push_back(eventName);
          std::vector<SoundVariant> collected;

          const json::Value& list = entry["sounds"];
          for (const json::Value& item : list.items()) {
            SoundVariant variant;
            std::string name;
            if (item.isString()) {
              name = item.str();
            } else if (item.isObject()) {
              // "type": "event" redirects to another event. Honouring it would
              // mean resolving one name through another mid-build, which is a
              // graph with cycles in it; the fallback table already covers the
              // case it exists for, so it is reported rather than followed.
              const std::string type = item["type"].str();
              if (type == "event") {
                warn(pack.id + ": \"type\": \"event\" is not supported (" + eventName + ")");
                continue;
              }
              name = item["name"].str();
              variant.volume = static_cast<float>(item["volume"].num(1.0));
              variant.pitch = static_cast<float>(item["pitch"].num(1.0));
              variant.weight = static_cast<int>(item["weight"].num(1.0));
            } else {
              continue;
            }
            if (name.empty()) continue;

            // Minecraft allows a namespaced name here. The namespace selects a
            // folder, not a pack, so `minecraft:block/x` in a hollowreach/ pack
            // reads from assets/minecraft/sounds/ if that folder exists.
            std::string fileNs = ns;
            const std::size_t colon = name.find(':');
            if (colon != std::string::npos) {
              fileNs = name.substr(0, colon);
              name = name.substr(colon + 1);
            }

            std::string found;
            for (const char* ext : kExtensions) {
              const std::string rel = "assets/" + fileNs + "/sounds/" + name + ext;
              const std::string full = resource::packFile(pack, rel);
              // Empty means safeRelativePath refused it — a name that climbs out
              // of the pack folder. Reported, because a pack doing that is worth
              // knowing about rather than silently ignoring.
              if (full.empty()) {
                warn(pack.id + ": refusing unsafe sound path in " + eventName);
                break;
              }
              if (fs::is_regular_file(full, ec)) {
                found = full;
                break;
              }
            }
            if (found.empty()) {
              ++stats_.missing;
              // "not in the pack" is the wrong answer when the file is right
              // there and Windows simply will not open a path that long. Naming
              // the real cause matters because the fix is completely different:
              // move the game somewhere shorter, not repack the sounds.
              const std::string probePath =
                  resource::packFile(pack, "assets/" + fileNs + "/sounds/" + name + ".ogg");
              if (probePath.size() >= kPathLimitWarning) {
                warn(pack.id + ": paths are too long for this system (" +
                     std::to_string(probePath.size()) +
                     " characters) — move the game to a shorter folder");
                break;  // the whole pack has the same problem; say it once
              }
              warn(pack.id + ": " + eventName + " names " + name + ", which is not in the pack");
              continue;
            }

            variant.clip = loadClip(found);
            if (!variant.clip) continue;
            variant.weight = std::max(1, std::min(1000, variant.weight));
            variant.volume = std::clamp(variant.volume, 0.0f, 4.0f);
            // Below about an eighth speed a clip is a drone rather than the
            // sound it was; above four times it is a chirp. Both are almost
            // certainly a typo, and clamping keeps the resample cursor sane.
            variant.pitch = std::clamp(variant.pitch, 0.125f, 4.0f);
            collected.push_back(variant);
          }

          // `replace` — and the one deliberate divergence from Minecraft in this
          // whole file: it defaults to TRUE here and to false there.
          //
          // Minecraft's default is additive because its base layer is vanilla's
          // own sounds.json, and "add my footstep to the four that already exist"
          // is the common thing to want. This game has no such base layer: below
          // the packs is a synthesiser that is not in any list. So additive across
          // two user packs means both packs' clips play at random for the same
          // event — and a player who has just dragged one pack above another to
          // choose between them hears a coin flip instead. The load order would
          // be a control that does nothing, which is worse than a divergence.
          //
          // With a single pack installed — overwhelmingly the common case — the
          // two defaults are indistinguishable. `"replace": false` opts back into
          // Minecraft's behaviour for anyone stacking variants on purpose.
          //
          // An entry that replaces with no usable sounds is a deliberate
          // *removal*, which is why the clear happens whether or not `collected`
          // has anything in it.
          const bool replace = entry["replace"].flag(true);
          std::vector<SoundVariant>& target = events_[eventName];
          if (replace) target.clear();
          target.insert(target.end(), collected.begin(), collected.end());
        }
      }
    }

    // Files with no sounds.json entry naming them. Two spellings are tried, in
    // this order, and the first that finds anything wins:
    //
    //   1. our own convention — `block.stone.break` -> block/stone/break[1..6]
    //   2. Minecraft's default paths — the same event -> dig/stone[1..6]
    //
    // (2) is what makes a real Minecraft pack work. Ours is tried first so that a
    // pack written for this game is never second-guessed by a legacy path that
    // happens to exist alongside it.
    auto probe = [&](const std::string& base, std::vector<SoundVariant>& into) {
      for (int variant = 0; variant <= kMaxConventionVariants; ++variant) {
        std::string stem = base;
        // Probed as `base` and then `base1..base6`, not `base1..` alone: vanilla
        // spells its three pickup sounds pop, pop2, pop3 — with no pop1 — while
        // its footsteps are stone1..stone6. One loop covers both only if the
        // unnumbered stem is a candidate too.
        if (variant > 0) stem += std::to_string(variant);
        for (const char* ext : kExtensions) {
          const std::string full = resource::packFile(pack, stem + ext);
          if (full.empty()) continue;
          std::error_code stat;
          if (!fs::is_regular_file(full, stat)) continue;
          SoundVariant v;
          v.clip = loadClip(full);
          if (v.clip) into.push_back(v);
          break;  // .ogg wins over .wav for the same stem
        }
      }
    };

    for (const std::string& event : soundEventCatalogue()) {
      if (std::find(described.begin(), described.end(), event) != described.end()) continue;
      std::vector<SoundVariant> found;
      probe(soundsRoot + eventToPath(event), found);
      if (found.empty()) {
        for (const VanillaPaths& row : kVanillaPaths) {
          if (event != row.event) continue;
          for (const char* base : row.bases) {
            if (base) probe(soundsRoot + base, found);
          }
          break;
        }
      }
      if (found.empty()) continue;
      // Replaces, for the same reason sounds.json does above — and here there is
      // not even a `replace` key to say otherwise, since the whole point of the
      // convention path is that it needs no metadata. A file dropped in at the
      // conventional path is an override; `break1..break4` beside it are that
      // override's own variants, which is why they are collected first and
      // written together.
      std::vector<SoundVariant>& target = events_[event];
      target.clear();
      target.insert(target.end(), found.begin(), found.end());
    }
  }
}

void SoundBank::rebuild(const std::vector<resource::PackInfo>& ordered) {
  clear();
  // Applied back to front: the list is highest priority first, and a pack's
  // `replace` has to be able to discard what the packs below it contributed, so
  // the highest priority pack must be the last one to write.
  for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
    if (!it->usable()) continue;
    addPack(*it);
  }

  // An event whose every entry failed to load is not an event.
  for (auto it = events_.begin(); it != events_.end();) {
    if (it->second.empty()) it = events_.erase(it);
    else ++it;
  }
  // Counted over the catalogue rather than over the index, and through pick() so
  // that an event reached via its fallback counts as replaced — it is, you can
  // hear it. Counting index entries instead reported "184 of 78", which is two
  // different denominators in one sentence.
  stats_.events = 0;
  for (const std::string& event : soundEventCatalogue()) {
    if (pick(event, 0.0f).valid()) ++stats_.events;
  }

  // Counted from what the selection actually references, not from what this
  // rebuild happened to decode.
  //
  // Incrementing in loadClip looked equivalent and was not: the arena is a cache,
  // so the SECOND rebuild of the same packs decodes nothing and reported "0 clips,
  // 0.0 MB" — which is every rebuild after the first, including every press of
  // Reload and every toggle. Distinct clips, because several events sharing one
  // file (a fallback, or four block actions pointing at the same hit) is normal
  // and must not be counted four times.
  std::vector<const SoundClip*> distinct;
  for (const auto& [name, variants] : events_) {
    for (const SoundVariant& v : variants) {
      if (std::find(distinct.begin(), distinct.end(), v.clip) == distinct.end()) {
        distinct.push_back(v.clip);
      }
    }
  }
  stats_.clips = static_cast<int>(distinct.size());
  stats_.bytes = 0;
  for (const SoundClip* clip : distinct) stats_.bytes += clip->mono.size() * sizeof(float);

  if (warningCount_ > static_cast<int>(kMaxWarnings)) {
    char line[96];
    std::snprintf(line, sizeof(line), "...and %d more; see the log",
                  warningCount_ - static_cast<int>(kMaxWarnings));
    warnings_.push_back(line);
  }
  log::info("packs: %d sound events from %d clips (%.1f MB)%s", stats_.events, stats_.clips,
            static_cast<double>(stats_.bytes) / (1024.0 * 1024.0),
            stats_.missing > 0 ? " — some entries name missing files" : "");
}

SoundPick SoundBank::pick(std::string_view event, float roll) const {
  auto choose = [&](std::string_view name) -> SoundPick {
    const auto it = events_.find(std::string(name));
    if (it == events_.end() || it->second.empty()) return {};
    const std::vector<SoundVariant>& variants = it->second;

    int total = 0;
    for (const SoundVariant& v : variants) total += v.weight;
    if (total <= 0) return {};
    // The draw is clamped rather than wrapped: a roll of exactly 1.0 must select
    // the last entry, not fall off the end and select nothing.
    int target = static_cast<int>(std::clamp(roll, 0.0f, 0.999999f) * static_cast<float>(total));
    for (const SoundVariant& v : variants) {
      target -= v.weight;
      if (target < 0) return SoundPick{v.clip, v.volume, v.pitch};
    }
    const SoundVariant& last = variants.back();
    return SoundPick{last.clip, last.volume, last.pitch};
  };

  SoundPick got = choose(event);
  if (got.valid()) return got;
  const std::string_view fallback = soundEventFallback(event);
  if (!fallback.empty()) {
    got = choose(fallback);
    if (got.valid()) return got;
  }
  return {};
}

std::vector<std::string> SoundBank::eventNames() const {
  std::vector<std::string> out;
  out.reserve(events_.size());
  for (const auto& [name, variants] : events_) {
    if (!variants.empty()) out.push_back(name);
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace hr::audio
