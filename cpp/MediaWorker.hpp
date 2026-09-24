#pragma once

#include <NitroModules/Promise.hpp>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace margelo::nitro::pdfwriter {

using namespace nitro;

/**
 * Media parse worker pool. Font/image file I/O, TTC index parsing and glyph
 * metric pre-parse run here so they stay OUTSIDE HaruLock. The brief HPDF_*
 * install step may then take HaruLock without holding it during I/O.
 */
class MediaWorker {
public:
  static MediaWorker& getInstance();

  MediaWorker(const MediaWorker&) = delete;
  MediaWorker& operator=(const MediaWorker&) = delete;

  template <typename T, typename Func, std::enable_if_t<!std::is_void_v<T>, int> = 0>
  void run(std::shared_ptr<Promise<T>> promise, Func&& func) {
    bool queued = enqueue([promise, func = std::forward<Func>(func)]() mutable {
      try {
        T result = func();
        promise->resolve(result);
      } catch (...) {
        promise->reject(std::current_exception());
      }
    });
    if (!queued) {
      promise->reject(std::make_exception_ptr(std::runtime_error("MediaWorker is shut down")));
    }
  }

  template <typename Func>
  void run(std::shared_ptr<Promise<void>> promise, Func&& func) {
    bool queued = enqueue([promise, func = std::forward<Func>(func)]() mutable {
      try {
        func();
        promise->resolve();
      } catch (...) {
        promise->reject(std::current_exception());
      }
    });
    if (!queued) {
      promise->reject(std::make_exception_ptr(std::runtime_error("MediaWorker is shut down")));
    }
  }

  ~MediaWorker();

private:
  MediaWorker();

  bool enqueue(std::function<void()> task);
  void loop();

  static constexpr size_t kThreadCount = 2;

  std::vector<std::thread> _threads;
  std::mutex _mutex;
  std::condition_variable _cv;
  std::queue<std::function<void()>> _queue;
  bool _stop = false;
};

} // namespace margelo::nitro::pdfwriter
