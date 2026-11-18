# R8/ProGuard rules shipped to host apps (consumer rules).
#
# The C++ bridge reaches these classes purely through JNI names:
#   - AndroidImageTranscoder.cpp does FindClass("...NitroPdfWriterImageTranscoder")
#     and GetStaticMethodID(..., "decode", ...)
# If R8 renames or strips them, the lookup silently yields null and every
# HEIC/WebP load fails with "Android image transcoder is not initialized".

-keep class com.margelo.nitro.pdfwriter.NitroPdfWriterImageTranscoder {
  <methods>;
}

-keep class com.margelo.nitro.pdfwriter.ImageTranscodePolicy {
  <methods>;
}

# The native entry point is looked up by symbol name from JNI.
-keepclasseswithmembernames class * {
  native <methods>;
}
