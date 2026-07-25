// The public game version — the ONLY place it is written down. Everything else
// (menu footer, save stamps, multiplayer mismatch messages, the release
// packager in tools/release.py) reads it from here.
//
// Semantic versioning, releases are tagged vMAJOR.MINOR.PATCH:
//   MAJOR — big milestones / compatibility breaks
//   MINOR — new features (a content update)
//   PATCH — fixes and tuning only
//
// This is NOT the compatibility gate. Saves are gated by SAVE_VERSION
// (js/save/serialize.js + a migration in js/save/migrate.js), world generation
// by GEN_VERSION (js/world/worldgen.js), and multiplayer by
// SAVE_VERSION.NET_VERSION (js/net/protocol.js). Bump those only when the
// respective format actually changes; bump this every release.
//
// tools/release.py parses the line below with a regex — keep its exact shape.
export const GAME_VERSION = "1.2.0";
