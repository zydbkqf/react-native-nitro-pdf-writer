#include "MediaCache.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace margelo::nitro::pdfwriter {

namespace fs = std::filesystem;

MediaCache& MediaCache::instance() {
  static MediaCache cache;
  return cache;
}

void MediaCache::setCacheDir(const std::string& dir) {
  std::unique_lock lock(_mutex);
  _cacheDir = dir;
  _diskBytesKnown = false;
  _diskBytes = 0;
  _diskWritesSincePrune = 0;
  if (!_cacheDir.empty()) {
    std::error_code ec;
    fs::create_directories(_cacheDir, ec);
    scanDiskUsageLocked();
  } else {
    _diskBytesKnown = true;
  }
}

std::string MediaCache::cacheDir() const {
  std::shared_lock lock(_mutex);
  return _cacheDir;
}

void MediaCache::setMaxBytes(size_t maxBytes) {
  std::unique_lock lock(_mutex);
  _maxBytes = maxBytes;
  evictLocked();
}

void MediaCache::setMaxDiskBytes(size_t maxBytes) {
  std::unique_lock lock(_mutex);
  _maxDiskBytes = maxBytes;
  if (_cacheDir.empty()) {
    _diskBytes = 0;
    _diskBytesKnown = true;
    return;
  }
  scanDiskUsageLocked();
  pruneDiskLocked();
}

size_t MediaCache::maxDiskBytes() const {
  std::shared_lock lock(_mutex);
  return _maxDiskBytes;
}

size_t MediaCache::diskBytes() const {
  // Reported bytes may lag until the next write; a full recount here would
  // turn a cheap getter into a directory walk on every call.
  std::shared_lock lock(_mutex);
  return _diskBytesKnown ? _diskBytes : 0;
}

size_t MediaCache::maxBytes() const {
  std::shared_lock lock(_mutex);
  return _maxBytes;
}

size_t MediaCache::totalBytes() const {
  std::shared_lock lock(_mutex);
  return _totalBytes;
}

size_t MediaCache::entryCount() const {
  std::shared_lock lock(_mutex);
  return _map.size();
}

std::shared_ptr<MediaBlob> MediaCache::get(const std::string& key) {
  std::shared_ptr<MediaBlob> raw;
  {
    std::unique_lock lock(_mutex);
    auto it = _map.find(key);
    if (it == _map.end()) {
      std::shared_ptr<MediaBlob> loaded;
      if (!loadFromDiskLocked(key, loaded) || !loaded) {
        return nullptr;
      }
      Entry entry;
      entry.blob = loaded;
      entry.byteSize = loaded->byteSize();
      entry.lruStamp = ++_lruClock;
      entry.refCount = 0;
      _totalBytes += entry.byteSize;
      it = _map.emplace(key, std::move(entry)).first;
      // Protect the entry we just restored from being immediately LRU-evicted.
      evictLocked(&key);
      it = _map.find(key);
      if (it == _map.end()) {
        return nullptr;
      }
    }
    it->second.refCount++;
    it->second.lruStamp = ++_lruClock;
    raw = it->second.blob;
  }

  // Pin wrapper: unpins (and triggers LRU) when the caller drops the last ref.
  return std::shared_ptr<MediaBlob>(raw.get(), [this, key, raw](MediaBlob*) mutable {
    raw.reset();
    this->unpin(key);
  });
}

void MediaCache::unpin(const std::string& key) {
  std::unique_lock lock(_mutex);
  auto it = _map.find(key);
  if (it != _map.end() && it->second.refCount > 0) {
    it->second.refCount--;
  }
  evictLocked();
}

void MediaCache::put(const std::string& key, std::shared_ptr<MediaBlob> blob) {
  if (!blob) {
    return;
  }
  std::unique_lock lock(_mutex);
  auto it = _map.find(key);
  if (it != _map.end()) {
    _totalBytes -= it->second.byteSize;
    it->second.blob = std::move(blob);
    it->second.byteSize = it->second.blob->byteSize();
    it->second.lruStamp = ++_lruClock;
    _totalBytes += it->second.byteSize;
  } else {
    Entry entry;
    entry.blob = std::move(blob);
    entry.byteSize = entry.blob->byteSize();
    entry.lruStamp = ++_lruClock;
    entry.refCount = 0;
    _totalBytes += entry.byteSize;
    _map.emplace(key, std::move(entry));
    it = _map.find(key);
  }
  if (it != _map.end()) {
    writeToDiskLocked(key, *it->second.blob);
  }
  // Never evict the entry we just wrote — the caller is about to pin it.
  evictLocked(&key);
}

void MediaCache::clearMemoryLocked() {
  for (auto it = _map.begin(); it != _map.end();) {
    if (it->second.refCount <= 0) {
      _totalBytes -= it->second.byteSize;
      it = _map.erase(it);
    } else {
      ++it;
    }
  }
}

void MediaCache::clearMemory() {
  std::unique_lock lock(_mutex);
  clearMemoryLocked();
}

void MediaCache::clear() {
  std::unique_lock lock(_mutex);
  clearMemoryLocked();
  clearDiskLocked();
}

void MediaCache::trimTo(size_t maxBytes) {
  std::unique_lock lock(_mutex);
  _maxBytes = maxBytes;
  evictLocked();
}

std::string MediaCache::fileKey(const std::string& kind, const std::string& path, int64_t mtime,
                                uint64_t size) {
  return "f:" + kind + ":" + path + ":" + std::to_string(mtime) + ":" + std::to_string(size);
}

std::string MediaCache::bufferKey(const std::string& kind, uint64_t contentHash, uint32_t width,
                                  uint32_t height, int colorSpace) {
  return "b:" + kind + ":" + std::to_string(contentHash) + ":" + std::to_string(width) + ":" +
         std::to_string(height) + ":" + std::to_string(colorSpace);
}

uint64_t MediaCache::hashBytes(const void* data, size_t size) {
  // FNV-1a 64-bit — fast content version for buffer cache keys.
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < size; ++i) {
    hash ^= p[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

void MediaCache::evictLocked(const std::string* keepKey) {
  if (_totalBytes <= _maxBytes) {
    return;
  }
  std::vector<std::pair<uint64_t, std::string>> victims;
  victims.reserve(_map.size());
  for (const auto& [key, entry] : _map) {
    if (keepKey != nullptr && key == *keepKey) {
      continue;
    }
    if (entry.refCount <= 0) {
      victims.emplace_back(entry.lruStamp, key);
    }
  }
  std::sort(victims.begin(), victims.end()); // oldest first (LRU)
  for (const auto& [stamp, key] : victims) {
    if (_totalBytes <= _maxBytes) {
      break;
    }
    auto it = _map.find(key);
    if (it == _map.end()) {
      continue;
    }
    _totalBytes -= it->second.byteSize;
    _map.erase(it);
  }
}

std::string MediaCache::diskPathFor(const std::string& key) const {
  uint64_t h = hashBytes(key.data(), key.size());
  char name[40];
  std::snprintf(name, sizeof(name), "%016llx.bin", static_cast<unsigned long long>(h));
  return (fs::path(_cacheDir) / "npdf-media-cache" / name).string();
}

namespace {

/**
 * Payload envelope written ahead of the raw bytes.
 *
 * The disk cache stores bytes plus exactly the fields the byte stream cannot
 * express. Derived state (font metrics, materialized font path) is never
 * persisted — it is rebuilt by the owning loader on restore.
 */
constexpr uint32_t kDiskMagic = 0x4E504332u; // "NPC2"

struct DiskEnvelope {
  uint32_t magic;
  uint32_t kind;
  uint32_t width;
  uint32_t height;
  int32_t colorSpace;
  uint32_t faceIndex;
  int64_t versionMtime;
  uint64_t payloadSize;
};

static_assert(sizeof(DiskEnvelope) == 40, "DiskEnvelope must stay fixed-width");

std::string temporaryPathFor(const std::string& path) { return path + ".tmp"; }

bool replaceFile(const std::string& from, const std::string& to) {
  std::error_code ec;
  fs::rename(from, to, ec);
  if (!ec) {
    return true;
  }
  // Windows refuses to rename over an existing file.
  fs::remove(to, ec);
  ec.clear();
  fs::rename(from, to, ec);
  return !ec;
}

} // namespace

bool MediaCache::loadFromDiskLocked(const std::string& key, std::shared_ptr<MediaBlob>& out) {
  if (_cacheDir.empty()) {
    return false;
  }
  std::string binPath = diskPathFor(key);
  std::string keyPath = binPath.substr(0, binPath.size() - 4) + ".key";
  std::ifstream keyIn(keyPath, std::ios::binary);
  if (!keyIn) {
    return false;
  }
  std::string storedKey((std::istreambuf_iterator<char>(keyIn)), std::istreambuf_iterator<char>());
  if (storedKey != key) {
    return false; // version/mtime mismatch
  }

  std::ifstream binIn(binPath, std::ios::binary | std::ios::ate);
  if (!binIn) {
    return false;
  }
  auto fileSize = binIn.tellg();
  if (fileSize <= static_cast<std::streamsize>(sizeof(DiskEnvelope))) {
    discardDiskEntryLocked(key);
    return false;
  }
  binIn.seekg(0, std::ios::beg);
  DiskEnvelope envelope{};
  if (!binIn.read(reinterpret_cast<char*>(&envelope), sizeof(envelope))) {
    discardDiskEntryLocked(key);
    return false;
  }
  uint64_t available = static_cast<uint64_t>(fileSize) - sizeof(DiskEnvelope);
  if (envelope.magic != kDiskMagic || envelope.payloadSize == 0 ||
      envelope.payloadSize > available || envelope.kind > 3) {
    // Written by an older/incompatible version — drop it so the caller rebuilds.
    discardDiskEntryLocked(key);
    return false;
  }

  auto blob = std::make_shared<MediaBlob>();
  blob->bytes.resize(static_cast<size_t>(envelope.payloadSize));
  if (!binIn.read(reinterpret_cast<char*>(blob->bytes.data()),
                  static_cast<std::streamsize>(envelope.payloadSize))) {
    discardDiskEntryLocked(key);
    return false;
  }
  blob->kind = static_cast<MediaBlob::Kind>(envelope.kind);
  blob->width = envelope.width;
  blob->height = envelope.height;
  blob->colorSpace = envelope.colorSpace;
  blob->faceIndex = envelope.faceIndex;
  blob->versionMtime = envelope.versionMtime;
  blob->sourceSize = envelope.payloadSize;
  blob->restoredFromDisk = true;
  out = std::move(blob);
  return true;
}

void MediaCache::writeToDiskLocked(const std::string& key, const MediaBlob& blob) {
  if (_cacheDir.empty()) {
    return;
  }
  std::error_code ec;
  fs::path dir = fs::path(_cacheDir) / "npdf-media-cache";
  fs::create_directories(dir, ec);
  std::string binPath = diskPathFor(key);
  std::string keyPath = binPath.substr(0, binPath.size() - 4) + ".key";
  std::string keyTmp = temporaryPathFor(keyPath);
  std::string binTmp = temporaryPathFor(binPath);

  // Every early exit removes both temp files: a failed write must not leave
  // .tmp litter behind for the next scan to trip over.
  auto abandon = [&]() {
    fs::remove(binTmp, ec);
    fs::remove(keyTmp, ec);
  };

  // A rewrite of the same key replaces the previous payload, so the accounted
  // total has to drop the old files first or the budget drifts upward.
  size_t previousBytes = 0;
  if (_diskBytesKnown) {
    auto sizeOf = [](const std::string& path) -> size_t {
      std::error_code ec2;
      auto s = fs::file_size(path, ec2);
      return ec2 ? 0 : static_cast<size_t>(s);
    };
    previousBytes = sizeOf(binPath) + sizeOf(keyPath);
  }

  // Write through temp files so a crash mid-write never leaves a half payload
  // that a later run would happily load.
  {
    std::ofstream keyOut(keyTmp, std::ios::binary | std::ios::trunc);
    if (!keyOut) {
      abandon();
      return;
    }
    keyOut.write(key.data(), static_cast<std::streamsize>(key.size()));
    if (!keyOut) {
      abandon();
      return;
    }
  }

  DiskEnvelope envelope{};
  envelope.magic = kDiskMagic;
  envelope.kind = static_cast<uint32_t>(blob.kind);
  envelope.width = blob.width;
  envelope.height = blob.height;
  envelope.colorSpace = blob.colorSpace;
  envelope.faceIndex = blob.faceIndex;
  envelope.versionMtime = blob.versionMtime;
  envelope.payloadSize = blob.bytes.size();

  {
    std::ofstream binOut(binTmp, std::ios::binary | std::ios::trunc);
    if (!binOut) {
      abandon();
      return;
    }
    binOut.write(reinterpret_cast<const char*>(&envelope), sizeof(envelope));
    if (!blob.bytes.empty()) {
      binOut.write(reinterpret_cast<const char*>(blob.bytes.data()),
                   static_cast<std::streamsize>(blob.bytes.size()));
    }
    if (!binOut) {
      abandon();
      return;
    }
  }

  // Commit payload first, sidecar last. The .key file is what makes an entry
  // loadable, so a crash between the two renames leaves an orphan .bin (harmless,
  // collected by the next prune) instead of a .key pointing at stale bytes from
  // another key that happens to hash into the same slot.
  if (!replaceFile(binTmp, binPath)) {
    abandon();
    return;
  }
  if (!replaceFile(keyTmp, keyPath)) {
    fs::remove(binPath, ec);
    abandon();
    return;
  }

  if (!_diskBytesKnown) {
    scanDiskUsageLocked();
  }
  size_t written = sizeof(DiskEnvelope) + blob.bytes.size() + key.size();
  _diskBytes = _diskBytes + written - (previousBytes > _diskBytes ? _diskBytes : previousBytes);

  // Pruning walks the directory, so amortize it instead of paying per write.
  if (++_diskWritesSincePrune >= 32 || _diskBytes > _maxDiskBytes) {
    pruneDiskLocked(&key);
  }
}

void MediaCache::discardDiskEntryLocked(const std::string& key) {
  if (_cacheDir.empty()) {
    return;
  }
  std::error_code ec;
  std::string binPath = diskPathFor(key);
  std::string keyPath = binPath.substr(0, binPath.size() - 4) + ".key";
  auto sizeOf = [](const std::string& path) -> size_t {
    std::error_code ec2;
    auto s = fs::file_size(path, ec2);
    return ec2 ? 0 : static_cast<size_t>(s);
  };
  if (_diskBytesKnown) {
    size_t released = sizeOf(binPath) + sizeOf(keyPath);
    _diskBytes = released >= _diskBytes ? 0 : _diskBytes - released;
  }
  fs::remove(binPath, ec);
  fs::remove(keyPath, ec);
}

void MediaCache::clearDiskLocked() {
  _diskBytes = 0;
  _diskBytesKnown = true;
  _diskWritesSincePrune = 0;
  if (_cacheDir.empty()) {
    return;
  }
  // Only remove cache payload (*.bin / *.key). Materialized font files under
  // font-src/ are owned by live MediaBlobs and deleted in ~MediaBlob.
  std::error_code ec;
  fs::path dir = fs::path(_cacheDir) / "npdf-media-cache";
  if (!fs::exists(dir, ec)) {
    return;
  }
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (ec) {
      return;
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    std::string name = entry.path().filename().string();
    if (name.size() > 4 && (name.compare(name.size() - 4, 4, ".bin") == 0 ||
                            name.compare(name.size() - 4, 4, ".key") == 0 ||
                            name.compare(name.size() - 4, 4, ".tmp") == 0)) {
      fs::remove(entry.path(), ec);
    }
  }
}

void MediaCache::scanDiskUsageLocked() {
  _diskBytes = 0;
  _diskBytesKnown = true;
  if (_cacheDir.empty()) {
    return;
  }
  std::error_code ec;
  fs::path dir = fs::path(_cacheDir) / "npdf-media-cache";
  if (!fs::exists(dir, ec)) {
    return;
  }
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (ec) {
      return;
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    std::string name = entry.path().filename().string();
    if (name.size() > 4 && (name.compare(name.size() - 4, 4, ".bin") == 0 ||
                            name.compare(name.size() - 4, 4, ".key") == 0)) {
      auto size = fs::file_size(entry.path(), ec);
      if (!ec) {
        _diskBytes += static_cast<size_t>(size);
      }
    }
  }
}

void MediaCache::pruneDiskLocked(const std::string* keepKey) {
  _diskWritesSincePrune = 0;
  if (_cacheDir.empty()) {
    return;
  }
  std::error_code ec;
  fs::path dir = fs::path(_cacheDir) / "npdf-media-cache";
  if (!fs::exists(dir, ec)) {
    _diskBytes = 0;
    _diskBytesKnown = true;
    return;
  }

  const std::string keepBin = keepKey != nullptr ? diskPathFor(*keepKey) : std::string();

  // Sidecars seen in this pass; checked for a missing payload after the scan
  // (removing files while iterating a directory entry list is not portable).
  std::vector<fs::path> sidecars;

  struct Victim {
    int64_t mtime;
    size_t bytes;
    fs::path bin;
  };
  std::vector<Victim> victims;
  size_t total = 0;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (ec) {
      return;
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    std::string name = entry.path().filename().string();
    auto size = fs::file_size(entry.path(), ec);
    if (ec) {
      continue;
    }

    // Interrupted writes never left a usable pair; they only waste budget.
    // prune runs under the cache mutex, so no writer can be mid-flight here.
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
      fs::remove(entry.path(), ec);
      continue;
    }

    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".key") == 0) {
      sidecars.push_back(entry.path());
      continue;
    }

    if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".bin") != 0) {
      continue;
    }

    // The sidecar counts toward the budget too, so it has to be part of the
    // total and of every victim's weight — otherwise the cap never holds.
    std::string keyPath = entry.path().string();
    keyPath.replace(keyPath.size() - 4, 4, ".key");
    size_t keyBytes = 0;
    {
      auto s = fs::file_size(keyPath, ec);
      if (!ec) {
        keyBytes = static_cast<size_t>(s);
      }
    }

    auto mtime = fs::last_write_time(entry.path(), ec);
    int64_t stamp = 0;
    if (!ec) {
      stamp = static_cast<int64_t>(mtime.time_since_epoch().count());
    }
    const size_t pairBytes = static_cast<size_t>(size) + keyBytes;
    total += pairBytes;
    victims.push_back({stamp, pairBytes, entry.path()});
  }

  // A .key without its .bin cannot be loaded (load requires the sidecar) and
  // is never a prune victim, so it would occupy budget forever. Drop it here.
  for (const auto& keyPath : sidecars) {
    std::string binPath = keyPath.string();
    binPath.replace(binPath.size() - 4, 4, ".bin");
    if (!fs::exists(binPath, ec)) {
      fs::remove(keyPath, ec);
    }
  }

  if (total <= _maxDiskBytes) {
    _diskBytes = total;
    _diskBytesKnown = true;
    return;
  }

  std::sort(victims.begin(), victims.end(),
            [](const Victim& a, const Victim& b) { return a.mtime < b.mtime; });
  for (const auto& victim : victims) {
    if (total <= _maxDiskBytes) {
      break;
    }
    // Never evict the payload that was just written: coarse mtime granularity
    // (1s on many filesystems) would otherwise make it the oldest candidate.
    if (!keepBin.empty() && victim.bin.string() == keepBin) {
      continue;
    }
    std::string binPath = victim.bin.string();
    std::string keyPath = binPath.substr(0, binPath.size() - 4) + ".key";
    fs::remove(victim.bin, ec);
    fs::remove(keyPath, ec);
    total = victim.bytes >= total ? 0 : total - victim.bytes;
  }
  _diskBytes = total;
  _diskBytesKnown = true;
}

void MediaCache::discardEntry(const std::string& key, const MediaBlob* expected) {
  std::unique_lock lock(_mutex);
  auto it = _map.find(key);
  if (it != _map.end()) {
    if (expected != nullptr && it->second.blob.get() != expected) {
      return; // a racing put() replaced it; leave the fresh entry alone
    }
    _totalBytes -= it->second.byteSize;
    _map.erase(it);
  }
  discardDiskEntryLocked(key);
}

double MediaRegistry::registerBlob(std::shared_ptr<MediaBlob> blob) {
  if (!blob) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(_mutex);
  double handle = ++_nextHandle;
  _entries.emplace(handle, std::move(blob));
  return handle;
}

std::shared_ptr<MediaBlob> MediaRegistry::get(double handle) const {
  std::lock_guard<std::mutex> lock(_mutex);
  auto it = _entries.find(handle);
  return it == _entries.end() ? nullptr : it->second;
}

void MediaRegistry::release(double handle) {
  std::lock_guard<std::mutex> lock(_mutex);
  _entries.erase(handle);
}

void MediaRegistry::clear() {
  std::lock_guard<std::mutex> lock(_mutex);
  _entries.clear();
}

size_t MediaRegistry::size() const {
  std::lock_guard<std::mutex> lock(_mutex);
  return _entries.size();
}

} // namespace margelo::nitro::pdfwriter
