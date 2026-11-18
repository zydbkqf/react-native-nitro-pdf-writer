#include <jni.h>

#include <string>
#include <vector>

#include "PlatformImageTranscoder.hpp"

using namespace margelo::nitro::pdfwriter;

namespace {

JavaVM* g_vm = nullptr;
jclass g_decoderClass = nullptr;   // global ref to NitroPdfWriterImageTranscoder
jmethodID g_decodeMethod = nullptr; // static decode(byte[], String, int, int): byte[]

std::string describeThrowable(JNIEnv* env, jthrowable throwable) {
  if (throwable == nullptr) {
    return {};
  }
  jclass cls = env->GetObjectClass(throwable);
  std::string message;
  if (cls != nullptr) {
    jmethodID getMessage = env->GetMethodID(cls, "getMessage", "()Ljava/lang/String;");
    if (getMessage != nullptr) {
      jstring value = static_cast<jstring>(env->CallObjectMethod(throwable, getMessage));
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
      } else if (value != nullptr) {
        const char* chars = env->GetStringUTFChars(value, nullptr);
        if (chars != nullptr) {
          message.assign(chars);
          env->ReleaseStringUTFChars(value, chars);
        }
        env->DeleteLocalRef(value);
      }
    }
    env->DeleteLocalRef(cls);
  }
  return message;
}

std::string takePendingException(JNIEnv* env) {
  if (!env->ExceptionCheck()) {
    return {};
  }
  jthrowable throwable = env->ExceptionOccurred();
  env->ExceptionClear();
  std::string message = describeThrowable(env, throwable);
  if (throwable != nullptr) {
    env->DeleteLocalRef(throwable);
  }
  return message;
}

/** Resolve the JNIEnv for the current (worker) thread, attaching if needed. */
JNIEnv* currentEnv() {
  if (g_vm == nullptr) {
    return nullptr;
  }
  JNIEnv* env = nullptr;
  jint status = g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (status == JNI_EDETACHED) {
    // MediaWorker threads stay attached: they are long lived and every
    // transcode happens on one of them.
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
      return nullptr;
    }
  } else if (status != JNI_OK) {
    return nullptr;
  }
  return env;
}

bool transcodeViaKotlin(const uint8_t* data, size_t size, MediaPrep::ImageContainer container,
                        const MediaPrep::TranscodeOptions& options,
                        MediaPrep::TranscodedImage& out, std::string& error) {
  JNIEnv* env = currentEnv();
  if (env == nullptr || g_decoderClass == nullptr || g_decodeMethod == nullptr) {
    error = "Android image transcoder is not initialized";
    return false;
  }

  jbyteArray input = env->NewByteArray(static_cast<jsize>(size));
  if (input == nullptr) {
    error = takePendingException(env);
    if (error.empty()) {
      error = "Out of memory while copying the image into JNI";
    }
    return false;
  }
  env->SetByteArrayRegion(input, 0, static_cast<jsize>(size),
                          reinterpret_cast<const jbyte*>(data));
  // SetByteArrayRegion can throw (OutOfMemoryError). Calling another JNI
  // function while an exception is pending is undefined behaviour.
  if (env->ExceptionCheck()) {
    std::string pending = takePendingException(env);
    env->DeleteLocalRef(input);
    error = pending.empty() ? "Failed to copy the image into JNI" : pending;
    return false;
  }

  jstring mime = env->NewStringUTF(MediaPrep::containerMime(container));
  if (mime == nullptr) {
    // NewStringUTF returns null on OOM; passing null to a non-null Kotlin
    // parameter would throw IllegalArgumentException with a confusing message.
    env->ExceptionClear();
    env->DeleteLocalRef(input);
    error = "Out of memory while creating the image mime string";
    return false;
  }

  jobject result = env->CallStaticObjectMethod(
      g_decoderClass, g_decodeMethod, input, mime,
      static_cast<jint>(options.maxPixelDimension), static_cast<jint>(options.jpegQuality));

  std::string pending = takePendingException(env);
  env->DeleteLocalRef(input);
  if (mime != nullptr) {
    env->DeleteLocalRef(mime);
  }
  if (!pending.empty()) {
    // This is how HEIC on API 24-27 surfaces: the Kotlin side throws with a
    // message naming the required API level.
    error = pending;
    return false;
  }
  if (result == nullptr) {
    error = std::string("Android decoder returned no data for ") +
            MediaPrep::containerName(container) + " image";
    return false;
  }

  jbyteArray encoded = static_cast<jbyteArray>(result);
  const jsize length = env->GetArrayLength(encoded);
  if (length <= 0) {
    env->DeleteLocalRef(result);
    error = "Android decoder produced an empty payload";
    return false;
  }
  out.bytes.resize(static_cast<size_t>(length));
  env->GetByteArrayRegion(encoded, 0, length, reinterpret_cast<jbyte*>(out.bytes.data()));
  env->DeleteLocalRef(result);

  // The Kotlin side already decided PNG vs JPEG; trust the bytes.
  MediaPrep::ImageContainer produced =
      MediaPrep::sniffImageContainer(out.bytes.data(), out.bytes.size());
  if (produced == MediaPrep::ImageContainer::Png) {
    out.kind = MediaBlob::Kind::Png;
  } else if (produced == MediaPrep::ImageContainer::Jpeg) {
    out.kind = MediaBlob::Kind::Jpeg;
  } else {
    error = "Android decoder returned an unsupported payload";
    return false;
  }
  out.width = 0;  // dimensions are re-parsed from the encoded header by the core
  out.height = 0;
  out.hadAlpha = out.kind == MediaBlob::Kind::Png;
  return true;
}

} // namespace

/** Called from cpp-adapter.cpp's JNI_OnLoad so worker threads can find the VM. */
extern "C" void NitroPdfWriterAndroidSetJavaVM(JavaVM* vm) {
  g_vm = vm;
}

extern "C" JNIEXPORT void JNICALL
Java_com_margelo_nitro_pdfwriter_NitroPdfWriterImageTranscoder_register(JNIEnv* env, jclass clazz) {
  if (g_decoderClass != nullptr) {
    return;
  }
  g_decoderClass = static_cast<jclass>(env->NewGlobalRef(clazz));
  if (g_decoderClass == nullptr) {
    return;
  }
  g_decodeMethod =
      env->GetStaticMethodID(g_decoderClass, "decode", "([BLjava/lang/String;II)[B");
  if (g_decodeMethod == nullptr) {
    takePendingException(env);
    env->DeleteGlobalRef(g_decoderClass);
    g_decoderClass = nullptr;
    return;
  }
  MediaPrep::setPlatformImageTranscoder(&transcodeViaKotlin);
}
