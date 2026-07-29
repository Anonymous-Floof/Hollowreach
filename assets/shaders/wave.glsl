// Atmospheric vertex motion, shared by the terrain G-buffer pass, the shadow pass
// and the water pass.
//
// It has to be *identical* in all three: if the shadow pass swayed a leaf
// differently from the G-buffer pass, its shadow would detach from it.
//
// The flag arrives in the packed vertex's fourth byte; see src/world/mesher.h.

#ifndef HR_WAVE_GLSL
#define HR_WAVE_GLSL

const float WAVE_NONE = 0.0;
const float WAVE_LEAF = 1.0;
const float WAVE_WATER = 2.0;

vec3 applyWave(vec3 pos, float wave, float t) {
  if (wave > 0.5 && wave < 1.5) {
    // Leaves sway as a mass: the phase is driven by world position, not a
    // per-vertex value, so neighbouring blocks move together.
    float ph = t * 1.6 + pos.x * 0.7 + pos.z * 0.7 + pos.y * 0.3;
    pos.x += sin(ph) * 0.045;
    pos.z += cos(ph * 0.9 + 1.3) * 0.045;
    pos.y += sin(ph * 1.3) * 0.022;
  } else if (wave > 1.5) {
    // Water: two non-harmonic waves, and a constant droop so the rippling surface
    // never rises above the cell it belongs to.
    pos.y += (sin(t * 0.9 + pos.x * 0.7) + sin(t * 1.27 + pos.z * 0.6)) * 0.03 - 0.045;
  }
  return pos;
}

#endif
