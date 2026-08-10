// Sound events, and the packs that supply them.
//
// Every sound the game makes is a *named event* — `block.stone.break`,
// `entity.cow.hurt`, `ui.button.click`. With no packs loaded the name is unused
// and audio/sfx.cpp synthesises the sound as it always has. With a pack loaded,
// the name is looked up here first and the file wins.
//
// The names are **Minecraft's**, wherever Minecraft has an equivalent. That is
// the single decision that makes format compatibility real rather than nominal:
// a Minecraft sound pack already contains `block.stone.break`, so it works when
// dropped in, with no translation table for anybody to maintain and no chance of
// that table drifting out of date. Where this game has a sound Minecraft does not
// (`entity.player.craft`, `ui.screenshot`) the name is ours, and where the
// materials do not line up there is a fallback chain instead — `block.ore.break`
// tries `block.stone.break`, which is the sound a Minecraft pack actually ships
// for an ore block anyway.
//
// Packs arrive highest priority first (see resource/pack.h) and are applied in
// reverse, so the first pack in the list is the last one to write and therefore
// the one that wins. Within a pack, an event is resolved as:
//
//   1. its sounds.json entry, if there is one. A higher pack's entries REPLACE a
//      lower pack's for that event; `"replace": false` appends instead. That
//      default is inverted from Minecraft's, deliberately and for one reason —
//      see the note beside it in soundbank.cpp. In short: Minecraft's additive
//      default exists to let a pack add to *vanilla's* list, this game has no
//      such list under the packs, and additive across two user packs turns the
//      load order into a coin flip.
//   2. otherwise, a file at the path the event name spells out with its dots
//      turned into slashes: `block.stone.break` -> `sounds/block/stone/break.ogg`,
//      plus `break1`..`break4` for variants.
//
// If no pack supplies the event, the fallback event is tried the same way, and if
// that fails too the sound is synthesised as it always was.
//
// Step 2 is not Minecraft's; it is here because the main cost of authoring a pack
// is writing a sounds.json by hand, and dropping a file at the path the event name
// spells out should be enough.
//
// ---------------------------------------------------------------------------
// Threading
//
// pick() runs on the game thread, from sfx. rebuild() runs on the game thread,
// from the menu. Nothing here is touched by the audio thread — but the audio
// thread holds a raw SoundClip* inside a live voice for as long as that voice is
// playing, which is why **clips are never freed while the game is running**.
// rebuild() replaces the index and leaves the arena alone; a reload of the same
// pack reuses the clips it already decoded rather than growing it. Freeing a clip
// the mixer is reading is a use-after-free with no symptom until it crackles.
// ---------------------------------------------------------------------------

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "resource/pack.h"

namespace hr::audio {

struct SoundClip {
  std::vector<float> mono;
  int sampleRate = 0;
  // The file it came from, for the pack report and the log. Not read by playback.
  std::string source;

  double seconds() const {
    return sampleRate > 0 ? static_cast<double>(mono.size()) / sampleRate : 0.0;
  }
};

// One choice within an event. Minecraft's per-sound `volume`, `pitch` and
// `weight` all mean what they mean there: weight biases the random choice, and
// the other two scale the clip when it is picked.
struct SoundVariant {
  const SoundClip* clip = nullptr;
  float volume = 1.0f;
  float pitch = 1.0f;
  int weight = 1;
};

struct SoundPick {
  const SoundClip* clip = nullptr;
  float volume = 1.0f;
  float pitch = 1.0f;
  bool valid() const { return clip != nullptr && clip->sampleRate > 0 && !clip->mono.empty(); }
};

class SoundBank {
 public:
  struct Stats {
    int events = 0;
    int clips = 0;
    std::size_t bytes = 0;
    int missing = 0;  // entries naming a file that is not there
  };

  // Decodes and indexes the ordered pack list, replacing whatever was loaded.
  // Called from the menu, never during play: decoding is milliseconds per clip
  // and there is no reason to hide it behind a job when the alternative is a
  // menu that pauses for a moment.
  void rebuild(const std::vector<resource::PackInfo>& ordered);

  // Drops the index so every event falls back to the synthesised sound. The
  // arena is deliberately kept — see the threading note above.
  void clear();

  bool empty() const { return events_.empty(); }
  bool has(std::string_view event) const;

  // The clip to play, chosen by weight, or an invalid pick when no pack supplies
  // this event or its fallback. `roll` is a 0..1 random draw, passed in rather
  // than taken here so a test can pin the choice.
  SoundPick pick(std::string_view event, float roll) const;

  const Stats& stats() const { return stats_; }
  // Per-pack lines for the Resource Packs screen and the log: what loaded, what
  // did not, and why. Bounded, so a pack with a thousand broken entries cannot
  // fill memory with complaints about itself.
  const std::vector<std::string>& warnings() const { return warnings_; }
  // Every event this bank can answer, sorted. For --dump-sounds.
  std::vector<std::string> eventNames() const;

 private:
  const SoundClip* loadClip(const std::string& path);
  // Fills `into` from one pack, honouring "replace" and the convention fallback.
  void addPack(const resource::PackInfo& pack);
  void warn(const std::string& line);

  // Append-only. Keyed by absolute path so a pack toggled off and on again is
  // free rather than decoded twice.
  std::unordered_map<std::string, std::unique_ptr<SoundClip>> arena_;
  // Everything the arena holds, including clips no longer referenced by any
  // enabled pack. This is the figure the memory cap is measured against, and it
  // is deliberately not the one reported: what a player wants to know is how much
  // the packs they have on are using, not what is still cached from one they
  // turned off.
  std::size_t arenaBytes_ = 0;
  std::unordered_map<std::string, std::vector<SoundVariant>> events_;
  std::vector<std::string> warnings_;
  // Every warning raised, including the ones past the cap that were neither
  // stored nor logged, so the summary line can say how many were dropped.
  int warningCount_ = 0;
  Stats stats_;
};

SoundBank& sounds();

// Every event this build can fire, sorted, for the example pack and --dump-sounds.
const std::vector<std::string>& soundEventCatalogue();

// The event to try when `event` is in no pack, or empty when there is none.
// One level only: the chains are hand-written and short by design, and a lookup
// that can walk is a lookup that can loop.
std::string_view soundEventFallback(std::string_view event);

}  // namespace hr::audio
