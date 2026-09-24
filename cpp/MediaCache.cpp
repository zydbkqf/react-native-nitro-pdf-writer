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
  if (!_cacheDir.empty()) {
    std::error_code ec;
    fs::create_directories(_cacheDir, ec);
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

void MediaCache::clear() {
  std::unique_lock lock(_mutex);
  for (auto it = _map.begin(); it != _map.end();) {
    if (it->second.refCount <= 0) {
      _totalBytes -= it->second.byteSize;
      it = _map.erase(it);
    } else {
      ++it;
    }
  }
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
  char name[32];
  std::snprintf(name, sizeof(name), "%016llx.bin", static_cast<unsigned long long>(h));
  return (fs::path(_cacheDir) / "npdf-media-cache" / name).string();
}

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
  auto size = binIn.tellg();
  if (size <= 0) {
    return false;
  }
  binIn.seekg(0, std::ios::beg);
  auto blob = std::make_shared<MediaBlob>();
  blob->bytes.resize(static_cast<size_t>(size));
  if (!binIn.read(reinterpret_cast<char*>(blob->bytes.data()), size)) {
    return false;
  }
  if (key.rfind("f:font:", 0) == 0) {
    blob->kind = MediaBlob::Kind::Font;
  } else if (key.rfind("f:png:", 0) == 0 || key.rfind("b:png:", 0) == 0) {
    blob->kind = MediaBlob::Kind::Png;
  } else if (key.rfind("f:jpeg:", 0) == 0 || key.rfind("b:jpeg:", 0) == 0) {
    blob->kind = MediaBlob::Kind::Jpeg;
  } else {
    blob->kind = MediaBlob::Kind::Raw;
  }
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
  {
    std::ofstream keyOut(keyPath, std::ios::binary | std::ios::trunc);
    if (!keyOut) {
      return;
    }
    keyOut.write(key.data(), static_cast<std::streamsize>(key.size()));
  }
  std::ofstream binOut(binPath, std::ios::binary | std::ios::trunc);
  if (!binOut) {
    return;
  }
  if (!blob.bytes.empty()) {
    binOut.write(reinterpret_cast<const char*>(blob.bytes.data()),
                 static_cast<std::streamsize>(blob.bytes.size()));
  }
}

void MediaCache::clearDiskLocked() {
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
                            name.compare(name.size() - 4, 4, ".key") == 0)) {
      fs::remove(entry.path(), ec);
    }
  }
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
