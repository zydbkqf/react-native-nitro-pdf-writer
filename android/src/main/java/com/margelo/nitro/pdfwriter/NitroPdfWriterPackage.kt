package com.margelo.nitro.pdfwriter

import com.facebook.react.ReactPackage
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.NativeModule
import com.facebook.react.uimanager.ViewManager

class NitroPdfWriterPackage : ReactPackage {
  companion object {
    init {
      NitroPdfWriterOnLoad.initializeNative()
    }
  }

  @Suppress("OVERRIDE_DEPRECATION")
  override fun createNativeModules(reactContext: ReactApplicationContext): List<NativeModule> {
    // Auto-wire the platform cache dir so the media cache can persist across restarts.
    try {
      NitroPdfWriterCacheDir.set(reactContext.cacheDir.absolutePath + "/nitro-pdf-writer")
    } catch (_: Throwable) {
      // Native lib not loaded yet — JS can still call setCacheDir later.
    }
    return emptyList()
  }

  override fun createViewManagers(reactContext: ReactApplicationContext): List<ViewManager<*, *>> {
    return emptyList()
  }
}
