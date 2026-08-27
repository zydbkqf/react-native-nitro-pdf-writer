#include "HaruLock.hpp"
#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <thread>
#include <vector>

using namespace margelo::nitro::pdfwriter;

TEST_CASE("HaruLock serializes concurrent access") {
  std::atomic<int> counter{0};
  std::atomic<int> maxConcurrent{0};
  std::atomic<int> current{0};
  constexpr int iterations = 100;
  constexpr int threads = 8;

  auto worker = [&]() {
    for (int i = 0; i < iterations; ++i) {
      std::lock_guard<std::mutex> lock(HaruLock::get());
      int before = ++current;
      maxConcurrent.store(std::max(maxConcurrent.load(), before));
      std::this_thread::yield();
      --current;
      ++counter;
    }
  };

  std::vector<std::thread> ts;
  ts.reserve(threads);
  for (int i = 0; i < threads; ++i) {
    ts.emplace_back(worker);
  }
  for (auto& t : ts) {
    t.join();
  }

  CHECK(counter == threads * iterations);
  CHECK(maxConcurrent == 1);
  CHECK(current == 0);
}
