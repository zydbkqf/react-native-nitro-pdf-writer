#include <jni.h>
#include <fbjni/fbjni.h>
#include "NitroPdfWriterOnLoad.hpp"
#include "MediaCache.hpp"

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  return facebook::jni::initialize(vm, []() {
    margelo::nitro::pdfwriter::registerAllNatives();
  });
}

extern "C" JNIEXPORT void JNICALL
Java_com_margelo_nitro_pdfwriter_NitroPdfWriterCacheDir_set(
    JNIEnv* env, jclass, jstring path) {
  if (path == nullptr) {
    return;
  }
  const char* chars = env->GetStringUTFChars(path, nullptr);
  if (chars == nullptr) {
    return;
  }
  margelo::nitro::pdfwriter::MediaCache::instance().setCacheDir(std::string(chars));
  env->ReleaseStringUTFChars(path, chars);
}
