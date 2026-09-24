#import <Foundation/Foundation.h>
#include "MediaCache.hpp"
#include <string>

/**
 * Auto-wire the iOS Caches directory into the shared media cache.
 * JS may override this later via `NitroPdfWriterInstance.setCacheDir(...)`.
 */
__attribute__((constructor)) static void NitroPdfWriterSetDefaultCacheDir(void) {
  @autoreleasepool {
    NSArray<NSString*>* dirs = NSSearchPathForDirectoriesInDomains(
        NSCachesDirectory, NSUserDomainMask, YES);
    if (dirs.count == 0) {
      return;
    }
    NSString* dir = [dirs.firstObject stringByAppendingPathComponent:@"nitro-pdf-writer"];
    margelo::nitro::pdfwriter::MediaCache::instance().setCacheDir(
        std::string([dir UTF8String]));
  }
}
