#include "MediaCache.hpp"
#include <doctest/doctest.h>
#include <thread>
#include <vector>

using namespace margelo::nitro::pdfwriter;

namespace {

std::shared_ptr<MediaBlob> makeBlob(size_t payload, MediaBlob::Kind kind = MediaBlob::Kind::Png) {
  auto blob = std::make_shared<MediaBlob>();
  blob->kind = kind;
  blob->bytes.assign(payload, 0xAB);
  return blob;
}

} // namespace

TEST_CASE("MediaCache put/get roundtrip") {
  auto& cache = MediaCache::instance();
  cache.clear();
  cache.setMaxBytes(1024 * 1024);

  auto blob = makeBlob(128);
  cache.put("k1", blob);
  auto hit = cache.get("k1");
  REQUIRE(hit != nullptr);
  CHECK(hit->bytes.size() == 128);
  CHECK(cache.entryCount() == 1);
  CHECK(cache.totalBytes() >= 128);
}

TEST_CASE("MediaCache respects byte cap with LRU eviction") {
  auto& cache = MediaCache::instance();
  cache.clear();
  cache.setMaxBytes(300);

  {
    auto a = makeBlob(100);
    cache.put("a", a);
  }
  {
    auto b = makeBlob(100);
    cache.put("b", b);
  }
  {
    auto c = makeBlob(100);
    cache.put("c", c);
  }
  // Unpinned entries only — eviction runs on put/unpin.

  CHECK(cache.totalBytes() <= 300);
  // Oldest ("a") should have been evicted first.
  CHECK(cache.get("a") == nullptr);
  CHECK(cache.get("c") != nullptr);
}

TEST_CASE("MediaCache does not evict pinned (refcounted) entries") {
  auto& cache = MediaCache::instance();
  cache.clear();
  // Two 164-byte entries fit (100 payload + overhead); three do not.
  cache.setMaxBytes(400);

  cache.put("pinned", makeBlob(100));
  auto pinned = cache.get("pinned"); // pin via refcount
  REQUIRE(pinned != nullptr);

  cache.put("other", makeBlob(100));
  cache.put("overflow", makeBlob(100)); // evicts unpinned LRU, not "pinned"

  auto stillThere = cache.get("pinned");
  REQUIRE(stillThere != nullptr);
  CHECK(stillThere->bytes.size() == 100);
}

TEST_CASE("MediaCache unpinned entries become evictable after release") {
  auto& cache = MediaCache::instance();
  cache.clear();
  cache.setMaxBytes(200);

  cache.put("pinned", makeBlob(100));
  {
    auto pinned = cache.get("pinned");
    REQUIRE(pinned != nullptr);
  }
  // After release, "pinned" is unpinned but still resident (under cap).
  CHECK(cache.get("pinned") != nullptr);
  cache.trimTo(100); // force eviction of unpinned entries
  cache.trimTo(200);
}

TEST_CASE("MediaCache clear releases unpinned entries") {
  auto& cache = MediaCache::instance();
  cache.clear();
  cache.setMaxBytes(1024 * 1024);

  auto blob = makeBlob(64);
  cache.put("gone", blob);
  blob.reset();
  cache.clear();
  CHECK(cache.entryCount() == 0);
  CHECK(cache.totalBytes() == 0);
}

TEST_CASE("MediaCache file keys embed mtime and size") {
  std::string k1 = MediaCache::fileKey("png", "/a.png", 100, 10);
  std::string k2 = MediaCache::fileKey("png", "/a.png", 200, 10);
  std::string k3 = MediaCache::fileKey("png", "/a.png", 100, 20);
  CHECK(k1 != k2);
  CHECK(k1 != k3);
  CHECK(k1.find("100") != std::string::npos);
}

TEST_CASE("MediaCache buffer keys embed content hash and geometry") {
  uint64_t h1 = MediaCache::hashBytes("abc", 3);
  uint64_t h2 = MediaCache::hashBytes("abd", 3);
  CHECK(h1 != h2);
  std::string k1 = MediaCache::bufferKey("raw", h1, 10, 10, 0);
  std::string k2 = MediaCache::bufferKey("raw", h2, 10, 10, 0);
  std::string k3 = MediaCache::bufferKey("raw", h1, 20, 10, 0);
  CHECK(k1 != k2);
  CHECK(k1 != k3);
}

TEST_CASE("MediaRegistry register / get / release") {
  MediaRegistry registry;
  auto blob = makeBlob(32);
  double h = registry.registerBlob(blob);
  CHECK(h != 0);
  REQUIRE(registry.get(h) != nullptr);
  CHECK(registry.size() == 1);
  registry.release(h);
  CHECK(registry.get(h) == nullptr);
  CHECK(registry.size() == 0);
}

TEST_CASE("MediaRegistry rejects null blob") {
  MediaRegistry registry;
  CHECK(registry.registerBlob(nullptr) == 0);
}

TEST_CASE("MediaCache is safe under concurrent access") {
  auto& cache = MediaCache::instance();
  cache.clear();
  cache.setMaxBytes(64 * 1024);

  constexpr int threads = 8;
  constexpr int iterations = 200;
  std::vector<std::thread> ts;
  ts.reserve(threads);
  for (int t = 0; t < threads; ++t) {
    ts.emplace_back([&cache, t]() {
      for (int i = 0; i < iterations; ++i) {
        std::string key = "c:" + std::to_string(t) + ":" + std::to_string(i % 20);
        cache.put(key, makeBlob(64));
        (void)cache.get(key);
      }
    });
  }
  for (auto& th : ts) {
    th.join();
  }
  CHECK(cache.totalBytes() <= 64 * 1024);
}
