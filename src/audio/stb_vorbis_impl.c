// The single translation unit that compiles stb_vorbis.
//
// Ogg Vorbis is here for one reason: it is the only format a Minecraft resource
// pack ships sounds in. Without a Vorbis decoder the MC-shaped layout the pack
// loader implements would be a layout and nothing more — every real pack would
// fail on the first file.
//
// Its own C file, like miniaudio's, for the same two reasons: it is 5,500 lines
// of someone else's C that warns copiously under /W4 (see CMakeLists.txt, which
// silences this file by name), and the configuration macros below must be seen
// by the implementation and by nobody else.
//
// The decoder is fed from memory only — audio/decode.cpp reads the file itself,
// so the pack loader has one place that touches the disk and one place that
// reports a path in an error message.

// No FILE* API: we never hand it a filename. stb_vorbis_decode_memory, the only
// entry point used, is deliberately outside this guard in the library.
#define STB_VORBIS_NO_STDIO
// The pushdata API is for streaming a partially-received file. Every clip is
// decoded whole, from a buffer already in memory, so this is dead weight.
#define STB_VORBIS_NO_PUSHDATA_API

#include "stb_vorbis.c"
