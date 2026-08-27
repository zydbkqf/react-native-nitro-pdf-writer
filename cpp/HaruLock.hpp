#pragma once

#include <mutex>

namespace margelo::nitro::pdfwriter {

/**
 * libHaru is not thread-safe. Every native call that touches an HPDF handle
 * must hold this mutex for the duration of the operation.
 */
class HaruLock {
public:
  static std::mutex& get() {
    static std::mutex mutex;
    return mutex;
  }

  static std::unique_lock<std::mutex> acquire() {
    return std::unique_lock<std::mutex>(get());
  }
};

} // namespace margelo::nitro::pdfwriter
