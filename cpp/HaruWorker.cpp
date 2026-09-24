#include "HaruWorker.hpp"

namespace margelo::nitro::pdfwriter {

HaruWorker::HaruWorker() : _thread([this]() { loop(); }) {}

HaruWorker::~HaruWorker() {
  {
    std::unique_lock<std::mutex> lock(_mutex);
    _stop = true;
  }
  _cv.notify_all();
  if (_thread.joinable()) {
    _thread.join();
  }
}

HaruWorker& HaruWorker::getInstance() {
  static HaruWorker instance;
  return instance;
}

bool HaruWorker::enqueue(std::function<void()> task) {
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

void HaruWorker::loop() {
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
