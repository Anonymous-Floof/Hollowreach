// The single translation unit that compiles miniaudio.
//
// miniaudio is used purely as a device abstraction: it opens the platform's audio
// output and hands us a callback. Every generator, filter, mixer and effect in this
// game is our own (audio/dsp.cpp), because the recipes in js/audio/sfx.js were tuned
// against Web Audio's specific node behaviour and a stock filter or oscillator would
// not reproduce them. So miniaudio's decoders, resource manager, node graph and
// high-level engine are all switched off — see cmake/deps.cmake for the defines,
// which must match in every TU that includes the header.

#define MA_IMPLEMENTATION
#include "miniaudio.h"
