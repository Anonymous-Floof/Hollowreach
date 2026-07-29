// One job system for the whole program.
//
// The plan is explicit that this is **one** pool shared by chunk generation,
// lighting and meshing rather than a pool each, and the reason is the failure it
// prevents: a player crossing a chunk boundary dirties a 3x3 of meshes per new
// chunk, so a burst of remeshes is routine, and with separate pools — or with one
// FIFO queue — that burst sits in front of the generation the player is walking
// toward. Terrain then arrives late while the CPU is busy re-drawing terrain that
// is already there. Lanes fix it: a worker always takes the highest-priority lane
// with anything in it, so generation cannot be queued behind meshing no matter how
// much meshing there is.
//
// **An unstarted system runs every job inline on the calling thread.** That is not
// a fallback, it is the design: the headless tools (`--selftest`, `--dump-golden`,
// `--dump-atlas`) never start it and keep working unchanged, `--threads 0` turns
// the whole milestone off in one flag, and the two modes therefore run the *same*
// pipeline code rather than a threaded path and a separate synchronous one that
// could drift apart. It is also what makes the determinism check meaningful: the
// self-test builds the same region at 0 threads and at N and compares, and both
// runs go through this file.

#pragma once

#include <array>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace hr::jobs {

// Highest priority first. A worker drains Generate completely before looking at
// Light, and Light before Mesh.
enum class Lane : int {
  Generate = 0,
  Light = 1,
  Mesh = 2,
  Count = 3,
};

// hardware_concurrency() minus one for the main thread, clamped to something
// sensible: below 1 there is nothing to gain, and above 8 the chunk pipeline is
// waiting on memory bandwidth rather than on cores.
int defaultThreadCount();

class System {
 public:
  ~System();

  // `threads` <= 0 leaves the system unstarted, which means every job runs inline.
  // Starting twice is a no-op; changing the count means stop() then start().
  void start(int threads);
  void stop();

  bool threaded() const { return !workers_.empty(); }
  int threadCount() const { return static_cast<int>(workers_.size()); }

  void submit(Lane lane, std::function<void()> job);

  // Blocks until every job submitted so far has finished. Used by primeSpawn, by
  // the self-test, and by shutdown. Not safe to call from a worker.
  void drain();

  // Queued plus running. For the debug overlay.
  int pending() const;

 private:
  void workerLoop();

  mutable std::mutex mutex_;
  std::condition_variable work_;
  std::condition_variable idle_;
  std::array<std::deque<std::function<void()>>, static_cast<std::size_t>(Lane::Count)> lanes_;
  std::vector<std::thread> workers_;
  int outstanding_ = 0;  // queued + running
  bool stopping_ = false;
};

System& system();

}  // namespace hr::jobs
