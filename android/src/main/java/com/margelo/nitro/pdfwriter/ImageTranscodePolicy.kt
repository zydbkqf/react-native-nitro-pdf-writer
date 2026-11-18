package com.margelo.nitro.pdfwriter

/**
 * Pure decisions behind the HEIC/WebP transcode: which API levels can decode
 * what, how much to subsample, and whether the output should be PNG or JPEG.
 *
 * Kept free of `android.graphics` calls so it can be covered by plain JVM unit
 * tests; the bitmap work lives in [NitroPdfWriterImageTranscoder].
 */
internal object ImageTranscodePolicy {
  /** Android 9 added platform HEIF decoding (ImageDecoder). */
  const val HEIF_MIN_API = 28

  /** Android 12 added AVIF to ImageDecoder. */
  const val AVIF_MIN_API = 31

  data class TargetSize(val width: Int, val height: Int)

  /**
   * Rejection message when the platform cannot decode `mime` at `sdkInt`,
   * or null when decoding may proceed.
   */
  fun unsupportedMessage(mime: String, sdkInt: Int): String? {
    if (mime == "image/heic" || mime == "image/heif") {
      if (sdkInt < HEIF_MIN_API) {
        return "HEIC/HEIF decoding requires Android 9 (API $HEIF_MIN_API) or newer; " +
          "this device runs API $sdkInt. Convert the image to JPEG or PNG before loading it."
      }
    }
    if (mime == "image/avif" && sdkInt < AVIF_MIN_API) {
      return "AVIF decoding requires Android 12 (API $AVIF_MIN_API) or newer; " +
        "this device runs API $sdkInt."
    }
    return null
  }

  /**
   * Power-of-two subsample that keeps the long edge at or above [maxDimension]
   * (BitmapFactory only accepts powers of two). Always at least 1.
   */
  fun sampleSize(width: Int, height: Int, maxDimension: Int): Int {
    if (maxDimension <= 0 || width <= 0 || height <= 0) {
      return 1
    }
    val longest = maxOf(width, height)
    var sample = 1
    while (longest / (sample * 2) >= maxDimension) {
      sample *= 2
    }
    return sample
  }

  /** Target size after scaling, or null when the image already fits. */
  fun scaledSize(width: Int, height: Int, maxDimension: Int): TargetSize? {
    if (maxDimension <= 0 || width <= 0 || height <= 0) {
      return null
    }
    val longest = maxOf(width, height)
    if (longest <= maxDimension) {
      return null
    }
    val ratio = maxDimension.toDouble() / longest.toDouble()
    val targetWidth = maxOf(1, (width * ratio).toInt())
    val targetHeight = maxOf(1, (height * ratio).toInt())
    return TargetSize(targetWidth, targetHeight)
  }

  /** PNG keeps alpha for libharu's SMask; JPEG is smaller for opaque photos. */
  fun isPng(hasAlpha: Boolean): Boolean = hasAlpha

  /** PNG ignores quality; clamp the JPEG quality into the accepted range. */
  fun clampQuality(jpegQuality: Int): Int = jpegQuality.coerceIn(1, 100)
}
