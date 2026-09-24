#include "MediaWorker.hpp"

namespace margelo::nitro::pdfwriter {

MediaWorker::MediaWorker() {
  _threads.reserve(kThreadCount);
  for (size_t i = 0; i < kThreadCount; ++i) {
    _threads.emplace_back([this]() { loop(); });
  }
}

MediaWorker::~MediaWorker() {
  {
    std::unique_lock<std::mutex> lock(_mutex);
    _stop = true;
  }
  _cv.notify_all();
  for (auto& t : _threads) {
    if (t.joinable()) {
      t.join();
    }
  }
}

MediaWorker& MediaWorker::getInstance() {
  static MediaWorker instance;
  return instance;
}

bool MediaWorker::enqueue(std::function<void()> task) {
  {
    std::unique_lock<std::mutex> lock(_mutex);
    if (_stop) {
      return false;
    }
    _queue.push(std::move(task));
  }
  _cv.notify_one();
  return true;
}

void MediaWorker::loop() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(_mutex);
      _cv.wait(lock, [this]() { return _stop || !_queue.empty(); });
      if (_stop && _queue.empty()) {
        return;
      }
      task = std::move(_queue.front());
      _queue.pop();
    }
    task();
  }
}

} // namespace margelo::nitro::pdfwriter
