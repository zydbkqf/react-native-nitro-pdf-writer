#include "MediaCache.hpp"
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>
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
  CHECK(cache.entryCount() == 0); // 164-byte entry no longer fits in 100 bytes
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

// ---------------------------------------------------------------------------
// Disk layer: metadata restore + independent capacity
// ---------------------------------------------------------------------------

namespace {

std::filesystem::path uniqueCacheDir() {
  static int counter = 0;
  auto dir = std::filesystem::temp_directory_path() /
             ("npdf-cache-test-" + std::to_string(::getpid()) + "-" +
              std::to_string(++counter));
  std::filesystem::create_directories(dir);
  return dir;
}

size_t diskPayloadBytes(const std::filesystem::path& dir) {
  size_t total = 0;
  auto cacheDir = dir / "npdf-media-cache";
  if (!std::filesystem::exists(cacheDir)) {
    return 0;
  }
  for (const auto& entry : std::filesystem::directory_iterator(cacheDir)) {
    if (entry.is_regular_file()) {
      total += static_cast<size_t>(std::filesystem::file_size(entry.path()));
    }
  }
  return total;
}

} // namespace

TEST_CASE("MediaCache disk restore keeps kind and raw geometry") {
  auto& cache = MediaCache::instance();
  cache.setCacheDir("");
  cache.clear();
  cache.setMaxBytes(1024 * 1024);

  auto dir = uniqueCacheDir();
  cache.setCacheDir(dir.string());

  auto blob = makeBlob(64, MediaBlob::Kind::Raw);
  blob->width = 40;
  blob->height = 30;
  blob->colorSpace = 9;
  std::string key = MediaCache::bufferKey("raw", 0x1234, 40, 30, 9);
  cache.put(key, blob);
  blob.reset();

  // Drop the in-memory copy so the next get() has to read the disk envelope.
  cache.clearMemory();
  CHECK(cache.entryCount() == 0);

  auto hit = cache.get(key);
  REQUIRE(hit != nullptr);
  CHECK(hit->kind == MediaBlob::Kind::Raw);
  CHECK(hit->width == 40);
  CHECK(hit->height == 30);
  CHECK(hit->colorSpace == 9);
  CHECK(hit->bytes.size() == 64);
  CHECK(hit->restoredFromDisk);

  hit.reset();
  cache.clear();
  cache.setCacheDir("");
  std::filesystem::remove_all(dir);
}

TEST_CASE("MediaCache disk restore never returns a half-built font blob") {
  auto& cache = MediaCache::instance();
  cache.setCacheDir("");
  cache.clear();
  cache.setMaxBytes(1024 * 1024);

  auto dir = uniqueCacheDir();
  cache.setCacheDir(dir.string());

  auto blob = makeBlob(32, MediaBlob::Kind::Font);
  blob->faceIndex = 2;
  blob->isTtc = true;
  blob->unitsPerEm = 1000;
  blob->numGlyphs = 500;
  blob->advanceWidths = {10, 20, 30};
  blob->fontPath = "/fonts/original.ttc";
  std::string key = MediaCache::fileKey("font", "/fonts/original.ttc#2", 111, 333);
  cache.put(key, blob);
  blob.reset();

  cache.clearMemory();
  auto hit = cache.get(key);
  REQUIRE(hit != nullptr);
  // Bytes + face index survive; derived state must be flagged for rebuild
  // rather than silently presented as a ready-to-attach font.
  CHECK(hit->kind == MediaBlob::Kind::Font);
  CHECK(hit->faceIndex == 2);
  CHECK(hit->restoredFromDisk);
  CHECK(hit->fontPath.empty());
  CHECK(hit->advanceWidths.empty());
  CHECK(hit->numGlyphs == 0);

  hit.reset();
  cache.clear();
  cache.setCacheDir("");
  std::filesystem::remove_all(dir);
}

TEST_CASE("MediaCache disk layer honors its own byte budget") {
  auto& cache = MediaCache::instance();
  cache.setCacheDir("");
  cache.clear();
  cache.setMaxBytes(8 * 1024 * 1024);

  auto dir = uniqueCacheDir();
  cache.setCacheDir(dir.string());
  cache.setMaxDiskBytes(4096);

  for (int i = 0; i < 16; ++i) {
    cache.put("disk:" + std::to_string(i), makeBlob(1024));
  }

  CHECK(cache.diskBytes() <= 4096);
  CHECK(diskPayloadBytes(dir) <= 4096);
  // The entry survives because prune receives it as keepKey, not because mtime
  // orders it last (1s granularity can tie it with older files).
  cache.clearMemory();
  CHECK(cache.get("disk:15") != nullptr);

  cache.clear();
  cache.setMaxDiskBytes(128ull * 1024ull * 1024ull);
  cache.setCacheDir("");
  std::filesystem::remove_all(dir);
}

TEST_CASE("diskBytes counts sidecars and prune removes orphan keys") {
  auto& cache = MediaCache::instance();
  cache.setCacheDir("");
  cache.clear();
  cache.setMaxBytes(1024 * 1024);

  auto dir = uniqueCacheDir();
  cache.setCacheDir(dir.string());
  cache.setMaxDiskBytes(1024 * 1024);

  cache.put("disk-sidecar", makeBlob(200));

  // An interrupted write can leave a .key whose .bin never landed. Such a
  // sidecar is unloadable and would otherwise sit in the budget forever.
  const auto payloadDir = dir / "npdf-media-cache";
  const auto orphan = payloadDir / "orphan-deadbeef.key";
  { std::ofstream out(orphan, std::ios::binary); out << "orphan"; }

  // diskBytes() has to include .key, otherwise the reported budget drifts low.
  // The orphan above was written behind the cache's back, so it only enters the
  // accounting once a scan/prune walks the directory.
  const size_t orphanBytes = static_cast<size_t>(std::filesystem::file_size(orphan));
  CHECK(cache.diskBytes() == diskPayloadBytes(dir) - orphanBytes);

  // Force a prune by shrinking the budget, then writing again.
  cache.setMaxDiskBytes(200);
  cache.put("disk-sidecar-2", makeBlob(200));

  CHECK(!std::filesystem::exists(orphan));
  CHECK(cache.diskBytes() == diskPayloadBytes(dir));
  size_t tmpFiles = 0;
  for (const auto& entry : std::filesystem::directory_iterator(payloadDir)) {
    const std::string name = entry.path().filename().string();
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
      ++tmpFiles;
    }
  }
  CHECK(tmpFiles == 0);
  // The payload written by the triggering put survives the prune.
  CHECK(cache.get("disk-sidecar-2") != nullptr);

  cache.clear();
  cache.setMaxDiskBytes(128ull * 1024ull * 1024ull);
  cache.setCacheDir("");
  std::filesystem::remove_all(dir);
}
