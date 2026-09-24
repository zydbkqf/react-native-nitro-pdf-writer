package com.margelo.nitro.pdfwriter

/**
 * Bridge for passing the platform cache directory into the native media cache.
 * Called automatically from NitroPdfWriterPackage; safe to call again from app code.
 */
object NitroPdfWriterCacheDir {
  init {
    NitroPdfWriterOnLoad.initializeNative()
  }

  @JvmStatic
  external fun set(path: String)
}
