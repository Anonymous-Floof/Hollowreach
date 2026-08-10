# Builds a Hollowreach sound pack from the FilmCow Royalty Free SFX library.
#
# Every entry is (source family, [indices], volume). The indices pick specific
# takes out of a family so the variants of one event are different recordings
# rather than the same one four times.
#
# Sheep, pigs and cows are deliberately absent: the library has no farm animals,
# and the game's own synthesised bleat/oink/moo are purpose-built for those
# creatures. Leaving them out is the pack format working as intended.

import json
import os
import re
import subprocess
import sys

FF = r"C:\Program Files\ShareX\ffmpeg.exe"
SCRATCH = os.path.dirname(os.path.abspath(__file__))
REC = os.path.join(SCRATCH, "filmcow", "FilmCow Recorded SFX", "FilmCow Recorded SFX")
DES = os.path.join(SCRATCH, "filmcow", "FilmCow Designed SFX", "FilmCow Designed SFX")
NS = "minecraft"  # exercises the Minecraft-namespace path, which is the compat claim

# The volumes are MEASURED, not guessed.
#
# Every clip is peak-normalised to -1 dBFS at build time, but the sounds this pack
# replaces are nowhere near full scale — a synthesised footstep peaks around -38 dB
# and the inventory tick around -44. Normalising and stopping there made the quiet
# events four to seven times louder than what they replaced, so turning the pack on
# jumped the volume of walking and clicking while leaving block breaks alone.
#
# So each number here is `old_volume / measured_ratio * 1.15`, where the ratio came
# from dumping all 42 events through the mixer twice — once with the pack on and
# once off — and comparing peaks. The 1.15 is deliberate headroom: a recording
# carries more detail than a synthesised burst and reads slightly quieter at equal
# peak. Re-derive with:
#     Hollowreach --dump-audio all <dir>     (with the pack on, then off)
R, D = "rec", "des"
MAP = {
    # --- stone -------------------------------------------------------------
    "block.stone.break":  (R, "ceramic statue breaking", [1, 2, 3, 4, 5], 0.85),
    "block.stone.hit":    (R, "rock %d low",             [1, 2, 3, 4],    0.11),
    "block.stone.place":  (R, "rock",                    [5, 6, 7, 8],    0.16),
    "block.stone.step":   (R, "rock %d low",             [11, 12, 13, 14], 0.11),
    # --- ore: metallic, and no step of its own so it falls back to stone ----
    "block.ore.break":    (R, "anvil hit",               [1, 2, 3, 4],    0.75),
    "block.ore.hit":      (R, "metal hit with hammer",   [1, 2, 3, 4],    0.15),
    "block.ore.place":    (R, "metal hit",               [1, 2, 3, 4],    0.16),
    # --- wood --------------------------------------------------------------
    "block.wood.break":   (R, "wood hitting ground",     [1, 2, 3, 4],    0.95),
    "block.wood.hit":     (R, "wood hit",                [1, 2, 3, 4],    0.32),
    "block.wood.place":   (R, "wood hitting ground",     [5, 6, 7, 8],    0.16),
    "block.wood.step":    (R, "wood hit",                [10, 11, 12, 13], 0.14),
    # --- dirt --------------------------------------------------------------
    "block.dirt.break":   (R, "bag of mulch dropped",    [1, 2, 3, 4],    0.20),
    "block.dirt.hit":     (R, "footstep dirt",           [1, 2, 3, 4],    0.15),
    "block.dirt.place":   (R, "bag of mulch dropped",    [5, 6],          0.16),
    "block.dirt.step":    (R, "footstep dirt",           [10, 11, 12, 13], 0.14),
    # --- sand --------------------------------------------------------------
    "block.sand.break":   (R, "sugar thrown",            [1, 2, 3, 4],    0.18),
    "block.sand.hit":     (R, "sugar",                   [1, 2, 3],       0.15),
    "block.sand.place":   (R, "sugar thrown",            [1, 2, 3],       0.16),
    "block.sand.step":    (R, "sugar",                   [1, 2, 3],       0.14),
    # --- gravel ------------------------------------------------------------
    "block.gravel.break": (R, "rocks handle",            [1, 2, 3, 4],    0.20),
    "block.gravel.hit":   (R, "rocks handle %d low",     [5, 6, 7, 8],    0.15),
    "block.gravel.place": (R, "rocks handle",            [9, 10, 11, 12], 0.16),
    "block.gravel.step":  (R, "rocks handle %d low",     [13, 14, 15, 16], 0.14),
    # --- grass -------------------------------------------------------------
    "block.grass.break":  (R, "bushes",                  [1, 2, 3, 4],    0.75),
    "block.grass.hit":    (R, "leaves rustle",           [1, 2, 3],       0.15),
    "block.grass.place":  (R, "bushes",                  [5, 6, 7, 8],    0.16),
    "block.grass.step":   (R, "footstep grass",          [1, 2, 3, 4],    0.15),
    # --- leaves ------------------------------------------------------------
    "block.leaves.break": (R, "bushes",                  [9, 10, 11, 12], 0.80),
    "block.leaves.hit":   (R, "leaves rustle",           [3, 4, 5],       0.15),
    "block.leaves.place": (R, "leaves rustle",           [1, 2, 3],       0.16),
    "block.leaves.step":  (R, "footstep leaves",         [1, 2, 3, 4],    0.15),
    # --- wool --------------------------------------------------------------
    "block.wool.break":   (R, "clothing movement",       [1, 2, 3, 4],    0.20),
    "block.wool.hit":     (R, "clothing movement",       [5, 6, 7, 8],    0.15),
    "block.wool.place":   (R, "cloth mask removed",      [1, 2, 3, 4],    0.16),
    "block.wool.step":    (R, "clothing movement",       [9, 10, 11, 12], 0.13),
    # --- glass -------------------------------------------------------------
    "block.glass.break":  (R, "hammer hits glass",       [1, 2, 3, 4],    0.75),
    "block.glass.hit":    (R, "glass ding",              [1, 2, 3, 4],    0.15),
    "block.glass.place":  (R, "glass set down on wood",  [1, 2, 3, 4],    0.16),
    "block.glass.step":   (R, "glass clink",             [1, 2, 3, 4],    0.13),
    # --- doors, containers, stations ---------------------------------------
    "block.wooden_door.open":      (R, "door open",                     [1, 2, 3, 4], 0.70),
    "block.wooden_door.close":     (R, "door close",                    [1, 2, 3, 4], 0.70),
    "block.wooden_trapdoor.open":  (R, "wood drawer opened",            [1, 2, 3],    0.70),
    "block.wooden_trapdoor.close": (R, "wood drawer closed",            [1, 2, 3, 4], 0.70),
    "block.chest.open":            (R, "crate open",                    [1, 2, 3, 4], 0.72),
    "block.chest.close":           (R, "wood and metal cabinet closed", [1, 2, 3, 4], 0.72),
    "block.furnace.smelt":         (R, "oven ding",                     [1, 2],       0.30),
    # --- the player --------------------------------------------------------
    "entity.player.hurt":          (R, "oof",                        [1, 2, 3, 4],  0.28),
    "entity.player.death":         (R, "scream",                     [1, 2, 3, 4],  0.24),
    "entity.player.big_fall":      (R, "body fall with lots of bass", [1, 2, 3, 4], 0.55),
    "entity.player.small_fall":    (R, "land in grass",              [1, 2, 3, 4],  0.40),
    "entity.player.splash":        (R, "splash big",                 [1, 2, 3, 4],  0.45),
    "entity.player.swim":          (R, "water splashing small",      [1, 2, 3, 4],  0.40),
    "entity.player.bubbles":       (R, "bubbles",                    [1, 2, 3, 4],  0.25),
    "entity.player.craft":         (R, "metal latches",              [1, 2, 3, 4],  0.45),
    "entity.player.attack.sweep":  (R, "woosh",                      [1, 2, 3, 4],  0.18),
    "entity.player.attack.strong": (R, "punch flesh",                [1, 2, 3, 4],  0.38),
    "entity.player.attack.crit":   (R, "bat hit",                    [1, 2, 3, 4],  0.75),
    "entity.generic.eat":          (R, "gulp",                       [1, 2, 3, 4],  0.38),
    "entity.generic.burn":         (R, "air duster",                 [1, 2, 3, 4],  0.38),
    "entity.item.pickup":          (R, "metal button pop up",        [1, 2, 3, 4],  0.70),
    "entity.item.throw":           (R, "swoosh",                     [1, 2, 3, 4],  0.26),
    "entity.enderman.teleport":    (D, "deep sci fi stinger",        [1, 2, 3, 4],  0.70),
    # --- the zombie. Sheep, pigs and cows keep their synthesised voices. ----
    "entity.zombie.ambient":       (D, "bug people",                 [1, 2, 3, 4],  0.17),
    "entity.zombie.hurt":          (R, "scream",                     [9, 10, 11, 12], 0.20),
    "entity.zombie.death":         (R, "scream",                     [5, 6, 7, 8],  0.20),
    # --- interface ---------------------------------------------------------
    "ui.button.click":  (R, "clicky button",    [1, 2, 3, 4], 0.17),
    "ui.slot.click":    (R, "button assorted",  [1, 2, 3, 4], 0.16),
    "ui.screenshot":    (R, "mouse click double", [1, 2, 3, 4], 0.35),
}


def source_path(library, family, take):
    root = REC if library == R else DES
    name = (family % take) if "%d" in family else "%s %d" % (family, take)
    return os.path.join(root, name + ".wav")


def shaping(limit):
    """Trim silence off both ends, then cut to `limit` with a short fade."""
    trim = ("silenceremove=start_periods=1:start_threshold=-55dB:start_silence=0.01,"
            "areverse,"
            "silenceremove=start_periods=1:start_threshold=-55dB:start_silence=0.01,"
            "areverse")
    fade = max(0.02, min(0.06, limit * 0.2))
    return "%s,atrim=0:%.3f,afade=t=out:st=%.3f:d=%.3f" % (trim, limit, limit - fade, fade)


def peak_db(path, limit):
    """Peak of the SHAPED audio, in dBFS.

    Measured after the trim and the cut, not before. Measuring the source instead
    is the obvious thing and it is wrong: when a take's loudest moment falls past
    the cut point, normalising against it leaves the delivered clip far quieter
    than its siblings. That put a 3.6x spread between the four variants of one
    event, so roughly every fourth footstep came out nearly silent.
    """
    out = subprocess.run([FF, "-i", path, "-af", shaping(limit) + ",volumedetect",
                          "-f", "null", "-"], capture_output=True, text=True).stderr
    m = re.search(r"max_volume: (-?[\d.]+) dB", out)
    return float(m.group(1)) if m else 0.0


# How long a clip is allowed to be, by what kind of event it is.
#
# This is the difference between a pack that sounds good and one that turns to
# mud. A mining tick fires about seven times a second and a footstep two or three
# — so a 1.4 second recording of sugar pouring, used as a sand mining tick, has
# ten copies of itself overlapping at any moment. The source library is foley,
# recorded as whole gestures; what these events want is the first moment of one.
LIMITS = (
    (".hit", 0.30),
    (".step", 0.35),
    (".place", 0.70),
    (".break", 1.40),
    ("ui.", 0.30),
    ("entity.item.", 0.40),
    ("entity.player.attack.", 0.50),
    # Crafting is a single confirming click, not a two-second rummage. Left at the
    # default it took the whole "metal latches" gesture, whose loudest moment is
    # near the end — so the sound you heard depended on how long you listened.
    ("entity.player.craft", 0.60),
)
DEFAULT_LIMIT = 2.50


def limit_for(event):
    for key, seconds in LIMITS:
        if event.endswith(key) or event.startswith(key):
            return seconds
    return DEFAULT_LIMIT


def convert(src, dst, gain_db, limit):
    """Mono 44.1k Ogg Vorbis, silence trimmed, length capped, peak at -1 dBFS.

    The gain goes on AFTER the shaping, matching where peak_db measured it, so
    every delivered variant of an event really does peak at the same level.
    """
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    subprocess.run([FF, "-y", "-loglevel", "error", "-i", src,
                    "-af", "%s,volume=%.2fdB" % (shaping(limit), gain_db),
                    "-ac", "1", "-ar", "44100", "-c:a", "libvorbis", "-q:a", "3", dst],
                   check=True, capture_output=True)


def main():
    dest = sys.argv[1]
    assets = os.path.join(dest, "assets", NS)
    sounds_root = os.path.join(assets, "sounds")

    manifest, missing, written = {}, [], 0
    for event, (library, family, takes, volume) in sorted(MAP.items()):
        rel_dir = event.replace(".", "/")
        limit = limit_for(event)
        entries = []
        for n, take in enumerate(takes, start=1):
            src = source_path(library, family, take)
            if not os.path.exists(src):
                missing.append(os.path.basename(src))
                continue
            rel = "%s%d" % (rel_dir, n)
            convert(src, os.path.join(sounds_root, rel + ".ogg"), -1.0 - peak_db(src, limit), limit)
            entries.append({"name": rel, "volume": volume})
            written += 1
        if entries:
            manifest[event] = {"sounds": entries}
        print("  %-32s %d take(s)  <=%.2fs" % (event, len(entries), limit))

    os.makedirs(assets, exist_ok=True)
    with open(os.path.join(assets, "sounds.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    with open(os.path.join(dest, "pack.mcmeta"), "w", encoding="utf-8") as f:
        json.dump({"pack": {"pack_format": 1,
                            "description": "FilmCow SFX"}}, f, indent=2)
        f.write("\n")

    print("\n%d events, %d clips" % (len(manifest), written))
    if missing:
        print("MISSING (%d): %s" % (len(missing), ", ".join(sorted(set(missing))[:20])))


main()
