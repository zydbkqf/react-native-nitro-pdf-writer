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

namespace margelo::nitro::pdfwriter {

using namespace nitro;

/**
 * Serial background worker. libHaru operations run here so the UI thread
 * is never blocked. Because only one worker thread exists and every task
 * is executed serially, libHaru handles are never accessed concurrently.
 * The global HaruLock is still acquired for each operation as an extra
 * safety net.
 */
class HaruWorker {
public:
  static HaruWorker& getInstance();

  HaruWorker(const HaruWorker&) = delete;
  HaruWorker& operator=(const HaruWorker&) = delete;

  /**
   * Enqueue a task that returns a value and resolves/rejects the promise.
   */
  template <typename T, typename Func, std::enable_if_t<!std::is_void_v<T>, int> = 0>
  void run(std::shared_ptr<Promise<T>> promise, Func&& func) {
    bool queued = enqueue([promise, func = std::forward<Func>(func)]() mutable {
      try {
        T result = func();
        promise->resolve(result);
      } catch (const std::exception&) {
        promise->reject(std::current_exception());
      } catch (...) {
        promise->reject(std::current_exception());
      }
    });
    if (!queued) {
      promise->reject(std::make_exception_ptr(std::runtime_error("HaruWorker is shut down")));
    }
  }

  /**
   * Enqueue a void task and resolves/rejects the promise.
   */
  template <typename Func>
  void run(std::shared_ptr<Promise<void>> promise, Func&& func) {
    bool queued = enqueue([promise, func = std::forward<Func>(func)]() mutable {
      try {
        func();
        promise->resolve();
      } catch (const std::exception&) {
        promise->reject(std::current_exception());
      } catch (...) {
        promise->reject(std::current_exception());
      }
    });
    if (!queued) {
      promise->reject(std::make_exception_ptr(std::runtime_error("HaruWorker is shut down")));
    }
  }

  ~HaruWorker();

private:
  HaruWorker();

  // Returns false if the worker is shutting down (task was not queued).
  bool enqueue(std::function<void()> task);
  void loop();

  std::thread _thread;
  std::mutex _mutex;
  std::condition_variable _cv;
  std::queue<std::function<void()>> _queue;
  bool _stop = false;
};

} // namespace margelo::nitro::pdfwriter
