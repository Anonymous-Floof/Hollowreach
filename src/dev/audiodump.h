// Offline renders of the sound library, for the A/B listen the milestone calls for.
//
// Audio is the one part of this port with no way to diff a number against the
// browser: there is no golden vector for "does a stone break sound like a stone
// breaking". So the engine is built to run without a device, and this dumps any
// named event — or the whole library, one file each — to a wav that can be played
// next to a recording of the web build.
//
// It also prints peak and RMS per event, which catches the things a listen would
// not: a recipe that clips, one that came out inaudible, or a mixer stage whose
// gain is out by a factor rather than a decibel.

#pragma once

#include <string>

namespace hr::dev {

// `name` is an event from the table in audiodump.cpp, or "all" to write every one
// into `path` as a directory. Returns false on an unknown name or a write failure.
bool dumpAudio(const std::string& name, const std::string& path);

// Prints the event names.
void listAudioEvents();

}  // namespace hr::dev
