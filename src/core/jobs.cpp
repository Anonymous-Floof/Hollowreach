#include "core/jobs.h"

#include <algorithm>
#include <exception>
#include <utility>

#include "core/log.h"

namespace hr::jobs {

int defaultThreadCount() {
  const unsigned hw = std::thread::hardware_concurrency();
  const int usable = hw == 0 ? 4 : static_cast<int>(hw);
  return std::clamp(usable - 1, 1, 8);
}

System::~System() { stop(); }

void System::start(int threads) {
  if (threaded() || threads <= 0) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = false;
  }
  workers_.reserve(static_cast<std::size_t>(threads));
  for (int i = 0; i < threads; ++i) workers_.emplace_back([this] { workerLoop(); });
  log::info("jobs: %d worker thread(s)", threads);
}

void System::stop() {
  if (workers_.empty()) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  work_.notify_all();
  for (std::thread& t : workers_) {
    if (t.joinable()) t.join();
  }
  workers_.clear();

  // Anything still queued is dropped: stop() happens at shutdown or when the
  // thread count changes, and in both cases the work is for a world that is going
  // away. The outstanding count has to go with it, or a later drain() hangs.
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& lane : lanes_) lane.clear();
  outstanding_ = 0;
  stopping_ = false;
}

void System::submit(Lane lane, std::function<void()> job) {
  if (!job) return;
  if (workers_.empty()) {
    // Inline. Deliberately outside the mutex and outside the counters: with no
    // workers there is nothing to wait for, and drain() must stay a no-op.
    job();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    lanes_[static_cast<std::size_t>(lane)].push_back(std::move(job));
    ++outstanding_;
  }
  work_.notify_one();
}

void System::workerLoop() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      work_.wait(lock, [this] {
        if (stopping_) return true;
        for (const auto& lane : lanes_) {
          if (!lane.empty()) return true;
        }
        return false;
      });
      if (stopping_) return;
      // Highest lane with anything in it. This one line is the whole reason the
      // pool is shared rather than split.
      for (auto& lane : lanes_) {
        if (lane.empty()) continue;
        job = std::move(lane.front());
        lane.pop_front();
        break;
      }
    }

    if (job) {
      // A throw escaping a worker would terminate the process, and a throw that
      // skipped the bookkeeping below would hang the next drain(). Neither is an
      // acceptable way to find out that a mesher hit a bad block id.
      try {
        job();
      } catch (const std::exception& e) {
        log::error("jobs: worker caught %s", e.what());
      } catch (...) {
        log::error("jobs: worker caught an unknown exception");
      }
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (--outstanding_ == 0) idle_.notify_all();
    }
  }
}

void System::drain() {
  if (workers_.empty()) return;
  std::unique_lock<std::mutex> lock(mutex_);
  idle_.wait(lock, [this] { return outstanding_ == 0; });
}

int System::pending() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return outstanding_;
}

System& system() {
  static System instance;
  return instance;
}

}  // namespace hr::jobs
