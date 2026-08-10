#include "dev/audiodump.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <vector>

#include "audio/engine.h"
#include "audio/sfx.h"
#include "audio/soundbank.h"
#include "platform/paths.h"
#include "resource/pack.h"
#include "ui/settings.h"
#include "world/blocks.h"

namespace fs = std::filesystem;

namespace hr::dev {
namespace {

constexpr int kSampleRate = 48000;

struct Event {
  const char* name;
  float seconds;
  void (*fire)();
};

const world::BlockDef& blockNamed(const char* key) {
  return world::blocks().def(world::blocks().idOf(key));
}

// A position a couple of blocks in front of a listener at the origin looking down
// -Z, so the panner and the distance falloff are exercised rather than bypassed.
constexpr Vec3 kNear{1.5f, 0.0f, -2.5f};

const Event kEvents[] = {
    {"dig_stone", 0.4f, [] { audio::sfx::blockHit(blockNamed("stone"), kNear); }},
    {"dig_wood", 0.4f, [] { audio::sfx::blockHit(blockNamed("planks"), kNear); }},
    {"dig_dirt", 0.4f, [] { audio::sfx::blockHit(blockNamed("turf"), kNear); }},
    {"break_stone", 1.2f, [] { audio::sfx::blockBreak(blockNamed("stone"), kNear); }},
    {"break_wood", 1.0f, [] { audio::sfx::blockBreak(blockNamed("planks"), kNear); }},
    {"break_glass", 1.0f, [] { audio::sfx::blockBreak(blockNamed("glass"), kNear); }},
    {"break_ore", 1.2f, [] { audio::sfx::blockBreak(blockNamed("ore_copper"), kNear); }},
    {"break_leaves", 0.8f, [] { audio::sfx::blockBreak(blockNamed("leaves"), kNear); }},
    {"break_sand", 0.8f, [] { audio::sfx::blockBreak(blockNamed("sand"), kNear); }},
    {"place", 0.6f, [] { audio::sfx::blockPlace(blockNamed("stone"), kNear); }},
    {"step_grass", 0.5f, [] { audio::sfx::step(blockNamed("turf"), false); }},
    {"step_sprint", 0.5f, [] { audio::sfx::step(blockNamed("stone"), true); }},
    {"wade", 0.6f, [] { audio::sfx::wadeStep(); }},
    {"door_open", 1.0f, [] { audio::sfx::doorToggle(blockNamed("door_oak"), true, kNear); }},
    {"door_close", 1.0f, [] { audio::sfx::doorToggle(blockNamed("door_oak"), false, kNear); }},
    {"chest_open", 1.2f, [] { audio::sfx::chestOpen(kNear); }},
    {"chest_close", 1.0f, [] { audio::sfx::chestClose(kNear); }},
    {"craft", 0.8f, [] { audio::sfx::craft(); }},
    {"smelt_done", 0.6f, [] { audio::sfx::smeltDone(kNear); }},
    {"hurt", 0.6f, [] { audio::sfx::hurt(); }},
    {"died", 1.2f, [] { audio::sfx::died(); }},
    {"land_soft", 0.5f, [] { audio::sfx::land(0.15f); }},
    {"land_hard", 0.5f, [] { audio::sfx::land(1.0f); }},
    {"eat", 1.4f, [] { audio::sfx::eat(); }},
    {"splash", 1.0f, [] { audio::sfx::splash(true); }},
    {"bubbles", 0.8f, [] { audio::sfx::bubbles(); }},
    {"pickup", 0.4f, [] { audio::sfx::pickup(); }},
    {"toss", 0.4f, [] { audio::sfx::toss(); }},
    {"swing", 0.4f, [] { audio::sfx::swing(); }},
    {"thwack", 0.4f, [] { audio::sfx::thwack(kNear); }},
    {"crit", 0.6f, [] { audio::sfx::crit(); }},
    {"warp", 1.2f, [] { audio::sfx::warp(); }},
    {"shutter", 0.4f, [] { audio::sfx::shutter(); }},
    {"sheep", 1.4f, [] { audio::sfx::sheep(audio::sfx::MobCall::Say, kNear, 3); }},
    {"pig", 0.8f, [] { audio::sfx::pig(audio::sfx::MobCall::Say, kNear, 3); }},
    {"cow", 1.8f, [] { audio::sfx::cow(audio::sfx::MobCall::Say, kNear, 3); }},
    {"zombie", 1.8f, [] { audio::sfx::zombie(audio::sfx::MobCall::Say, kNear, 3); }},
    {"sizzle", 0.9f, [] { audio::sfx::sizzle(kNear); }},
    {"ui_click", 0.3f, [] { audio::sfx::uiClick(); }},
    {"ui_slot", 0.3f, [] { audio::sfx::uiSlot(); }},
    // The two persistent beds. Nothing else exercises the looping noise readers or
    // the detuned triangle drone, and both wrap their buffer inside these windows —
    // the cave murmur back to its 0.7-second loop point, the wind to zero — so a
    // click here would mean the loop arithmetic is wrong.
    {"bed_wind", 4.0f,
     [] {
       audio::engine().setBedGain(audio::Bed::Cave, 0.0f, 0.05f);
       audio::engine().setBedGain(audio::Bed::Wind, 0.15f, 0.2f);
       audio::engine().setWindFilter(400.0f, 0.2f);
     }},
    {"bed_cave", 4.0f,
     [] {
       audio::engine().setBedGain(audio::Bed::Wind, 0.0f, 0.05f);
       audio::engine().setBedGain(audio::Bed::Cave, 0.11f, 0.2f);
     }},
};

void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFF));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}
void putU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFF));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}
void putTag(std::vector<std::uint8_t>& out, const char* tag) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(tag[i]));
}

bool writeWav(const std::string& path, const std::vector<float>& interleaved, int sampleRate) {
  const auto frames = static_cast<std::uint32_t>(interleaved.size() / 2);
  const std::uint32_t dataBytes = frames * 2 * 2;  // stereo, 16-bit

  std::vector<std::uint8_t> out;
  out.reserve(44 + dataBytes);
  putTag(out, "RIFF");
  putU32(out, 36 + dataBytes);
  putTag(out, "WAVE");
  putTag(out, "fmt ");
  putU32(out, 16);
  putU16(out, 1);  // PCM
  putU16(out, 2);
  putU32(out, static_cast<std::uint32_t>(sampleRate));
  putU32(out, static_cast<std::uint32_t>(sampleRate) * 4);
  putU16(out, 4);
  putU16(out, 16);
  putTag(out, "data");
  putU32(out, dataBytes);
  for (float v : interleaved) {
    const float clamped = std::max(-1.0f, std::min(1.0f, v));
    putU16(out, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(clamped * 32767.0f))));
  }

  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

// Renders one event and reports what came out. The engine is a singleton, so this
// resets the mix by rendering to silence between events rather than rebuilding it.
bool renderEvent(const Event& event, const std::string& path) {
  const int frames = static_cast<int>(event.seconds * kSampleRate);
  std::vector<float> buffer(static_cast<std::size_t>(frames) * 2, 0.0f);

  event.fire();
  audio::engine().renderOffline(buffer.data(), frames);

  float peak = 0.0f;
  double sum = 0.0;
  int clipped = 0;
  for (float v : buffer) {
    peak = std::max(peak, std::fabs(v));
    sum += static_cast<double>(v) * v;
    if (std::fabs(v) >= 1.0f) ++clipped;
  }
  const double rms = std::sqrt(sum / std::max<std::size_t>(1, buffer.size()));

  const bool ok = writeWav(path, buffer, kSampleRate);
  // Clipping is called out because it is the one artefact that sounds like a broken
  // recipe rather than a loud one: a handful of flat-topped samples on a splash's
  // transient reads as grit, and a listener has no way to tell it came from the
  // 16-bit writer rather than from the sound design.
  std::printf("%-14s %5.2fs  peak %6.3f (%6.1f dB)  rms %6.4f%s%s\n", event.name, event.seconds,
              peak, 20.0 * std::log10(std::max(1e-6f, peak)), rms,
              clipped > 0 ? "  CLIPPED" : "", ok ? "" : "  WRITE FAILED");
  return ok;
}

// Lets the tail of the previous event decay and the compressor settle.
void renderSilence(float seconds) {
  const int frames = static_cast<int>(seconds * kSampleRate);
  std::vector<float> scratch(static_cast<std::size_t>(frames) * 2, 0.0f);
  audio::engine().renderOffline(scratch.data(), frames);
}

// A clean mixer for each event. Silence between them is not enough: the long tails
// (a cow's moo runs 1.3 s, a chest lid 0.65 s) would bleed into the next file and
// show up as peaks that belong to the wrong sound — which is exactly the kind of
// contamination that makes a level table lie.
void resetMixer() {
  audio::engine().startOffline(kSampleRate);
  audio::engine().updateListener(Vec3{0.0f, 0.0f, 0.0f}, 0.0f);
  // The shipped defaults, not unity: those are what the browser is playing on the
  // other side of the A/B, and at unity the two loudest events clip the 16-bit
  // writer — which sounds like a defect in the recipe rather than in the dump.
  audio::engine().setVolumes(0.8f, 0.8f, 0.6f, 0.5f);
  renderSilence(0.05f);  // let the listener smoothers reach their targets
}

}  // namespace

void listAudioEvents() {
  std::printf("audio events:\n");
  int column = 0;
  for (const Event& e : kEvents) {
    std::printf("  %-14s", e.name);
    if (++column % 4 == 0) std::printf("\n");
  }
  if (column % 4 != 0) std::printf("\n");
  std::printf("  all            (writes every event into the given directory)\n");
}

bool dumpAudio(const std::string& name, const std::string& path) {
  // Renders through whichever resource packs are enabled, because every event
  // below goes through the sfx facade and that is where a pack takes over. Without
  // this the dump always produced the synthesised sound, which made it useless for
  // the one job a pack author most wants it for: hearing a pack in the game's own
  // mixer — ducked, panned, through the compressor — without launching the game.
  // Turn the pack off in the Resource Packs screen to get the A/B.
  ui::settings().load(paths::settingsFile());
  const std::vector<resource::PackInfo> installed = resource::scanPacks();
  const std::vector<resource::PackInfo> active = resource::enabledPacks(installed);
  audio::sounds().rebuild(active);
  if (!active.empty()) {
    std::printf("through %d resource pack(s): %d of %d events replaced\n",
                static_cast<int>(active.size()), audio::sounds().stats().events,
                static_cast<int>(audio::soundEventCatalogue().size()));
  }

  if (name == "all") {
    std::error_code ec;
    fs::create_directories(path, ec);
    bool ok = true;
    for (const Event& e : kEvents) {
      resetMixer();
      ok &= renderEvent(e, paths::join(path, std::string(e.name) + ".wav"));
    }
    std::printf("\n%d events written to %s\n", static_cast<int>(std::size(kEvents)), path.c_str());
    return ok;
  }

  for (const Event& e : kEvents) {
    if (name != e.name) continue;
    resetMixer();
    return renderEvent(e, path);
  }
  std::printf("unknown audio event \"%s\"\n", name.c_str());
  listAudioEvents();
  return false;
}

}  // namespace hr::dev
