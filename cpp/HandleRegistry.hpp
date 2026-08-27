#pragma once

#include <hpdf.h>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>

namespace margelo::nitro::pdfwriter {

/**
 * Typed handle registry. Maps integer handles to libHaru pointers and, when
 * applicable, the owning HPDF_Doc. All access must be performed while HaruLock
 * is held.
 */
template <typename T>
class HandleRegistry {
public:
  struct Entry {
    T pointer;
    HPDF_Doc owner;
  };

  double registerPointer(T pointer, HPDF_Doc owner = nullptr) {
    if (pointer == nullptr) {
      return 0;
    }
    double handle = ++_nextHandle;
    _entries[handle] = {pointer, owner};
    _pointerToHandle[pointer] = handle;
    return handle;
  }

  const Entry* getEntry(double handle) const {
    auto it = _entries.find(handle);
    if (it == _entries.end()) {
      return nullptr;
    }
    return &it->second;
  }

  T getPointer(double handle) const {
    auto entry = getEntry(handle);
    return entry ? entry->pointer : nullptr;
  }

  HPDF_Doc getOwner(double handle) const {
    auto entry = getEntry(handle);
    return entry ? entry->owner : nullptr;
  }

  double getHandle(T pointer) const {
    auto it = _pointerToHandle.find(pointer);
    if (it == _pointerToHandle.end()) {
      return 0;
    }
    return it->second;
  }

  void unregisterPointer(T pointer) {
    auto it = _pointerToHandle.find(pointer);
    if (it == _pointerToHandle.end()) {
      return;
    }
    double handle = it->second;
    _pointerToHandle.erase(it);
    _entries.erase(handle);
  }

  void unregisterHandle(double handle) {
    auto it = _entries.find(handle);
    if (it == _entries.end()) {
      return;
    }
    T pointer = it->second.pointer;
    _entries.erase(it);
    _pointerToHandle.erase(pointer);
  }

  void clear() {
    _entries.clear();
    _pointerToHandle.clear();
    _nextHandle = 0;
  }

  const std::unordered_map<double, Entry>& allEntries() const {
    return _entries;
  }

private:
  double _nextHandle = 0;
  std::unordered_map<double, Entry> _entries;
  std::unordered_map<T, double> _pointerToHandle;
};

/**
 * Strongly-typed wrapper for a handle that validates the underlying pointer.
 */
template <typename T>
class Handle {
public:
  Handle(double handle, const HandleRegistry<T>& registry) : _handle(handle), _registry(registry) {}

  bool isValid() const {
    return _registry.getPointer(_handle) != nullptr;
  }

  T get() const {
    T pointer = _registry.getPointer(_handle);
    if (pointer == nullptr) {
      throw std::invalid_argument("Invalid handle");
    }
    return pointer;
  }

  HPDF_Doc owner() const {
    return _registry.getOwner(_handle);
  }

  double value() const {
    return _handle;
  }

private:
  double _handle;
  const HandleRegistry<T>& _registry;
};

} // namespace margelo::nitro::pdfwriter
